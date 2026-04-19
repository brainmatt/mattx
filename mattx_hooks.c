#include "mattx.h"
#include <linux/wait.h> 

static struct kretprobe openat_kprobe;

// --- NEW: The Workqueue Handler (Runs in Process Context!) ---
static void mattx_rpc_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    struct mattx_sys_open_req req;
    int i;
    
    // 1. Prepare the network request
    memset(&req, 0, sizeof(req));
    req.orig_pid = rpc->orig_pid;
    strncpy(req.filename, rpc->filename, sizeof(req.filename) - 1);

    printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending OPEN_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
    
    // 2. Send the request to Node 1
    if (cluster_map[rpc->home_node]) {
        mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_OPEN_REQ, &req, sizeof(req));
    }

    // 3. Wait for the reply (We can safely sleep here!)
    // For this micro-step, we will just simulate the wait using a simple loop.
    // In the next step, we will wire this to the actual network reply.
    int retries = 50;
    bool done = false;
    while (!done && retries > 0) {
        msleep(100);
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == rpc->local_pid) {
                done = guest_registry[i].rpc_done;
                break;
            }
        }
        spin_unlock(&guest_lock);
        retries--;
    }

    printk(KERN_INFO "MattX:[RPC] Worker finished for PID %d. Waking Surrogate...\n", rpc->local_pid);

    // 4. Wake the Surrogate back up!
    struct task_struct *surrogate = NULL;
    rcu_read_lock();
    surrogate = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
    if (surrogate) get_task_struct(surrogate);
    rcu_read_unlock();

    if (surrogate) {
        send_sig(SIGCONT, surrogate, 0);
        put_task_struct(surrogate);
    }

    kfree(rpc);
}
// ---------------------------------------------------------

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

    if (!is_guest_process(my_pid)) return 0;

    filename_ptr = *((const char __user **)ri->data);

    if (strncpy_from_user(filename, filename_ptr, sizeof(filename) - 1) > 0) {
        
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == my_pid) {
                home_node = guest_registry[i].home_node;
                orig_pid = guest_registry[i].orig_pid;
                guest_registry[i].rpc_done = false; // Reset the flag
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (home_node != -1) {
            // --- NEW: The Escape Hatch ---
            // We allocate a work struct, fill it, and throw it to the kernel's global workqueue
            struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); // MUST use GFP_ATOMIC inside a Kprobe!
            if (rpc) {
                INIT_WORK(&rpc->work, mattx_rpc_worker);
                rpc->local_pid = my_pid;
                rpc->orig_pid = orig_pid;
                rpc->home_node = home_node;
                strncpy(rpc->filename, filename, sizeof(rpc->filename) - 1);

                printk(KERN_INFO "MattX:[HOOK] Intercepted open('%s'). Freezing Surrogate %d and escaping to Workqueue...\n", filename, my_pid);
                
                // Freeze the Surrogate!
                send_sig(SIGSTOP, current, 0);
                
                // Schedule the background worker
                schedule_work(&rpc->work);
            }
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

