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
 
 /*
 * migtest4.c - MattX Batch 3 Syscall QA Tester
 * Tests: brk, mmap, munmap, mprotect, mremap, madvise, mbind
 *        sched_getaffinity, sched_setaffinity, sched_yield
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/syscall.h>
#include <string.h>

void worker_process() {
    printf("[Worker] Started with PID: %d\n", getpid());
    
    while (1) {
        printf("\n--- MattX Batch 3 QA Test ---\n");
        
        // ==========================================
        // 1. The Memory Allocators (brk, mmap, munmap, mremap)
        // ==========================================
        
        // brk / sbrk
        void *curr_brk = sbrk(0);
        sbrk(4096);  // Allocate 4KB
        sbrk(-4096); // Free 4KB
        printf("[Worker] brk/sbrk tested. Current break: %p\n", curr_brk);

        // mmap (Allocate 4KB of private anonymous RAM)
        void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr != MAP_FAILED) {
            printf("[Worker] mmap(4096 bytes) -> Success at %p\n", ptr);
            
            // print message to migrate while the memory is allocated
            printf("[Worker] Memory allocated. You can now migrate PID: %d\n", getpid());
            sleep(5); // Give time for migration

            // mremap (Expand the 4KB allocation to 8KB)
            void *new_ptr = mremap(ptr, 4096, 8192, MREMAP_MAYMOVE);
            if (new_ptr != MAP_FAILED) {
                printf("[Worker] mremap(4096 -> 8192 bytes) -> Success at %p\n", new_ptr);
                ptr = new_ptr; // Update pointer for cleanup
            } else {
                perror("[Worker] mremap failed");
            }

            // ==========================================
            // 2. The Memory Tuners (mprotect, madvise, mbind)
            // ==========================================
            
            // mprotect (Make the memory Read-Only)
            if (mprotect(ptr, 8192, PROT_READ) == 0) {
                printf("[Worker] mprotect(PROT_READ) -> Success\n");
            } else {
                perror("[Worker] mprotect failed");
            }

            // madvise (Tell the kernel we don't need this memory right now)
            if (madvise(ptr, 8192, MADV_DONTNEED) == 0) {
                printf("[Worker] madvise(MADV_DONTNEED) -> Success\n");
            } else {
                perror("[Worker] madvise failed");
            }

            // mbind (Raw syscall to avoid libnuma dependency. MPOL_DEFAULT = 0)
            long mbind_res = syscall(SYS_mbind, ptr, 8192, 0, NULL, 0, 0);
            if (mbind_res == 0) {
                printf("[Worker] mbind(MPOL_DEFAULT) -> Success\n");
            } else {
                perror("[Worker] mbind failed (expected if kernel lacks NUMA support)");
            }

            // munmap (Clean up the memory)
            if (munmap(ptr, 8192) == 0) {
                printf("[Worker] munmap() -> Success\n");
            } else {
                perror("[Worker] munmap failed");
            }
        } else {
            perror("[Worker] mmap failed");
        }

        // ==========================================
        // 3. The Scheduler Controls
        // ==========================================
        
        cpu_set_t mask;
        CPU_ZERO(&mask);
        
        // sched_getaffinity
        if (sched_getaffinity(0, sizeof(cpu_set_t), &mask) == 0) {
            printf("[Worker] sched_getaffinity -> CPU 0 is %s\n", CPU_ISSET(0, &mask) ? "AVAILABLE" : "UNAVAILABLE");
        } else {
            perror("[Worker] sched_getaffinity failed");
        }

        // sched_setaffinity (Pin to CPU 0 just for testing)
        CPU_ZERO(&mask);
        CPU_SET(0, &mask);
        if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == 0) {
            printf("[Worker] sched_setaffinity(CPU 0) -> Success\n");
        } else {
            perror("[Worker] sched_setaffinity failed");
        }

        // sched_yield (Politely give up the CPU timeslice)
        if (sched_yield() == 0) {
            printf("[Worker] sched_yield() -> Success\n");
        } else {
            perror("[Worker] sched_yield failed");
        }

        printf("----------------------------\n");
        
        // Sleep for 3 seconds to give Matt time to echo into /proc/mattx/admin!
        sleep(3); 
    }
}

int main() {
    printf("[Master] Starting migtest4 (Batch 3 Syscall QA)...\n");
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("Fork failed");
        return 1;
    }
    
    if (pid == 0) {
        // Child process
        worker_process();
        exit(0);
    } else {
        // Parent process
        printf("[Master] Spawned worker with PID %d. Waiting for it to finish...\n", pid);
        wait(NULL);
        printf("[Master] Worker finished. Exiting.\n");
    }
    
    return 0;
}

