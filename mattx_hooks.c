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
    req.flags = rpc->flags;
    req.mode = rpc->mode;

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
            int local_fd = regs ? regs->ax : -1; 

            if (local_fd >= 0) {
                struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_KERNEL);
                if (fd_info) {
                    fd_info->home_node = rpc->home_node;
                    fd_info->orig_pid = rpc->orig_pid;
                    fd_info->remote_fd = remote_fd;
                    
                    // Use the original flags (filtered to file access modes) to create the fake file
                    struct file *fake_file = anon_inode_getfile("mattx_vfs_proxy", &mattx_fops, fd_info, rpc->flags & O_ACCMODE);
                    struct file *old_file = NULL;

                    if (!IS_ERR(fake_file) && surrogate->files) {
                        spin_lock(&surrogate->files->file_lock);
                        struct fdtable *fdt = files_fdtable(surrogate->files);
                        
                        if (local_fd < fdt->max_fds) {
                            old_file = rcu_dereference_raw(fdt->fd[local_fd]);
                            rcu_assign_pointer(fdt->fd[local_fd], fake_file);
                        }
                        spin_unlock(&surrogate->files->file_lock);
                        
                        if (old_file) fput(old_file); 
                        
                        // --- RESTORED: Hijack the return value! ---
                        regs->ax = local_fd; 
                        
                        printk(KERN_INFO "MattX:[RPC] Illusion Complete! Mapped Remote FD %d to Local FD %d\n", remote_fd, local_fd);
                    } else {
                        if (!IS_ERR(fake_file)) fput(fake_file);
                        kfree(fd_info);
                    }
                }
            }
        }

        printk(KERN_INFO "MattX:[RPC] Worker finished for PID %d. Waking Surrogate...\n", rpc->local_pid);
        send_sig(SIGCONT, surrogate, 0);
        put_task_struct(surrogate);
    }

    kfree(rpc);
}

struct kretprobe_data {
    const char __user *filename_ptr;
    int flags;
    int mode;
};

static int entry_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct kretprobe_data *data = (struct kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 

    // Extract the original arguments from do_sys_openat2
    // args: dfd (di), filename (si), open_how (dx)
    data->filename_ptr = (const char __user *)regs->si;
    
    // In do_sys_openat2, the third argument (dx) is a kernel-space pointer to struct open_how
    struct open_how *how = (struct open_how *)regs->dx;
    if (how) {
        data->flags = how->flags;
        data->mode = how->mode;
    } else {
        data->flags = O_RDONLY; // Safe default
        data->mode = 0;
    }

    regs->si = 0; // Sabotage the syscall!

    return 0;
}

static int ret_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct kretprobe_data *data = (struct kretprobe_data *)ri->data;
    char filename[256] = {0};
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid)) return 0;

    if (strncpy_from_user(filename, data->filename_ptr, sizeof(filename) - 1) > 0) {
        
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
                rpc->flags = data->flags;
                rpc->mode = data->mode;
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
    openat_kprobe.data_size = sizeof(struct kretprobe_data); 
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

