/* pvmtest-client.c - The Client/Worker Process */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <pvm3.h>

#define MSG_INIT 1
#define MSG_DONE 2

void log_debug(const char *fmt, ...) {
    FILE *f;
    va_list args;

    // Print to stdout
    va_start(args, fmt);
    printf("[CLIENT] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);

    // Append to logfile
    f = fopen("/tmp/pvmtest.log", "a");
    if (f) {
        va_start(args, fmt);
        fprintf(f, "[CLIENT] ");
        vfprintf(f, fmt, args);
        fprintf(f, "\n");
        va_end(args);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    int my_tid, master_tid;
    int current_val = 0;
    int i;

    log_debug("Starting up. Enrolling in PVM...");
    my_tid = pvm_mytid();
    if (my_tid < 0) {
        log_debug("ERROR: Failed to enroll in PVM.");
        exit(1);
    }

    master_tid = pvm_parent();
    if (master_tid < 0) {
        log_debug("ERROR: Failed to get parent TID. Was I spawned manually?");
        pvm_exit();
        exit(1);
    }
    log_debug("Enrolled successfully. My TID is %x. Master TID is %x", my_tid, master_tid);

    log_debug("Waiting for initial message from Master...");
    pvm_recv(master_tid, MSG_INIT);
    pvm_upkint(&current_val, 1, 1);
    log_debug("Received starting integer: %d", current_val);

    log_debug("Beginning count-up to 1000...");
    for (i = current_val; i <= 1000; i++) {
        // Log every single step so we can see exactly when migration freezes it!
        log_debug("Counting... %d", i);
        sleep(1);
    }

    log_debug("Reached 1000! Packing and sending result to Master...");
    pvm_initsend(PvmDataDefault);
    pvm_pkint(&i, 1, 1); // Send the final value (which will be 1001 after the loop, or we can send 1000)
    
    // Let's explicitly send 1000 as requested
    int final_result = 1000;
    pvm_pkint(&final_result, 1, 1);
    pvm_send(master_tid, MSG_DONE);

    log_debug("Result sent. Shutting down PVM and exiting.");
    pvm_exit();
    return 0;
}
