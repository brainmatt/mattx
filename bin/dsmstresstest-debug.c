#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>

#define SHM_SIZE 4096
#define SHM_KEY 0x4D415456 // Hex for "MATV" (Different key to avoid collisions)

pid_t *child_pids = NULL;
int num_workers = 0;
int shmid = -1;
volatile char *shm_data = (volatile char *)-1;

// The Parent catches Ctrl-C and kills all children cleanly
void handle_sigint(int sig) {
    printf("\n[Manager] Caught Ctrl-C! Sending kill signals to workers...\n");
    for (int i = 0; i < num_workers; i++) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGKILL); 
    }
    if (shm_data != (char *)-1) shmdt((char *)shm_data);
    if (shmid >= 0) shmctl(shmid, IPC_RMID, NULL);
    printf("[Manager] Shared memory destroyed. Exiting.\n");
    exit(0);
}

// Helper to dynamically read the current MattX Node ID
int get_local_node_id() {
    FILE *f = fopen("/proc/mattx/nodes", "r");
    if (!f) return -1;
    char line[256];
    int node_id = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "(Local)")) {
            sscanf(line, "%d", &node_id);
            break;
        }
    }
    fclose(f);
    return node_id;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <num_workers>\n", argv[0]);
        exit(1);
    }

    num_workers = atoi(argv[1]);
    if (num_workers <= 0) exit(1);

    child_pids = malloc(num_workers * sizeof(pid_t));

    printf("==================================================\n");
    printf("[Manager] MattX DSM Interactive Debugger Initializing...\n");
    
    // 1. MASTER CREATES THE SHARED MEMORY
    shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid < 0) { perror("shmget failed"); exit(1); }
    
    shm_data = (volatile char *)shmat(shmid, NULL, 0);
    if (shm_data == (char *)-1) { perror("shmat failed"); exit(1); }
    
    snprintf((char *)shm_data, SHM_SIZE, "INIT MattX DSM Debugger");
    printf("[Manager] SHM created and attached at %p (SHMID: %d)\n", shm_data, shmid);
    printf("==================================================\n");

    signal(SIGINT, handle_sigint);

    // 2. FORK THE WORKERS
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("Fork failed"); handle_sigint(SIGINT); }

        if (pid == 0) {
            // --- CHILD PROCESS (The Worker) ---
            pid_t my_pid = getpid();
            char cmd_file[256];
            snprintf(cmd_file, sizeof(cmd_file), "/tmp/%d.cmd", my_pid);
            
            printf("[Worker PID %d] Started. Listening for commands on: %s\n", my_pid, cmd_file);
            
            // Create an empty file initially
            FILE *f = fopen(cmd_file, "w");
            if (f) fclose(f);

            while (1) {
                // Poll the file every 500ms
                f = fopen(cmd_file, "r");
                if (f) {
                    char cmd[256] = {0};
                    if (fgets(cmd, sizeof(cmd), f)) {
                        // Strip the newline character
                        cmd[strcspn(cmd, "\n")] = 0;
                        
                        if (strlen(cmd) > 0) {
                            int current_node = get_local_node_id();
                            
                            if (strncmp(cmd, "read", 4) == 0) {
                                printf("[Worker PID %d | Node %d] Executed READ: '%s'\n", my_pid, current_node, (char *)shm_data);
                            } else if (strncmp(cmd, "write ", 6) == 0) {
                                // Write everything after "write " into the shared memory
                                snprintf((char *)shm_data, SHM_SIZE, "%s", cmd + 6);
                                printf("[Worker PID %d | Node %d] Executed WRITE: '%s'\n", my_pid, current_node, cmd + 6);
                            } else if (strncmp(cmd, "exit", 4) == 0) {
                                printf("[Worker PID %d | Node %d] Received EXIT command. Goodbye!\n", my_pid, current_node);
                                fclose(f);
                                // Clear the file one last time
                                f = fopen(cmd_file, "w");
                                if (f) fclose(f);
                                break;
                            } else {
                                printf("[Worker PID %d | Node %d] Unknown command: '%s'\n", my_pid, current_node, cmd);
                            }
                            
                            // Clear the file so we don't execute the command twice!
                            freopen(cmd_file, "w", f);
                        }
                    }
                    fclose(f);
                }
                usleep(500000); // Sleep 500ms to save CPU
            }
            exit(0);
        } else {
            child_pids[i] = pid;
            printf("[Manager] Spawned worker %d/%d (PID %d). Command file: /tmp/%d.cmd\n", i+1, num_workers, pid, pid);
            sleep(1); // Stagger the spawns slightly
        }
    }

    // --- PARENT PROCESS (The Manager) ---
    printf("[Manager] All workers spawned. Waiting for them to finish. Press Ctrl-C to abort.\n");
    
    for (int i = 0; i < num_workers; i++) {
        waitpid(child_pids[i], NULL, 0);
    }

    printf("[Manager] All workers finished. Cleaning up Shared Memory...\n");
    shmdt((char *)shm_data);
    shmctl(shmid, IPC_RMID, NULL);
    printf("[Manager] Clean exit. Goodbye!\n");

    free(child_pids);
    return 0;
}
