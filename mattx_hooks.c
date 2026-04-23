#include "mattx.h"
#include <linux/wait.h> 

static struct kretprobe openat_kprobe;

static void mattx_rpc_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    int i;
    int remote_fd = -1;
    
    if (rpc->is_statx) {
        // ... (statx logic, removed here since we deleted statx kprobe)
    } else if (rpc->is_dup) {
        struct mattx_sys_dup_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.old_remote_fd = rpc->remote_fd;
        req.new_local_fd = rpc->new_local_fd;

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending DUP_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_DUP_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_socket) {
        struct mattx_sys_socket_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.domain = rpc->domain;
        req.type = rpc->type;
        req.protocol = rpc->protocol;

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending SOCKET_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SOCKET_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_connect) {
        struct mattx_sys_connect_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.addrlen = rpc->addrlen;
        memcpy(&req.addr, &rpc->addr, rpc->addrlen);

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending CONNECT_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_CONNECT_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_bind) {
        struct mattx_sys_bind_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.addrlen = rpc->addrlen;
        memcpy(&req.addr, &rpc->addr, rpc->addrlen);

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending BIND_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_BIND_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_listen) {
        struct mattx_sys_listen_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.backlog = rpc->backlog;

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending LISTEN_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_LISTEN_REQ, &req, sizeof(req));
        }
    } else {
        struct mattx_sys_open_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        strncpy(req.filename, rpc->filename, sizeof(req.filename) - 1);
        req.flags = rpc->flags;
        req.mode = rpc->mode;

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending OPEN_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_OPEN_REQ, &req, sizeof(req));
        }
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
        if (rpc->is_statx) {
            // (Removed statx block since we deleted it)
        } else if (rpc->is_connect) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int error = -1;
            bool success = false;

            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == rpc->local_pid) {
                    error = guest_registry[i].rpc_fsync_res; // We reused this field for connect errors
                    success = true;
                    break;
                }
            }
            spin_unlock(&guest_lock);

            if (success) {
                regs->ax = error;
                printk(KERN_INFO "MattX:[RPC] Illusion Complete! Networking syscall returned %d to Surrogate %d\n", error, rpc->local_pid);
            } else {
                regs->ax = -EBADF;
            }
        } else if (remote_fd >= 0) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int local_fd = regs ? regs->ax : -1; 

            printk(KERN_INFO "MattX:[DEBUG] Remote FD from VM1: %d. Local regs->ax: %d\n", remote_fd, local_fd);
            if (local_fd < 0) {
                printk(KERN_ERR "MattX:[DEBUG] Local syscall failed (expected)! Searching for a free FD slot...\n");
                
                if (surrogate->files) {
                    spin_lock(&surrogate->files->file_lock);
                    struct fdtable *fdt = files_fdtable(surrogate->files);
                    int fd;
                    for (fd = 3; fd < fdt->max_fds; fd++) {
                        if (!rcu_dereference_raw(fdt->fd[fd])) {
                            local_fd = fd;
                            __set_bit(fd, fdt->open_fds);
                            break;
                        }
                    }
                    spin_unlock(&surrogate->files->file_lock);
                }
            }

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
                        // Inject our custom inode_operations so we can catch fstat/getattr!
                        if (fake_file->f_inode) {
                            fake_file->f_inode->i_op = &mattx_iops;
                        }

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
            if (!config_migrate_file_io) {
                // Local Breakout! 🏎️
                // User disabled File I/O Wormhole routing. Let the Surrogate execute
                // openat locally on the remote node's native filesystem!
                return 0;
            }

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

// Removed statx hooks

// --- DUP HOOK ---

struct dup_kretprobe_data {
    int oldfd;
    int newfd;
    bool is_wormhole_fd;
    int old_remote_fd;
};

static struct kretprobe dup_kprobe;
static struct kretprobe dup2_kprobe;

static int entry_handler_dup(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct dup_kretprobe_data *data = (struct dup_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0;

    data->oldfd = (int)regs->di;
    
    // Check which kprobe triggered this by using the standard kernel helper
    if (get_kretprobe(ri) == &dup2_kprobe) {
        data->newfd = (int)regs->si;
    } else {
        data->newfd = -1;
    }
    
    data->is_wormhole_fd = false;
    data->old_remote_fd = -1;

    if (data->oldfd >= 0) {
        struct file *f = fget(data->oldfd);
        if (f) {
            if (f->f_op == &mattx_fops) {
                struct mattx_fake_fd_info *fd_info = f->private_data;
                if (fd_info) {
                    data->is_wormhole_fd = true;
                    data->old_remote_fd = fd_info->remote_fd;
                }
            }
            fput(f);
        }
    }

    if (data->is_wormhole_fd) {
        regs->di = -1; // Sabotage! The local dup will fail with -EBADF
    }

    return 0;
}

static int ret_handler_dup(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct dup_kretprobe_data *data = (struct dup_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !data->is_wormhole_fd) return 0;

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
            
            rpc->is_dup = true;
            rpc->remote_fd = data->old_remote_fd;
            rpc->new_local_fd = data->newfd;
            
            // We need to pass valid flags/mode to open a new fake file proxy later in mattx_rpc_worker
            struct file *f = fget(data->oldfd);
            if (f) {
                rpc->flags = f->f_flags;
                fput(f);
            } else {
                rpc->flags = O_RDWR;
            }
            rpc->mode = 0666;

            printk(KERN_INFO "MattX:[HOOK] Intercepted dup(fd=%d). Freezing Surrogate %d and escaping to Workqueue...\n", data->oldfd, my_pid);
            
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- SOCKET HOOK ---
struct socket_kretprobe_data {
    int domain;
    int type;
    int protocol;
};

static struct kretprobe socket_kprobe;

static int entry_handler_socket(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct socket_kretprobe_data *data = (struct socket_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // sys_socket(int family, int type, int protocol)
    data->domain = (int)regs->di;
    data->type = (int)regs->si;
    data->protocol = (int)regs->dx;

    regs->di = -1; // Sabotage! The local __sys_socket will fail with -EAFNOSUPPORT

    return 0;
}

static int ret_handler_socket(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct socket_kretprobe_data *data = (struct socket_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0;

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
            
            rpc->is_socket = true;
            rpc->domain = data->domain;
            rpc->type = data->type;
            rpc->protocol = data->protocol;

            // Fake an O_RDWR open so mattx_vfs_proxy is fully featured
            rpc->flags = O_RDWR;

            printk(KERN_INFO "MattX:[HOOK] Intercepted socket(domain=%d). Freezing Surrogate %d and escaping to Workqueue...\n", data->domain, my_pid);
            
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- CONNECT HOOK ---
struct connect_kretprobe_data {
    int fd;
    struct sockaddr_storage addr;
    int addrlen;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe connect_kprobe;

static int entry_handler_connect(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // sys_connect(int fd, struct sockaddr __user *uservaddr, int addrlen)
    data->fd = (int)regs->di;
    data->addrlen = (int)regs->dx;
    data->is_wormhole_fd = false;
    data->remote_fd = -1;

    // Safely copy the sockaddr struct from user space to kernel space
    if (data->addrlen > 0 && data->addrlen <= sizeof(struct sockaddr_storage)) {
        if (copy_from_user(&data->addr, (void __user *)regs->si, data->addrlen)) {
            data->addrlen = 0;
        }
    } else {
        data->addrlen = 0;
    }

    if (data->fd >= 0) {
        struct file *f = fget(data->fd);
        if (f) {
            if (f->f_op == &mattx_fops) {
                struct mattx_fake_fd_info *fd_info = f->private_data;
                if (fd_info) {
                    data->is_wormhole_fd = true;
                    data->remote_fd = fd_info->remote_fd;
                }
            }
            fput(f);
        }
    }

    if (data->is_wormhole_fd) {
        regs->di = -1; // Sabotage! The local __sys_connect will fail
    }

    return 0;
}

static int ret_handler_connect(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

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
            
            rpc->is_connect = true;
            rpc->remote_fd = data->remote_fd;
            rpc->addrlen = data->addrlen;
            memcpy(&rpc->addr, &data->addr, data->addrlen);

            printk(KERN_INFO "MattX:[HOOK] Intercepted connect(fd=%d). Freezing Surrogate %d and escaping to Workqueue...\n", data->fd, my_pid);
            
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- BIND HOOK ---
static struct kretprobe bind_kprobe;

static int entry_handler_bind(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // sys_bind(int fd, struct sockaddr __user *umyaddr, int addrlen)
    data->fd = (int)regs->di;
    data->addrlen = (int)regs->dx;
    data->is_wormhole_fd = false;
    data->remote_fd = -1;

    if (data->addrlen > 0 && data->addrlen <= sizeof(struct sockaddr_storage)) {
        if (copy_from_user(&data->addr, (void __user *)regs->si, data->addrlen)) {
            data->addrlen = 0;
        }
    } else {
        data->addrlen = 0;
    }

    if (data->fd >= 0) {
        struct file *f = fget(data->fd);
        if (f) {
            if (f->f_op == &mattx_fops) {
                struct mattx_fake_fd_info *fd_info = f->private_data;
                if (fd_info) {
                    data->is_wormhole_fd = true;
                    data->remote_fd = fd_info->remote_fd;
                }
            }
            fput(f);
        }
    }

    if (data->is_wormhole_fd) {
        regs->di = -1; // Sabotage!
    }

    return 0;
}

static int ret_handler_bind(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

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
            
            rpc->is_bind = true;
            rpc->remote_fd = data->remote_fd;
            rpc->addrlen = data->addrlen;
            memcpy(&rpc->addr, &data->addr, data->addrlen);

            printk(KERN_INFO "MattX:[HOOK] Intercepted bind(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- LISTEN HOOK ---
struct listen_kretprobe_data {
    int fd;
    int backlog;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe listen_kprobe;

static int entry_handler_listen(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct listen_kretprobe_data *data = (struct listen_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // sys_listen(int fd, int backlog)
    data->fd = (int)regs->di;
    data->backlog = (int)regs->si;
    data->is_wormhole_fd = false;
    data->remote_fd = -1;

    if (data->fd >= 0) {
        struct file *f = fget(data->fd);
        if (f) {
            if (f->f_op == &mattx_fops) {
                struct mattx_fake_fd_info *fd_info = f->private_data;
                if (fd_info) {
                    data->is_wormhole_fd = true;
                    data->remote_fd = fd_info->remote_fd;
                }
            }
            fput(f);
        }
    }

    if (data->is_wormhole_fd) {
        regs->di = -1; // Sabotage!
    }

    return 0;
}

static int ret_handler_listen(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct listen_kretprobe_data *data = (struct listen_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

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
            
            rpc->is_listen = true;
            rpc->remote_fd = data->remote_fd;
            rpc->backlog = data->backlog;

            printk(KERN_INFO "MattX:[HOOK] Intercepted listen(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
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

    memset(&dup_kprobe, 0, sizeof(dup_kprobe));
    dup_kprobe.kp.symbol_name = "__x64_sys_dup";
    dup_kprobe.entry_handler = entry_handler_dup;
    dup_kprobe.handler = ret_handler_dup;
    dup_kprobe.data_size = sizeof(struct dup_kretprobe_data);
    dup_kprobe.maxactive = 64;

    ret = register_kretprobe(&dup_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for dup, returned %d\n", ret);
    }

    memset(&dup2_kprobe, 0, sizeof(dup2_kprobe));
    dup2_kprobe.kp.symbol_name = "__x64_sys_dup2";
    dup2_kprobe.entry_handler = entry_handler_dup;
    dup2_kprobe.handler = ret_handler_dup;
    dup2_kprobe.data_size = sizeof(struct dup_kretprobe_data);
    dup2_kprobe.maxactive = 64;

    ret = register_kretprobe(&dup2_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for dup2, returned %d\n", ret);
    }

    memset(&socket_kprobe, 0, sizeof(socket_kprobe));
    socket_kprobe.kp.symbol_name = "__sys_socket";
    socket_kprobe.entry_handler = entry_handler_socket;
    socket_kprobe.handler = ret_handler_socket;
    socket_kprobe.data_size = sizeof(struct socket_kretprobe_data);
    socket_kprobe.maxactive = 64;

    ret = register_kretprobe(&socket_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for socket, returned %d\n", ret);
    }

    memset(&connect_kprobe, 0, sizeof(connect_kprobe));
    connect_kprobe.kp.symbol_name = "__sys_connect";
    connect_kprobe.entry_handler = entry_handler_connect;
    connect_kprobe.handler = ret_handler_connect;
    connect_kprobe.data_size = sizeof(struct connect_kretprobe_data);
    connect_kprobe.maxactive = 64;

    ret = register_kretprobe(&connect_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for connect, returned %d\n", ret);
    }

    memset(&bind_kprobe, 0, sizeof(bind_kprobe));
    bind_kprobe.kp.symbol_name = "__sys_bind";
    bind_kprobe.entry_handler = entry_handler_bind;
    bind_kprobe.handler = ret_handler_bind;
    bind_kprobe.data_size = sizeof(struct connect_kretprobe_data); // we reuse connect data structure
    bind_kprobe.maxactive = 64;

    ret = register_kretprobe(&bind_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for bind, returned %d\n", ret);

    memset(&listen_kprobe, 0, sizeof(listen_kprobe));
    listen_kprobe.kp.symbol_name = "__sys_listen";
    listen_kprobe.entry_handler = entry_handler_listen;
    listen_kprobe.handler = ret_handler_listen;
    listen_kprobe.data_size = sizeof(struct listen_kretprobe_data);
    listen_kprobe.maxactive = 64;

    ret = register_kretprobe(&listen_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for listen, returned %d\n", ret);

    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) registered successfully.\n");
    return 0;
}

void mattx_hooks_exit(void) {
    unregister_kretprobe(&listen_kprobe);
    unregister_kretprobe(&bind_kprobe);
    unregister_kretprobe(&connect_kprobe);
    unregister_kretprobe(&socket_kprobe);
    unregister_kretprobe(&dup2_kprobe);
    unregister_kretprobe(&dup_kprobe);
    unregister_kretprobe(&openat_kprobe);
    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) unregistered.\n");
}

