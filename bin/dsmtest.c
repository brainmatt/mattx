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

#define SHM_SIZE 4096
#define SHM_KEY 0x4D415454 // Hex for "MATT"

pid_t child_pid = -1;
extern int errno;

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
    pid_t child_pid = fork();

    if (child_pid < 0) {
        perror("Fork failed");
        exit(1);
    }

    if (child_pid == 0) {
        // --- CHILD PROCESS (The Worker) ---

        // Redirect stdout and stderr to a log file so Matt can watch it safely!
        // freopen("/tmp/dsmtest.log", "w", stdout);
        // freopen("/tmp/dsmtest.log", "w", stderr);
        // setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for real-time logs

        printf("[PID %d] dsmtest worker started.\n", getpid());
        printf("[PID %d] Waiting 10 seconds for Funny Matt to migrate me...\n", getpid());
        
        // The 10-second migration window!
        for (int i = 10; i > 0; i--) {
            printf("[PID %d] T-minus %d seconds...\n", getpid(), i);
            sleep(1);
        }

        printf("[PID %d] Waking up! Creating Shared Memory Segment...\n", getpid());
        
        // 1. SHMGET (Microstep 2)
        int shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
        if (shmid < 0) {
            perror("shmget failed");
            exit(1);
        }
        printf("[PID %d] shmget successful! SHMID: %d\n", getpid(), shmid);

        // 2. SHMAT (Microstep 3)
        char *shm_data = shmat(shmid, NULL, 0);
        if (shm_data == (char *)-1) {
            perror("shmat failed");
            exit(1);
        }
        printf("[PID %d] shmat successful! Attached at virtual address: %p\n", getpid(), shm_data);

        printf("[PID %d] Starting 100 Read/Write loops...\n", getpid());
        
        // 3. PAGE FAULTS (Microsteps 4 & 5)
        for (int i = 0; i < 100; i++) {
            // Write to DSM (Triggers Page Fault on first touch!)
            snprintf(shm_data, SHM_SIZE, "%d MattX DSM Magic! Loop %d", i, i);
            
            // Read from DSM
            printf("[PID %d] Loop %d - Read from SHM: '%s'\n", getpid(), i, shm_data);
            
            sleep(1);
        }

        printf("[PID %d] Loops complete. Detaching memory...\n", getpid());
        
        // 4. SHMDT (Microstep 2)
        if (shmdt(shm_data) == -1) {
            perror("shmdt failed");
        }

        printf("[PID %d] Removing Shared Memory Segment...\n", getpid());
        
        // 5. SHMCTL (Microstep 2)
        if (shmctl(shmid, IPC_RMID, NULL) == -1) {
            perror("shmctl IPC_RMID failed");
        }

        printf("[PID %d] dsmtest finished cleanly. Goodbye!\n", getpid());
        return 0;

    } else {

        // --- PARENT PROCESS (The Manager) ---
        // Register the Ctrl-C handler
        signal(SIGINT, handle_sigint);

        // Parent process
        printf("==================================================\n");
        printf("dsmtest: Spawned child worker with PID %d.\n", child_pid);
        printf("dsmtest: Parent exiting to detach from terminal.\n");
        // printf("dsmtest: Run 'tail -f /tmp/dsmtest.log' to watch the output!\n");
        printf("==================================================\n");
        printf("[Manager] Waiting for worker. Press Ctrl-C to terminate the cluster job.\n");
        
        // Go to sleep and wait for the child to finish or die
        waitpid(child_pid, NULL, 0);
        printf("[Manager] Worker finished. Exiting.\n");
    }

    return 0;
}
    



