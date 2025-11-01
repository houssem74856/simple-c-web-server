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
    struct epoll_event events[MAX_EVENTS];

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = server_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    printf("Server running on port %d...\n", PORT);

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                int client_fd = accept(server_fd, NULL, NULL);
                if (client_fd == -1) continue;

                make_socket_non_blocking(client_fd);
                
                printf("hello client\n");

                struct epoll_event ev = { .events = EPOLLIN, .data.fd = client_fd };
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
            } else {
                if (events[i].events & EPOLLIN) {
                    int client_fd = events[i].data.fd;

                    printf("ready to read from client\n");

                    struct epoll_event ev = { .events = EPOLLOUT, .data.fd = client_fd };
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                } else if (events[i].events & EPOLLOUT) {
                    int client_fd = events[i].data.fd;

                    printf("ready to write to client\n");

                    close(client_fd);
                }
            }
        }
    }
}