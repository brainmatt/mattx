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
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <string.h>

extern int errno;
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
	    FILE *testfile = fopen("/mnt/shared/mattx-dfsa.log", "w");

        while (1) {
            printf("[Worker %d] Hello from the MattX Cluster! (Tick: %d)\n", getpid(), counter++);
            fflush(stdout);

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
            fprintf(testfile, "[Worker %d] Hello from the MattX DFSA Cluster! (Tick: %d).\n", getpid(), counter);
            fflush(testfile);

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

