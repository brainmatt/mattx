#include "mattx.h"
#include <linux/wait.h>

// --- The VFS Proxy (Fake FDs) ---

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

// --- NEW: The Fake File Read Operation (Runs on Node 2) ---
static ssize_t mattx_fake_read(struct file *file, char __user *buf, size_t count, loff_t *pos) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    struct mattx_sys_read_req req;
    DECLARE_WAIT_QUEUE_HEAD_ONSTACK(rpc_wq);
    pid_t my_pid = current->pid;
    int i;
    ssize_t ret_bytes = 0;
    void *read_buf = NULL;

    if (!fd_info || !cluster_map[fd_info->home_node]) return -EIO;

    // Cap read size to prevent massive kmallocs on the network
    size_t to_read = min_t(size_t, count, 4096);

    req.orig_pid = fd_info->orig_pid;
    req.fd = fd_info->remote_fd;
    req.count = to_read;

    // 1. Attach our wait queue to the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            guest_registry[i].rpc_wq = &rpc_wq;
            guest_registry[i].rpc_done = false;
            guest_registry[i].rpc_read_buf = NULL;
            guest_registry[i].rpc_read_bytes = 0;
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d requesting %zu bytes from FD %u. Sleeping...\n", my_pid, to_read, req.fd);

    // 2. Send the request to Node 1
    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_READ_REQ, &req, sizeof(req));

    // 3. Go to sleep until Node 1 replies!
    wait_event_interruptible(rpc_wq, check_rpc_done(my_pid));

    // 4. We woke up! Collect the data from the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            read_buf = guest_registry[i].rpc_read_buf;
            ret_bytes = guest_registry[i].rpc_read_bytes;
            
            guest_registry[i].rpc_wq = NULL;
            guest_registry[i].rpc_read_buf = NULL;
            break;
        }
    }
    spin_unlock(&guest_lock);

    // 5. Copy the data to user-space
    if (ret_bytes > 0 && read_buf) {
        if (copy_to_user(buf, read_buf, ret_bytes)) {
            ret_bytes = -EFAULT;
        } else {
            *pos += ret_bytes; // Advance the file position!
        }
    }

    if (read_buf) kfree(read_buf);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d woke up! Read %zd bytes.\n", my_pid, ret_bytes);
    return ret_bytes;
}

static ssize_t mattx_fake_write(struct file *file, const char __user *buf, size_t count, loff_t *pos) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    size_t to_send;
    size_t packet_size;
    void *packet_buf;
    struct mattx_header *hdr;
    struct mattx_syscall_req *req;

    if (!fd_info || !cluster_map[fd_info->home_node]) return count; 

    to_send = min_t(size_t, count, 4096);

    packet_size = sizeof(struct mattx_header) + sizeof(struct mattx_syscall_req) + to_send;
    packet_buf = kmalloc(packet_size, GFP_KERNEL);
    if (!packet_buf) return -ENOMEM;

    hdr = (struct mattx_header *)packet_buf;
    req = (struct mattx_syscall_req *)(packet_buf + sizeof(struct mattx_header));

    req->orig_pid = fd_info->orig_pid;
    req->fd = fd_info->remote_fd; 
    req->len = to_send;

    if (copy_from_user(req->data, buf, to_send)) {
        kfree(packet_buf);
        return -EFAULT;
    }

    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYSCALL_FWD, packet_buf + sizeof(struct mattx_header), sizeof(struct mattx_syscall_req) + to_send);

    kfree(packet_buf);
    *pos += to_send;
    return to_send; 
}

static int mattx_fake_release(struct inode *inode, struct file *file) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    struct mattx_sys_close_req req;

    if (fd_info) {
        if (cluster_map[fd_info->home_node]) {
            req.orig_pid = fd_info->orig_pid;
            req.remote_fd = fd_info->remote_fd;

            printk(KERN_INFO "MattX:[WORMHOLE] Surrogate closed FD %u. Sending CLOSE_REQ to Node %d...\n", req.remote_fd, fd_info->home_node);
            mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_CLOSE_REQ, &req, sizeof(req));
        }
        kfree(fd_info);
    }
    return 0;
}

const struct file_operations mattx_fops = {
    .read = mattx_fake_read,
    .write = mattx_fake_write,
    .release = mattx_fake_release, 
};

// --- Network Handlers for File I/O ---

static void handle_syscall_fwd(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_syscall_req *req = (struct mattx_syscall_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        
        if (req->fd >= 1000) {
            int slot = req->fd - 1000;
            spin_lock(&export_lock);
            for (i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == req->orig_pid) {
                    if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                        file = export_registry[i].remote_files[slot];
                        get_file(file); 
                    }
                    break;
                }
            }
            spin_unlock(&export_lock);
        } else {
            rcu_read_lock();
            deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();
            
            if (deputy) {
                struct files_struct *files = deputy->files;
                if (files) {
                    spin_lock(&files->file_lock);
                    struct fdtable *fdt = files_fdtable(files);
                    if (req->fd < fdt->max_fds) {
                        file = rcu_dereference_raw(fdt->fd[req->fd]);
                        if (file) get_file(file); 
                    }
                    spin_unlock(&files->file_lock);
                }
            }
        }
        
        if (file) {
            loff_t pos = file->f_pos;
            ssize_t ret;
            const struct cred *old_cred = NULL;
            
            if (deputy) {
                if (deputy->mm) kthread_use_mm(deputy->mm);
                old_cred = override_creds(deputy->cred);
            }
            
            ret = kernel_write(file, req->data, req->len, &pos);
            
            if (deputy) {
                revert_creds(old_cred);
                if (deputy->mm) kthread_unuse_mm(deputy->mm);
            }
            
            if (ret >= 0) {
                file->f_pos = pos;
            } else {
                printk(KERN_ERR "MattX:[WORMHOLE] kernel_write failed for FD %u with error %zd\n", req->fd, ret);
            }
            fput(file); 
        }
        if (deputy) put_task_struct(deputy);
    }
}

static void handle_sys_open_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_open_req *req = (struct mattx_sys_open_req *)payload;
        struct mattx_sys_open_reply reply;
        struct file *filp = NULL;
        struct task_struct *deputy = NULL;
        int remote_fd = -1;
        int i, j;

        printk(KERN_INFO "MattX:[RPC] Received OPEN request from Node %u for file: '%s'\n", hdr->sender_id, req->filename);

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            const struct cred *old_cred;
            
            if (deputy->mm) kthread_use_mm(deputy->mm);
            old_cred = override_creds(deputy->cred);

            filp = filp_open(req->filename, req->flags, req->mode);
            
            revert_creds(old_cred);
            if (deputy->mm) kthread_unuse_mm(deputy->mm);

            if (IS_ERR(filp)) {
                printk(KERN_ERR "MattX:[RPC] Failed to open file '%s' on Home Node (err: %ld)\n", req->filename, PTR_ERR(filp));
                reply.error = PTR_ERR(filp);
            } else {
                spin_lock(&export_lock);
                for (i = 0; i < export_count; i++) {
                    if (export_registry[i].orig_pid == req->orig_pid) {
                        for (j = 0; j < MAX_FDS; j++) {
                            if (export_registry[i].remote_files[j] == NULL) {
                                export_registry[i].remote_files[j] = filp;
                                remote_fd = j + 1000; 
                                break;
                            }
                        }
                        break;
                    }
                }
                spin_unlock(&export_lock);
                
                if (remote_fd == -1) {
                    fput(filp); 
                    reply.error = -ENFILE;
                } else {
                    reply.error = 0;
                }
            }
            put_task_struct(deputy);
        } else {
            reply.error = -ESRCH;
        }

        reply.orig_pid = req->orig_pid;
        reply.remote_fd = remote_fd;

        printk(KERN_INFO "MattX:[RPC] Sending OPEN_REPLY (Remote FD: %d) back to Node %u...\n", remote_fd, hdr->sender_id);
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_OPEN_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_open_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_open_reply *reply = (struct mattx_sys_open_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received OPEN_REPLY for Orig PID %u. Remote FD is %d.\n", reply->orig_pid, reply->remote_fd);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                guest_registry[i].rpc_remote_fd = reply->remote_fd;
                guest_registry[i].rpc_done = true;
                if (guest_registry[i].rpc_wq) {
                    wake_up_interruptible(guest_registry[i].rpc_wq);
                }
                break;
            }
        }
        spin_unlock(&guest_lock);
    }
}

static void handle_sys_close_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_close_req *req = (struct mattx_sys_close_req *)payload;
        int i;
        
        printk(KERN_INFO "MattX:[RPC] Received CLOSE request for Remote FD %u from Node %u\n", req->remote_fd, hdr->sender_id);

        if (req->remote_fd >= 1000) {
            int slot = req->remote_fd - 1000;
            spin_lock(&export_lock);
            for (i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == req->orig_pid) {
                    if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                        fput(export_registry[i].remote_files[slot]);
                        export_registry[i].remote_files[slot] = NULL;
                        printk(KERN_INFO "MattX:[RPC] Successfully closed Remote FD %u\n", req->remote_fd);
                    }
                    break;
                }
            }
            spin_unlock(&export_lock);
        }
    }
}

static void handle_sys_read_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_read_req *req = (struct mattx_sys_read_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        
        printk(KERN_INFO "MattX:[WORMHOLE] Received READ request for FD %u from Node %u. Reading from Deputy...\n", req->fd, hdr->sender_id);

        if (req->fd >= 1000) {
            int slot = req->fd - 1000;
            spin_lock(&export_lock);
            for (i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == req->orig_pid) {
                    if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                        file = export_registry[i].remote_files[slot];
                        get_file(file); 
                    }
                    break;
                }
            }
            spin_unlock(&export_lock);
        } else {
            rcu_read_lock();
            deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();
            
            if (deputy) {
                struct files_struct *files = deputy->files;
                if (files) {
                    spin_lock(&files->file_lock);
                    struct fdtable *fdt = files_fdtable(files);
                    if (req->fd < fdt->max_fds) {
                        file = rcu_dereference_raw(fdt->fd[req->fd]);
                        if (file) get_file(file); 
                    }
                    spin_unlock(&files->file_lock);
                }
                put_task_struct(deputy);
            }
        }
        
        if (file) {
            loff_t pos = file->f_pos;
            ssize_t ret;
            const struct cred *old_cred = NULL;
            void *read_buf = kmalloc(req->count, GFP_KERNEL);
            
            if (read_buf) {
                if (deputy) {
                    if (deputy->mm) kthread_use_mm(deputy->mm);
                    old_cred = override_creds(deputy->cred);
                }
                
                ret = kernel_read(file, read_buf, req->count, &pos);
                
                if (deputy) {
                    revert_creds(old_cred);
                    if (deputy->mm) kthread_unuse_mm(deputy->mm);
                }
                
                if (ret >= 0) {
                    file->f_pos = pos;
                }

                // Prepare and send reply
                size_t reply_size = sizeof(struct mattx_sys_read_reply) + (ret > 0 ? ret : 0);
                struct mattx_sys_read_reply *reply = kmalloc(reply_size, GFP_KERNEL);
                if (reply) {
                    reply->orig_pid = req->orig_pid;
                    reply->bytes_read = ret;
                    reply->error = (ret < 0) ? ret : 0;
                    if (ret > 0) {
                        memcpy(reply->data, read_buf, ret);
                    }
                    mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_READ_REPLY, reply, reply_size);
                    kfree(reply);
                }
                kfree(read_buf);
            }
            fput(file); 
        } else {
            // Send error reply
            struct mattx_sys_read_reply reply;
            reply.orig_pid = req->orig_pid;
            reply.bytes_read = -EBADF;
            reply.error = -EBADF;
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_READ_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_read_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_read_reply *reply = (struct mattx_sys_read_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received READ_REPLY for Orig PID %u. Bytes read: %zd\n", reply->orig_pid, reply->bytes_read);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                guest_registry[i].rpc_read_bytes = reply->bytes_read;
                
                if (reply->bytes_read > 0) {
                    guest_registry[i].rpc_read_buf = kmalloc(reply->bytes_read, GFP_ATOMIC);
                    if (guest_registry[i].rpc_read_buf) {
                        memcpy(guest_registry[i].rpc_read_buf, reply->data, reply->bytes_read);
                    } else {
                        guest_registry[i].rpc_read_bytes = -ENOMEM;
                    }
                } else {
                    guest_registry[i].rpc_read_buf = NULL;
                }

                guest_registry[i].rpc_done = true;
                
                if (guest_registry[i].rpc_wq) {
                    wake_up_interruptible(guest_registry[i].rpc_wq);
                }
                break;
            }
        }
        spin_unlock(&guest_lock);
    }
}

void mattx_fileio_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_SYSCALL_FWD, handle_syscall_fwd);
    mattx_register_handler(MATTX_MSG_SYS_OPEN_REQ, handle_sys_open_req);
    mattx_register_handler(MATTX_MSG_SYS_OPEN_REPLY, handle_sys_open_reply);
    mattx_register_handler(MATTX_MSG_SYS_CLOSE_REQ, handle_sys_close_req);
    mattx_register_handler(MATTX_MSG_SYS_READ_REQ, handle_sys_read_req);
    mattx_register_handler(MATTX_MSG_SYS_READ_REPLY, handle_sys_read_reply);
    printk(KERN_INFO "MattX: [FILEIO] Network handlers registered.\n");
}

