#include "mattx.h"
#include <linux/wait.h> 
#include <linux/poll.h>

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
    // SENDTO WORKER ---
    } else if (rpc->is_sendto) {
        size_t to_send = min_t(size_t, rpc->len, 4096);
        size_t payload_size = sizeof(struct mattx_sys_send_req) + to_send;
        void *payload_buf = kmalloc(payload_size, GFP_KERNEL);
        
        if (payload_buf) {
            struct mattx_sys_send_req *req = (struct mattx_sys_send_req *)payload_buf;
            req->orig_pid = rpc->orig_pid;
            req->fd = rpc->remote_fd;
            req->flags = rpc->flags;
            req->len = to_send;
            
            if (copy_from_user(req->data, rpc->buff, to_send) == 0) {
                printk(KERN_INFO "MattX:[RPC] Worker sending SEND_REQ to Node %d...\n", rpc->home_node);
                if (cluster_map[rpc->home_node]) {
                    mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SEND_REQ, payload_buf, payload_size);
                }
            }
            kfree(payload_buf);
        }
    // RECVFROM WORKER ---
    } else if (rpc->is_recvfrom) {
        struct mattx_sys_recv_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.flags = rpc->flags;
        req.size = min_t(size_t, rpc->size, 4096);
        
        printk(KERN_INFO "MattX:[RPC] Worker sending RECV_REQ to Node %d...\n", rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_RECV_REQ, &req, sizeof(req));
        }        
    // ACCEPT WORKER
    } else if (rpc->is_accept) {
        struct mattx_sys_accept_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.flags = rpc->flags;

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending ACCEPT_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_ACCEPT_REQ, &req, sizeof(req));
        }
    // POLL WORKER
    } else if (rpc->is_poll) {
        struct mattx_sys_poll_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.nfds = rpc->nfds;
        req.timeout = rpc->timeout;
        memcpy(req.fds, rpc->poll_fds, sizeof(struct mattx_pollfd) * rpc->nfds);

        printk(KERN_INFO "MattX:[RPC] Worker started for PID %d. Sending POLL_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_POLL_REQ, &req, sizeof(req));
        }

    } else if (rpc->is_select) {
        struct mattx_sys_poll_req req;
        fd_set *in_fds = NULL, *out_fds = NULL, *ex_fds = NULL;
        int timeout_ms = -1;
        int poll_idx = 0;
        int fd;

        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;

        // 1. Translate timeval to milliseconds
        if (rpc->select_timeout) {
            struct __kernel_old_timeval tv; // FIXED: Use the Y2038-safe legacy struct!
            if (copy_from_user(&tv, rpc->select_timeout, sizeof(tv)) == 0) {
                timeout_ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
            }
        }
        req.timeout = timeout_ms;

        // 2. Allocate temporary bitmaps
        in_fds = kzalloc(sizeof(fd_set), GFP_KERNEL);
        out_fds = kzalloc(sizeof(fd_set), GFP_KERNEL);
        ex_fds = kzalloc(sizeof(fd_set), GFP_KERNEL);

        // Check return values to satisfy the compiler's safety checks!
        if (rpc->select_readfds && copy_from_user(in_fds, rpc->select_readfds, sizeof(fd_set))) {}
        if (rpc->select_writefds && copy_from_user(out_fds, rpc->select_writefds, sizeof(fd_set))) {}
        if (rpc->select_exceptfds && copy_from_user(ex_fds, rpc->select_exceptfds, sizeof(fd_set))) {}

        // 3. Translate Bitmaps to Poll Array!
        for (fd = 0; fd < rpc->select_nfds && poll_idx < 16; fd++) {
            short events = 0;
            if (in_fds && test_bit(fd, (unsigned long *)in_fds)) events |= POLLIN;
            if (out_fds && test_bit(fd, (unsigned long *)out_fds)) events |= POLLOUT;
            if (ex_fds && test_bit(fd, (unsigned long *)ex_fds)) events |= POLLPRI;

            if (events) {
                struct file *f = fget(fd);
                if (f) {
                    if (f->f_op == &mattx_fops) {
                        struct mattx_fake_fd_info *fd_info = f->private_data;
                        if (fd_info) {
                            req.fds[poll_idx].fd = fd_info->remote_fd;
                            req.fds[poll_idx].events = events;
                            
                            // Secret trick: We save the Local FD in our work struct so we can map it back later!
                            rpc->poll_fds[poll_idx].fd = fd; 
                            poll_idx++;
                        }
                    }
                    fput(f);
                }
            }
        }
        req.nfds = poll_idx;
        rpc->nfds = poll_idx; // Save how many we mapped

        kfree(in_fds); kfree(out_fds); kfree(ex_fds);

        printk(KERN_INFO "MattX:[RPC] Translated select() to poll(). Sending POLL_REQ to Node %d...\n", rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_POLL_REQ, &req, sizeof(req));
        }

    // OPEN WORKER (DEFAULT FALLBACK)
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

    bool done = false;
    while (!done) {
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
    }

    struct task_struct *surrogate = NULL;
    rcu_read_lock();
    surrogate = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
    if (surrogate) get_task_struct(surrogate);
    rcu_read_unlock();

    if (surrogate) {
        if (rpc->is_statx) {
            // (Removed statx block since we deleted it)
        // --- FIXED: Group ALL FD-Operating Syscalls together! ---
        } else if (rpc->is_connect || rpc->is_bind || rpc->is_listen || rpc->is_sendto) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int error = -1;
            bool success = false;

            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == rpc->local_pid) {
                    // We reused rpc_fsync_res to store the generic integer reply from VM1
                    error = guest_registry[i].rpc_fsync_res; 
                    success = true;
                    break;
                }
            }
            spin_unlock(&guest_lock);

            if (success) {
                // Shove the return code (e.g., 0) directly into RAX! No Fake FDs allocated!
                regs->ax = error;
                printk(KERN_INFO "MattX:[RPC] Illusion Complete! Networking syscall returned %d to Surrogate %d\n", error, rpc->local_pid);
            } else {
                regs->ax = -EBADF;
            }

        // RECVFROM AWAKENING ---
        } else if (rpc->is_recvfrom) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            ssize_t ret_bytes = -1;
            void *read_buf = NULL;

            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == rpc->local_pid) {
                    ret_bytes = guest_registry[i].rpc_read_bytes;
                    read_buf = guest_registry[i].rpc_read_buf;
                    guest_registry[i].rpc_read_buf = NULL;
                    break;
                }
            }
            spin_unlock(&guest_lock);

            if (ret_bytes > 0 && read_buf) {
                if (copy_to_user(rpc->buff, read_buf, ret_bytes)) {
                    ret_bytes = -EFAULT;
                }
            }
            
            if (read_buf) kfree(read_buf);
            
            if (regs) {
                regs->ax = ret_bytes;
                printk(KERN_INFO "MattX:[RPC] Illusion Complete! recvfrom returned %zd\n", ret_bytes);
            }

        // ACCEPT AWAKENING
        } else if (rpc->is_accept) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int local_fd = -1;
            
            // If remote_fd is >= 0, VM1 successfully accepted a connection!
            if (remote_fd >= 0) {
                struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_KERNEL);
                if (fd_info) {
                    fd_info->home_node = rpc->home_node;
                    fd_info->orig_pid = rpc->orig_pid;
                    fd_info->remote_fd = remote_fd;
                    
                    struct file *fake_file = anon_inode_getfile("mattx_vfs_proxy", &mattx_fops, fd_info, O_RDWR);
                    
                    if (!IS_ERR(fake_file) && surrogate->files) {
                        spin_lock(&surrogate->files->file_lock);
                        struct fdtable *fdt = files_fdtable(surrogate->files);
                        for (int j = 3; j < fdt->max_fds; j++) {
                            if (!rcu_dereference_raw(fdt->fd[j])) {
                                rcu_assign_pointer(fdt->fd[j], fake_file);
                                __set_bit(j, fdt->open_fds);
                                local_fd = j;
                                break;
                            }
                        }
                        spin_unlock(&surrogate->files->file_lock);
                        
                        if (local_fd >= 0) {
                            regs->ax = local_fd; // Return the new Local FD!

                            // Copy the client's IP address back to user-space if requested
                            if (rpc->buff && rpc->size) {
                                int __user *ulen = (int __user *)rpc->size;
                                int len, g_idx;
                                void *addr_buf = NULL;
                                int addr_len = 0;

                                spin_lock(&guest_lock);
                                for (g_idx = 0; g_idx < guest_count; g_idx++) {
                                    if (guest_registry[g_idx].local_pid == rpc->local_pid) {
                                        addr_len = guest_registry[g_idx].rpc_fsync_res;
                                        addr_buf = guest_registry[g_idx].rpc_read_buf;
                                        guest_registry[g_idx].rpc_read_buf = NULL; 
                                        break;
                                    }
                                }
                                spin_unlock(&guest_lock);

                                // Ensure addr_len is > 0 before copying!
                                if (addr_buf && addr_len > 0 && get_user(len, ulen) == 0) {
                                    len = min_t(int, len, addr_len);
                                    if (copy_to_user(rpc->buff, addr_buf, len)) {
                                        printk(KERN_WARNING "MattX:[RPC] Failed to copy sockaddr to user!\n");
                                    } else {
                                        put_user(len, ulen); 
                                    }
                                }
                                if (addr_buf) kfree(addr_buf);
                            }

                            printk(KERN_INFO "MattX:[RPC] Illusion Complete! Mapped New Remote FD %d to Local FD %d\n", remote_fd, local_fd);
                        } else {
                            fput(fake_file);
                            regs->ax = -EMFILE;
                        }
                    } else {
                        kfree(fd_info);
                        regs->ax = -ENFILE;
                    }
                }
            } else {
                // VM1 returned an error (e.g., -EAGAIN for non-blocking sockets)
                regs->ax = remote_fd; 
            }            
 
        // POLL AWAKENING
        } else if (rpc->is_poll) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int retval = -EBADF;
            bool success = false;
            struct mattx_sys_poll_reply *reply_data = NULL;

            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == rpc->local_pid) {
                    retval = guest_registry[i].rpc_fsync_res; // We stored retval here
                    reply_data = guest_registry[i].rpc_read_buf; // We stored the array here
                    guest_registry[i].rpc_read_buf = NULL;
                    success = true;
                    break;
                }
            }
            spin_unlock(&guest_lock);

            if (success && reply_data) {
                // Copy the updated revents back to the user-space array!
                if (copy_to_user(rpc->poll_ufds, reply_data->fds, sizeof(struct pollfd) * rpc->nfds)) {
                    regs->ax = -EFAULT;
                } else {
                    regs->ax = retval; // Return the number of ready FDs
                    printk(KERN_INFO "MattX:[RPC] Illusion Complete! poll returned %d\n", retval);
                }
                kfree(reply_data);
            } else {
                regs->ax = -EBADF;
            }

        // --- SELECT AWAKENING ---
        } else if (rpc->is_select) {
            struct pt_regs *regs = task_pt_regs(surrogate);
            int retval = -EBADF;
            bool success = false;
            struct mattx_sys_poll_reply *reply_data = NULL;

            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == rpc->local_pid) {
                    retval = guest_registry[i].rpc_fsync_res; 
                    reply_data = guest_registry[i].rpc_read_buf; 
                    guest_registry[i].rpc_read_buf = NULL;
                    success = true;
                    break;
                }
            }
            spin_unlock(&guest_lock);

            if (success && reply_data) {
                fd_set *in_fds = kzalloc(sizeof(fd_set), GFP_KERNEL);
                fd_set *out_fds = kzalloc(sizeof(fd_set), GFP_KERNEL);
                fd_set *ex_fds = kzalloc(sizeof(fd_set), GFP_KERNEL);

                // 4. Translate Poll Array back to Bitmaps!
                for (int j = 0; j < reply_data->nfds; j++) {
                    int local_fd = rpc->poll_fds[j].fd; // We saved the local FD here earlier!
                    short revents = reply_data->fds[j].revents;

                    if (revents & (POLLIN | POLLERR | POLLHUP)) __set_bit(local_fd, (unsigned long *)in_fds);
                    if (revents & (POLLOUT | POLLERR | POLLHUP)) __set_bit(local_fd, (unsigned long *)out_fds);
                    if (revents & (POLLPRI | POLLERR | POLLHUP)) __set_bit(local_fd, (unsigned long *)ex_fds);
                }

                // 5. Copy the updated bitmaps back to user-space
                // Check return values to satisfy the compiler!
                if (rpc->select_readfds && copy_to_user(rpc->select_readfds, in_fds, sizeof(fd_set))) {}
                if (rpc->select_writefds && copy_to_user(rpc->select_writefds, out_fds, sizeof(fd_set))) {}
                if (rpc->select_exceptfds && copy_to_user(rpc->select_exceptfds, ex_fds, sizeof(fd_set))) {}

                kfree(in_fds); kfree(out_fds); kfree(ex_fds);

                regs->ax = retval;
                printk(KERN_INFO "MattX:[RPC] Illusion Complete! select() returned %d\n", retval);
                kfree(reply_data);
            } else {
                regs->ax = -EBADF;
            }

        // --- FD-Creating Syscalls (open, socket, dup) fall through here! ---
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

// SENDTO HOOK ---
struct sendto_kretprobe_data {
    int fd;
    void __user *buff;
    size_t len;
    unsigned int flags;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe sendto_kprobe;

static int entry_handler_sendto(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct sendto_kretprobe_data *data = (struct sendto_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // __sys_sendto(int fd, void __user *buff, size_t len, unsigned int flags, struct sockaddr __user *addr, int addr_len)
    data->fd = (int)regs->di;
    data->buff = (void __user *)regs->si;
    data->len = (size_t)regs->dx;
    data->flags = (unsigned int)regs->cx;
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

static int ret_handler_sendto(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct sendto_kretprobe_data *data = (struct sendto_kretprobe_data *)ri->data;
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
            
            rpc->is_sendto = true;
            rpc->remote_fd = data->remote_fd;
            rpc->buff = data->buff;
            rpc->len = data->len;
            rpc->flags = data->flags;

            printk(KERN_INFO "MattX:[HOOK] Intercepted sendto(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// RECVFROM HOOK ---
struct recvfrom_kretprobe_data {
    int fd;
    void __user *ubuf;
    size_t size;
    unsigned int flags;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe recvfrom_kprobe;

static int entry_handler_recvfrom(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct recvfrom_kretprobe_data *data = (struct recvfrom_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // __sys_recvfrom(int fd, void __user *ubuf, size_t size, unsigned int flags, struct sockaddr __user *addr, int __user *addr_len)
    data->fd = (int)regs->di;
    data->ubuf = (void __user *)regs->si;
    data->size = (size_t)regs->dx;
    data->flags = (unsigned int)regs->cx;
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

static int ret_handler_recvfrom(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct recvfrom_kretprobe_data *data = (struct recvfrom_kretprobe_data *)ri->data;
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
            
            rpc->is_recvfrom = true;
            rpc->remote_fd = data->remote_fd;
            rpc->buff = data->ubuf;
            rpc->size = data->size;
            rpc->flags = data->flags;

            printk(KERN_INFO "MattX:[HOOK] Intercepted recvfrom(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- ACCEPT HOOK ---
struct accept_kretprobe_data {
    int fd;
    struct sockaddr __user *upeer_sockaddr;
    int __user *upeer_addrlen;
    int flags;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe accept_kprobe;

static int entry_handler_accept(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct accept_kretprobe_data *data = (struct accept_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // __sys_accept4(int fd, struct sockaddr __user *upeer_sockaddr, int __user *upeer_addrlen, int flags)
    data->fd = (int)regs->di;
    data->upeer_sockaddr = (struct sockaddr __user *)regs->si;
    data->upeer_addrlen = (int __user *)regs->dx;
    data->flags = (int)regs->cx;
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

static int ret_handler_accept(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct accept_kretprobe_data *data = (struct accept_kretprobe_data *)ri->data;
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
            
            rpc->is_accept = true;
            rpc->remote_fd = data->remote_fd;
            rpc->flags = data->flags;
            
            // We save the user-space pointers so we can copy the client IP later!
            rpc->buff = data->upeer_sockaddr; 
            rpc->size = (size_t)data->upeer_addrlen; 

            printk(KERN_INFO "MattX:[HOOK] Intercepted accept(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- POLL HOOK ---
struct poll_kretprobe_data {
    void __user *ufds;
    int nfds;
    int timeout;
    bool is_wormhole;
    struct mattx_pollfd fds[16];
};

static struct kretprobe poll_kprobe;

static int entry_handler_poll(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct poll_kretprobe_data *data = (struct poll_kretprobe_data *)ri->data;
    int i;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // __x64_sys_poll(struct pollfd __user *ufds, unsigned int nfds, int timeout)
    data->ufds = (void __user *)regs->di;
    data->nfds = (int)regs->si;
    data->timeout = (int)regs->dx;
    data->is_wormhole = false;

    if (data->nfds > 0 && data->nfds <= 16) {
        // Copy the array from user-space
        if (copy_from_user(data->fds, data->ufds, data->nfds * sizeof(struct pollfd)) == 0) {
            
            // Translate Local FDs to Remote FDs!
            for (i = 0; i < data->nfds; i++) {
                if (data->fds[i].fd >= 0) {
                    struct file *f = fget(data->fds[i].fd);
                    if (f) {
                        if (f->f_op == &mattx_fops) {
                            struct mattx_fake_fd_info *fd_info = f->private_data;
                            if (fd_info) {
                                data->fds[i].fd = fd_info->remote_fd; // Swap it!
                                data->is_wormhole = true;
                            }
                        }
                        fput(f);
                    }
                }
            }
        }
    }

    if (data->is_wormhole) {
        regs->di = 0; // Sabotage! Pass NULL to the real sys_poll so it fails instantly with -EFAULT
    }

    return 0;
}

static int ret_handler_poll(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct poll_kretprobe_data *data = (struct poll_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole) return 0;

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
            
            rpc->is_poll = true;
            rpc->nfds = data->nfds;
            rpc->timeout = data->timeout;
            rpc->poll_ufds = data->ufds;
            memcpy(rpc->poll_fds, data->fds, sizeof(struct mattx_pollfd) * data->nfds);

            printk(KERN_INFO "MattX:[HOOK] Intercepted poll(). Freezing Surrogate %d...\n", my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- SELECT HOOK ---
struct select_kretprobe_data {
    int n;
    void __user *inp;
    void __user *outp;
    void __user *exp;
    void __user *tvp;
};

static struct kretprobe select_kprobe;

static int entry_handler_select(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct select_kretprobe_data *data = (struct select_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    // __x64_sys_select(int n, fd_set __user *inp, fd_set __user *outp, fd_set __user *exp, struct timeval __user *tvp)
    data->n = (int)regs->di;
    data->inp = (void __user *)regs->si;
    data->outp = (void __user *)regs->dx;
    data->exp = (void __user *)regs->cx;
    data->tvp = (void __user *)regs->r8;

    regs->di = -1; // Sabotage! Force -EBADF so the local syscall aborts instantly

    return 0;
}

static int ret_handler_select(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct select_kretprobe_data *data = (struct select_kretprobe_data *)ri->data;
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
            
            rpc->is_select = true;
            rpc->select_nfds = data->n;
            rpc->select_readfds = data->inp;
            rpc->select_writefds = data->outp;
            rpc->select_exceptfds = data->exp;
            rpc->select_timeout = data->tvp;

            printk(KERN_INFO "MattX:[HOOK] Intercepted select(). Freezing Surrogate %d...\n", my_pid);
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

    // SENDTO / RECVFROM ---
    memset(&sendto_kprobe, 0, sizeof(sendto_kprobe));
    sendto_kprobe.kp.symbol_name = "__sys_sendto";
    sendto_kprobe.entry_handler = entry_handler_sendto;
    sendto_kprobe.handler = ret_handler_sendto;
    sendto_kprobe.data_size = sizeof(struct sendto_kretprobe_data);
    sendto_kprobe.maxactive = 64;

    ret = register_kretprobe(&sendto_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for sendto, returned %d\n", ret);

    memset(&recvfrom_kprobe, 0, sizeof(recvfrom_kprobe));
    recvfrom_kprobe.kp.symbol_name = "__sys_recvfrom";
    recvfrom_kprobe.entry_handler = entry_handler_recvfrom;
    recvfrom_kprobe.handler = ret_handler_recvfrom;
    recvfrom_kprobe.data_size = sizeof(struct recvfrom_kretprobe_data);
    recvfrom_kprobe.maxactive = 64;

    ret = register_kretprobe(&recvfrom_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for recvfrom, returned %d\n", ret);

    memset(&accept_kprobe, 0, sizeof(accept_kprobe));
    accept_kprobe.kp.symbol_name = "__sys_accept4";
    accept_kprobe.entry_handler = entry_handler_accept;
    accept_kprobe.handler = ret_handler_accept;
    accept_kprobe.data_size = sizeof(struct accept_kretprobe_data);
    accept_kprobe.maxactive = 64;
    
    ret = register_kretprobe(&accept_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for accept, returned %d\n", ret);

    memset(&poll_kprobe, 0, sizeof(poll_kprobe));
    poll_kprobe.kp.symbol_name = "__x64_sys_poll";
    poll_kprobe.entry_handler = entry_handler_poll;
    poll_kprobe.handler = ret_handler_poll;
    poll_kprobe.data_size = sizeof(struct poll_kretprobe_data);
    poll_kprobe.maxactive = 64;

    ret = register_kretprobe(&poll_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for poll, returned %d\n", ret);

    memset(&select_kprobe, 0, sizeof(select_kprobe));
    select_kprobe.kp.symbol_name = "__x64_sys_select";
    select_kprobe.entry_handler = entry_handler_select;
    select_kprobe.handler = ret_handler_select;
    select_kprobe.data_size = sizeof(struct select_kretprobe_data);
    select_kprobe.maxactive = 64;
    
    ret = register_kretprobe(&select_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for select, returned %d\n", ret);

    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) registered successfully.\n");
    return 0;
}

void mattx_hooks_exit(void) {
    unregister_kretprobe(&select_kprobe);
    unregister_kretprobe(&poll_kprobe);
    unregister_kretprobe(&accept_kprobe);
    unregister_kretprobe(&recvfrom_kprobe);
    unregister_kretprobe(&sendto_kprobe);
    unregister_kretprobe(&listen_kprobe);
    unregister_kretprobe(&bind_kprobe);
    unregister_kretprobe(&connect_kprobe);
    unregister_kretprobe(&socket_kprobe);
    unregister_kretprobe(&dup2_kprobe);
    unregister_kretprobe(&dup_kprobe);
    unregister_kretprobe(&openat_kprobe);
    printk(KERN_INFO "MattX: Syscall Hooks (Kprobes) unregistered.\n");
}

