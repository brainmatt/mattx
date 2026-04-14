#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

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
        printf("[Worker] I am alive! My PID is %d. Migrate ME!\n", getpid());
        int counter = 0;
        
        while (1) {
            printf("[Worker %d] Hello from the MattX Cluster! (Tick: %d)\n", getpid(), counter++);
            fflush(stdout);
            
            // --- THE CPU HOG ---
            // Instead of sleeping, we burn 100% of the CPU for 1 second
            time_t start = time(NULL);
            volatile double x = 1.0; // 'volatile' stops the compiler from optimizing this loop away
            
            while (time(NULL) - start < 1) {
                x = x * 1.000001 + 0.000001;
            }
        }
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

