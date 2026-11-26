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
#include <sys/syscall.h>

// Safe fallback just in case the glibc headers are weird
#ifndef SYS_clone
#define SYS_clone 56
#endif

#include <sys/mount.h>  // For mount()
#include <sys/stat.h>   // For mkdir()
#include <fcntl.h>      // open() for the file mappings
#include <errno.h>
#include <limits.h>      // PATH_MAX
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

// Match the kernel's new MAX_FDS ---
#define MAX_FDS 256
#define MAX_GANG_THREADS 16
// MUST match mattx.h -- see the comment there for why this can't just be
// bumped to PAGE_SIZE (netlink attribute length is a 16-bit field).
#define MATTX_FPU_STATE_MAX 1024

enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, MATTX_ATTR_BLUEPRINT, MATTX_ATTR_MY_NODE_ID, MATTX_ATTR_LOCAL_IP,
       MATTX_ATTR_CONFIG_FILE_IO, MATTX_ATTR_CONFIG_NET_IO, MATTX_ATTR_MATTXFS_ENABLED, MATTX_ATTR_DFSA_DIR, MATTX_ATTR_ACCEPT_GUESTS,
       MATTX_ATTR_CONFIG_LOCAL_LIBS, MATTX_ATTR_DSM_MODE, MATTX_ATTR_MESI_MODE,   /* MUST STAY IN SYNC with the enum in mattx_main.c */
       MATTX_ATTR_BP_OFFSET, MATTX_ATTR_BP_TOTAL, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)

enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, MATTX_CMD_GET_BLUEPRINT, MATTX_CMD_SET_LOCAL_IP, __MATTX_CMD_MAX };
#define MATTX_CMD_MAX (__MATTX_CMD_MAX - 1)

struct mattx_vma_info {
    uint64_t vm_start;
    uint64_t vm_end;
    uint64_t vm_flags;
    uint32_t shmid;
    uint8_t is_shm;    
    uint8_t is_file;    // MUST STAY IN SYNC with struct mattx_vma_info in mattx.h (uses the padding: sizeof() is 32)
    uint16_t file_idx;  // index into the mattx_vma_file table behind vmas[vma_count]
};

// One entry per is_file VMA, behind vmas[vma_count] (file_count entries). MUST STAY IN SYNC with mattx.h.
struct mattx_vma_file {
    uint64_t pgoff;     // offset into the file, in BYTES
    uint64_t size;      // i_size at the home node
    int64_t mtime_sec;  // mtime at the home node
    int64_t mtime_nsec;
    char path[256];
} __attribute__((packed));

// One entry per inherited fd that the TARGET KERNEL opens itself (0035). The stub does not use the table (the fd numbers
// it would have to open are the ones of its own netlink socket and log file): the mirror and the size check are here to
// keep the layout honest. MUST STAY IN SYNC with struct mattx_fd_file in mattx.h.
struct mattx_fd_file {
    uint32_t fd;
    uint32_t group;
    uint32_t flags;
    uint32_t cloexec;
    uint64_t pos;
    uint64_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    char path[256];
} __attribute__((packed));
_Static_assert(sizeof(struct mattx_fd_file) == 304, "struct mattx_fd_file is out of sync with mattx.h (MATTX_FD_FILE_SIZE)");

struct mattx_cpu_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10;
    uint64_t r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
};

struct mattx_thread_info {
    uint32_t tid;
    struct mattx_cpu_regs regs;
    uint64_t fsbase;
    uint64_t gsbase;
    uint64_t clear_child_tid; // The Futex Wake Pointer!
    uint64_t set_child_tid;   // The Thread Init Pointer!
    // MUST STAY IN SYNC with struct mattx_thread_info in mattx.h -- this
    // is a hand-mirrored copy since this file is a separate userspace
    // program and can't #include the kernel module's header.
    uint32_t fpu_size;
    uint8_t fpu_state[MATTX_FPU_STATE_MAX];
} __attribute__((packed)); // FORCE EXACT LAYOUT



// --- IN mattx_stub.c ---

struct mattx_migration_req {
    uint32_t orig_pid;
    uint32_t uid; 
    uint32_t gid; 
    uint32_t home_node;
    
    uint32_t thread_count;
    struct mattx_thread_info threads[MAX_GANG_THREADS];
    
    uint64_t arg_start; 
    uint64_t arg_end;   
    uint64_t start_brk;
    uint64_t brk;
    uint64_t vdso_addr; // The vDSO Transplant Address!

    // MUST STAY IN SYNC with struct mattx_migration_req in mattx.h -- see
    // the comment on monotonic_at_freeze there. The stub never reads this
    // field itself (only the kernel does, at Awakening), but it MUST be
    // present here in the same position so every field after it (comm,
    // dfsa_dir, fd_count, open_fds, vma_count, mattxfs_enabled, local_libs, vmas[])
    // lands at the same offset the kernel used when it serialized this
    // struct -- otherwise the whole blueprint parses as garbage (observed:
    // vma_count decoding to garbage and EVERY subsequent mmap(MAP_FIXED)/
    // page-injection step failing).
    uint64_t monotonic_at_freeze;

    char comm[16];
    char dfsa_dir[256];    
    uint32_t fd_count;          
    uint32_t open_fds[MAX_FDS]; 
    uint32_t vma_count;
    uint8_t mattxfs_enabled;
    uint8_t local_libs;     // MUST STAY IN SYNC with struct mattx_migration_req in mattx.h (packed: vmas[] follows)
    uint32_t file_count;    // mattx_vma_file entries behind vmas[vma_count]
    uint32_t fdfile_count;  // mattx_fd_file entries behind the mattx_vma_file table (0035: the kernel opens them, the stub only keeps the layout)
    struct mattx_vma_info vmas[]; 
} __attribute__((packed)); // <-- FORCE EXACT LAYOUT


/*
 * --- FILE MAPPINGS OF THE BLUEPRINT (issue #67, P3, 0032) ---
 * The home node did not send the pages of a big read-only file mapping (the weights of an LLM): it sent the file's path,
 * the offset, the size and the mtime. The file is mapped here, at the same address, with the same protection; the pages
 * come from this node's own page cache on demand. A path under DFSA_DIR (the shared NAS: the very same file is mounted
 * at the very same path on this node, native speed, no MattX traffic) is opened as it is; any other path is opened
 * through MattXFS (/mattxfs/<home>/...: the MattXFS .mmap, pages come from the home node). The file must be the file of the home
 * node: size and mtime are compared. There is no way back to "send the pages" any more, so a mapping that cannot be made
 * is fatal (a zero filled hole in the weights would be silently wrong): the stub says why and ends, the home node keeps
 * its process.
 */
static int map_file_vma(const struct mattx_migration_req *req, const struct mattx_vma_info *v, const struct mattx_vma_file *vf)
{
    size_t dlen = strnlen(req->dfsa_dir, sizeof(req->dfsa_dir));
    char path[sizeof(vf->path) + 1];
    char mpath[sizeof(vf->path) + 32];
    const char *openpath = path;
    struct stat st;
    int prot = 0, flags = MAP_FIXED;
    void *addr;
    int fd;

    memcpy(path, vf->path, sizeof(vf->path));
    path[sizeof(vf->path)] = '\0';

    while (dlen > 1 && req->dfsa_dir[dlen - 1] == '/') dlen--;
    if (dlen == 0 || strncmp(path, req->dfsa_dir, dlen) != 0 || (path[dlen] != '/' && path[dlen] != '\0')) {
        if (!req->mattxfs_enabled) {
            fprintf(stderr, "MattX-Stub: FATAL: file mapping '%s' is not under DFSA_DIR '%s' and MattXFS is off\n", path, req->dfsa_dir);
            return -1;
        }
        snprintf(mpath, sizeof(mpath), "/mattxfs/%u%s", req->home_node, path);     // the home node's file, through MattXFS
        openpath = mpath;
    }

    fd = open(openpath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "MattX-Stub: FATAL: cannot open '%s' for the file mapping at 0x%lx: %s (DFSA_DIR mounted / MattXFS up on this node?)\n",
                openpath, (unsigned long)v->vm_start, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != vf->size ||
        (int64_t)st.st_mtim.tv_sec != vf->mtime_sec || (int64_t)st.st_mtim.tv_nsec != vf->mtime_nsec) {
        fprintf(stderr, "MattX-Stub: FATAL: '%s' here is not the file of the home node (size %llu/%llu, mtime %lld.%09ld/%lld.%09lld)\n",
                openpath, (unsigned long long)st.st_size, (unsigned long long)vf->size,
                (long long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec, (long long)vf->mtime_sec, (long long)vf->mtime_nsec);
        close(fd);
        return -1;
    }

    if (v->vm_flags & 0x1) prot |= PROT_READ;     // VM_READ
    if (v->vm_flags & 0x2) prot |= PROT_WRITE;    // VM_WRITE
    if (v->vm_flags & 0x4) prot |= PROT_EXEC;     // VM_EXEC
    flags |= (v->vm_flags & 0x8) ? MAP_SHARED : MAP_PRIVATE;     // VM_SHARED

    addr = mmap((void *)v->vm_start, v->vm_end - v->vm_start, prot, flags, fd, (off_t)vf->pgoff);
    close(fd);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "MattX-Stub: FATAL: mmap of '%s' at 0x%lx failed: %s\n", openpath, (unsigned long)v->vm_start, strerror(errno));
        return -1;
    }
    printf("MattX-Stub: File mapping 0x%lx - 0x%lx = '%s' offset %llu (%s, no pages sent)\n",
           (unsigned long)v->vm_start, (unsigned long)v->vm_end, openpath, (unsigned long long)vf->pgoff,
           (v->vm_flags & 0x8) ? "shared" : "private");
    return 0;
}

/*
 * --- THE HPC FAST-PATH OF MATTXFS (LOCAL_LIBS) ---
 * In MattXFS mode the Surrogate's whole root is the home node's tree served over the wire. MattXFS files cannot be mmap()ed,
 * so a late dlopen() (pthread_cancel -> libgcc_s.so.1, NSS, iconv, Python extension modules ...) failed, and every library
 * page-in of a lazily loaded library would have been a network round trip anyway. Like the kprobe mode (is_hpc_local_lib() in
 * mattx_fileio.c: LOCAL_LIBS=true lets a Surrogate use ITS node's own libraries; all nodes have the same packages) the local
 * library trees are bind-mounted over the MattXFS tree BEFORE the chroot: every system call (open, stat, access, readlink,
 * getdents, execve, mmap ...) then sees them natively, no hook needed. /proc is mounted by the stub itself.
 * KEEP THIS LIST IN SYNC with is_hpc_local_lib() in mattx_fileio.c.
 */
static const char *const hpc_local_paths[] = {
    "/lib", "/lib64", "/usr/lib", "/usr/lib64", "/usr/share/locale", "/sys", "/etc/ld.so.cache",
};

static void bind_local_libs(const char *mfs_path)
{
    const int n = sizeof(hpc_local_paths) / sizeof(hpc_local_paths[0]);
    char done[sizeof(hpc_local_paths) / sizeof(hpc_local_paths[0])][PATH_MAX];
    int ndone = 0;

    for (int i = 0; i < n; i++) {
        char real[PATH_MAX], target[PATH_MAX + 256];
        struct stat st;
        int dup = 0;

        // /lib and /lib64 are symlinks on usr-merged systems: bind the real directory once
        if (!realpath(hpc_local_paths[i], real) || stat(real, &st) != 0) {
            printf("MattX-Stub: HPC local libs: %s does not exist on this node, skipped\n", hpc_local_paths[i]);
            continue;
        }
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            continue;
        for (int j = 0; j < ndone; j++)
            if (strcmp(done[j], real) == 0)
                dup = 1;
        if (dup)
            continue;
        memcpy(done[ndone], real, strlen(real) + 1);    // remember it, whatever happens below: no second try, no second message
        ndone++;

        snprintf(target, sizeof(target), "%s%s", mfs_path, real);

        // Same trick as the DFSA bind mount: access() makes MattXFS send the lookup RPC to the home node, so the dentry
        // we mount over exists. If the home node has no such path, there is nothing to cover.
        if (access(target, F_OK) != 0) {
            printf("MattX-Stub: HPC local libs: %s does not exist on the home node, skipped\n", real);
            continue;
        }

        if (mount(real, target, NULL, MS_BIND | (S_ISDIR(st.st_mode) ? MS_REC : 0), NULL) == 0) {
            printf("MattX-Stub: HPC local libs: bind mount %s -> %s\n", real, target);
        } else {
            perror("MattX-Stub: HPC local libs: bind mount failed (continuing with the remote file)");
        }
    }
}

static struct mattx_migration_req *received_req = NULL;
static uint32_t bp_total = 0;   // 0033: full blueprint length announced by the kernel
static uint32_t bp_got = 0;     // 0033: bytes assembled so far

static int blueprint_cb(struct nl_msg *msg, void *arg) {
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct nlattr *attrs[MATTX_ATTR_MAX + 1];
    struct nla_policy policy[MATTX_ATTR_MAX + 1];
    
    printf("MattX-Stub:[CALLBACK] Received a Netlink message from kernel!\n");

    memset(policy, 0, sizeof(policy));
    policy[MATTX_ATTR_BLUEPRINT].type = NLA_BINARY;
    policy[MATTX_ATTR_BP_OFFSET].type = NLA_U32;
    policy[MATTX_ATTR_BP_TOTAL].type = NLA_U32;

    if (genlmsg_parse(nlh, 0, attrs, MATTX_ATTR_MAX, policy) < 0) {
        printf("MattX-Stub:[CALLBACK] Message parsed, but no blueprint found (likely an ACK).\n");
        return NL_SKIP;
    }

    if (attrs[MATTX_ATTR_BLUEPRINT]) {
        printf("MattX-Stub: [CALLBACK] SUCCESS: Blueprint attribute found!\n");
        void *chunk = nla_data(attrs[MATTX_ATTR_BLUEPRINT]);
        int len = nla_len(attrs[MATTX_ATTR_BLUEPRINT]);
        uint32_t off = attrs[MATTX_ATTR_BP_OFFSET] ? nla_get_u32(attrs[MATTX_ATTR_BP_OFFSET]) : 0;
        uint32_t total = attrs[MATTX_ATTR_BP_TOTAL] ? nla_get_u32(attrs[MATTX_ATTR_BP_TOTAL]) : (uint32_t)len;

        // 0033: the kernel sends the blueprint in chunks (a netlink attribute holds at most 65535 bytes)
        if (!received_req) {
            if (off != 0 || total == 0 || total < (uint32_t)len) {
                fprintf(stderr, "MattX-Stub: FATAL - bad first blueprint chunk (offset %u, len %d, total %u)\n", off, len, total);
                return NL_SKIP;
            }
            received_req = malloc(total);
            if (!received_req) return NL_SKIP;
            bp_total = total;
            bp_got = 0;
        }
        if (off == bp_got && off + (uint32_t)len <= bp_total) {
            memcpy((char *)received_req + off, chunk, len);
            bp_got += len;
            printf("MattX-Stub: [CALLBACK] Blueprint chunk: offset %u, %d bytes (%u of %u)\n", off, len, bp_got, bp_total);
        } else {
            fprintf(stderr, "MattX-Stub: WARNING - unexpected blueprint chunk (offset %u, len %d, have %u of %u): ignored\n", off, len, bp_got, bp_total);
        }
    }
    return NL_OK;
}


static int dummy_thread_fn(void *arg) {
    // Child thread: just spin until the kernel hijacks my brain!
    //
    // This thread is created with a raw clone() below, WITHOUT
    // CLONE_SETTLS -- it has no thread-local-storage area of its own and
    // is unknowingly sharing the parent stub thread's TLS block (errno,
    // and any other libc per-thread state). Any libc call that can touch
    // TLS -- even sleep(), which sets errno to EINTR on the signal that's
    // specifically used to freeze/drain this thread for the brain
    // transplant -- races with the real stub thread and any other dummy
    // Gang threads over that single shared TLS block. Use a bare spin
    // loop with no function calls at all so this thread touches nothing
    // beyond its own registers until the kernel overwrites its pt_regs
    // (rip/rsp/fsbase/gsbase) with the real captured thread's state.
    for (;;) {
        __asm__ __volatile__("pause");
    }
    return 0;
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
    // The blueprint (now including per-thread FPU/XSAVE state) can be
    // ~70KB+, comfortably over netlink's defaults. Two SEPARATE size
    // limits both need raising: the kernel socket's SO_RCVBUF (queue
    // capacity) and libnl's own internal per-message read buffer, which
    // is what actually errors here -- nl_recvmsgs_default() logs "-5"
    // (NLE_NOMEM) when its fixed-size read buffer is too small, and we
    // end up parsing a garbage/zeroed blueprint (VMAs: 0, FDs: 0, empty
    // comm) instead of failing loudly -- every subsequent mmap/page-
    // injection step then fails because the VMAs were never actually
    // there to carve.
    nl_socket_set_buffer_size(sock, 1024 * 1024, 0);
    nl_socket_set_msg_buf_size(sock, 1024 * 1024);
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

    // 0033: fetch the rest of the blueprint, one chunk per request
    while (bp_got < bp_total) {
        uint32_t before = bp_got;
        int tries = 10;

        msg = nlmsg_alloc();
        genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_GET_BLUEPRINT, 1);
        nla_put_u32(msg, MATTX_ATTR_BP_OFFSET, bp_got);
        if (nl_send_auto(sock, msg) < 0) {
            fprintf(stderr, "MattX-Stub: FATAL - Failed to request blueprint chunk at offset %u.\n", bp_got);
            return -1;
        }
        nlmsg_free(msg);

        while (bp_got == before && tries > 0) {
            int err = nl_recvmsgs_default(sock);
            if (err < 0) fprintf(stderr, "MattX-Stub: Error receiving blueprint chunk: %d\n", err);
            tries--;
        }
        if (bp_got == before) {
            fprintf(stderr, "MattX-Stub: FATAL - Blueprint chunk at offset %u never arrived (have %u of %u)!\n", before, bp_got, bp_total);
            return -1;
        }
    }
    printf("MattX-Stub: Blueprint complete: %u bytes\n", bp_total);

    printf("MattX-Stub: Blueprint received. Original PID: %u, Name: '%s', UID: %u, GID: %u, VMAs: %u, FDs: %u\n", 
           received_req->orig_pid, received_req->comm, received_req->uid, received_req->gid, received_req->vma_count, received_req->fd_count);

    for (uint32_t i = 0; i < received_req->vma_count; i++) {
        // --- FIXED: Copy by value to avoid unaligned pointer warnings! ---
        struct mattx_vma_info v = received_req->vmas[i]; 
        size_t size = v.vm_end - v.vm_start;

        // --- 0032: a file mapping: map the file here, nothing was sent for it ---
        if (v.is_file) {
            struct mattx_vma_file vf;
            if (v.file_idx >= received_req->file_count) {
                fprintf(stderr, "MattX-Stub: FATAL: VMA %u wants file entry %u, the blueprint has %u\n", i, v.file_idx, received_req->file_count);
                exit(1);
            }
            // the table is behind vmas[vma_count], packed: copy it out
            memcpy(&vf, (const char *)&received_req->vmas[received_req->vma_count] + (size_t)v.file_idx * sizeof(vf), sizeof(vf));
            if (map_file_vma(received_req, &v, &vf) != 0)
                exit(1);
            continue;
        }

        int prot = PROT_READ | PROT_WRITE | PROT_EXEC;

        // --- THE MAP_SHARED FIX ---
        int flags = MAP_ANONYMOUS | MAP_FIXED;
        if (v.is_shm) {
            flags |= MAP_SHARED; // Shared Memory gets MAP_SHARED!
        } else {
            flags |= MAP_PRIVATE; // Normal memory stays MAP_PRIVATE!
        }

        // The Stack Growth Protector ---
        if (v.vm_flags & 0x0100) {
            flags |= MAP_GROWSDOWN;
        }

        void *addr = mmap((void *)v.vm_start, size, prot, flags, -1, 0);        
        if (addr == MAP_FAILED) {
            perror("MattX-Stub: mmap MAP_FIXED failed");
        } else {
            printf("MattX-Stub: Carved VMA %u: 0x%lx - 0x%lx (RWX%s)\n", 
                   i, (unsigned long)v.vm_start, (unsigned long)v.vm_end, (flags & MAP_GROWSDOWN) ? " + GROWSDOWN" : "");
        }
    }

    printf("MattX-Stub: Spawning %u dummy threads for Gang Migration...\n", received_req->thread_count);
    int clone_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    
    // Generous stack even though dummy_thread_fn makes no calls and needs
    // almost none of it -- cheap insurance against any signal-delivery
    // frame the kernel pushes onto this thread's stack before it reaches
    // the brain transplant.
    #define MATTX_DUMMY_STACK_SIZE (64 * 1024)
    for (uint32_t i = 1; i < received_req->thread_count; i++) {
        void *dummy_stack = malloc(MATTX_DUMMY_STACK_SIZE);

        // Use the glibc clone() wrapper! It safely handles the stack return address.
        if (clone(dummy_thread_fn, (char *)dummy_stack + MATTX_DUMMY_STACK_SIZE, clone_flags, NULL) == -1) {
            perror("MattX-Stub: clone failed");
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

            // --- THE HPC FAST-PATH (LOCAL_LIBS of the home node): the local library trees over the MattXFS tree ---
            if (received_req->local_libs)
                bind_local_libs(mfs_path);
            else
                printf("MattX-Stub: LOCAL_LIBS is off: libraries come from the home node\n");

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

    // --- THE SYMMETRICAL FREEZE ---
    // Just loop peacefully. The kernel will inject a Task Work and freeze us natively!
    while (1) sleep(1); 
    
    return 0;
}