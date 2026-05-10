#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
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
 * Slave/Worker/Client Program
 * Receives initial value from master, counts up to 1000, and sends result back
 */
int main(int argc, char *argv[]) {
    int mytid;
    int mastertid;
    int result;
    int received_value = 0;
    int counter;
    
    /* Initialize PVM */
    mytid = pvm_mytid();
    mastertid = pvm_parent();
    
    log_message("CLIENT: PVM Client process started, mytid=%d, parent (master)=%d", mytid, mastertid);
    
    /* Receive the initial message from master */
    log_message("CLIENT: Entering receive mode, waiting for initial message from master...");
    result = pvm_recv(mastertid, -1);
    
    if (result < 0) {
        log_message("CLIENT: ERROR - Failed to receive initial message from master");
        pvm_exit();
        exit(1);
    }
    
    log_message("CLIENT: Received message from master, unpacking initial value...");
    
    /* Unpack the received integer */
    pvm_upkint(&received_value, 1, 1);
    
    log_message("CLIENT: Successfully unpacked initial value: %d", received_value);
    log_message("CLIENT: Starting count-up from %d to 1000 with 1-second delays...", received_value);
    
    /* Count up from received value to 1000 with 1-second delay per step */
    for (counter = received_value; counter <= 1000; counter++) {
        log_message("CLIENT: Counter value: %d", counter);
        
        /* Sleep for 1 second (except after the last iteration) */
        if (counter < 1000) {
            sleep(1);
        }
    }
    
    log_message("CLIENT: Count-up completed, reached value 1000");
    
    /* Prepare and send the final result back to master */
    log_message("CLIENT: Initializing send buffer for result message...");
    pvm_initsend(PvmDataDefault);
    
    log_message("CLIENT: Packing final value 1000...");
    int final_value = 1000;
    pvm_pkint(&final_value, 1, 1);
    
    log_message("CLIENT: Sending final result value 1000 to master (tid=%d)", mastertid);
    result = pvm_send(mastertid, 2);
    
    if (result < 0) {
        log_message("CLIENT: ERROR - Failed to send result to master");
        pvm_exit();
        exit(1);
    }
    
    log_message("CLIENT: Successfully sent final result to master");
    
    /* Exit PVM */
    log_message("CLIENT: Calling pvm_exit()...");
    pvm_exit();
    
    log_message("CLIENT: Client process finished successfully");
    
    return 0;
}
