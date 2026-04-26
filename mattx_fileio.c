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

#include <linux/version.h>

// --- NEW: The Fake File Getattr Operation (Runs on Node 2) ---
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int mattx_fake_getattr(struct mnt_idmap *idmap, const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags) {
#else
static int mattx_fake_getattr(struct user_namespace *mnt_userns, const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags) {
#endif
    struct file *file = NULL;
    struct mattx_fake_fd_info *fd_info = NULL;
    struct mattx_sys_statx_req req;
    DECLARE_WAIT_QUEUE_HEAD_ONSTACK(rpc_wq);
    pid_t my_pid = current->pid;
    int i;
    int ret_error = -EIO;

    // We need to find the file associated with this dentry to get the private_data
    // Since this is an anonymous inode, we can check if it belongs to current process
    rcu_read_lock();
    if (current->files) {
        struct fdtable *fdt = files_fdtable(current->files);
        for (i = 0; i < fdt->max_fds; i++) {
            file = rcu_dereference(fdt->fd[i]);
            if (file && file->f_path.dentry == path->dentry && file->f_op == &mattx_fops) {
                fd_info = file->private_data;
                break;
            }
        }
    }
    rcu_read_unlock();

    if (!fd_info || !cluster_map[fd_info->home_node]) return -EIO;

    req.orig_pid = fd_info->orig_pid;
    req.fd = fd_info->remote_fd;
    req.mask = request_mask;
    req.flags = query_flags;

    // 1. Attach our wait queue to the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            guest_registry[i].rpc_wq = &rpc_wq;
            guest_registry[i].rpc_done = false;
            guest_registry[i].rpc_statx_buf = NULL; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d requesting GETATTR for FD %u. Sleeping...\n", my_pid, req.fd);

    // 2. Send the request to Node 1
    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_STATX_REQ, &req, sizeof(req));

    // 3. Go to sleep until Node 1 replies!
    wait_event_interruptible(rpc_wq, check_rpc_done(my_pid));

    // 4. We woke up! Collect the data from the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            if (guest_registry[i].rpc_statx_buf) {
                struct statx *s = guest_registry[i].rpc_statx_buf;
                
                // Manually convert statx back to kstat for the VFS layer
                stat->result_mask = s->stx_mask;
                stat->blksize = s->stx_blksize;
                stat->attributes = s->stx_attributes;
                stat->nlink = s->stx_nlink;
                stat->uid = make_kuid(current_user_ns(), s->stx_uid);
                stat->gid = make_kgid(current_user_ns(), s->stx_gid);
                stat->mode = s->stx_mode;
                stat->ino = s->stx_ino;
                stat->size = s->stx_size;
                stat->blocks = s->stx_blocks;
                stat->attributes_mask = s->stx_attributes_mask;
                stat->atime.tv_sec = s->stx_atime.tv_sec;
                stat->atime.tv_nsec = s->stx_atime.tv_nsec;
                stat->btime.tv_sec = s->stx_btime.tv_sec;
                stat->btime.tv_nsec = s->stx_btime.tv_nsec;
                stat->ctime.tv_sec = s->stx_ctime.tv_sec;
                stat->ctime.tv_nsec = s->stx_ctime.tv_nsec;
                stat->mtime.tv_sec = s->stx_mtime.tv_sec;
                stat->mtime.tv_nsec = s->stx_mtime.tv_nsec;
                stat->rdev = MKDEV(s->stx_rdev_major, s->stx_rdev_minor);
                stat->dev = MKDEV(s->stx_dev_major, s->stx_dev_minor);
                
                ret_error = 0;
                kfree(s);
            } else {
                ret_error = -EBADF;
            }
            guest_registry[i].rpc_wq = NULL;
            guest_registry[i].rpc_statx_buf = NULL;
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d woke up! GETATTR result: %d (Size: %lld).\n", my_pid, ret_error, stat->size);
    return ret_error;
}

const struct inode_operations mattx_iops = {
    .getattr = mattx_fake_getattr,
};

// --- NEW: The Fake File LSeek Operation (Runs on Node 2) ---
static loff_t mattx_fake_llseek(struct file *file, loff_t offset, int whence) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    struct mattx_sys_lseek_req req;
    DECLARE_WAIT_QUEUE_HEAD_ONSTACK(rpc_wq);
    pid_t my_pid = current->pid;
    int i;
    loff_t ret_offset = -EINVAL;

    if (!fd_info || !cluster_map[fd_info->home_node]) return -EIO;

    req.orig_pid = fd_info->orig_pid;
    req.fd = fd_info->remote_fd;
    req.offset = offset;
    req.whence = whence;

    // 1. Attach our wait queue to the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            guest_registry[i].rpc_wq = &rpc_wq;
            guest_registry[i].rpc_done = false;
            guest_registry[i].rpc_lseek_res = -1;
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d seeking FD %u (offset %lld, whence %d). Sleeping...\n", my_pid, req.fd, offset, whence);

    // 2. Send the request to Node 1
    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_LSEEK_REQ, &req, sizeof(req));

    // 3. Go to sleep until Node 1 replies!
    wait_event_interruptible(rpc_wq, check_rpc_done(my_pid));

    // 4. We woke up! Collect the data from the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            ret_offset = guest_registry[i].rpc_lseek_res;
            guest_registry[i].rpc_wq = NULL;
            break;
        }
    }
    spin_unlock(&guest_lock);

    // 5. Update local f_pos
    if (ret_offset >= 0) {
        file->f_pos = ret_offset;
    }

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d woke up! Seek result: %lld.\n", my_pid, ret_offset);
    return ret_offset;
}

// --- NEW: The Fake File FSync Operation (Runs on Node 2) ---
static int mattx_fake_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    struct mattx_sys_fsync_req req;
    DECLARE_WAIT_QUEUE_HEAD_ONSTACK(rpc_wq);
    pid_t my_pid = current->pid;
    int i;
    int ret_error = -EIO;

    if (!fd_info || !cluster_map[fd_info->home_node]) return -EIO;

    req.orig_pid = fd_info->orig_pid;
    req.fd = fd_info->remote_fd;
    req.start = start;
    req.end = end;
    req.datasync = datasync;

    // 1. Attach our wait queue to the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            guest_registry[i].rpc_wq = &rpc_wq;
            guest_registry[i].rpc_done = false;
            guest_registry[i].rpc_fsync_res = -1;
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d requesting FSYNC for FD %u. Sleeping...\n", my_pid, req.fd);

    // 2. Send the request to Node 1
    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_FSYNC_REQ, &req, sizeof(req));

    // 3. Go to sleep until Node 1 replies!
    wait_event_interruptible(rpc_wq, check_rpc_done(my_pid));

    // 4. We woke up! Collect the data from the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            ret_error = guest_registry[i].rpc_fsync_res;
            guest_registry[i].rpc_wq = NULL;
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d woke up! FSYNC result: %d.\n", my_pid, ret_error);
    return ret_error;
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
    .llseek = mattx_fake_llseek,
    .fsync = mattx_fake_fsync,
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

static void handle_sys_lseek_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_lseek_req *req = (struct mattx_sys_lseek_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        struct mattx_sys_lseek_reply reply;
        
        reply.orig_pid = req->orig_pid;
        reply.result_offset = -EBADF;
        reply.error = -EBADF;

        printk(KERN_INFO "MattX:[WORMHOLE] Received LSEEK req for FD %u from Node %u (offset %lld, whence %d)\n", req->fd, hdr->sender_id, req->offset, req->whence);

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
            loff_t res = vfs_llseek(file, req->offset, req->whence);
            reply.result_offset = res;
            reply.error = (res < 0) ? res : 0;
            fput(file); 
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_LSEEK_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_statx_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_statx_req *req = (struct mattx_sys_statx_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        struct mattx_sys_statx_reply reply;
        
        reply.orig_pid = req->orig_pid;
        reply.error = -EBADF;
        memset(&reply.statx_buf, 0, sizeof(reply.statx_buf));

        printk(KERN_INFO "MattX:[WORMHOLE] Received STATX req for FD %u from Node %u\n", req->fd, hdr->sender_id);

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
            struct kstat stat;
            int err;
            const struct cred *old_cred = NULL;
            
            if (deputy) {
                if (deputy->mm) kthread_use_mm(deputy->mm);
                old_cred = override_creds(deputy->cred);
            }

            // Clean the flags! vfs_getattr only tolerates the sync flags when called directly on a struct path.
            // Other flags like AT_EMPTY_PATH will cause an immediate -EINVAL or -EPERM!
            err = vfs_getattr(&file->f_path, &stat, req->mask, req->flags & AT_STATX_SYNC_TYPE);
            reply.error = err;
            
            if (deputy) {
                revert_creds(old_cred);
                if (deputy->mm) kthread_unuse_mm(deputy->mm);
            }

            if (err == 0) {
                reply.statx_buf.stx_mask = stat.result_mask;
                reply.statx_buf.stx_blksize = stat.blksize;
                reply.statx_buf.stx_attributes = stat.attributes;
                reply.statx_buf.stx_nlink = stat.nlink;
                reply.statx_buf.stx_uid = from_kuid_munged(current_user_ns(), stat.uid);
                reply.statx_buf.stx_gid = from_kgid_munged(current_user_ns(), stat.gid);
                reply.statx_buf.stx_mode = stat.mode;
                reply.statx_buf.stx_ino = stat.ino;
                reply.statx_buf.stx_size = stat.size;
                reply.statx_buf.stx_blocks = stat.blocks;
                reply.statx_buf.stx_attributes_mask = stat.attributes_mask;
                reply.statx_buf.stx_atime.tv_sec = stat.atime.tv_sec;
                reply.statx_buf.stx_atime.tv_nsec = stat.atime.tv_nsec;
                reply.statx_buf.stx_btime.tv_sec = stat.btime.tv_sec;
                reply.statx_buf.stx_btime.tv_nsec = stat.btime.tv_nsec;
                reply.statx_buf.stx_ctime.tv_sec = stat.ctime.tv_sec;
                reply.statx_buf.stx_ctime.tv_nsec = stat.ctime.tv_nsec;
                reply.statx_buf.stx_mtime.tv_sec = stat.mtime.tv_sec;
                reply.statx_buf.stx_mtime.tv_nsec = stat.mtime.tv_nsec;
                reply.statx_buf.stx_rdev_major = MAJOR(stat.rdev);
                reply.statx_buf.stx_rdev_minor = MINOR(stat.rdev);
                reply.statx_buf.stx_dev_major = MAJOR(stat.dev);
                reply.statx_buf.stx_dev_minor = MINOR(stat.dev);
            }
            fput(file); 
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_STATX_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_dup_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_dup_req *req = (struct mattx_sys_dup_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        struct mattx_sys_dup_reply reply;
        
        reply.orig_pid = req->orig_pid;
        reply.new_remote_fd = -EBADF;
        reply.error = -EBADF;

        printk(KERN_INFO "MattX:[WORMHOLE] Received DUP req for Remote FD %u from Node %u\n", req->old_remote_fd, hdr->sender_id);

        if (req->old_remote_fd >= 1000) {
            int slot = req->old_remote_fd - 1000;
            spin_lock(&export_lock);
            for (i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == req->orig_pid) {
                    if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                        file = export_registry[i].remote_files[slot];
                        get_file(file); 
                        
                        // It's an exported file! We need to find an empty slot in our export registry for the clone.
                        int j;
                        for (j = 0; j < MAX_FDS; j++) {
                            if (export_registry[i].remote_files[j] == NULL) {
                                export_registry[i].remote_files[j] = file;
                                reply.new_remote_fd = j + 1000;
                                reply.error = 0;
                                printk(KERN_INFO "MattX:[WORMHOLE] Duplicated Exported FD %u to %d\n", req->old_remote_fd, reply.new_remote_fd);
                                break;
                            }
                        }
                        if (reply.new_remote_fd < 0) {
                            fput(file); // No space left
                            reply.error = -EMFILE;
                        }
                    }
                    break;
                }
            }
            spin_unlock(&export_lock);
        } else {
            // It's a real file descriptor on the Deputy!
            rcu_read_lock();
            deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();
            
            if (deputy) {
                struct files_struct *files = deputy->files;
                if (files) {
                    spin_lock(&files->file_lock);
                    struct fdtable *fdt = files_fdtable(files);
                    if (req->old_remote_fd < fdt->max_fds) {
                        file = rcu_dereference_raw(fdt->fd[req->old_remote_fd]);
                        if (file) {
                            get_file(file); 
                            
                            // Let's allocate a real new FD on the Deputy!
                            int new_fd = req->new_local_fd;
                            if (new_fd < 0) {
                                // Dynamic dup() -> Find first free slot
                                int j;
                                for (j = 3; j < fdt->max_fds; j++) {
                                    if (!rcu_dereference_raw(fdt->fd[j])) {
                                        new_fd = j;
                                        break;
                                    }
                                }
                            }

                            if (new_fd >= 0 && new_fd < fdt->max_fds) {
                                // Overwrite the slot if dup2() requested an existing open one
                                struct file *old_target = rcu_dereference_raw(fdt->fd[new_fd]);
                                if (old_target) {
                                    fput(old_target);
                                }
                                rcu_assign_pointer(fdt->fd[new_fd], file);
                                __set_bit(new_fd, fdt->open_fds);
                                reply.new_remote_fd = new_fd;
                                reply.error = 0;
                                printk(KERN_INFO "MattX:[WORMHOLE] Duplicated Deputy FD %u to %d\n", req->old_remote_fd, reply.new_remote_fd);
                            } else {
                                fput(file);
                                reply.error = -EMFILE;
                            }
                        }
                    }
                    spin_unlock(&files->file_lock);
                }
                put_task_struct(deputy);
            }
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_DUP_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_fsync_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_fsync_req *req = (struct mattx_sys_fsync_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        struct mattx_sys_fsync_reply reply;
        
        reply.orig_pid = req->orig_pid;
        reply.error = -EBADF;

        printk(KERN_INFO "MattX:[WORMHOLE] Received FSYNC req for FD %u from Node %u\n", req->fd, hdr->sender_id);

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
            const struct cred *old_cred = NULL;
            
            if (deputy) {
                if (deputy->mm) kthread_use_mm(deputy->mm);
                old_cred = override_creds(deputy->cred);
            }

            reply.error = vfs_fsync_range(file, req->start, req->end, req->datasync);
            
            if (deputy) {
                revert_creds(old_cred);
                if (deputy->mm) kthread_unuse_mm(deputy->mm);
            }

            fput(file); 
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_FSYNC_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_socket_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_socket_req *req = (struct mattx_sys_socket_req *)payload;
        struct mattx_sys_socket_reply reply;
        struct socket *sock = NULL;
        struct task_struct *deputy = NULL;
        int remote_fd = -1;
        int i, j;

        printk(KERN_INFO "MattX:[NETWORK] Received SOCKET request from Node %u (domain: %d, type: %d, protocol: %d)\n", 
               hdr->sender_id, req->domain, req->type, req->protocol);

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            const struct cred *old_cred;
            if (deputy->mm) kthread_use_mm(deputy->mm);
            old_cred = override_creds(deputy->cred);

            // Create the real socket on VM1
            int err = sock_create(req->domain, req->type, req->protocol, &sock);
            
            revert_creds(old_cred);
            if (deputy->mm) kthread_unuse_mm(deputy->mm);

            if (err < 0) {
                printk(KERN_ERR "MattX:[NETWORK] Failed to create socket on Home Node (err: %d)\n", err);
                reply.error = err;
            } else {
                // Map the newly created socket to a file and store it in the export registry
                struct file *filp = sock_alloc_file(sock, 0, NULL);
                if (IS_ERR(filp)) {
                    sock_release(sock);
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
            }
            put_task_struct(deputy);
        } else {
            reply.error = -ESRCH;
        }

        reply.orig_pid = req->orig_pid;
        reply.remote_fd = remote_fd;

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_SOCKET_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_connect_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_connect_req *req = (struct mattx_sys_connect_req *)payload;
        struct mattx_sys_connect_reply reply;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;

        reply.orig_pid = req->orig_pid;
        reply.error = -EBADF;

        printk(KERN_INFO "MattX:[NETWORK] Received CONNECT request for FD %u from Node %u\n", req->fd, hdr->sender_id);

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
        }

        if (file) {
            struct socket *sock = sock_from_file(file);
            if (sock) {
                const struct cred *old_cred = NULL;
                
                rcu_read_lock();
                deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    if (deputy->mm) kthread_use_mm(deputy->mm);
                    old_cred = override_creds(deputy->cred);
                }

                // Execute the connect operation on the real socket!
                reply.error = sock->ops->connect(sock, (struct sockaddr *)&req->addr, req->addrlen, file->f_flags);

                if (deputy) {
                    revert_creds(old_cred);
                    if (deputy->mm) kthread_unuse_mm(deputy->mm);
                    put_task_struct(deputy);
                }
            } else {
                reply.error = -ENOTSOCK;
            }
            fput(file); 
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_CONNECT_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_lseek_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_lseek_reply *reply = (struct mattx_sys_lseek_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received LSEEK_REPLY for Orig PID %u. Result: %lld\n", reply->orig_pid, reply->result_offset);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                guest_registry[i].rpc_lseek_res = reply->result_offset;
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

static void handle_sys_statx_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_statx_reply *reply = (struct mattx_sys_statx_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received STATX_REPLY for Orig PID %u. Error: %d\n", reply->orig_pid, reply->error);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                if (reply->error == 0) {
                    guest_registry[i].rpc_statx_buf = kmalloc(sizeof(struct statx), GFP_ATOMIC);
                    if (guest_registry[i].rpc_statx_buf) {
                        memcpy(guest_registry[i].rpc_statx_buf, &reply->statx_buf, sizeof(struct statx));
                    }
                } else {
                    guest_registry[i].rpc_statx_buf = NULL;
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

static void handle_sys_dup_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_dup_reply *reply = (struct mattx_sys_dup_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received DUP_REPLY for Orig PID %u. New Remote FD: %d\n", reply->orig_pid, reply->new_remote_fd);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                guest_registry[i].rpc_remote_fd = reply->new_remote_fd;
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

static void handle_sys_fsync_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_fsync_reply *reply = (struct mattx_sys_fsync_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received FSYNC_REPLY for Orig PID %u. Error: %d\n", reply->orig_pid, reply->error);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                guest_registry[i].rpc_fsync_res = reply->error;
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

static void handle_sys_socket_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_socket_reply *reply = (struct mattx_sys_socket_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received SOCKET_REPLY for Orig PID %u. Remote FD: %d\n", reply->orig_pid, reply->remote_fd);

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

static void handle_sys_connect_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_connect_reply *reply = (struct mattx_sys_connect_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received CONNECT_REPLY for Orig PID %u. Error: %d\n", reply->orig_pid, reply->error);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                // We reuse rpc_fsync_res to store generic error codes for now to save space
                guest_registry[i].rpc_fsync_res = reply->error;
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

static void handle_sys_bind_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_bind_req *req = (struct mattx_sys_bind_req *)payload;
        struct mattx_sys_bind_reply reply;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;

        reply.orig_pid = req->orig_pid;
        reply.error = -EBADF;

        printk(KERN_INFO "MattX:[NETWORK] Received BIND request for FD %u from Node %u\n", req->fd, hdr->sender_id);

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
        }

        if (file) {
            struct socket *sock = sock_from_file(file);
            if (sock && sock->ops && sock->ops->bind) {
                const struct cred *old_cred = NULL;
                
                rcu_read_lock();
                deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    if (deputy->mm) kthread_use_mm(deputy->mm);
                    old_cred = override_creds(deputy->cred);
                }

                reply.error = sock->ops->bind(sock, (struct sockaddr *)&req->addr, req->addrlen);

                if (deputy) {
                    revert_creds(old_cred);
                    if (deputy->mm) kthread_unuse_mm(deputy->mm);
                    put_task_struct(deputy);
                }
            } else {
                reply.error = -ENOTSOCK;
            }
            fput(file); 
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_BIND_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_listen_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_listen_req *req = (struct mattx_sys_listen_req *)payload;
        struct mattx_sys_listen_reply reply;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;

        reply.orig_pid = req->orig_pid;
        reply.error = -EBADF;

        printk(KERN_INFO "MattX:[NETWORK] Received LISTEN request for FD %u from Node %u\n", req->fd, hdr->sender_id);

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
        }

        if (file) {
            struct socket *sock = sock_from_file(file);
            if (sock && sock->ops && sock->ops->listen) {
                const struct cred *old_cred = NULL;
                
                rcu_read_lock();
                deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    if (deputy->mm) kthread_use_mm(deputy->mm);
                    old_cred = override_creds(deputy->cred);
                }

                reply.error = sock->ops->listen(sock, req->backlog);

                if (deputy) {
                    revert_creds(old_cred);
                    if (deputy->mm) kthread_unuse_mm(deputy->mm);
                    put_task_struct(deputy);
                }
            } else {
                reply.error = -ENOTSOCK;
            }
            fput(file); 
        }
        
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_LISTEN_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_generic_int_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // We can reuse this logic for bind, listen, connect, fsync since they all return an integer
    // The payload structs happen to have 'orig_pid' and 'error' in the exact same memory layout
    if (payload) {
        struct mattx_sys_connect_reply *reply = (struct mattx_sys_connect_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received Integer REPLY (Err: %d) for Orig PID %u\n", reply->error, reply->orig_pid);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                guest_registry[i].rpc_fsync_res = reply->error;
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

static void handle_sys_send_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_send_req *req = (struct mattx_sys_send_req *)payload;
        struct mattx_sys_send_reply reply;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;

        reply.orig_pid = req->orig_pid;
        reply.bytes_sent = -EBADF;
        reply.error = -EBADF;

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
        }

        if (file) {
            struct socket *sock = sock_from_file(file);
            if (sock) {
                struct msghdr msg = {0};
                struct kvec iov;
                const struct cred *old_cred = NULL;

                rcu_read_lock();
                deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    if (deputy->mm) kthread_use_mm(deputy->mm);
                    old_cred = override_creds(deputy->cred);
                }

                iov.iov_base = req->data;
                iov.iov_len = req->len;
                
                reply.bytes_sent = kernel_sendmsg(sock, &msg, &iov, 1, req->len);
                reply.error = (reply.bytes_sent < 0) ? reply.bytes_sent : 0;

                if (deputy) {
                    revert_creds(old_cred);
                    if (deputy->mm) kthread_unuse_mm(deputy->mm);
                    put_task_struct(deputy);
                }
            } else {
                reply.bytes_sent = -ENOTSOCK;
                reply.error = -ENOTSOCK;
            }
            fput(file);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_SEND_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_recv_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_recv_req *req = (struct mattx_sys_recv_req *)payload;
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
        }

        if (file) {
            struct socket *sock = sock_from_file(file);
            if (sock) {
                struct msghdr msg = {0};
                struct kvec iov;
                const struct cred *old_cred = NULL;
                void *recv_buf = kmalloc(req->size, GFP_KERNEL);

                if (recv_buf) {
                    rcu_read_lock();
                    deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                    if (deputy) get_task_struct(deputy);
                    rcu_read_unlock();

                    if (deputy) {
                        if (deputy->mm) kthread_use_mm(deputy->mm);
                        old_cred = override_creds(deputy->cred);
                    }

                    iov.iov_base = recv_buf;
                    iov.iov_len = req->size;
                    
                    ssize_t ret = kernel_recvmsg(sock, &msg, &iov, 1, req->size, req->flags);

                    if (deputy) {
                        revert_creds(old_cred);
                        if (deputy->mm) kthread_unuse_mm(deputy->mm);
                        put_task_struct(deputy);
                    }

                    size_t reply_size = sizeof(struct mattx_sys_recv_reply) + (ret > 0 ? ret : 0);
                    struct mattx_sys_recv_reply *reply = kmalloc(reply_size, GFP_KERNEL);
                    if (reply) {
                        reply->orig_pid = req->orig_pid;
                        reply->bytes_recv = ret;
                        reply->error = (ret < 0) ? ret : 0;
                        if (ret > 0) {
                            memcpy(reply->data, recv_buf, ret);
                        }
                        if (cluster_map[hdr->sender_id]) {
                            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_RECV_REPLY, reply, reply_size);
                        }
                        kfree(reply);
                    }
                    kfree(recv_buf);
                }
            } else {
                struct mattx_sys_recv_reply reply;
                reply.orig_pid = req->orig_pid;
                reply.bytes_recv = -ENOTSOCK;
                reply.error = -ENOTSOCK;
                if (cluster_map[hdr->sender_id]) {
                    mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_RECV_REPLY, &reply, sizeof(reply));
                }
            }
            fput(file);
        } else {
            struct mattx_sys_recv_reply reply;
            reply.orig_pid = req->orig_pid;
            reply.bytes_recv = -EBADF;
            reply.error = -EBADF;
            if (cluster_map[hdr->sender_id]) {
                mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_RECV_REPLY, &reply, sizeof(reply));
            }
        }
    }
}

static void handle_sys_recv_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_recv_reply *reply = (struct mattx_sys_recv_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received RECV_REPLY for Orig PID %u. Bytes recv: %zd\n", reply->orig_pid, reply->bytes_recv);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                guest_registry[i].rpc_read_bytes = reply->bytes_recv; 
                
                if (reply->bytes_recv > 0) {
                    guest_registry[i].rpc_read_buf = kmalloc(reply->bytes_recv, GFP_ATOMIC);
                    if (guest_registry[i].rpc_read_buf) {
                        memcpy(guest_registry[i].rpc_read_buf, reply->data, reply->bytes_recv);
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

// Background Worker for Accept (Runs on Node 1) ---
static void mattx_accept_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    struct mattx_sys_accept_reply reply;
    struct file *file = NULL;
    struct task_struct *deputy = NULL;
    int i, j;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = rpc->orig_pid;
    reply.remote_fd = -EBADF;
    reply.error = -EBADF;

    printk(KERN_INFO "MattX:[RPC] Accept Worker started for Deputy PID %u\n", rpc->orig_pid);

    if (rpc->remote_fd >= 1000) {
        int slot = rpc->remote_fd - 1000;
        spin_lock(&export_lock);
        for (i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == rpc->orig_pid) {
                if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                    file = export_registry[i].remote_files[slot];
                    get_file(file); 
                }
                break;
            }
        }
        spin_unlock(&export_lock);
    } else {
        // look up native FDs!
        rcu_read_lock();
        deputy = pid_task(find_vpid(rpc->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();
        
        if (deputy) {
            struct files_struct *files = deputy->files;
            if (files) {
                spin_lock(&files->file_lock);
                struct fdtable *fdt = files_fdtable(files);
                if (rpc->remote_fd < fdt->max_fds) {
                    file = rcu_dereference_raw(fdt->fd[rpc->remote_fd]);
                    if (file) get_file(file); 
                }
                spin_unlock(&files->file_lock);
            }
            put_task_struct(deputy);
            deputy = NULL; // Reset deputy pointer for the next block!
        }
    }

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock && sock->ops && sock->ops->accept) {
            const struct cred *old_cred = NULL;
            
            rcu_read_lock();
            deputy = pid_task(find_vpid(rpc->orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();

            if (deputy) {
                if (deputy->mm) kthread_use_mm(deputy->mm);
                old_cred = override_creds(deputy->cred);
            }

            // The Clean Accept ---
            // We do NOT use sock_create here, because it initializes the internal 'sk' struct.
            // inet_accept expects a completely blank socket shell!
            struct socket *newsock = sock_alloc();
            if (newsock) {
                struct proto_accept_arg accept_arg = {
                    .flags = rpc->flags,
                    .kern = true,
                };
                
                newsock->type = sock->type;
                newsock->ops = sock->ops;
                
                // THIS WILL SAFELY BLOCK UNTIL A CONNECTION ARRIVES!
                int err = sock->ops->accept(sock, newsock, &accept_arg);
                
                if (err == 0) {
                    if (newsock->ops->getname) {
                        reply.addrlen = newsock->ops->getname(newsock, (struct sockaddr *)&reply.addr, 1);
                    }

                    struct file *newfilp = sock_alloc_file(newsock, 0, NULL);
                    if (!IS_ERR(newfilp)) {
                        spin_lock(&export_lock);
                        for (i = 0; i < export_count; i++) {
                            if (export_registry[i].orig_pid == rpc->orig_pid) {
                                for (j = 0; j < MAX_FDS; j++) {
                                    if (export_registry[i].remote_files[j] == NULL) {
                                        export_registry[i].remote_files[j] = newfilp;
                                        reply.remote_fd = j + 1000; 
                                        reply.error = 0;
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        spin_unlock(&export_lock);
                        
                        if (reply.remote_fd == -EBADF) {
                            fput(newfilp);
                            reply.error = -ENFILE;
                        }
                    } else {
                        sock_release(newsock);
                        reply.error = PTR_ERR(newfilp);
                    }
                } else {
                    sock_release(newsock);
                    reply.error = err;
                }
            } else {
                reply.error = -ENOMEM;
            }

            if (deputy) {
                revert_creds(old_cred);
                if (deputy->mm) kthread_unuse_mm(deputy->mm);
                put_task_struct(deputy);
            }
        } else {
            reply.error = -ENOTSOCK;
        }
        fput(file); 
    }

    if (cluster_map[rpc->home_node]) {
        printk(KERN_INFO "MattX:[RPC] Sending ACCEPT_REPLY (Remote FD: %d) back to Node %u...\n", reply.remote_fd, rpc->home_node);
        mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_ACCEPT_REPLY, &reply, sizeof(reply));
    }
    
    kfree(rpc);
}

static void handle_sys_accept_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_accept_req *req = (struct mattx_sys_accept_req *)payload;
        
        printk(KERN_INFO "MattX:[RPC] Received ACCEPT request from Node %u. Escaping to Workqueue...\n", hdr->sender_id);

        // Throw the blocking accept call onto a background worker!
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC);
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_accept_worker);
            rpc->orig_pid = req->orig_pid;
            rpc->remote_fd = req->fd;
            rpc->flags = req->flags;
            rpc->home_node = hdr->sender_id;
            schedule_work(&rpc->work);
        }
    }
}

static void handle_sys_accept_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_accept_reply *reply = (struct mattx_sys_accept_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received ACCEPT_REPLY for Orig PID %u. New Remote FD: %d\n", reply->orig_pid, reply->remote_fd);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                if (reply->error == 0) {
                    guest_registry[i].rpc_remote_fd = reply->remote_fd;
                    
                    // We reuse read_buf to temporarily store the client's IP address
                    guest_registry[i].rpc_read_buf = kmalloc(reply->addrlen, GFP_ATOMIC);
                    if (guest_registry[i].rpc_read_buf) {
                        memcpy(guest_registry[i].rpc_read_buf, &reply->addr, reply->addrlen);
                        guest_registry[i].rpc_fsync_res = reply->addrlen; // Store the length
                    }
                } else {
                    guest_registry[i].rpc_remote_fd = reply->error; // Pass the error code back
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
    mattx_register_handler(MATTX_MSG_SYS_LSEEK_REQ, handle_sys_lseek_req);
    mattx_register_handler(MATTX_MSG_SYS_LSEEK_REPLY, handle_sys_lseek_reply);
    mattx_register_handler(MATTX_MSG_SYS_STATX_REQ, handle_sys_statx_req);
    mattx_register_handler(MATTX_MSG_SYS_STATX_REPLY, handle_sys_statx_reply);
    mattx_register_handler(MATTX_MSG_SYS_DUP_REQ, handle_sys_dup_req);
    mattx_register_handler(MATTX_MSG_SYS_DUP_REPLY, handle_sys_dup_reply);
    mattx_register_handler(MATTX_MSG_SYS_FSYNC_REQ, handle_sys_fsync_req);
    mattx_register_handler(MATTX_MSG_SYS_FSYNC_REPLY, handle_sys_fsync_reply);
    mattx_register_handler(MATTX_MSG_SYS_SOCKET_REQ, handle_sys_socket_req);
    mattx_register_handler(MATTX_MSG_SYS_SOCKET_REPLY, handle_sys_socket_reply);
    mattx_register_handler(MATTX_MSG_SYS_CONNECT_REQ, handle_sys_connect_req);
    mattx_register_handler(MATTX_MSG_SYS_CONNECT_REPLY, handle_sys_connect_reply);
    mattx_register_handler(MATTX_MSG_SYS_BIND_REQ, handle_sys_bind_req);
    mattx_register_handler(MATTX_MSG_SYS_BIND_REPLY, handle_sys_generic_int_reply);
    mattx_register_handler(MATTX_MSG_SYS_LISTEN_REQ, handle_sys_listen_req);
    mattx_register_handler(MATTX_MSG_SYS_LISTEN_REPLY, handle_sys_generic_int_reply);
    mattx_register_handler(MATTX_MSG_SYS_SEND_REQ, handle_sys_send_req);
    mattx_register_handler(MATTX_MSG_SYS_SEND_REPLY, handle_sys_generic_int_reply); // We can reuse the generic int reply for send!
    mattx_register_handler(MATTX_MSG_SYS_RECV_REQ, handle_sys_recv_req);
    mattx_register_handler(MATTX_MSG_SYS_RECV_REPLY, handle_sys_recv_reply);
    mattx_register_handler(MATTX_MSG_SYS_ACCEPT_REQ, handle_sys_accept_req);
    mattx_register_handler(MATTX_MSG_SYS_ACCEPT_REPLY, handle_sys_accept_reply);
    printk(KERN_INFO "MattX: [FILEIO] Network handlers registered.\n");
}

