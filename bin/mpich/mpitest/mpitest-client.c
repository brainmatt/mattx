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
 
 /* mpitest-client.c - The MPI Client/Worker Process */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <mpi.h>

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
    f = fopen("/tmp/mpitest.log", "a");
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
    MPI_Comm parent_comm;
    int my_rank;
    int current_val = 0;
    int i;

    log_debug("Starting up. Initializing MPI...");
    MPI_Init(&argc, &argv);
    
    // Get the intercommunicator to talk to the master that spawned us
    MPI_Comm_get_parent(&parent_comm);
    
    if (parent_comm == MPI_COMM_NULL) {
        log_debug("ERROR: No parent communicator found! Was I spawned manually?");
        MPI_Finalize();
        exit(1);
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    log_debug("Initialized successfully. My Rank is %d.", my_rank);

    log_debug("Waiting for initial message from Master...");
    // Receive from rank 0 of the parent
    MPI_Recv(&current_val, 1, MPI_INT, 0, 0, parent_comm, MPI_STATUS_IGNORE);
    log_debug("Received starting integer: %d", current_val);

    log_debug("Beginning count-up to 1000...");
    for (i = current_val; i <= 1000; i++) {
        // Log every single step so we can see exactly when migration freezes it!
        log_debug("Counting... %d", i);
        sleep(1);
    }

    log_debug("Reached 1000! Packing and sending result to Master...");
    
    int final_result = 1000;
    MPI_Send(&final_result, 1, MPI_INT, 0, 0, parent_comm);

    log_debug("Result sent. Disconnecting communicator and shutting down MPI.");
    MPI_Comm_disconnect(&parent_comm);
    MPI_Finalize();
    return 0;
}
