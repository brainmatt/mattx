#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

pid_t child_pid = -1;

// The Parent catches Ctrl-C and kills the child
void handle_sigint(int sig) {
    printf("\n[Manager] Caught Ctrl-C! Sending kill signal to worker PID %d...\n", child_pid);
    if (child_pid > 0) {
        // This kills the Deputy on VM1, which triggers the Assassination Order to VM2!
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
        printf("[Worker] I am alive! My PID is %d Migrate ME!\n", getpid());
        int counter = 0;
	    FILE *testfile = fopen("/tmp/mattx-fd.log", "w");

        while (1) {
            printf("[Worker %d] Hello from the MattX Cluster! (Tick: %d)\n", getpid(), counter++);
            fflush(stdout);

            FILE *hosts_fp = fopen("/etc/hosts", "r");
            // FILE *hosts_fp = fopen("/tmp/super.txt", "r");

            
            if (hosts_fp != NULL) {
                int hosts_fd = fileno(hosts_fp);
                struct stat statbuf;

                if (fstat(hosts_fd, &statbuf) == 0) {
                    printf("[Worker %d] fstat success! /etc/hosts size is %ld bytes.\n", getpid(), statbuf.st_size);
                    fflush(stdout);
                } else {
                    perror("fstat failed");
                    printf("[Worker %d] WARNING: fstat failed on /etc/hosts! Errno: %d\n", getpid(), errno);
                    fflush(stdout);
                }

                // TEST DUP2
                int cloned_fd = dup(hosts_fd);
                if (cloned_fd >= 0) {
                    printf("[Worker %d] DUP success! Cloned FD %d -> New FD %d\n", getpid(), hosts_fd, cloned_fd);
                    fflush(stdout);
                    
                    // Try to read from the clone to prove it works
                    char clone_buf[256] = {0};
                    if (read(cloned_fd, clone_buf, 10) > 0) {
                        printf("[Worker %d] Read from cloned FD: %10s\n", getpid(), clone_buf);
                        fflush(stdout);
                    }
                    close(cloned_fd);
                } else {
                    printf("[Worker %d] WARNING: DUP failed! Errno: %d\n", getpid(), errno);
                    fflush(stdout);
                }

                char hosts_buf[256] = {0};
                if (fscanf(hosts_fp, "%255s", hosts_buf) == 1) {
                    printf("[Worker %d] /etc/hosts read 1: %s\n", getpid(), hosts_buf);
                    fflush(stdout);
                }
                
                // Jump 10 bytes forward using fseek
                if (fseek(hosts_fp, 10, SEEK_SET) == 0) {
                    char hosts_buf2[256] = {0};
                    if (fscanf(hosts_fp, "%255s", hosts_buf2) == 1) {
                        printf("[Worker %d] /etc/hosts read 2 (after seek): %s\n", getpid(), hosts_buf2);
                        fflush(stdout);
                    }
                } else {
                    printf("[Worker %d] WARNING: fseek failed! Errno: %d\n", getpid(), errno);
                    fflush(stdout);
                }
                
                fclose(hosts_fp);
            }

	        // do some dumb calculations 
            sleep(0.5);
            double x = 1.0;
            while (x < 10000000) {
                x = x + 0.1;
            }
    	    fflush(stdout);

	        // here we write to a file sequentially
            if (testfile == NULL) {
                 perror("ERROR: opening file");
                 return 1;
            }
            fprintf(testfile, "[Worker %d] Hello from the MattX Cluster! (Tick: %d).\n", getpid(), counter);
            fflush(testfile);

            // TEST FSYNC - Open a new writeable file to test Wormhole FSYNC
            FILE *sync_fp = fopen("/tmp/mattx-sync.log", "a");
            if (sync_fp != NULL) {
                fprintf(sync_fp, "[Worker %d] Sync Tick: %d\n", getpid(), counter);
                fflush(sync_fp); // Flushes glibc buffer to kernel
                
                int sync_fd = fileno(sync_fp);
                if (fsync(sync_fd) == 0) {
                    printf("[Worker %d] WORMHOLE FSYNC success! Data committed to VM1 disk.\n", getpid());
                } else {
                    printf("[Worker %d] WARNING: WORMHOLE FSYNC failed! Errno: %d (%s)\n", getpid(), errno, strerror(errno));
                }
                fflush(stdout);
                fclose(sync_fp);
            }

            // TEST NETWORK SOCKET CONNECT
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in serv_addr;
                serv_addr.sin_family = AF_INET;
                serv_addr.sin_port = htons(80);
                inet_pton(AF_INET, "1.1.1.1", &serv_addr.sin_addr); // Try connecting to Cloudflare DNS

                if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0) {
                    printf("[Worker %d] WORMHOLE CONNECT success! TCP Socket established on VM1.\n", getpid());
                } else {
                    printf("[Worker %d] WARNING: WORMHOLE CONNECT failed! Errno: %d (%s)\n", getpid(), errno, strerror(errno));
                }
                close(sock);
            } else {
                printf("[Worker %d] WARNING: WORMHOLE SOCKET creation failed! Errno: %d (%s)\n", getpid(), errno, strerror(errno));
            }

            // TEST NETWORK SOCKET BIND & LISTEN
            int srv_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (srv_sock >= 0) {
                struct sockaddr_in srv_addr;
                srv_addr.sin_family = AF_INET;
                // Rotate ports slightly to avoid TIME_WAIT conflicts if we run this loop fast
                srv_addr.sin_port = htons(8080 + (counter % 1000)); 
                srv_addr.sin_addr.s_addr = INADDR_ANY;

                int bind_res = bind(srv_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
                if (bind_res < 0) {
                    printf("[Worker %d] WARNING: WORMHOLE BIND failed! Errno: %d (%s)\n", getpid(), errno, strerror(errno));
                } else {
                    printf("[Worker %d] WORMHOLE BIND success! Bound to port %d on VM1. Bind returned %d\n", getpid(), ntohs(srv_addr.sin_port), bind_res);
                    
                    if (listen(srv_sock, 5) == 0) {
                        printf("[Worker %d] WORMHOLE LISTEN success! Listening on VM1.\n", getpid());
                    } else {
                        printf("[Worker %d] WARNING: WORMHOLE LISTEN failed! Errno: %d (%s)\n", getpid(), errno, strerror(errno));
                    }
                }
                close(srv_sock);
            } else {
                printf("[Worker %d] WARNING: WORMHOLE SERVER SOCKET creation failed! Errno: %d (%s)\n", getpid(), errno, strerror(errno));
            }

            fflush(stdout);

        }
	    fclose(testfile);
    } else {
        // --- PARENT PROCESS (The Manager) ---
        // Register the Ctrl-C handler
        signal(SIGINT, handle_sigint);
        
        printf("[Manager] I am PID %d. I spawned worker PID %d.\n", getpid(), child_pid);
        printf("[Manager] Waiting for worker. Press Ctrl-C to terminate the cluster job.\n");
        
        // Go to sleep and wait for the child to finish or die
        waitpid(child_pid, NULL, 0);
        printf("[Manager] Worker finished. Exiting.\n");
    }

    return 0;
}

