#include "mattx.h"
#include <linux/wait.h> 

static struct kretprobe openat_kprobe;

static void mattx_rpc_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    struct mattx_sys_open_req req;
    int i;
    int remote_fd = -1;
    
    memset(&req, 0, sizeof(req));
    req.orig_pid = rpc->orig_pid;
    strncpy(req.filename, rpc->filename, sizeof(req.filename) - 1);

    printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending OPEN_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
    
    if (cluster_map[rpc->home_node]) {
        mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_OPEN_REQ, &req, sizeof(req));
    }

    int retries = 50;
    bool done = false;
    while (!done && retries > 0) {
        msleep(100);
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == rpc->local_pid) {
                done = guest_registry[i].rpc_done;
                if (done) remote_fd = guest_registry[i].rpc_remote_fd;
                break;
            }
        }
        spin_unlock(&guest_lock);
        retries--;
    }

    struct task_struct *surrogate = NULL;
    rcu_read_lock();
    surrogate = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
    if (surrogate) get_task_struct(surrogate);
    rcu_read_unlock();

    if (surrogate) {
        if (remote_fd >= 0) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int local_fd = -1; 

            // --- FIXED: Manual FD Allocation ---
            // Because we sabotaged the syscall, it returned -EFAULT and didn't allocate an FD.
            // We find an empty slot in the pre-expanded table and install our Fake File!
            struct file *fake_file = anon_inode_getfile("mattx_vfs_proxy", &mattx_fops, (void *)(uintptr_t)remote_fd, O_WRONLY);

            if (!IS_ERR(fake_file) && surrogate->files) {
                spin_lock(&surrogate->files->file_lock);
                struct fdtable *fdt = files_fdtable(surrogate->files);
                
                for (int j = 3; j < fdt->max_fds; j++) {
                    if (rcu_dereference_raw(fdt->fd[j]) == NULL) {
                        rcu_assign_pointer(fdt->fd[j], fake_file);
                        __set_bit(j, fdt->open_fds); 
                        local_fd = j;
                        break;
                    }
                }
                spin_unlock(&surrogate->files->file_lock);
                
                if (local_fd >= 0) {
                    if (regs) regs->ax = local_fd; // Overwrite the -EFAULT error with our success FD!
                    printk(KERN_INFO "MattX:[RPC] Illusion Complete! Mapped Remote FD %d to Local FD %d\n", remote_fd, local_fd);
                } else {
                    printk(KERN_ERR "MattX:[RPC] Failed to find empty FD slot!\n");
                    fput(fake_file);
                }
            }
        }

        printk(KERN_INFO "MattX:[RPC] Worker finished for PID %d. Waking Surrogate...\n", rpc->local_pid);
        send_sig(SIGCONT, surrogate, 0);
        put_task_struct(surrogate);
    }

    kfree(rpc);
}

static int entry_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;

    if (!is_guest_process(my_pid)) return 0; 

    // Save the real filename pointer
    *((const char __user **)ri->data) = (const char __user *)regs->si;
    
    // --- FIXED: The Sabotage! ---
    // We set the filename pointer to NULL. do_sys_openat2 will instantly fail with -EFAULT
    // and return without touching VM2's hard drive!
    regs->si = 0; 

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
                guest_registry[i].rpc_done = false; 
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (home_node != -1) {
            struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
            if (rpc) {
                INIT_WORK(&rpc->work, mattx_rpc_worker);
                rpc->local_pid = my_pid;
                rpc->orig_pid = orig_pid;
                rpc->home_node = home_node;
                strncpy(rpc->filename, filename, sizeof(rpc->filename) - 1);

                printk(KERN_INFO "MattX:[HOOK] Intercepted open('%s'). Freezing Surrogate %d and escaping to Workqueue...\n", filename, my_pid);
                
                send_sig(SIGSTOP, current, 0);
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

