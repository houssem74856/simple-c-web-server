#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#define PORT 9000
#define MAX_EVENTS 32
#define WORKER_COUNT 4

/* client states */
#define STATE_READING     0
#define STATE_IN_WORKER   1
#define STATE_WAITING_OUT 2
#define STATE_WRITING     3

struct client {
  int fd;
  char buffer[4096];
  int buffer_len;
  char header[512];
  int header_len;
  ssize_t write_offset;
  int state;
  struct client *next;
};

struct job_queue {
  struct client *head;
  struct client *tail;
  pthread_mutex_t m;
  pthread_cond_t c;
} jobs;

struct completed_queue {
  struct client *head;
  struct client *tail;
  pthread_mutex_t m;
} completed;

int event_fd;
int epoll_fd;
atomic_int file_counter = 0;

void push_job(struct client *c) {
  pthread_mutex_lock(&jobs.m);
  c->next = NULL;
  if (!jobs.tail) jobs.head = jobs.tail = c;
  else { jobs.tail->next = c; jobs.tail = c; }
  pthread_cond_signal(&jobs.c);
  pthread_mutex_unlock(&jobs.m);
}

struct client *pop_job() {
  pthread_mutex_lock(&jobs.m);
  while (!jobs.head) pthread_cond_wait(&jobs.c, &jobs.m);
  struct client *c = jobs.head;
  jobs.head = c->next;
  if (!jobs.head) jobs.tail = NULL;
  pthread_mutex_unlock(&jobs.m);
  c->next = NULL;
  return c;
}

void push_completed(struct client *c) {
  pthread_mutex_lock(&completed.m);
  c->next = NULL;
  if (!completed.tail) completed.head = completed.tail = c;
  else { completed.tail->next = c; completed.tail = c; }
  pthread_mutex_unlock(&completed.m);

  uint64_t one = 1;
  // signal main thread
  if (write(event_fd, &one, sizeof(one)) != sizeof(one)) {
      // non-fatal, ignore
  }
}

void *worker_thread(void *arg) {
  (void)arg;
  while (1) {
    struct client *c = pop_job();
    if (!c) continue;

    // Build a filename: /workspace/request_<fd>_<counter>.txt
    int id = atomic_fetch_add(&file_counter, 1);
    char filename[256];
    snprintf(filename, sizeof(filename), "/workspace/request_%d_%d.txt", c->fd, id);

    // write request into file (overwrite)
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd != -1) {
      ssize_t to_write = c->buffer_len;
      ssize_t offset = 0;
      while (to_write > 0) {
        ssize_t w = write(fd, c->buffer + offset, to_write);
        if (w < 0) {
          if (errno == EINTR) continue;
            break;
        }
        offset += w;
        to_write -= w;
      }
      close(fd);
    }
    // done with file - notify main loop
    // mark state
    c->state = STATE_WAITING_OUT;
    push_completed(c);
  }
  return NULL;
}

void compose_header(struct client *c) {
  c->header_len = snprintf(
    c->header, 
    sizeof(c->header),
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: %d\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n",
    c->buffer_len
  );
  if (c->header_len < 0) c->header_len = 0;
  c->write_offset = 0;
}

/* returns 1 if finished and closed; 0 otherwise */
int attempt_write_client(struct client *c) {
  ssize_t total = c->header_len + c->buffer_len;
  while (c->write_offset < total) {
    ssize_t w;
    if (c->write_offset < c->header_len) {
      ssize_t off = c->write_offset;
      ssize_t need = c->header_len - off;
      w = write(c->fd, c->header + off, need);
      if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; // wait for EPOLLOUT
        return 1; // error -> close
      }
      c->write_offset += w;
      continue;
    } else {
      ssize_t body_off = c->write_offset - c->header_len;
      ssize_t need = c->buffer_len - body_off;
      w = write(c->fd, c->buffer + body_off, need);
      if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return 1;
      }
      c->write_offset += w;
    }
  }
  return 1;
}

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
  // initialize queues
  pthread_mutex_init(&jobs.m, NULL);
  pthread_cond_init(&jobs.c, NULL);
  jobs.head = jobs.tail = NULL;
  pthread_mutex_init(&completed.m, NULL);
  completed.head = completed.tail = NULL;

  // start worker threads
  pthread_t workers[WORKER_COUNT];
  for (int i = 0; i < WORKER_COUNT; ++i) {
    if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0) {
      perror("pthread_create");
      exit(1);
    }
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) { perror("socket"); exit(1); }
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT) };
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) { perror("bind"); exit(1); }
  if (listen(server_fd, SOMAXCONN) == -1) { perror("listen"); exit(1); }
  set_nonblocking(server_fd);

  // create eventfd for worker->main notifications
  event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (event_fd == -1) { perror("eventfd"); exit(1); }

  // create epoll
  epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

  // add server_fd to epoll
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) { perror("epoll_ctl add server"); exit(1); }

  // add event_fd to epoll
  struct epoll_event ev2;
  ev2.events = EPOLLIN;
  ev2.data.fd = event_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev2) == -1) { perror("epoll_ctl add eventfd"); exit(1); }

  struct epoll_event events[MAX_EVENTS];
  printf("Server with thread-pool running on port %d...\n", PORT);

  while (1) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (n == -1) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < n; ++i) {
      if (events[i].data.fd == server_fd) {
        // accept loop (non-blocking)
        while (1) {
          int client_fd = accept(server_fd, NULL, NULL);
          if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
          }
          set_nonblocking(client_fd);
          struct client *c = calloc(1, sizeof(struct client));
          c->fd = client_fd;
          c->buffer_len = 0;
          c->write_offset = 0;
          c->state = STATE_READING;

          struct epoll_event cev;
          cev.events = EPOLLIN;
          cev.data.ptr = c;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) == -1) {
            perror("epoll_ctl add client");
            close(client_fd);
            free(c);
          }
        }
        continue;
      }

      // eventfd notification (workers finished jobs)
      if (events[i].data.fd == event_fd) {
        uint64_t cnt;
        // consume the eventfd counter
        if (read(event_fd, &cnt, sizeof(cnt)) != sizeof(cnt)) {
          // ignore
        }

        // pop all completed clients and re-add them for EPOLLOUT
        pthread_mutex_lock(&completed.m);
        struct client *cur = completed.head;
        completed.head = completed.tail = NULL;
        pthread_mutex_unlock(&completed.m);

        for (; cur != NULL; ) {
          struct client *next = cur->next;
          // compose header now in main thread
          compose_header(cur);
          cur->state = STATE_WAITING_OUT;

          // re-add fd to epoll with EPOLLOUT interest
          struct epoll_event out_ev;
          out_ev.events = EPOLLOUT;
          out_ev.data.ptr = cur;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cur->fd, &out_ev) == -1) {
            // if add fails (maybe fd was closed), clean up
            close(cur->fd);
            free(cur);
          }
          cur = next;
        }
        continue;
      }

      // client events: events[i].data.ptr is a client pointer
      struct client *c = (struct client *)events[i].data.ptr;
      if (!c) continue;

      // If EPOLLIN and we are in READING state -> read and enqueue job
      if ((events[i].events & EPOLLIN) && c->state == STATE_READING) {
        int count = read(c->fd, c->buffer, sizeof(c->buffer) - 1);
        if (count <= 0) {
          // client closed or error
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
          close(c->fd);
          free(c);
          continue;
        }
        c->buffer[count] = '\0';
        c->buffer_len = count;

        // remove fd from epoll while worker works
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL) == -1) {
          // ignore error
        }

        // set state and push job to workers
        c->state = STATE_IN_WORKER;
        push_job(c);
        continue;
      }

      // EPOLLOUT: try to write response (either newly scheduled or partial)
      if (events[i].events & EPOLLOUT) {
        // mark writing state
        if (c->state == STATE_WAITING_OUT || c->state == STATE_WRITING) {
          c->state = STATE_WRITING;
          int done_or_error = attempt_write_client(c);
          if (done_or_error) {
            // finished or error -> cleanup
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
            close(c->fd);
            free(c);
          } else {
            // partial write -> keep EPOLLOUT registered (do nothing)
            // epoll will notify when writable again
          }
        } else {
          // unexpected state: clean up
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
          close(c->fd);
          free(c);
        }
        continue;
      }

      // any other event: close
      epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
      close(c->fd);
      free(c);
    } // for each ready event
  } // while epoll

  // cleanup (not reached normally)
  close(event_fd);
  close(server_fd);
  return 0;
}