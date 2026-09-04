#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <sys/types.h>

#define SHM_SIZE 4096
#define SHM_KEY 0x4D415454 // Hex for "MATT"

int main() {
    pid_t pid = fork();

    if (pid < 0) {
        perror("Fork failed");
        exit(1);
    }

    if (pid > 0) {
        // Parent process
        printf("==================================================\n");
        printf("dsmtest: Spawned child worker with PID %d.\n", pid);
        printf("dsmtest: Parent exiting to detach from terminal.\n");
        printf("dsmtest: Run 'tail -f /tmp/dsmtest.log' to watch the output!\n");
        printf("==================================================\n");
        exit(0);
    }

    // Child Process (The Worker)
    // Redirect stdout and stderr to a log file so Matt can watch it safely!
    freopen("/tmp/dsmtest.log", "w", stdout);
    freopen("/tmp/dsmtest.log", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for real-time logs

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
        snprintf(shm_data, SHM_SIZE, "MattX DSM Magic! Loop %d", i);
        
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
}
