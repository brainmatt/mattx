/*
 * MattX - The Modern Single System Image (SSI) Cluster
 * 
 * Copyright (c) 2026 by Matthias Rechenburg
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Commercial licensing options are available upon request.
 */
 

/*
 * migtest2.c - The MattX Scatter/Gather & SockOpt Validator (Dual-Mode)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#define PORT 8264

void run_server() {
    int server_fd, client_fd;
    struct sockaddr_in addr;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 3);

    printf("[SERVER] Listening on port %d...\n", PORT);
    client_fd = accept(server_fd, NULL, NULL);
    printf("[SERVER] Client connected!\n");

    while (1) {
        char buf[1024] = {0};
        struct iovec iov[1];
        struct msghdr msg = {0};

        iov[0].iov_base = buf;
        iov[0].iov_len = sizeof(buf);
        msg.msg_iov = iov;
        msg.msg_iovlen = 1;

        int bytes = recvmsg(client_fd, &msg, 0);
        if (bytes <= 0) break;

        // Echo it back!
        iov[0].iov_len = bytes;
        sendmsg(client_fd, &msg, 0);
    }
    close(client_fd);
    close(server_fd);
    exit(0);
}

void run_client(int is_nonblock) {
    int sock_fd;
    struct sockaddr_in server_addr;

    printf("[CLIENT] My PID is %d. Mode: %s. Sleeping for 10 seconds. MIGRATE ME NOW!\n", 
           getpid(), is_nonblock ? "NON-BLOCKING" : "BLOCKING");
    sleep(10);

    
    // --- THE DUAL-MODE TOGGLE ---
    if (is_nonblock) {
        sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    } else {
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);

        // we do not have fnctl yet
        // int flags = fcntl(sock_fd, F_GETFL, 0);
        // fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    printf("[CLIENT] Initiating connect()...\n");
    int res = connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    
    if (res < 0) {
        if (is_nonblock && errno == EINPROGRESS) {
            printf("[CLIENT] Connect returned EINPROGRESS (-115). Polling to finish handshake...\n");
            
            struct pollfd pfd;
            pfd.fd = sock_fd;
            pfd.events = POLLOUT;
            
            int pret = poll(&pfd, 1, 5000); // Wait up to 5 seconds for the handshake
            if (pret > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error != 0) {
                    printf("[CLIENT] Connect failed after poll: %s\n", strerror(so_error));
                    exit(1);
                }
            } else {
                printf("[CLIENT] Poll timed out or failed!\n");
                exit(1);
            }
        } else {
            perror("[CLIENT] Connect failed");
            exit(1);
        }
    }
    
    printf("[CLIENT] Connected to server!\n");

    int loop = 0;
    while (1) {
        loop++;
        printf("\n--- Loop %d ---\n", loop);

        // 1. SETSOCKOPT & GETSOCKOPT
        int flag = 1;
        if (setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0) {
            printf("[CLIENT] setsockopt(TCP_NODELAY, 1) SUCCESS\n");
        } else {
            perror("[CLIENT] setsockopt failed");
        }

        int check_flag = 0;
        socklen_t optlen = sizeof(check_flag);
        if (getsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &check_flag, &optlen) == 0) {
            printf("[CLIENT] getsockopt(TCP_NODELAY) returned: %d\n", check_flag);
        } else {
            perror("[CLIENT] getsockopt failed");
        }

        // 2. GETSOCKNAME & GETPEERNAME
        struct sockaddr_in local_addr, remote_addr;
        socklen_t addr_len = sizeof(local_addr);
        
        if (getsockname(sock_fd, (struct sockaddr *)&local_addr, &addr_len) == 0) {
            printf("[CLIENT] getsockname() -> Local Port: %d\n", ntohs(local_addr.sin_port));
        } else {
            perror("[CLIENT] getsockname failed");
        }

        addr_len = sizeof(remote_addr);
        if (getpeername(sock_fd, (struct sockaddr *)&remote_addr, &addr_len) == 0) {
            printf("[CLIENT] getpeername() -> Remote Port: %d\n", ntohs(remote_addr.sin_port));
        } else {
            perror("[CLIENT] getpeername failed");
        }

        // 3. SENDMSG (Scatter/Gather Send)
        char *part1 = "MattX ";
        char *part2 = "Scatter/Gather Rules!";
        struct iovec send_iov[2];
        send_iov[0].iov_base = part1;
        send_iov[0].iov_len = strlen(part1);
        send_iov[1].iov_base = part2;
        send_iov[1].iov_len = strlen(part2);

        struct msghdr send_msg = {0};
        send_msg.msg_iov = send_iov;
        send_msg.msg_iovlen = 2;

        int sent = sendmsg(sock_fd, &send_msg, 0);
        printf("[CLIENT] sendmsg() sent %d bytes across 2 iovecs.\n", sent);

        // 4. RECVMSG (Scatter/Gather Recv)
        char recv_part1[6] = {0};
        char recv_part2[32] = {0};
        struct iovec recv_iov[2];
        recv_iov[0].iov_base = recv_part1;
        recv_iov[0].iov_len = 6;
        recv_iov[1].iov_base = recv_part2;
        recv_iov[1].iov_len = sizeof(recv_part2) - 1;

        struct msghdr recv_msg = {0};
        recv_msg.msg_iov = recv_iov;
        recv_msg.msg_iovlen = 2;

        int recvd = recvmsg(sock_fd, &recv_msg, 0);
        printf("[CLIENT] recvmsg() received %d bytes.\n", recvd);
        printf("[CLIENT] -> iovec[0]: '%s'\n", recv_part1);
        printf("[CLIENT] -> iovec[1]: '%s'\n", recv_part2);

        sleep(3);
    }
}

int main(int argc, char **argv) {
    int is_nonblock = 0;
    
    // Parse command line arguments
    if (argc > 1 && strcmp(argv[1], "nonblock") == 0) {
        is_nonblock = 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        run_server();
    } else if (pid > 0) {
        sleep(1); // Give server a moment to bind
        run_client(is_nonblock);
        wait(NULL);
    } else {
        perror("fork");
    }
    return 0;
}
