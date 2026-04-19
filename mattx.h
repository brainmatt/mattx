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
#include <linux/mmu_context.h>   
#include <linux/kprobes.h>       
#include <linux/workqueue.h>     
#include <linux/bitops.h>        // NEW: For manipulating the FD bitmap

#define MATTX_PORT 7226
#define MAX_NODES 1024 
#define BALANCER_INTERVAL_MS 2000 

#define FIXED_LOAD_1_0 2048
#define FIXED_LOAD_0_2 409
#define MAX_VMAS 256 
#define MAX_GUESTS 1024 
#define MAX_FDS 64 

#define MATTX_MAGIC 0x4D415454 
#define MATTX_MAX_PAYLOAD (10 * 1024 * 1024) 

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
};

struct mattx_header {
    u32 magic; 
    u32 type;
    u32 length;
    u32 sender_id;
};

struct mattx_load_info {
    u32 cpu_load;    
    u32 mem_free_mb; 
};

struct mattx_vma_info {
    unsigned long vm_start;
    unsigned long vm_end;
    unsigned long vm_flags;
};

struct mattx_cpu_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10;
    uint64_t r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
};

struct mattx_migration_req {
    u32 orig_pid;
    u32 uid; 
    u32 gid; 
    u32 pad; 
    struct mattx_cpu_regs regs; 
    uint64_t fsbase; 
    uint64_t gsbase; 
    uint64_t arg_start; 
    uint64_t arg_end;   
    char comm[16]; 
    u32 fd_count;          
    u32 open_fds[MAX_FDS]; 
    u32 vma_count;
    u32 pad2;
    struct mattx_vma_info vmas[]; 
};

struct mattx_page_header {
    u32 vma_index;
    u32 offset;
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

struct mattx_rpc_work {
    struct work_struct work;
    pid_t local_pid;
    u32 orig_pid;
    int home_node;
    char filename[256];
};

struct mattx_link {
    int node_id;
    u32 ip_addr; 
    struct socket *sock;
    struct sock *sk;
    struct task_struct *receiver_thread;
};

struct mattx_guest_info {
    pid_t local_pid;
    u32 orig_pid;
    int home_node;
    wait_queue_head_t *rpc_wq;
    int rpc_remote_fd;
    bool rpc_done;
};

struct mattx_export_info {
    pid_t orig_pid;
    int target_node;
    struct file *remote_files[MAX_FDS]; // NEW: Store files opened on behalf of the Surrogate
};

extern struct mattx_load_info cluster_load_table[MAX_NODES];
extern struct mattx_link *cluster_map[MAX_NODES];
extern struct mattx_migration_req *pending_migration;
extern struct genl_family mattx_genl_family;
extern int pending_source_node;
extern struct task_struct *hijacked_stub_task;

extern struct mattx_guest_info guest_registry[MAX_GUESTS];
extern int guest_count;
extern spinlock_t guest_lock;

extern struct mattx_export_info export_registry[MAX_GUESTS];
extern int export_count;
extern spinlock_t export_lock;

extern bool balancer_enabled;
extern u32 my_node_id; 
extern u32 my_ip_addr; 

int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len);
struct mattx_link* mattx_comm_connect(__be32 ip_addr, int node_id);
void mattx_comm_disconnect(int node_id);
int mattx_listener_loop(void *data);
int mattx_balancer_loop(void *data);
void mattx_capture_and_send_state(struct task_struct *task, int target_node);
void mattx_capture_and_return_state(struct task_struct *task, u32 orig_pid, int target_node); 
void mattx_send_vma_data(void); 

bool is_guest_process(pid_t pid);
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

static extern const struct file_operations mattx_fops; // NEW: Expose fops for the Workqueue

#endif // MATTX_H

