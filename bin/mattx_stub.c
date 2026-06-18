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

#define _GNU_SOURCE // REQUIRED for CLONE_NEWNS!
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/mman.h> 
#include <signal.h>
#include <sched.h>      // For unshare()
#include <sys/mount.h>  // For mount()
#include <sys/stat.h>   // For mkdir()
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

// Match the kernel's new MAX_FDS ---
#define MAX_FDS 256

enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, MATTX_ATTR_BLUEPRINT, MATTX_ATTR_MY_NODE_ID, MATTX_ATTR_LOCAL_IP, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)

enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, MATTX_CMD_GET_BLUEPRINT, MATTX_CMD_SET_LOCAL_IP, __MATTX_CMD_MAX };
#define MATTX_CMD_MAX (__MATTX_CMD_MAX - 1)

struct mattx_vma_info {
    uint64_t vm_start;
    uint64_t vm_end;
    uint64_t vm_flags;
};

struct mattx_cpu_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10;
    uint64_t r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
};

struct mattx_migration_req {
    uint32_t orig_pid;
    uint32_t uid; 
    uint32_t gid; 
    uint32_t home_node;
    struct mattx_cpu_regs regs;
    uint64_t fsbase; 
    uint64_t gsbase; 
    uint64_t arg_start; 
    uint64_t arg_end;   
    char comm[16];
    char dfsa_dir[256];    
    uint32_t fd_count;          
    uint32_t open_fds[MAX_FDS]; 
    uint32_t vma_count;
    uint8_t mattxfs_enabled;
    uint8_t pad[3];
    struct mattx_vma_info vmas[]; 
};

static struct mattx_migration_req *received_req = NULL;

static int blueprint_cb(struct nl_msg *msg, void *arg) {
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct nlattr *attrs[MATTX_ATTR_MAX + 1];
    struct nla_policy policy[MATTX_ATTR_MAX + 1];
    
    printf("MattX-Stub:[CALLBACK] Received a Netlink message from kernel!\n");

    memset(policy, 0, sizeof(policy));
    policy[MATTX_ATTR_BLUEPRINT].type = NLA_BINARY;

    if (genlmsg_parse(nlh, 0, attrs, MATTX_ATTR_MAX, policy) < 0) {
        printf("MattX-Stub:[CALLBACK] Message parsed, but no blueprint found (likely an ACK).\n");
        return NL_SKIP;
    }

    if (attrs[MATTX_ATTR_BLUEPRINT]) {
        printf("MattX-Stub: [CALLBACK] SUCCESS: Blueprint attribute found!\n");
        struct mattx_migration_req *req = nla_data(attrs[MATTX_ATTR_BLUEPRINT]);
        int len = nla_len(attrs[MATTX_ATTR_BLUEPRINT]);
        
        received_req = malloc(len);
        if (received_req) memcpy(received_req, req, len);
    }
    return NL_OK;
}

int main() {
    // Redirect stdout and stderr to a log file for debugging
    if (freopen("/tmp/mattx_stub.log", "a", stdout) == NULL) {
        perror("MattX Stub: Failed to redirect stdout");
    }
    if (freopen("/tmp/mattx_stub.log", "a", stderr) == NULL) {
        perror("MattX Stub: Failed to redirect stderr");
    }
    setvbuf(stdout, NULL, _IONBF, 0); 
    setvbuf(stderr, NULL, _IONBF, 0);

    struct nl_sock *sock;
    struct nl_msg *msg;
    int family_id;
    uint32_t my_pid = getpid();

    printf("\n========================================\n");
    printf("MattX-Stub: Waking up! PID %u. Requesting blueprint...\n", my_pid);

    sock = nl_socket_alloc();
    genl_connect(sock);
    family_id = genl_ctrl_resolve(sock, "MATTX");
    
    if (family_id < 0) {
        fprintf(stderr, "MattX-Stub: Kernel module not loaded!\n");
        return -1;
    }

    nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, blueprint_cb, NULL);

    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_GET_BLUEPRINT, 1);
    
    if (nl_send_auto(sock, msg) < 0) {
        fprintf(stderr, "MattX-Stub: Failed to request blueprint.\n");
        return -1;
    }
    nlmsg_free(msg);

    printf("MattX-Stub: Waiting for kernel reply...\n");
    
    int retries = 10;
    while (!received_req && retries > 0) {
        int err = nl_recvmsgs_default(sock);
        if (err < 0) {
            fprintf(stderr, "MattX-Stub: Error receiving netlink message: %d\n", err);
        }
        retries--;
    }

    if (!received_req) {
        fprintf(stderr, "MattX-Stub: FATAL - No blueprint received after 10 attempts!\n");
        return -1;
    }

    printf("MattX-Stub: Blueprint received. Original PID: %u, Name: '%s', UID: %u, GID: %u, VMAs: %u, FDs: %u\n", 
           received_req->orig_pid, received_req->comm, received_req->uid, received_req->gid, received_req->vma_count, received_req->fd_count);

    // Get the approximate memory address of our own code to prevent suicide!
    uint64_t my_brain_addr = (uint64_t)&main & ~(0xFFFULL); 

    for (uint32_t i = 0; i < received_req->vma_count; i++) {
        struct mattx_vma_info *v = &received_req->vmas[i];
        size_t size = v->vm_end - v->vm_start;

        // --- THE ALMA ANOMALY SHIELD ---
        if (my_brain_addr >= v->vm_start && my_brain_addr < v->vm_end) {
            fprintf(stderr, "\n=======================================================\n");
            fprintf(stderr, "MattX-Stub: FATAL ALMA ANOMALY DETECTED! 🚨\n");
            fprintf(stderr, "VMA 0x%lx - 0x%lx overlaps with my own brain (0x%lx)!\n", v->vm_start, v->vm_end, my_brain_addr);
            fprintf(stderr, "I refuse to overwrite my own memory. Please compile mattx-stub with '-fPIE -pie'!\n");
            fprintf(stderr, "=======================================================\n\n");
            exit(1);
        }

        int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

        // The Stack Growth Protector ---
        if (v->vm_flags & 0x0100) {
            flags |= MAP_GROWSDOWN;
        }

        void *addr = mmap((void *)v->vm_start, size, prot, flags, -1, 0);        
        if (addr == MAP_FAILED) {
            perror("MattX-Stub: mmap MAP_FIXED failed");
        } else {
            printf("MattX-Stub: Carved VMA %u: 0x%lx - 0x%lx (RWX%s)\n", 
                   i, v->vm_start, v->vm_end, (flags & MAP_GROWSDOWN) ? " + GROWSDOWN" : "");
        }
    }

    // Clean, dynamic FD table expansion! ---
    for (int i = 3; i < MAX_FDS; i++) {
        dup2(0, i); 
    }
    for (int i = 3; i < MAX_FDS; i++) {
        close(i);
    }
    
    printf("MattX-Stub: Memory carved and FD table expanded to %d. Ready for hijack.\n", MAX_FDS);

    // --- PHASE 22 & 23 - THE NAMESPACE ILLUSION & DFSA ---
    if (received_req->mattxfs_enabled) {
        char mfs_path[256];
        snprintf(mfs_path, sizeof(mfs_path), "/mattxfs/%u", received_req->home_node);
        
        printf("MattX-Stub: MattXFS is enabled. Building Namespace Illusion...\n");
        
        if (unshare(CLONE_NEWNS) == 0) {
            mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);
            
            // --- NEW: PHASE 23 (DFSA BIND MOUNT) ---
            if (received_req->dfsa_dir[0] != '\0') {
                char dfsa_target[512];
                snprintf(dfsa_target, sizeof(dfsa_target), "%s%s", mfs_path, received_req->dfsa_dir);
                
                // Secret Trick: We call access() to force MattXFS to send a lookup RPC to Node 1.
                // This ensures the virtual inode for the target folder actually exists in memory 
                // before we try to mount over it!
                access(dfsa_target, F_OK);
                
                // The Magic Bind Mount!
                if (mount(received_req->dfsa_dir, dfsa_target, NULL, MS_BIND | MS_REC, NULL) == 0) {
                    printf("MattX-Stub: DFSA Bind Mount successful: %s -> %s\n", received_req->dfsa_dir, dfsa_target);
                } else {
                    perror("MattX-Stub: DFSA Bind Mount failed (Does the folder exist on both nodes?)");
                }
            }
            // ---------------------------------------

            if (chroot(mfs_path) == 0) {
                // 4. Pivot into the new root!
                if (chdir("/") < 0) {
                    perror("MattX Stub: Failed to chdir to new root");
                }                
                mkdir("/proc", 0755);
                mount("proc", "/proc", "proc", 0, NULL);
                printf("MattX-Stub: Illusion complete! Trapped in %s\n", mfs_path);
            } else {
                perror("MattX-Stub: chroot failed");
            }
        } else {
            perror("MattX-Stub: unshare failed");
        }
    }

    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_HIJACK_ME, 1);
    nla_put_u32(msg, MATTX_ATTR_STUB_PID, my_pid);

    if (nl_send_auto(sock, msg) < 0) {
        fprintf(stderr, "MattX-Stub: Failed to send HIJACK_ME\n");
    } else {
        printf("MattX-Stub: HIJACK_ME sent. Stopping myself for transplant...\n");
    }

    nlmsg_free(msg);
    nl_socket_free(sock);
    free(received_req);

    raise(SIGSTOP); 
    
    printf("MattX-Stub: ERROR - I woke up but I am still the stub!\n");
    while (1) sleep(1); 
    return 0;
}

