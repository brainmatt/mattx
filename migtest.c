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
        printf("[Worker] I am alive! My PID is %d Migrate ME!\n", getpid());
        int counter = 0;
	    FILE *testfile = fopen("/tmp/mattx-fd.log", "w");

        while (1) {
            printf("[Worker %d] Hello from the MattX Cluster! (Tick: %d)\n", getpid(), counter++);
            fflush(stdout);

            FILE *hosts_fp = fopen("/etc/hosts", "r");
//            FILE *hosts_fp = fopen("/tmp/super.txt", "r");

            
            if (hosts_fp != NULL) {
                char hosts_buf[256] = {0};
                if (fscanf(hosts_fp, "%255s", hosts_buf) == 1) {
                    printf("[Worker %d] /etc/hosts read: %s\n", getpid(), hosts_buf);
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

