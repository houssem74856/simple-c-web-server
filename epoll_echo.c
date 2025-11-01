#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 9000
#define MAX_EVENTS 10

struct client {
    int fd;
    char buffer[4096];
    int buffer_len;
};

int make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(sfd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); exit(1);
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen"); exit(1);
    }

    make_socket_non_blocking(server_fd);

    int epoll_fd = epoll_create1(0);

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = server_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    struct epoll_event events[MAX_EVENTS];
    printf("Server running on port %d...\n", PORT);

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                int client_fd = accept(server_fd, NULL, NULL);
                make_socket_non_blocking(client_fd);

                struct client *c = malloc(sizeof(struct client));
                c->fd = client_fd;
                c->buffer_len = 0;

                struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
            } else {
                struct client *c = events[i].data.ptr;

                if (events[i].events & EPOLLIN) {
                    // Read request
                    int count = read(c->fd, c->buffer, sizeof(c->buffer) - 1);
                    if (count <= 0) {
                        close(c->fd);
                        free(c);
                        continue;
                    }

                    c->buffer[count] = '\0';
                    c->buffer_len = count;

                    // Now switch to EPOLLOUT to send it back
                    struct epoll_event out_ev = {
                        .events = EPOLLOUT,
                        .data.ptr = c
                    };
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &out_ev);
                } else if (events[i].events & EPOLLOUT) {
                    char header[256];
                    int header_len = snprintf(
                        header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: %d\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n",
                        c->buffer_len
                    );

                    write(c->fd, header, header_len);
                    write(c->fd, c->buffer, c->buffer_len);

                    close(c->fd);
                    free(c);
                }
            }
        }
    }
}