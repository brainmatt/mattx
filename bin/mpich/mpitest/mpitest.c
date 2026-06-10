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
 
/* mpitest.c - The MPI Master Process */
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
    printf("[MASTER] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);

    // Append to logfile
    f = fopen("/tmp/mpitest.log", "a");
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
    MPI_Comm intercomm;
    int my_rank;
    int start_val = 1;
    int result_val = 0;
    int errcodes[1];

    log_debug("Starting up. Initializing MPI...");
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    
    log_debug("Initialized successfully. My Rank is %d", my_rank);

    log_debug("Spawning 'mpitest-client'...");
    // Spawn 1 instance of the client
    int spawn_res = MPI_Comm_spawn("./mpitest-client", MPI_ARGV_NULL, 1, 
                                   MPI_INFO_NULL, 0, MPI_COMM_SELF, 
                                   &intercomm, errcodes);
                                   
    if (spawn_res != MPI_SUCCESS) {
        log_debug("ERROR: Failed to spawn client! Error code: %d", spawn_res);
        MPI_Finalize();
        exit(1);
    }
    log_debug("Successfully spawned client.");

    log_debug("Packing and sending initial integer: %d", start_val);
    // Send to rank 0 of the spawned children
    MPI_Send(&start_val, 1, MPI_INT, 0, 0, intercomm);
    log_debug("Message sent. Entering retrieve mode (waiting for result)...");

    // Wait for the final result from rank 0 of the children
    MPI_Recv(&result_val, 1, MPI_INT, 0, 0, intercomm, MPI_STATUS_IGNORE);
    log_debug("Received final result from client: %d", result_val);

    log_debug("Workflow complete. Disconnecting communicator and shutting down MPI.");
    MPI_Comm_disconnect(&intercomm);
    MPI_Finalize();
    return 0;
}
