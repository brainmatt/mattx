#include "mattx.h"
#include <linux/wait.h> // NEW: For wait_event_interruptible

static struct kretprobe openat_kprobe;

// Helper to safely check if the RPC is done while we are sleeping
static bool check_rpc_done(pid_t pid) {
    bool done = false;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            done = guest_registry[i].rpc_done;
            break;
        }
    }
    spin_unlock(&guest_lock);
    return done;
}

static int entry_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;

    if (!is_guest_process(my_pid)) {
        return 0; 
    }

    *((const char __user **)ri->data) = (const char __user *)regs->si;
    return 0;
}

static int ret_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    const char __user *filename_ptr;
    char filename[256] = {0};
    int home_node = -1;
    u32 orig_pid = 0;
    int i;
    
    // NEW: Create a wait queue directly on the kernel stack!
    DECLARE_WAIT_QUEUE_HEAD_ONSTACK(rpc_wq);

    if (!is_guest_process(my_pid)) return 0;

    filename_ptr = *((const char __user **)ri->data);

    if (strncpy_from_user(filename, filename_ptr, sizeof(filename) - 1) > 0) {
        
        // 1. Find our Home Node and attach the Wait Queue
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == my_pid) {
                home_node = guest_registry[i].home_node;
                orig_pid = guest_registry[i].orig_pid;
                
                // Attach our stack's wait queue to the registry so the receiver thread can wake us up
                guest_registry[i].rpc_wq = &rpc_wq;
                guest_registry[i].rpc_done = false;
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (home_node != -1 && cluster_map[home_node]) {
            struct mattx_sys_open_req req;
            memset(&req, 0, sizeof(req));
            req.orig_pid = orig_pid;
            strncpy(req.filename, filename, sizeof(req.filename) - 1);

            printk(KERN_INFO "MattX:[HOOK] Intercepted open('%s'). Pausing Surrogate %d...\n", filename, my_pid);
            printk(KERN_INFO "MattX:[HOOK] Sending OPEN_REQ to Home Node %d...\n", home_node);
            
            // 2. Send the request to Node 1
            mattx_comm_send(cluster_map[home_node], MATTX_MSG_SYS_OPEN_REQ, &req, sizeof(req));

            // 3. GO TO SLEEP! The kernel will pause this thread until check_rpc_done returns true.
            wait_event_interruptible(rpc_wq, check_rpc_done(my_pid));

            // 4. We woke up! Node 1 must have replied. Let's get the result.
            int remote_fd = -1;
            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == my_pid) {
                    remote_fd = guest_registry[i].rpc_remote_fd;
                    guest_registry[i].rpc_wq = NULL; // Detach the wait queue
                    break;
                }
            }
            spin_unlock(&guest_lock);

            printk(KERN_INFO "MattX:[HOOK] Surrogate %d woke up! Node 1 assigned Remote FD: %d\n", my_pid, remote_fd);
            
            // TODO Phase 12.4: Hijack the return value (regs->ax) and inject the Fake FD!
        }
    }

    return 0;
}

int mattx_hooks_init(void) {
    int ret;
    memset(&openat_kprobe, 0, sizeof(openat_kprobe));
    openat_kprobe.kp.symbol_name = "do_sys_openat2";
    openat_kprobe.entry_handler = entry_handler_openat;
    openat_kprobe.handler = ret_handler_openat;
    openat_kprobe.data_size = sizeof(const char __user *); 
    openat_kprobe.maxactive = 64; 

    ret = register_kretprobe(&openat_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed, returned %d\n", ret);
        return ret;
    }
    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) registered successfully.\n");
    return 0;
}

void mattx_hooks_exit(void) {
    unregister_kretprobe(&openat_kprobe);
    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) unregistered.\n");
}

