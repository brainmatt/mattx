#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/select.h> // NEW: For select()
#include <errno.h>      // FIXED: For EINTR

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
        char *hello = "Hello from the MattX Surrogate (Select Edition)!\n";

        printf("[Worker %d] Starting up...\n", getpid());

        if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        
        // Allow port reuse
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

        // --- NEW: The Distributed Sleep Test (select) ---
        fd_set readfds;

        while (1) {
            // Clear the bitmap and add our server socket
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);

            printf("[Worker %d] Calling select(). Sleeping until a connection arrives...\n", getpid());
            
            // Block indefinitely (NULL timeout) until server_fd is readable
            // The first argument to select must be the highest FD number + 1
            int select_count = select(server_fd + 1, &readfds, NULL, NULL, NULL);
            
            if (select_count < 0) {
                if (errno == EINTR) {
                    printf("[Worker %d] Select interrupted by migration (EINTR). Retrying...\n", getpid());
                    continue; 
                }
                perror("Select failed");
                break;
            }

            // Check if our server socket is the one that triggered the wake-up
            if (FD_ISSET(server_fd, &readfds)) {
                printf("[Worker %d] Select triggered! Accepting connection...\n", getpid());
                
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
