/*
 * epolltest.c - The MattX Epoll Wormhole Validator
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define PORT 8888
#define MAX_EVENTS 5

int main() {
    printf("[EPOLLTEST] My PID is %d. Doing useless math for 10 seconds. MIGRATE ME NOW!\n", getpid());
    
    // A volatile counter prevents the compiler from optimizing the loop away!
    volatile unsigned long long counter = 0;
    while (counter < 3000000000ULL) {
        counter++; // Just burn CPU cycles for a few seconds!
    }


    // printf("[EPOLLTEST] My PID is %d. Busy waiting for 5 seconds. MIGRATE ME NOW!\n", getpid());
    // sleep(5);
    
    // time_t start = time(NULL);
    // while (time(NULL) - start < 10) {
    //     printf(".");
    //     fflush(stdout);
    //     // Busy wait! SIGSTOP will freeze us here, and we will wake up on VM2!
    // }

    // printf("[EPOLLTEST] My PID is %d. 1. Sleep for 10 seconds. MIGRATE ME NOW!\n", getpid());
    // sleep(10);
    // printf("[EPOLLTEST] My PID is %d. 2. Sleep for 10 seconds. DELAY STARTUP MIGRATED!\n", getpid());
    // sleep(10);

    int sock_fd, epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    printf("[EPOLLTEST] Starting up...\n");

    // 1. Create UDP socket
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        exit(1);
    }

    // 2. Bind to port 8888
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(1);
    }
    printf("[EPOLLTEST] Bound to UDP port %d. My PID is %d\n", PORT, getpid());

    // 3. Create epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        exit(1);
    }
    printf("[EPOLLTEST] Created epoll instance (FD: %d)\n", epoll_fd);

    // 4. Add socket to epoll
    ev.events = EPOLLIN;
    // We use a highly recognizable payload to test the Translation Engine!
    ev.data.u64 = 0xDEADBEEFCAFEBABE; 
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev) < 0) {
        perror("epoll_ctl");
        exit(1);
    }
    printf("[EPOLLTEST] Added socket (FD: %d) to epoll. Waiting for data...\n", sock_fd);
    printf("[EPOLLTEST] -> Test me: echo 'hello' | nc -u -w1 127.0.0.1 %d\n", PORT);

    // 5. The Wait Loop
    while (1) {
        // Wait for 5 seconds, then print a timeout message to show we are alive
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 5000); 
        
        if (nfds < 0) {
            perror("epoll_wait");
            sleep(1);
            continue;
        }

        if (nfds == 0) {
            printf("[EPOLLTEST] Timeout... still waiting.\n");
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            printf("[EPOLLTEST] Event triggered! User Data: 0x%llx\n", (unsigned long long)events[i].data.u64);
            
            if (events[i].events & EPOLLIN) {
                char buffer[1024];
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                
                int bytes = recvfrom(sock_fd, buffer, sizeof(buffer)-1, 0, 
                                     (struct sockaddr *)&client_addr, &client_len);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    // Strip newline for cleaner printing
                    if (buffer[bytes-1] == '\n') buffer[bytes-1] = '\0';
                    printf("[EPOLLTEST] Received %d bytes: '%s'\n", bytes, buffer);
                }
            }
            fflush(stdout);
        }
    }

    close(sock_fd);
    close(epoll_fd);
    return 0;
}
