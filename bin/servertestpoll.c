#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#define PORT 8080

pid_t child_pid = -1;

// The Parent catches Ctrl-C and kills the child
void handle_sigint(int sig) {
    printf("\n[Manager] Caught Ctrl-C! Sending kill signal to worker PID %d...\n", child_pid);
    if (child_pid > 0) {
        // This kills the Deputy on VM1, triggering the Assassination Order to VM2!
        kill(child_pid, SIGKILL); 
    }
    exit(0);
}

int main() {
    // Spawn the worker
    child_pid = fork();

    if (child_pid < 0) {
        perror("fork failed");
        return 1;
    }

    if (child_pid == 0) {
        // --- CHILD PROCESS (The Worker) ---
        int server_fd, client_fd;
        struct sockaddr_in server_addr, client_addr;
        socklen_t client_len = sizeof(client_addr);
        char *hello = "Hello from the MattX Surrogate!\n";

        printf("[Worker %d] Starting up...\n", getpid());

        if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        
        // Allow port reuse so we don't get "Address already in use" if we restart quickly
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(PORT);

        if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("Bind failed");
            exit(1);
        }

        if (listen(server_fd, 3) < 0) {
            perror("Listen failed");
            exit(1);
        }
        
        printf("[Worker %d] Listening on port %d... MIGRATE ME NOW!\n", getpid(), PORT);

        // --- NEW: The Distributed Sleep Test (poll) ---
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN; // We want to wake up when data (a connection) is ready to read

        while (1) {
            printf("[Worker %d] Calling poll(). Sleeping until a connection arrives...\n", getpid());
            
            // This will block indefinitely (-1) until someone connects!
            int poll_count = poll(fds, 1, -1);
            
            if (poll_count < 0) {
                // Ignore EINTR and just try again! ---
                if (errno == EINTR) {
                    printf("[Worker %d] Poll interrupted by migration (EINTR). Retrying...\n", getpid());
                    continue; 
                }
                perror("Poll failed");
                break;
            }

            if (fds[0].revents & POLLIN) {
                printf("[Worker %d] Poll triggered! Accepting connection...\n", getpid());
                
                client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    perror("Accept failed");
                    continue;
                }

                printf("[Worker %d] SUCCESS! Connection accepted from %s:%d!\n", 
                       getpid(), inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

                write(client_fd, hello, strlen(hello));
                printf("[Worker %d] Message sent. Closing client connection.\n", getpid());
                close(client_fd);
            }
        }
        close(server_fd);
        exit(0);
    } else {
        // --- PARENT PROCESS (The Manager) ---
        signal(SIGINT, handle_sigint);
        
        printf("[Manager] I am PID %d. I spawned worker PID %d.\n", getpid(), child_pid);
        printf("[Manager] Waiting for worker. Press Ctrl-C to terminate the cluster job.\n");
        
        waitpid(child_pid, NULL, 0);
        printf("[Manager] Worker finished. Exiting.\n");
    }

    return 0;
}
