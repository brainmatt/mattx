#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pvm3.h>

#define LOG_FILE "/tmp/pvmtest.log"
#define MAX_MSG_LEN 256

/**
 * Log to both stdout and log file
 */
void log_message(const char *format, ...) {
    FILE *logfile;
    va_list args;
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    va_start(args, format);
    
    /* Print to stdout */
    printf("[%s] ", timestamp);
    vprintf(format, args);
    printf("\n");
    
    /* Print to log file */
    logfile = fopen(LOG_FILE, "a");
    if (logfile != NULL) {
        fprintf(logfile, "[%s] ", timestamp);
        vfprintf(logfile, format, args);
        fprintf(logfile, "\n");
        fclose(logfile);
    }
    
    va_end(args);
}

/**
 * Master Program
 * Spawns a client process and communicates with it via PVM
 */
int main(int argc, char *argv[]) {
    int mytid;
    int slavetid;
    int result;
    int value = 1;
    
    /* Initialize PVM */
    mytid = pvm_mytid();
    log_message("MASTER: PVM Master process started, mytid=%d", mytid);
    
    /* Spawn the slave process */
    log_message("MASTER: Spawning pvmtest-client...");
    result = pvm_spawn("pvmtest-client", (char **)NULL, PvmTaskDefault, (char *)NULL, 1, &slavetid);
    
    if (result < 1) {
        log_message("MASTER: ERROR - Failed to spawn slave process");
        pvm_exit();
        exit(1);
    }
    
    log_message("MASTER: Successfully spawned slave, slavetid=%d", slavetid);
    
    /* Initialize send buffer and pack the initial integer value */
    log_message("MASTER: Initializing send buffer and packing integer value %d", value);
    pvm_initsend(PvmDataDefault);
    pvm_pkint(&value, 1, 1);
    
    /* Send the integer to the slave */
    log_message("MASTER: Sending initial value %d to slave (tid=%d)", value, slavetid);
    result = pvm_send(slavetid, 1);
    
    if (result < 0) {
        log_message("MASTER: ERROR - Failed to send data to slave");
        pvm_exit();
        exit(1);
    }
    
    log_message("MASTER: Successfully sent initial value, waiting for result...");
    
    /* Wait for the result from the slave */
    log_message("MASTER: Entering receive mode, waiting for slave to send result...");
    result = pvm_recv(slavetid, -1);
    
    if (result < 0) {
        log_message("MASTER: ERROR - Failed to receive data from slave");
        pvm_exit();
        exit(1);
    }
    
    log_message("MASTER: Received message from slave, unpacking result...");
    
    /* Unpack the received integer */
    int final_result = 0;
    pvm_upkint(&final_result, 1, 1);
    
    log_message("MASTER: Successfully received final result value: %d", final_result);
    
    if (final_result == 1000) {
        log_message("MASTER: Received expected final value 1000, shutting down...");
    } else {
        log_message("MASTER: WARNING - Received unexpected value %d (expected 1000)", final_result);
    }
    
    /* Exit PVM */
    log_message("MASTER: Calling pvm_exit()...");
    pvm_exit();
    
    log_message("MASTER: Master process finished successfully");
    
    return 0;
}
