#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define SHM_SIZE 4096
#define SHM_KEY 0x4D415455 // Hex for "MATU" (Different key to avoid collisions)

pid_t *child_pids = NULL;
int num_workers = 0;
int shmid = -1;
volatile char *shm_data = (volatile char *)-1;

// The Parent catches Ctrl-C and kills all children cleanly
void handle_sigint(int sig) {
    printf("\n[Manager] Caught Ctrl-C! Sending kill signals to workers...\n");
    for (int i = 0; i < num_workers; i++) {
        if (child_pids[i] > 0) {
            kill(child_pids[i], SIGKILL); 
        }
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
    if (argc != 3) {
        printf("Usage: %s <num_workers> <max_rw_ops_per_loop>\n", argv[0]);
        exit(1);
    }

    num_workers = atoi(argv[1]);
    int max_ops = atoi(argv[2]);

    if (num_workers <= 0 || max_ops <= 0) {
        printf("Arguments must be > 0\n");
        exit(1);
    }

    child_pids = malloc(num_workers * sizeof(pid_t));

    printf("==================================================\n");
    printf("[Manager] MattX DSM Stress Test Initializing...\n");
    printf("[Manager] Workers: %d | Max Ops/Loop: %d\n", num_workers, max_ops);
    
    // 1. MASTER CREATES THE SHARED MEMORY
    shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget failed");
        exit(1);
    }
    
    shm_data = (volatile char *)shmat(shmid, NULL, 0);
    if (shm_data == (char *)-1) {
        perror("shmat failed");
        exit(1);
    }
    
    // Initialize the memory
    snprintf((char *)shm_data, SHM_SIZE, "INIT MattX DSM Stress Test");
    printf("[Manager] SHM created and attached at %p (SHMID: %d)\n", shm_data, shmid);
    printf("==================================================\n");

    // Register the Ctrl-C handler
    signal(SIGINT, handle_sigint);

    // 2. FORK THE WORKERS
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("Fork failed");
            handle_sigint(SIGINT);
        }

        if (pid == 0) {
            // --- CHILD PROCESS (The Worker) ---
            srand(time(NULL) ^ getpid()); // Unique random seed per worker
            
            printf("[Worker PID %d] Started. Waiting 10 seconds for migration...\n", getpid());
            for (int t = 10; t > 0; t--) {
                printf("[Worker PID %d] T-minus %d seconds...\n", getpid(), t);
                sleep(1);
            }

            printf("[Worker PID %d] Waking up! Starting 100 loops...\n", getpid());
            
            for (int tick = 0; tick < 100; tick++) {
                int is_write = rand() % 2; // 0 = Read, 1 = Write
                int ops = (rand() % max_ops) + 1;
                int current_node = get_local_node_id();
                
                printf("[Worker PID %d | Node %d] Tick %d: Doing %d %s operations...\n", 
                       getpid(), current_node, tick, ops, is_write ? "WRITE" : "READ");
                
                for (int o = 1; o <= ops; o++) {
                    if (is_write) {
                        // The tick number is at the very front so it shows up in /proc/mattx/dsm!
                        snprintf((char *)shm_data, SHM_SIZE, "%03d MattX Stress! W:%d N:%d Op:%d", 
                                 tick, getpid(), current_node, o);
                        printf("  -> [PID %d] Wrote data.\n", getpid());
                    } else {
                        // Read from DSM
                        printf("  -> [PID %d] Read data: '%s'\n", getpid(), (char *)shm_data);
                    }
                    usleep(500000); // Sleep 0.5s between ops to allow observation
                }
            }

            printf("[Worker PID %d] Finished 100 loops. Exiting cleanly.\n", getpid());
            exit(0);
        } else {
            // Parent records the PID and waits 2 seconds before spawning the next
            child_pids[i] = pid;
            printf("[Manager] Spawned worker %d/%d (PID %d). Staggering next spawn...\n", i+1, num_workers, pid);
            sleep(2);
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
