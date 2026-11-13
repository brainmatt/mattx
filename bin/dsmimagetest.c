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
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>

#define CHUNK_SIZE 4096
#define SHM_KEY 0x4D415456 // Hex for "MATV"
#define MAX_CHUNKS 250000  // Supports up to ~1GB images
#define MAX_WORKERS 64

// The Control Header sits at the very beginning of the Shared Memory (Page 0)
struct dsm_ctrl {
    volatile int total_chunks;
    volatile int chunks_written;
    volatile int heartbeat[MAX_WORKERS]; // The Coherence Forcer!
    volatile int chunk_status[MAX_CHUNKS]; // 0 = unwritten, 1 = written
};

pid_t *child_pids = NULL;
int num_workers = 0;
int shmid = -1;
volatile char *shm_data = (volatile char *)-1;
char *local_bmp = NULL;
size_t file_size = 0;

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
    if (local_bmp) free(local_bmp);
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
    if (argc != 5) {
        printf("Usage: %s <num_workers> <max_ops_per_loop> <action_delay> <image.bmp>\n", argv[0]);
        exit(1);
    }

    num_workers = atoi(argv[1]);
    int max_ops = atoi(argv[2]);
    float action_delay = atof(argv[3]);    
    const char *bmp_path = argv[4];

    if (num_workers <= 0 || num_workers > MAX_WORKERS || max_ops <= 0 || action_delay < 0.0) {
        printf("Invalid arguments. (Max workers: %d)\n", MAX_WORKERS);
        exit(1);
    }

    // 1. READ THE BMP FILE INTO LOCAL MEMORY
    FILE *f = fopen(bmp_path, "rb");
    if (!f) {
        perror("[Manager] Failed to open BMP file");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    local_bmp = malloc(file_size);
    if (!local_bmp) {
        printf("[Manager] Failed to allocate local memory for BMP!\n");
        exit(1);
    }
    fread(local_bmp, 1, file_size, f);
    fclose(f);

    int total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (total_chunks > MAX_CHUNKS) {
        printf("[Manager] Image too large! Max chunks exceeded.\n");
        exit(1);
    }

    size_t shm_size = sizeof(struct dsm_ctrl) + file_size;
    child_pids = malloc(num_workers * sizeof(pid_t));

    printf("==================================================\n");
    printf("[Manager] MattX DSM Image Test Initializing...\n");
    printf("[Manager] Image: %s | Size: %zu bytes | Chunks: %d\n", bmp_path, file_size, total_chunks);
    printf("[Manager] Workers: %d | Max Ops/Loop: %d\n", num_workers, max_ops);
    
    // 2. MASTER CREATES THE SHARED MEMORY
    shmid = shmget(SHM_KEY, shm_size, IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget failed");
        exit(1);
    }
    
    shm_data = (volatile char *)shmat(shmid, NULL, 0);
    if (shm_data == (char *)-1) {
        perror("shmat failed");
        exit(1);
    }
    
    // Initialize the Control Header
    struct dsm_ctrl *ctrl = (struct dsm_ctrl *)shm_data;
    ctrl->total_chunks = total_chunks;
    ctrl->chunks_written = 0;
    memset((void *)ctrl->heartbeat, 0, sizeof(ctrl->heartbeat));
    memset((void *)ctrl->chunk_status, 0, sizeof(ctrl->chunk_status));

    // Initialize the Canvas with a glaring pattern (0x55 = striped gray)
    char *canvas = (char *)shm_data + sizeof(struct dsm_ctrl);
    memset(canvas, 0x55, file_size);

    printf("[Manager] SHM created and attached at %p (SHMID: %d)\n", shm_data, shmid);
    printf("==================================================\n");

    signal(SIGINT, handle_sigint);

    // 3. FORK THE WORKERS
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("Fork failed");
            handle_sigint(SIGINT);
        }

        if (pid == 0) {
            // --- CHILD PROCESS (The Worker) ---
            int my_worker_id = i;
            srand(time(NULL) ^ getpid()); 
            
            printf("[Worker %d | PID %d] Started. Waiting 10 seconds for migration...\n", my_worker_id, getpid());
            sleep(10);

            printf("[Worker %d | PID %d] Waking up! Starting to paint...\n", my_worker_id, getpid());
            
            bool image_done = false;

            // Loop until the entire image is painted!
            while (ctrl->chunks_written < ctrl->total_chunks && !image_done) {
                int current_node = get_local_node_id();
                int ops = (rand() % max_ops) + 1;
                
                // THE COHERENCE FORCER: Write to Page 0 to steal the EXCLUSIVE lock!
                // This guarantees we fetch the absolute latest 'chunks_written' value from VM1.
                __sync_fetch_and_add(&ctrl->heartbeat[my_worker_id], 1);

                for (int o = 1; o <= ops && ctrl->chunks_written < ctrl->total_chunks; o++) {
                    
                    int action = rand() % 100;

                    // --- 5% CHANCE: THE CHAOS OVERWRITE (Steal EXCLUSIVE) ---
                    if (action < 5 && ctrl->chunks_written > 0) {
                        int c = rand() % ctrl->total_chunks;
                        if (ctrl->chunk_status[c] == 1) {
                            int copy_size = CHUNK_SIZE;
                            if ((c + 1) * CHUNK_SIZE > file_size) copy_size = file_size - c * CHUNK_SIZE;
                            
                            memcpy(canvas + c * CHUNK_SIZE, local_bmp + c * CHUNK_SIZE, copy_size);
                            printf("  -> [PID %d | Node %d] Chaos Overwrite on chunk %d (Steal EXCLUSIVE)\n", getpid(), current_node, c);
                        }
                    } 
                    // --- 15% CHANCE: THE READ TORTURE (Downgrade to SHARED) ---
                    else if (action < 20 && ctrl->chunks_written > 0) {
                        int c = rand() % ctrl->total_chunks;
                        if (ctrl->chunk_status[c] == 1) {
                            volatile char dummy = canvas[c * CHUNK_SIZE];
                            (void)dummy; // Prevent compiler optimization
                            printf("  -> [PID %d | Node %d] Read Torture on chunk %d (Downgrade to SHARED)\n", getpid(), current_node, c);
                        }
                    }
                    // --- 80% CHANCE: THE NORMAL PAINT ---
                    else {
                        int c = rand() % ctrl->total_chunks;
                        int start_c = c;
                        
                        // Find an unwritten chunk
                        while (1) {
                            if (ctrl->chunk_status[c] == 0) {
                                // Lock-free atomic claim!
                                if (__sync_bool_compare_and_swap(&ctrl->chunk_status[c], 0, 1)) {
                                    int copy_size = CHUNK_SIZE;
                                    if ((c + 1) * CHUNK_SIZE > file_size) copy_size = file_size - c * CHUNK_SIZE;
                                    
                                    memcpy(canvas + c * CHUNK_SIZE, local_bmp + c * CHUNK_SIZE, copy_size);
                                    __sync_fetch_and_add(&ctrl->chunks_written, 1);
                                    
                                    printf("  -> [PID %d | Node %d] Painted chunk %d (%d/%d)\n", 
                                           getpid(), current_node, c, ctrl->chunks_written, ctrl->total_chunks);
                                    break;
                                }
                            }
                            c = (c + 1) % ctrl->total_chunks;
                            if (c == start_c) {
                                // We scanned the whole image and found no empty chunks!
                                image_done = true;
                                break; 
                            }
                        }
                    }
                    usleep((useconds_t)(action_delay * 1000000.0)); 
                }
            }

            printf("[Worker %d | PID %d] Image complete! Exiting cleanly (Triggering Funeral Flush).\n", my_worker_id, getpid());
            exit(0);
        } else {
            child_pids[i] = pid;
            printf("[Manager] Spawned worker %d/%d (PID %d).\n", i+1, num_workers, pid);
            usleep(500000); // Stagger spawns slightly
        }
    }

    // --- PARENT PROCESS (The Manager) ---
    printf("[Manager] All workers spawned. Waiting for the masterpiece to finish...\n");
    
    for (int i = 0; i < num_workers; i++) {
        waitpid(child_pids[i], NULL, 0);
    }

    printf("[Manager] All workers finished! Extracting the final image from DSM...\n");
    
    //char *canvas = (char *)shm_data + sizeof(struct dsm_ctrl);
    FILE *out = fopen("output_dsm_test.bmp", "wb");
    if (out) {
        fwrite(canvas, 1, file_size, out);
        fclose(out);
        printf("[Manager] SUCCESS! Image saved to 'output_dsm_test.bmp'.\n");
        printf("[Manager] Open it to verify MESI Cache Coherence!\n");
    } else {
        perror("[Manager] Failed to write output image");
    }

    printf("[Manager] Cleaning up Shared Memory...\n");
    shmdt((char *)shm_data);
    shmctl(shmid, IPC_RMID, NULL);
    free(local_bmp);
    free(child_pids);
    
    printf("[Manager] Clean exit. Goodbye!\n");
    return 0;
}
