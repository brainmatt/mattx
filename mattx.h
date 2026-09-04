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

#ifndef MATTX_H
#define MATTX_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <net/genetlink.h>
#include <net/sock.h>
#include <linux/in.h>
#include <net/tcp.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>         
#include <linux/sched/loadavg.h> 
#include <linux/mm.h>            
#include <linux/sched/signal.h>  
#include <linux/sched/task.h>    
#include <linux/rcupdate.h>      
#include <linux/mmap_lock.h>     
#include <asm/ptrace.h>          
#include <linux/umh.h>           
#include <linux/highmem.h>       
#include <linux/cred.h>          
#include <linux/uidgid.h>        
#include <linux/fs.h>            
#include <linux/fdtable.h>       
#include <linux/anon_inodes.h>   
#include <linux/uaccess.h>       
#include <linux/mman.h>
#include <linux/shm.h>
#include <linux/mmu_context.h>   
#include <linux/kprobes.h>       
#include <linux/workqueue.h>     
#include <linux/bitops.h>        
#include <linux/stat.h>
#include <linux/statfs.h>          
#include <linux/version.h>
#include <linux/time64.h> // Ensure we have time64_t
#include <linux/task_work.h>
#include <linux/utsname.h>
#include <linux/resource.h>

// --- SYSTEM V IPC FLAGS (Hidden by the kernel) ---
#ifndef SHM_RDONLY
#define SHM_RDONLY 010000
#endif

// --- KERNEL COMPATIBILITY: The Sockaddr Evolution ---
// In late 2025 (Linux 6.18/6.19+), the kernel replaced 'struct sockaddr *' 
// with 'struct sockaddr_unsized *' in internal socket APIs to fix flexible array bounds checking.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
    #define MATTX_SA_CAST(addr) ((struct sockaddr_unsized *)(addr))
#else
    #define MATTX_SA_CAST(addr) ((struct sockaddr *)(addr))
#endif


#define MATTX_PORT 7226
#define MAX_NODES 1024 
#define BALANCER_INTERVAL_MS 2000 

#define FIXED_LOAD_1_0 2048
#define FIXED_LOAD_0_2 409
#define MAX_VMAS 1024 
#define MAX_GUESTS 1024 
#define MAX_GANG_THREADS 16
#define MAX_FDS 256


// Max size of a captured thread's raw FPU/SSE/AVX register image
// (XSAVE/FXSAVE area). Covers up through AVX2 (~832-960 bytes on typical
// hardware) with headroom. NOTE: this whole struct gets shipped to
// mattx-stub as a single Generic Netlink attribute, whose length field is
// only 16 bits wide (65535-byte hard cap) -- MAX_GANG_THREADS (16) copies
// of this buffer are embedded directly in mattx_migration_req, so keep
// this modest. A full PAGE_SIZE (4096) buffer here blows straight through
// that limit (16 * 4096 = 65536 alone) and silently corrupts the
// blueprint on the wire (nla_len wraps, mattx-stub parses a garbage/zeroed
// struct: "VMAs: 0, FDs: 0" and every page injection then fails). If this
// ever needs to grow (e.g. AVX-512 support, ~2696 bytes), the attribute
// would need to be split/chunked instead of raised further.
#define MATTX_FPU_STATE_MAX 1024

#define MATTX_MAGIC 0x4D415454 
#define MATTX_MAX_PAYLOAD (10 * 1024 * 1024) 
// max dsm segments
#define MAX_DSM_SEGMENTS 16


enum mattx_msg_type {
    MATTX_MSG_HEARTBEAT = 1,
    MATTX_MSG_LOAD_UPDATE,
    MATTX_MSG_MIGRATE_REQ,
    MATTX_MSG_READY_FOR_DATA, 
    MATTX_MSG_PAGE_TRANSFER, 
    MATTX_MSG_MIGRATE_DONE,   
    MATTX_MSG_PROCESS_EXIT,   
    MATTX_MSG_KILL_SURROGATE, 
    MATTX_MSG_SYSCALL_FWD,
    MATTX_MSG_RECALL_REQ,     
    MATTX_MSG_RETURN_BLUEPRINT, 
    MATTX_MSG_RETURN_DONE,    
    MATTX_MSG_SYS_OPEN_REQ,   
    MATTX_MSG_SYS_OPEN_REPLY, 
    MATTX_MSG_SYS_CLOSE_REQ, 
    MATTX_MSG_SYS_READ_REQ,
    MATTX_MSG_SYS_READ_REPLY,
    MATTX_MSG_SYS_LSEEK_REQ,
    MATTX_MSG_SYS_LSEEK_REPLY,
    MATTX_MSG_SYS_STATX_REQ,
    MATTX_MSG_SYS_STATX_REPLY,
    MATTX_MSG_SYS_DUP_REQ,
    MATTX_MSG_SYS_DUP_REPLY,
    MATTX_MSG_SYS_FSYNC_REQ,
    MATTX_MSG_SYS_FSYNC_REPLY,
    MATTX_MSG_SYS_SOCKET_REQ,
    MATTX_MSG_SYS_SOCKET_REPLY,
    MATTX_MSG_SYS_CONNECT_REQ,
    MATTX_MSG_SYS_CONNECT_REPLY,
    MATTX_MSG_SYS_BIND_REQ,
    MATTX_MSG_SYS_BIND_REPLY,
    MATTX_MSG_SYS_LISTEN_REQ,
    MATTX_MSG_SYS_LISTEN_REPLY,
    MATTX_MSG_SYS_SEND_REQ,
    MATTX_MSG_SYS_SEND_REPLY,
    MATTX_MSG_SYS_RECV_REQ,
    MATTX_MSG_SYS_RECV_REPLY,
    MATTX_MSG_SYS_ACCEPT_REQ,
    MATTX_MSG_SYS_ACCEPT_REPLY,
    MATTX_MSG_SYS_POLL_REQ,
    MATTX_MSG_SYS_POLL_REPLY,
    MATTX_MSG_VFS_GETATTR_REQ,
    MATTX_MSG_VFS_GETATTR_REPLY,
    MATTX_MSG_VFS_READDIR_REQ,
    MATTX_MSG_VFS_READDIR_REPLY,
    MATTX_MSG_VFS_OPEN_REQ,
    MATTX_MSG_VFS_OPEN_REPLY,
    MATTX_MSG_VFS_READ_REQ,
    MATTX_MSG_VFS_READ_REPLY,
    MATTX_MSG_VFS_WRITE_REQ,
    MATTX_MSG_VFS_WRITE_REPLY,
    MATTX_MSG_VFS_CLOSE_REQ,
    MATTX_MSG_VFS_LSEEK_REQ,
    MATTX_MSG_VFS_LSEEK_REPLY,
    MATTX_MSG_VFS_FSYNC_REQ,
    MATTX_MSG_VFS_FSYNC_REPLY,
    MATTX_MSG_SYS_UNLINK_REQ,
    MATTX_MSG_SYS_UNLINK_REPLY,
    MATTX_MSG_SYS_EPOLL_CREATE_REQ,
    MATTX_MSG_SYS_EPOLL_CREATE_REPLY,
    MATTX_MSG_SYS_EPOLL_CTL_REQ,
    MATTX_MSG_SYS_EPOLL_CTL_REPLY,
    MATTX_MSG_SYS_EPOLL_WAIT_REQ,
    MATTX_MSG_SYS_EPOLL_WAIT_REPLY,
    MATTX_MSG_SYS_GETSOCKNAME_REQ,
    MATTX_MSG_SYS_GETSOCKNAME_REPLY,
    MATTX_MSG_SYS_GETPEERNAME_REQ,
    MATTX_MSG_SYS_GETPEERNAME_REPLY,
    MATTX_MSG_SYS_SETSOCKOPT_REQ,
    MATTX_MSG_SYS_SETSOCKOPT_REPLY,
    MATTX_MSG_SYS_GETSOCKOPT_REQ,
    MATTX_MSG_SYS_GETSOCKOPT_REPLY,
    MATTX_MSG_SYS_SENDMSG_REQ,
    MATTX_MSG_SYS_SENDMSG_REPLY,
    MATTX_MSG_SYS_RECVMSG_REQ,
    MATTX_MSG_SYS_RECVMSG_REPLY,
    MATTX_MSG_SYS_UNAME_REQ,
    MATTX_MSG_SYS_UNAME_REPLY,
    MATTX_MSG_SYS_PRLIMIT64_REQ,
    MATTX_MSG_SYS_PRLIMIT64_REPLY,
    MATTX_MSG_SYS_PRCTL_REQ,
    MATTX_MSG_SYS_PRCTL_REPLY,
    MATTX_MSG_SYS_FCNTL_REQ,
    MATTX_MSG_SYS_FCNTL_REPLY,
    MATTX_MSG_SYS_IOCTL_REQ,
    MATTX_MSG_SYS_IOCTL_REPLY,
    MATTX_MSG_SYS_PREAD64_REQ,
    MATTX_MSG_SYS_PREAD64_REPLY,
    MATTX_MSG_SYS_STATFS_REQ, 
    MATTX_MSG_SYS_STATFS_REPLY,
    MATTX_MSG_SYS_FSTATFS_REQ, 
    MATTX_MSG_SYS_FSTATFS_REPLY,
    MATTX_MSG_SYS_NEWFSTATAT_REQ, 
    MATTX_MSG_SYS_NEWFSTATAT_REPLY,
    MATTX_MSG_SYS_FACCESSAT2_REQ, 
    MATTX_MSG_SYS_FACCESSAT2_REPLY,
    MATTX_MSG_SYS_READLINK_REQ, 
    MATTX_MSG_SYS_READLINK_REPLY,
    MATTX_MSG_SYS_READLINKAT_REQ, 
    MATTX_MSG_SYS_READLINKAT_REPLY,
    MATTX_MSG_SYS_GETDENTS64_REQ, 
    MATTX_MSG_SYS_GETDENTS64_REPLY,
    MATTX_MSG_SYS_PIPE2_REQ, 
    MATTX_MSG_SYS_PIPE2_REPLY,
    MATTX_MSG_SYS_SHMGET_REQ,
    MATTX_MSG_SYS_SHMGET_REPLY,
    MATTX_MSG_SYS_SHMAT_REQ,
    MATTX_MSG_SYS_SHMAT_REPLY,
    MATTX_MSG_SYS_SHMDT_REQ,
    MATTX_MSG_SYS_SHMDT_REPLY,
    MATTX_MSG_SYS_SHMCTL_REQ,
    MATTX_MSG_SYS_SHMCTL_REPLY,
    MATTX_MSG_DSM_PAGE_FAULT_REQ,
    MATTX_MSG_DSM_PAGE_FAULT_REPLY,    
};

struct mattx_header {
    u32 magic; 
    u32 type;
    u32 length;
    u32 sender_id;
};

struct mattx_load_info {
    u32 cpu_load;    // Now instantaneous runqueue length!
    u32 mem_free_mb;
    u32 affinity;    // Node Speed / Affinity
    u32 accept_guests; // Explicit Cordon Flag!
};

struct mattx_vma_info {
    unsigned long vm_start;
    unsigned long vm_end;
    unsigned long vm_flags;
    u32 shmid; // The unique DSM ID
    u8 is_shm; // Flag indicating this is Shared Memory    
};

struct mattx_cpu_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10;
    uint64_t r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;

};

// NOTE: bin/mattx_stub.c hand-mirrors this struct (and mattx_migration_req
// below) byte-for-byte, since it's a separate userspace program that can't
// #include this kernel header. Keep both in sync -- a layout mismatch here
// silently corrupts the vmas[] flexible array offset the stub parses,
// which fails every page injection ("Failed to inject... res: 0") without
// any obvious error pointing back at the struct mismatch itself.
struct mattx_thread_info {
    uint32_t tid;
    struct mattx_cpu_regs regs;
    uint64_t fsbase;
    uint64_t gsbase;
    uint64_t clear_child_tid; // The Futex Wake Pointer!
    uint64_t set_child_tid;   // The Thread Init Pointer!
    // struct mattx_cpu_regs above is general-purpose registers only (it
    // mirrors struct pt_regs). It does NOT cover the FPU/SSE/AVX register
    // file (XMM/YMM/x87/MXCSR), which lives in a completely separate
    // per-task structure. Virtually every compiler-generated x86_64 binary
    // uses those registers routinely (glibc's memcpy/memset/strlen are
    // SSE2-vectorized by default on this ABI), so without capturing and
    // restoring this too, the resumed thread runs with the freshly-spawned
    // stub's own leftover FPU garbage instead of the real captured state.
    uint32_t fpu_size; // actual bytes valid in fpu_state below
    u8 fpu_state[MATTX_FPU_STATE_MAX]; // raw XSAVE/FXSAVE register image
} __attribute__((packed)); // FORCE EXACT LAYOUT

struct mattx_migration_req {
    u32 orig_pid;
    u32 uid; 
    u32 gid; 
    u32 home_node; 
    
    u32 thread_count;
    struct mattx_thread_info threads[MAX_GANG_THREADS]; 
    
    uint64_t arg_start; 
    uint64_t arg_end;   
    uint64_t start_brk; // The start of the Heap
    uint64_t brk;       // The current end of the Heap
    uint64_t vdso_addr; // The vDSO Transplant Address!

    char comm[16]; 
    char dfsa_dir[256]; 
    u32 fd_count;          
    u32 open_fds[MAX_FDS]; 
    u32 vma_count;
    u8 mattxfs_enabled; 
    struct mattx_vma_info vmas[]; 
} __attribute__((packed)); // <-- FORCE EXACT LAYOUT


struct mattx_page_header {
    uint64_t absolute_addr; // <--: The exact memory address!
    u32 length;
};

struct mattx_process_exit {
    u32 orig_pid;
    int exit_code; 
};

struct mattx_syscall_req {
    u32 orig_pid;
    u32 fd;
    u32 len;
    char data[]; 
};

struct mattx_recall_req {
    u32 orig_pid;
};

struct mattx_sys_open_req {
    u32 orig_pid;
    int flags;
    int mode;
    char filename[256];
};

struct mattx_sys_open_reply {
    u32 orig_pid;
    int remote_fd;
    int error;
};

struct mattx_sys_close_req {
    u32 orig_pid;
    u32 remote_fd;
};

struct mattx_sys_read_req {
    u32 orig_pid;
    u32 fd;
    size_t count;
};

struct mattx_sys_read_reply {
    u32 orig_pid;
    ssize_t bytes_read;
    int error;
    char data[];
};

struct mattx_sys_lseek_req {
    u32 orig_pid;
    u32 fd;
    loff_t offset;
    int whence;
};

struct mattx_sys_lseek_reply {
    u32 orig_pid;
    loff_t result_offset;
    int error;
};

struct mattx_sys_statx_req {
    u32 orig_pid;
    u32 fd;
    u32 mask;
    u32 flags;
};

struct mattx_sys_statx_reply {
    u32 orig_pid;
    int error;
    struct statx statx_buf;
};

struct mattx_sys_dup_req {
    u32 orig_pid;
    u32 old_remote_fd;
    u32 new_local_fd; // -1 if dynamic dup, else specific fd for dup2/dup3
};

struct mattx_sys_dup_reply {
    u32 orig_pid;
    int new_remote_fd;
    int error;
};

struct mattx_sys_fsync_req {
    u32 orig_pid;
    u32 fd;
    loff_t start;
    loff_t end;
    int datasync;
};

struct mattx_sys_fsync_reply {
    u32 orig_pid;
    int error;
};

struct mattx_sys_socket_req {
    u32 orig_pid;
    int domain;
    int type;
    int protocol;
};

struct mattx_sys_socket_reply {
    u32 orig_pid;
    int remote_fd;
    int error;
};

struct mattx_sys_connect_req {
    u32 orig_pid;
    u32 fd;
    struct sockaddr_storage addr;
    int addrlen;
};

struct mattx_sys_connect_reply {
    u32 orig_pid;
    int error;
};

struct mattx_sys_bind_req {
    u32 orig_pid;
    u32 fd;
    struct sockaddr_storage addr;
    int addrlen;
};

struct mattx_sys_bind_reply {
    u32 orig_pid;
    int error;
};

struct mattx_sys_listen_req {
    u32 orig_pid;
    u32 fd;
    int backlog;
};

struct mattx_sys_listen_reply {
    u32 orig_pid;
    int error;
};

// Send/Recv Payloads ---
struct mattx_sys_send_req {
    u32 orig_pid;
    u32 fd;
    int flags;
    size_t len;
    char data[]; 
};

struct mattx_sys_send_reply {
    u32 orig_pid;
    ssize_t bytes_sent;
    int error;
};

struct mattx_sys_recv_req {
    u32 orig_pid;
    u32 fd;
    size_t size;
    int flags;
};

struct mattx_sys_recv_reply {
    u32 orig_pid;
    ssize_t bytes_recv;
    int error;
    char data[];
};

struct mattx_sys_accept_req {
    u32 orig_pid;
    u32 fd; // The listening socket
    int flags;
};

struct mattx_sys_accept_reply {
    u32 orig_pid;
    int remote_fd; // The newly created socket for the connection
    int error;
    struct sockaddr_storage addr; // The IP of the client who connected
    int addrlen;
};

// Poll Structures
struct mattx_pollfd {
    u32 fd;
    short events;
    short revents;
    u64 user_data; // Carry the epoll payload across the network! ---
};

struct mattx_sys_poll_req {
    u32 orig_pid;
    int nfds;
    int timeout;
    struct mattx_pollfd fds[16]; // Max 16 FDs for this prototype
};

struct mattx_sys_poll_reply {
    u32 orig_pid;
    int nfds;
    int error;
    int retval; // The number of FDs with events
    struct mattx_pollfd fds[16];
};

struct mattx_sys_epoll_create_req {
    u32 orig_pid;
    int flags; // For epoll_create1
};

struct mattx_sys_epoll_create_reply {
    u32 orig_pid;
    int remote_fd;
    int error;
};

struct mattx_sys_epoll_ctl_req {
    u32 orig_pid;
    int epfd;
    int op;
    int fd;
    struct epoll_event event; // We pass the actual struct, not a pointer!
};

struct mattx_sys_epoll_wait_req {
    u32 orig_pid;
    int epfd;
    int maxevents;
    int timeout;
};

struct mattx_sys_epoll_wait_reply {
    u32 orig_pid;
    int retval; // Number of events, or error
    struct epoll_event events[]; // Flexible array for the returned events!
};

struct mattx_sys_getsockname_req { 
    u32 orig_pid; int fd; 
};

struct mattx_sys_getsockname_reply { 
    u32 orig_pid; 
    int error; 
    struct sockaddr_storage addr; 
    int addrlen; 
};

struct mattx_sys_getpeername_req { 
    u32 orig_pid; int fd; 
};

struct mattx_sys_getpeername_reply { 
    u32 orig_pid; 
    int error; 
    struct sockaddr_storage addr; 
    int addrlen; 
};

struct mattx_sys_setsockopt_req { 
    u32 orig_pid; 
    int fd; 
    int level; 
    int optname; 
    int optlen; 
    char optval[]; 
};

struct mattx_sys_setsockopt_reply { 
    u32 orig_pid; 
    int error; 
};

struct mattx_sys_getsockopt_req { 
    u32 orig_pid; 
    int fd; 
    int level; 
    int optname; 
    int optlen; 
};

struct mattx_sys_getsockopt_reply { 
    u32 orig_pid; 
    int error; 
    int optlen; 
    char optval[]; 
};

struct mattx_sys_sendmsg_req {
    u32 orig_pid;
    int fd;
    int flags;
    int addrlen;
    size_t datalen;
    struct sockaddr_storage addr;
    char data[]; // Flexible array for the flattened data!
};

struct mattx_sys_sendmsg_reply {
    u32 orig_pid;
    ssize_t bytes_sent;
    int error;
};

struct mattx_sys_recvmsg_req {
    u32 orig_pid;
    int fd;
    int flags;
    int addrlen;
    size_t datalen;
};

struct mattx_sys_recvmsg_reply {
    u32 orig_pid;
    ssize_t bytes_recv;
    int error;
    int addrlen;
    struct sockaddr_storage addr;
    char data[]; // Flexible array for the flattened data!
};

struct mattx_sys_uname_req { 
    u32 orig_pid; 
};
struct mattx_sys_uname_reply { 
    u32 orig_pid; 
    int error; 
    struct new_utsname uts; 
};

struct mattx_sys_prlimit64_req { 
    u32 orig_pid; pid_t pid; int resource; 
    u8 has_new; u8 has_old; struct rlimit new_rlim; 
};
struct mattx_sys_prlimit64_reply { 
    u32 orig_pid; 
    int error; 
    struct rlimit old_rlim; 
};

struct mattx_sys_prctl_req { 
    u32 orig_pid; 
    int option; 
    unsigned long arg2, arg3, arg4, arg5; 
};

struct mattx_sys_prctl_reply { 
    u32 orig_pid; 
    int error; 
};


struct mattx_sys_fcntl_req { 
    u32 orig_pid; int fd; 
    int cmd; unsigned long arg; 
    u8 has_ptr; char data[256]; 
};

struct mattx_sys_fcntl_reply { 
    u32 orig_pid; 
    int error; 
    char data[256]; 
};

struct mattx_sys_ioctl_req { 
    u32 orig_pid; 
    int fd; 
    unsigned int cmd; 
    unsigned long arg; 
    u8 has_ptr; 
    char data[256]; 
};

struct mattx_sys_ioctl_reply { 
    u32 orig_pid; 
    int error; 
    char data[256]; 
};

struct mattx_sys_pread64_req { 
    u32 orig_pid; 
    int fd; 
    size_t count; 
    loff_t pos; 
};
// Note: We will reuse mattx_sys_read_reply for pread64's reply!

struct mattx_sys_statfs_req { 
    u32 orig_pid; 
    char path[256]; 
};

struct mattx_sys_statfs_reply { 
    u32 orig_pid; 
    int error; 
    struct statfs buf; 
};

struct mattx_sys_fstatfs_req { 
    u32 orig_pid; 
    int fd; 
};

struct mattx_sys_fstatfs_reply { 
    u32 orig_pid; 
    int error; 
    struct statfs buf; 
};

struct mattx_sys_newfstatat_req { 
    u32 orig_pid; 
    int dfd; 
    int flags; 
    char path[256]; 
};
struct mattx_sys_newfstatat_reply { 
    u32 orig_pid; 
    int error; 
    struct stat buf; 
};

struct mattx_sys_faccessat2_req { 
    u32 orig_pid; 
    int dfd; 
    int mode; 
    int flags; 
    char path[256]; 
};
struct mattx_sys_faccessat2_reply { 
    u32 orig_pid; 
    int error; 
};

struct mattx_sys_readlink_req { 
    u32 orig_pid; 
    size_t bufsiz; 
    char path[256]; 
};
struct mattx_sys_readlink_reply { 
    u32 orig_pid; 
    int error; 
    char data[]; 
};

struct mattx_sys_readlinkat_req { 
    u32 orig_pid; 
    int dfd; 
    size_t bufsiz; 
    char path[256]; 
};
struct mattx_sys_readlinkat_reply { 
    u32 orig_pid; 
    int error; 
    char data[]; 
};

struct mattx_sys_getdents64_req { 
    u32 orig_pid; 
    int fd; u32 count; 
};

struct mattx_sys_getdents64_reply { 
    u32 orig_pid; 
    int error; 
    char data[]; 
};

struct mattx_sys_pipe2_req { 
    u32 orig_pid; 
    int flags; 
};

struct mattx_sys_pipe2_reply { 
    u32 orig_pid; 
    int error; 
    int fd0; 
    int fd1; 
};







struct mattx_fake_fd_info {
    int home_node;
    u32 orig_pid;
    u32 remote_fd;
};

struct mattx_rpc_work {
    struct work_struct work;
    pid_t local_pid;
    u32 orig_pid;
    int home_node;
    
    // For OPEN
    char filename[256];
    int flags;
    int mode;

    // For STATX
    bool is_statx;
    int remote_fd;
    u32 mask;
    struct statx __user *statx_buffer;

    // For DUP
    bool is_dup;
    int new_local_fd; // -1 if just dup()
    
    // For UNLINK
    bool is_unlink; // For the Kprobe worker

    // For LSEEK
    bool is_lseek;
    loff_t offset;
    int whence;

    // For FSYNC
    bool is_fsync;
    loff_t fsync_start;
    loff_t fsync_end;
    int datasync;

    // For SOCKET
    bool is_socket;
    int domain;
    int type;
    int protocol;

    // For CONNECT / BIND
    bool is_connect;
    bool is_bind;
    struct sockaddr_storage addr;
    int addrlen;

    // For LISTEN
    bool is_listen;
    int backlog;

    // --- Send/Recv Workqueue Fields ---
    bool is_sendto;
    bool is_recvfrom;
    bool is_read;   // For the IPC Wormhole
    bool is_write;  // For the IPC Wormhole
    void __user *buff;
    size_t len;
    size_t size;    

    // For ACCEPT
    bool is_accept;

    // For POLL
    bool is_poll;
    int nfds;
    int timeout;
    struct mattx_pollfd poll_fds[16];
    void __user *poll_ufds; // Pointer to the user-space array    

    // For SELECT / PSELECT6 ---
    bool is_select;
    bool is_pselect6;
    int select_nfds;
    fd_set in_fds;
    fd_set out_fds;
    fd_set ex_fds;
    int timeout_ms;
    void __user *select_readfds_ptr;
    void __user *select_writefds_ptr;
    void __user *select_exceptfds_ptr;

    // For EPOLL_CREATE ---
    bool is_epoll_create;
    int epoll_flags;

    // For EPOLL_CTL ---
    bool is_epoll_ctl;
    int epoll_op;
    struct epoll_event epoll_ev;

    // For EPOLL_WAIT ---
    bool is_epoll_wait;
    int epoll_maxevents;
    void __user *epoll_events_ptr;

    // For SOCKOPTS & NAMES ---
    bool is_getsockname;
    bool is_getpeername;
    bool is_setsockopt;
    bool is_getsockopt;
    int sock_level;
    int sock_optname;
    int sock_optlen;

    // For SENDMSG & RECVMSG ---
    bool is_sendmsg;
    bool is_recvmsg;
    struct msghdr __user *msg_ptr;

    // For UNAME
    bool is_uname;

    // For PRLIMIT64
    bool is_prlimit64;
    pid_t prlimit_pid;
    int prlimit_resource;
    bool prlimit_has_new;
    bool prlimit_has_old;
    void __user *prlimit_new_rlim_ptr;
    void __user *prlimit_old_rlim_ptr;

    // For PRCTL
    bool is_prctl;
    int prctl_option;
    unsigned long prctl_arg2, prctl_arg3, prctl_arg4, prctl_arg5;

    // For FCNTL & IOCTL
    bool is_fcntl;
    bool is_ioctl;
    int fcntl_cmd;
    unsigned int ioctl_cmd;
    unsigned long ioctl_arg; // Reused for fcntl_arg
    bool ioctl_has_ptr;      // Reused for fcntl
    char ioctl_data[256];    // Reused for fcntl

    // For PREAD64
    bool is_pread64;
    loff_t pread64_pos;

    // For META FETCHERS (Microstep 2.2)
    bool is_statfs, is_fstatfs, is_newfstatat, is_faccessat2, is_readlink, is_readlinkat;
    int meta_dfd, meta_flags, meta_mode;
    size_t meta_bufsiz;
    char meta_path[256];
    void __user *meta_buf_ptr;

    // For GETDENTS64
    bool is_getdents64;
    int getdents64_fd;
    u32 getdents64_count;
    void __user *getdents64_dirp;

    // For PIPE2
    bool is_pipe2;
    int pipe2_flags;
    void __user *pipe2_pipefd;

    // For DSM Control Plane ---
    bool is_shmget;
    bool is_shmctl;
    bool is_shmdt;
    bool is_shmat; // <-- NEW
    int shm_key;
    size_t shm_size;
    int shm_flg;
    int shm_id;
    int shm_cmd;
    unsigned long shm_addr;    
};

struct mattx_link {
    int node_id;
    u32 ip_addr; 
    struct socket *sock;
    struct sock *sk;
    struct task_struct *receiver_thread;
};


struct mattx_dsm_mapping {
    unsigned long base_addr;
    u32 shmid;
    unsigned long size;
};


struct mattx_guest_info {
    pid_t local_pid;
    u32 orig_pid;
    int home_node;
    wait_queue_head_t *rpc_wq;
    int rpc_remote_fd;
    bool rpc_done;
    void *rpc_read_buf;
    ssize_t rpc_read_bytes;
    loff_t rpc_lseek_res;
    struct statx *rpc_statx_buf;
    int rpc_fsync_res;
    bool is_migrating; // The Migration Lock!

    // The TID Translation Map ---
    int thread_count;
    u32 orig_tids[MAX_GANG_THREADS];
    pid_t local_tids[MAX_GANG_THREADS];    

    // The DSM Translation Map ---
    int dsm_count;
    struct mattx_dsm_mapping dsm_map[MAX_DSM_SEGMENTS];    
};

struct mattx_export_info {
    pid_t orig_pid;
    int target_node;
    struct file *remote_files[MAX_FDS]; 
    bool abort_rpc; // The Kworker Kill-Switch! ---
    bool is_growing_gang; // The Gang Grower Flag!
};

struct mattx_vfs_getattr_req {
    u64 req_id;
    char path[256];
};

struct mattx_vfs_getattr_reply {
    u64 req_id;
    int error;
    u32 mode;
    u64 size;
    u64 blocks;
    u32 blksize;
    u32 uid;
    u32 gid;
    u32 nlink;
};

struct mattx_dirent {
    u64 ino;
    u64 offset; // Track the offset of each specific file!
    u8 type;
    char name[64]; 
};

struct mattx_vfs_readdir_req {
    u64 req_id;
    u64 offset;
    char path[256];
};

struct mattx_vfs_readdir_reply {
    u64 req_id;
    int error;
    u64 new_offset;
    u32 entry_count;
    struct mattx_dirent entries[20]; // block of 20 dir entries
};

struct mattx_vfs_open_req {
    u64 req_id;
    int flags;
    int mode;
    u32 uid; // Teleport the user's ID!
    u32 gid; // Teleport the user's group ID!    
    char path[256];
};

struct mattx_vfs_open_reply {
    u64 req_id;
    int remote_fd;
    int error;
};

struct mattx_vfs_read_req {
    u64 req_id;
    int remote_fd;
    size_t count;
    loff_t pos;
};

struct mattx_vfs_read_reply {
    u64 req_id;
    ssize_t bytes_read;
    int error;
    char data[]; 
};

struct mattx_vfs_write_req {
    u64 req_id;
    int remote_fd;
    size_t count;
    loff_t pos;
    char data[]; 
};

struct mattx_vfs_write_reply {
    u64 req_id;
    ssize_t bytes_written;
    int error;
};

struct mattx_vfs_close_req {
    int remote_fd;
};

struct mattx_vfs_lseek_req {
    u64 req_id;
    int remote_fd;
    loff_t offset;
    int whence;
};

struct mattx_vfs_lseek_reply {
    u64 req_id;
    loff_t offset;
    int error;
};

struct mattx_vfs_fsync_req {
    u64 req_id;
    int remote_fd;
    loff_t start;
    loff_t end;
    int datasync;
};

struct mattx_vfs_fsync_reply {
    u64 req_id;
    int error;
};

// Notice we include BOTH req_id (for MattXFS) and orig_pid (for Kprobes)!
struct mattx_sys_unlink_req {
    u64 req_id;
    u32 orig_pid;
    u32 uid; // Teleport the user's ID!
    u32 gid; // Teleport the user's group ID!    
    char path[256];
};

struct mattx_sys_unlink_reply {
    u64 req_id;
    u32 orig_pid;
    int error;
};




// --- DSM Payloads ---
struct mattx_sys_shmget_req { u32 orig_pid; int key; size_t size; int shmflg; };
struct mattx_sys_shmget_reply { u32 orig_pid; int shmid; int error; };

struct mattx_sys_shmat_req { u32 orig_pid; int shmid; unsigned long shmaddr; int shmflg; };
struct mattx_sys_shmat_reply { u32 orig_pid; unsigned long ret_addr; size_t size; int error; };

struct mattx_sys_shmdt_req { u32 orig_pid; unsigned long shmaddr; };
struct mattx_sys_shmdt_reply { u32 orig_pid; int error; };

struct mattx_sys_shmctl_req { u32 orig_pid; int shmid; int cmd; };
struct mattx_sys_shmctl_reply { 
    u32 orig_pid; 
    int error; 
    char data[128]; // Large enough to hold struct shmid_ds
};

// --- DSM Page Fault Payloads ---
struct mattx_dsm_page_fault_req {
    u64 req_id;
    u32 orig_pid;
    u32 shmid;
    unsigned long offset;
};

struct mattx_dsm_page_fault_reply {
    u64 req_id;
    int error;
    char data[4096]; // Exactly one PAGE_SIZE!
};



// This defines the standard signature for all message handlers
typedef void (*mattx_msg_handler_fn)(struct mattx_link *link, struct mattx_header *hdr, void *payload);

// Function to let other files register their handlers
void mattx_register_handler(u32 type, mattx_msg_handler_fn handler);

extern struct mattx_load_info cluster_load_table[MAX_NODES];
extern struct mattx_link *cluster_map[MAX_NODES];
extern struct mattx_migration_req *pending_migration;
extern struct genl_family mattx_genl_family;
extern int pending_source_node;
extern struct task_struct *hijacked_stub_task;

extern struct mattx_guest_info guest_registry[MAX_GUESTS];
extern int guest_count;
extern spinlock_t guest_lock;

// guest_registry is keyed by the GUEST's thread-group leader PID (see
// add_guest_process()), but a wormhole-hooked syscall (open/read/write/
// lseek/fsync/poll/...) can be issued by ANY thread in a multi-threaded
// Gang, not just the leader -- current->pid for a non-leader thread is
// its own unique TID, not the shared TGID. Comparing that raw per-thread
// pid directly against guest_registry[i].local_pid never matches for a
// non-leader thread, so its RPC-wait loop immediately concludes "Surrogate
// disappeared" and gives up -- without ever sending the SIGCONT to un-stop
// the very thread the hook itself had just SIGSTOP'd, permanently wedging
// the whole Gang (see docs/BUGS.md BUG-007). Use this everywhere a
// thread-specific pid needs to be checked against a (leader-keyed)
// guest_registry entry.
static inline bool mattx_pid_shares_tgid_with_guest(pid_t local_pid, pid_t guest_local_pid) {
    struct task_struct *t;
    bool match;

    if (local_pid == guest_local_pid) return true; // fast path: it IS the leader

    rcu_read_lock();
    t = pid_task(find_vpid(local_pid), PIDTYPE_PID);
    match = t && (t->tgid == guest_local_pid);
    rcu_read_unlock();
    return match;
}

extern struct mattx_export_info export_registry[MAX_GUESTS];
extern int export_count;
extern spinlock_t export_lock;

extern bool balancer_enabled;
extern u32 my_node_id; 
extern u32 my_ip_addr; 
extern bool config_mattxfs_enabled; // The MattxFs Feature Flag
extern char config_dfsa_dir[256]; // and the DFSA exclude
extern bool config_debug_mode; // NEW: The Debug Toggle

extern char config_migration_excludes[512]; // The Blacklist! ---
extern char config_migration_includes[512]; // The VIP Whitelist! ---
extern u32 config_node_affinity;
u32 mattx_calc_local_load(void);

// --- THE GHOST RESOLVERS FOR EPOLL ---
typedef int (*mattx_task_work_add_fn)(struct task_struct *task, struct callback_head *twork, enum task_work_notify_mode mode);
extern mattx_task_work_add_fn real_task_work_add;

typedef long (*mattx_sys_epoll_create1_fn)(const struct pt_regs *regs);
extern mattx_sys_epoll_create1_fn real_sys_epoll_create1;

typedef long (*mattx_sys_epoll_ctl_fn)(const struct pt_regs *regs);
extern mattx_sys_epoll_ctl_fn real_sys_epoll_ctl;

typedef long (*mattx_sys_epoll_wait_fn)(const struct pt_regs *regs);
extern mattx_sys_epoll_wait_fn real_sys_epoll_wait;


// --- THE NETWORK GHOST RESOLVERS ---
typedef long (*mattx_sys_bind_fn)(const struct pt_regs *regs);
extern mattx_sys_bind_fn real_sys_bind;

typedef long (*mattx_sys_connect_fn)(const struct pt_regs *regs);
extern mattx_sys_connect_fn real_sys_connect;

typedef long (*mattx_sys_listen_fn)(const struct pt_regs *regs);
extern mattx_sys_listen_fn real_sys_listen;

typedef long (*mattx_sys_sendto_fn)(const struct pt_regs *regs);
extern mattx_sys_sendto_fn real_sys_sendto;

typedef long (*mattx_sys_recvfrom_fn)(const struct pt_regs *regs);
extern mattx_sys_recvfrom_fn real_sys_recvfrom;

typedef long (*mattx_sys_socket_fn)(const struct pt_regs *regs);
extern mattx_sys_socket_fn real_sys_socket;

typedef long (*mattx_sys_accept4_fn)(const struct pt_regs *regs);
extern mattx_sys_accept4_fn real_sys_accept4;

typedef long (*mattx_sys_getsockname_fn)(const struct pt_regs *regs);
extern mattx_sys_getsockname_fn real_sys_getsockname;

typedef long (*mattx_sys_getpeername_fn)(const struct pt_regs *regs);
extern mattx_sys_getpeername_fn real_sys_getpeername;

typedef long (*mattx_sys_setsockopt_fn)(const struct pt_regs *regs);
extern mattx_sys_setsockopt_fn real_sys_setsockopt;

typedef long (*mattx_sys_getsockopt_fn)(const struct pt_regs *regs);
extern mattx_sys_getsockopt_fn real_sys_getsockopt;

typedef long (*mattx_sys_sendmsg_fn)(const struct pt_regs *regs);
extern mattx_sys_sendmsg_fn real_sys_sendmsg;

typedef long (*mattx_sys_recvmsg_fn)(const struct pt_regs *regs);
extern mattx_sys_recvmsg_fn real_sys_recvmsg;



// --- THE FILE-IO GHOST RESOLVERS ---
typedef long (*mattx_sys_dup_fn)(const struct pt_regs *regs);
extern mattx_sys_dup_fn real_sys_dup;

typedef long (*mattx_sys_dup2_fn)(const struct pt_regs *regs);
extern mattx_sys_dup2_fn real_sys_dup2;

typedef long (*mattx_sys_close_fn)(const struct pt_regs *regs);
extern mattx_sys_close_fn real_sys_close;

typedef long (*mattx_sys_prlimit64_fn)(const struct pt_regs *regs);
extern mattx_sys_prlimit64_fn real_sys_prlimit64;

typedef long (*mattx_sys_prctl_fn)(const struct pt_regs *regs);
extern mattx_sys_prctl_fn real_sys_prctl;

typedef long (*mattx_sys_fcntl_fn)(const struct pt_regs *regs);
extern mattx_sys_fcntl_fn real_sys_fcntl;

typedef long (*mattx_sys_ioctl_fn)(const struct pt_regs *regs);
extern mattx_sys_ioctl_fn real_sys_ioctl;

typedef long (*mattx_sys_statfs_fn)(const struct pt_regs *regs);
extern mattx_sys_statfs_fn real_sys_statfs;

typedef long (*mattx_sys_fstatfs_fn)(const struct pt_regs *regs);
extern mattx_sys_fstatfs_fn real_sys_fstatfs;

typedef long (*mattx_sys_newfstatat_fn)(const struct pt_regs *regs);
extern mattx_sys_newfstatat_fn real_sys_newfstatat;

typedef long (*mattx_sys_faccessat2_fn)(const struct pt_regs *regs);
extern mattx_sys_faccessat2_fn real_sys_faccessat2;

typedef long (*mattx_sys_readlink_fn)(const struct pt_regs *regs);
extern mattx_sys_readlink_fn real_sys_readlink;

typedef long (*mattx_sys_readlinkat_fn)(const struct pt_regs *regs);
extern mattx_sys_readlinkat_fn real_sys_readlinkat;

typedef long (*mattx_sys_getdents64_fn)(const struct pt_regs *regs);
extern mattx_sys_getdents64_fn real_sys_getdents64;

typedef long (*mattx_sys_pipe2_fn)(const struct pt_regs *regs);
extern mattx_sys_pipe2_fn real_sys_pipe2;

// --- THE DSM GHOST RESOLVERS ---
typedef long (*mattx_sys_shmget_fn)(const struct pt_regs *regs);
extern mattx_sys_shmget_fn real_sys_shmget;

typedef long (*mattx_sys_shmctl_fn)(const struct pt_regs *regs);
extern mattx_sys_shmctl_fn real_sys_shmctl;

typedef long (*mattx_sys_shmdt_fn)(const struct pt_regs *regs);
extern mattx_sys_shmdt_fn real_sys_shmdt;

typedef long (*mattx_sys_shmat_fn)(const struct pt_regs *regs);
extern mattx_sys_shmat_fn real_sys_shmat;

// The DSM Tripwire Struct ---
extern const struct vm_operations_struct mattx_dsm_vm_ops;





// --- THE THREAD GHOST EXORCIST RESOLVER ---
typedef long (*mattx_sys_exit_fn)(const struct pt_regs *regs);
extern mattx_sys_exit_fn real_sys_exit;

// --- THE GANG GROWER RESOLVER ---
typedef long (*mattx_sys_clone_fn)(const struct pt_regs *regs);
extern mattx_sys_clone_fn real_sys_clone;

// --- THE vDSO TRANSPLANT RESOLVER ---
typedef long (*mattx_sys_mremap_fn)(const struct pt_regs *regs);
extern mattx_sys_mremap_fn real_sys_mremap;

// --- THE TLS HARDWARE SYNC RESOLVERS ---
typedef void (*mattx_x86_fsbase_write_task_fn)(struct task_struct *task, unsigned long fsbase);
typedef void (*mattx_x86_gsbase_write_task_fn)(struct task_struct *task, unsigned long gsbase);
extern mattx_x86_fsbase_write_task_fn real_x86_fsbase_write_task;
extern mattx_x86_gsbase_write_task_fn real_x86_gsbase_write_task;



// The Extreme Debugging Macro ---
// This replaces printk(KERN_INFO...). It checks the flag before printing!
#define mattx_dbg(fmt, ...) \
    do { if (config_debug_mode) printk(KERN_INFO "MattX:" fmt, ##__VA_ARGS__); } while (0)

// Configuration Toggles
extern bool config_migrate_file_io;
extern bool config_migrate_network_io;
extern bool config_mpi_support;
extern bool config_accept_guests;
extern bool config_hpc_local_libs; // The HPC Fast-Path Toggle!

// Expose the balancer thread so we can borrow its host root filesystem!
extern struct task_struct *balancer_thread;

// The HPC Fast-Path Helper
bool is_hpc_local_lib(const char *path);

int mattx_expel_guest(pid_t local_pid);

int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len);
int mattx_comm_send_ctrl(struct mattx_link *link, u32 type, void *data, u32 len);
struct mattx_link* mattx_comm_connect(__be32 ip_addr, int node_id);
void mattx_comm_disconnect(int node_id);
int mattx_listener_loop(void *data);
int mattx_balancer_loop(void *data);
void mattx_capture_and_send_state(struct task_struct *task, int target_node);
void mattx_capture_and_return_state(struct task_struct *task, u32 orig_pid, int target_node); 
void mattx_send_vma_data(void); 
void mattx_freeze_task_safely(struct task_struct *task); // Expose the Freezer!

bool is_guest_process(pid_t pid);
bool is_rpc_pending(pid_t pid); // Check if a Wormhole is open!
bool check_rpc_done(pid_t pid);

void add_guest_process(pid_t local_pid, u32 orig_pid, int home_node);
void remove_guest_process(int index);

void add_export_process(pid_t orig_pid, int target_node);
void remove_export_process(int index);
int get_export_target(pid_t orig_pid); 
void mattx_trigger_recall(pid_t orig_pid); 

int mattx_proc_init(void);
void mattx_proc_exit(void);

int mattx_hooks_init(void);
void mattx_hooks_exit(void);

void mattx_sched_init_handlers(void);
void mattx_import_init_handlers(void);
void mattx_migr_init_handlers(void);
void mattx_guest_init_handlers(void);
void mattx_fileio_init_handlers(void);
void mattx_fileio_exit(void); // The Cleanup function!

extern const struct file_operations mattx_fops; 
extern const struct inode_operations mattx_iops;

// API for MattXFS ---
int mattx_get_active_nodes(int *node_array, int max_nodes);
int mattx_rpc_vfs_getattr(int node_id, const char *path, struct kstat *stat_out);
int mattx_rpc_vfs_readdir(int node_id, const char *path, u64 *offset, struct mattx_dirent *entries, u32 *out_count);
int mattx_rpc_vfs_open(int node_id, const char *path, int flags, int mode, int *remote_fd);
ssize_t mattx_rpc_vfs_read(int node_id, int remote_fd, void *buf, size_t count, loff_t *pos);
ssize_t mattx_rpc_vfs_write(int node_id, int remote_fd, const void *buf, size_t count, loff_t *pos);
void mattx_rpc_vfs_close(int node_id, int remote_fd);
loff_t mattx_rpc_vfs_llseek(int node_id, int remote_fd, loff_t offset, int whence);
int mattx_rpc_vfs_fsync(int node_id, int remote_fd, loff_t start, loff_t end, int datasync);
int mattx_rpc_vfs_unlink(int node_id, const char *path);

#endif // MATTX_H

