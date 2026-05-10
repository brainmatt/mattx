/* pvmtest.c - The Master Process */
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
    printf("[MASTER] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);

    // Append to logfile
    f = fopen("/tmp/pvmtest.log", "a");
    if (f) {
        va_start(args, fmt);
        fprintf(f, "[MASTER] ");
        vfprintf(f, fmt, args);
        fprintf(f, "\n");
        va_end(args);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    int my_tid, client_tid;
    int numt;
    int start_val = 1;
    int result_val = 0;

    log_debug("Starting up. Enrolling in PVM...");
    my_tid = pvm_mytid();
    if (my_tid < 0) {
        log_debug("ERROR: Failed to enroll in PVM. Is pvmd running?");
        exit(1);
    }
    log_debug("Enrolled successfully. My TID is %x", my_tid);

    log_debug("Spawning 'pvmtest-client'...");
    // Spawn 1 instance of the client
    numt = pvm_spawn("pvmtest-client", NULL, PvmTaskDefault, "", 1, &client_tid);
    if (numt != 1) {
        log_debug("ERROR: Failed to spawn client! Return code: %d", numt);
        pvm_exit();
        exit(1);
    }
    log_debug("Successfully spawned client. Client TID is %x", client_tid);

    log_debug("Packing and sending initial integer: %d", start_val);
    pvm_initsend(PvmDataDefault);
    pvm_pkint(&start_val, 1, 1);
    pvm_send(client_tid, MSG_INIT);
    log_debug("Message sent. Entering retrieve mode (waiting for result)...");

    // Wait for the final result
    pvm_recv(client_tid, MSG_DONE);
    pvm_upkint(&result_val, 1, 1);
    log_debug("Received final result from client: %d", result_val);

    log_debug("Workflow complete. Shutting down PVM and exiting.");
    pvm_exit();
    return 0;
}
