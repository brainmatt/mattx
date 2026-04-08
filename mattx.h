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
#include <linux/sched/loadavg.h> 
#include <linux/mm.h>            
#include <linux/sched/signal.h>  
#include <linux/sched/task.h>    
#include <linux/rcupdate.h>      
#include <linux/mmap_lock.h>     
#include <asm/ptrace.h>          
#include <linux/umh.h>           
#include <linux/highmem.h>       

#define MATTX_PORT 7226
#define MAX_NODES 1024 
#define BALANCER_INTERVAL_MS 2000 

#define FIXED_LOAD_1_0 2048
#define FIXED_LOAD_0_2 409
#define MAX_VMAS 256 

// NEW: Protocol robustness
#define MATTX_MAGIC 0x4D415454 // "MATT"
#define MATTX_MAX_PAYLOAD (10 * 1024 * 1024) // 10 MB max per packet

enum mattx_msg_type {
    MATTX_MSG_HEARTBEAT = 1,
    MATTX_MSG_LOAD_UPDATE,
    MATTX_MSG_MIGRATE_REQ,
    MATTX_MSG_READY_FOR_DATA, 
    MATTX_MSG_PAGE_TRANSFER, 
    MATTX_MSG_MIGRATE_DONE,   
    MATTX_MSG_SYSCALL_FWD,
};

struct mattx_header {
    u32 magic; // NEW: Magic signature
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

struct mattx_migration_req {
    u32 orig_pid;
    unsigned long rip;
    unsigned long rsp;
    u32 vma_count;
    struct mattx_vma_info vmas[]; 
};

struct mattx_page_header {
    u32 vma_index;
    u32 offset;
    u32 length;
};

struct mattx_link {
    int node_id;
    struct socket *sock;
    struct sock *sk;
    struct task_struct *receiver_thread;
};

extern struct mattx_load_info cluster_load_table[MAX_NODES];
extern struct mattx_link *cluster_map[MAX_NODES];
extern struct mattx_migration_req *pending_migration;
extern struct genl_family mattx_genl_family;
extern int pending_source_node;
extern struct task_struct *hijacked_stub_task;

int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len);
struct mattx_link* mattx_comm_connect(__be32 ip_addr, int node_id);
void mattx_comm_disconnect(int node_id);
int mattx_listener_loop(void *data);
int mattx_balancer_loop(void *data);
void mattx_capture_and_send_state(struct task_struct *task, int target_node);
void mattx_send_vma_data(void); 

#endif // MATTX_H

