#include "mattx.h"
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/namei.h> // For kern_path

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

// Global File Table for MattXFS ---
// This holds the real files opened on behalf of remote MattXFS clients
static struct file *mfs_open_files[MAX_FDS];
static DEFINE_SPINLOCK(mfs_file_lock);

#include <linux/version.h>

// The Fake File Getattr Operation (Runs on Node 2) ---
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
                        // Pass 2 instead of 1 to bypass the strict state check!
                        int addr_len = newsock->ops->getname(newsock, (struct sockaddr *)&reply.addr, 2);
                        // Only save the length if it's a valid positive number!
                        if (addr_len > 0) {
                            reply.addrlen = addr_len;
                        } else {
                            reply.addrlen = 0;
                        }
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
                    
                    // FIXED: Only allocate if addrlen is valid and sane!
                    if (reply->addrlen > 0 && reply->addrlen <= sizeof(struct sockaddr_storage)) {
                        guest_registry[i].rpc_read_buf = kmalloc(reply->addrlen, GFP_ATOMIC);
                        if (guest_registry[i].rpc_read_buf) {
                            memcpy(guest_registry[i].rpc_read_buf, &reply->addr, reply->addrlen);
                            guest_registry[i].rpc_fsync_res = reply->addrlen; 
                        } else {
                            guest_registry[i].rpc_fsync_res = 0;
                        }
                    } else {
                        guest_registry[i].rpc_fsync_res = 0;
                    }
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


static void mattx_poll_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    struct mattx_sys_poll_reply reply;
    int i;
    unsigned long expire;
    bool has_timeout = (rpc->timeout >= 0);
    
    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = rpc->orig_pid;
    reply.nfds = rpc->nfds;
    memcpy(reply.fds, rpc->poll_fds, sizeof(struct mattx_pollfd) * rpc->nfds);

    printk(KERN_INFO "MattX:[RPC] Poll Worker started for Deputy PID %u (Timeout: %dms)\n", rpc->orig_pid, rpc->timeout);

    if (has_timeout) {
        expire = jiffies + msecs_to_jiffies(rpc->timeout);
    }

    // --- THE LAZY POLL LOOP ---
    while (1) {
        int ready_count = 0;
        bool deputy_alive = false;

        // 1. Check if the Deputy is still alive in the export registry!
        spin_lock(&export_lock);
        for (i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == rpc->orig_pid) {
                deputy_alive = true;
                
                // 2. Check every FD in the array
                for (int j = 0; j < rpc->nfds; j++) {
                    int remote_fd = reply.fds[j].fd;
                    if (remote_fd >= 1000) {
                        int slot = remote_fd - 1000;
                        if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                            struct file *f = export_registry[i].remote_files[slot];
                            
                            // vfs_poll with NULL doesn't sleep, it just returns the current state!
                            __poll_t mask = vfs_poll(f, NULL);
                            
                            reply.fds[j].revents = mask & reply.fds[j].events;
                            if (reply.fds[j].revents) {
                                ready_count++;
                            }
                        }
                    }
                }
                break;
            }
        }
        spin_unlock(&export_lock);

        if (!deputy_alive) {
            reply.error = -ESRCH;
            break;
        }

        if (ready_count > 0) {
            reply.retval = ready_count;
            break;
        }

        if (has_timeout && time_after(jiffies, expire)) {
            reply.retval = 0; // Timeout reached, 0 FDs ready
            break;
        }

        // Sleep for 10ms and check again!
        msleep(10);
    }

    if (cluster_map[rpc->home_node]) {
        printk(KERN_INFO "MattX:[RPC] Sending POLL_REPLY (Ready: %d) back to Node %u...\n", reply.retval, rpc->home_node);
        mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_POLL_REPLY, &reply, sizeof(reply));
    }
    
    kfree(rpc);
}

static void handle_sys_poll_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_poll_req *req = (struct mattx_sys_poll_req *)payload;
        
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC);
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_poll_worker);
            rpc->orig_pid = req->orig_pid;
            rpc->home_node = hdr->sender_id;
            rpc->nfds = req->nfds;
            rpc->timeout = req->timeout;
            memcpy(rpc->poll_fds, req->fds, sizeof(struct mattx_pollfd) * req->nfds);
            schedule_work(&rpc->work);
        }
    }
}

static void handle_sys_poll_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_poll_reply *reply = (struct mattx_sys_poll_reply *)payload;
        int i;

        printk(KERN_INFO "MattX:[RPC] Received POLL_REPLY for Orig PID %u. Ready FDs: %d\n", reply->orig_pid, reply->retval);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                guest_registry[i].rpc_fsync_res = reply->retval; // Store the return value
                
                // Allocate a buffer to hold the updated array and pass it to the worker
                guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct mattx_sys_poll_reply), GFP_ATOMIC);
                if (guest_registry[i].rpc_read_buf) {
                    memcpy(guest_registry[i].rpc_read_buf, reply, sizeof(struct mattx_sys_poll_reply));
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


// --- VFS RPC Registry ---
// This allows mattxfs.ko to sleep while waiting for network replies
#define MAX_VFS_RPC 64
struct vfs_rpc_ctx {
    u64 req_id;
    wait_queue_head_t wq;
    bool done;
    int error;
    struct kstat stat;
    struct mattx_dirent *dirents;
    u32 dirent_count;
    u64 new_offset;

    int remote_fd;
    ssize_t bytes_rw;
    void *data_buf;
    loff_t ret_offset;

    bool in_use;    
};

static struct vfs_rpc_ctx vfs_rpc_registry[MAX_VFS_RPC];
static DEFINE_SPINLOCK(vfs_rpc_lock);
static u64 next_req_id = 1;

// This context helps us catch the filenames as the kernel reads the local disk
struct mattx_readdir_ctx {
    struct dir_context ctx;
    struct mattx_vfs_readdir_reply *reply;
};

// The kernel calls this for every file it finds on the hard drive!
// 1. Update the filldir actor to save the offset:
static bool mattx_filldir(struct dir_context *ctx, const char *name, int namlen, loff_t offset, u64 ino, unsigned int d_type) {
    struct mattx_readdir_ctx *mctx = container_of(ctx, struct mattx_readdir_ctx, ctx);
    struct mattx_vfs_readdir_reply *reply = mctx->reply;

    if (reply->entry_count >= 20) return false; 

    if (namlen > 63) namlen = 63; 
    
    reply->entries[reply->entry_count].ino = ino;
    reply->entries[reply->entry_count].offset = offset; // FIXED: Save the offset!
    reply->entries[reply->entry_count].type = d_type;
    memcpy(reply->entries[reply->entry_count].name, name, namlen);
    reply->entries[reply->entry_count].name[namlen] = '\0';
    
    reply->entry_count++;
    return true;
}


// --- API FOR MATTXFS.KO ---
int mattx_rpc_vfs_getattr(int node_id, const char *path, struct kstat *stat_out) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_vfs_getattr_req req;

    // The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct path local_path;
        int err = kern_path(path, LOOKUP_FOLLOW, &local_path);
        if (!err) {
            err = vfs_getattr(&local_path, stat_out, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
            path_put(&local_path);
        }
        return err;
    }

    // If it's not local, it must be a connected remote node!
    if (!cluster_map[node_id]) return -ENOTCONN;

    // 1. Find an empty slot and generate a Request ID
    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) return -EBUSY;

    // 2. Send the request
    req.req_id = req_id;
    strncpy(req.path, path, sizeof(req.path) - 1);
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_GETATTR_REQ, &req, sizeof(req));

    // 3. Sleep until the reply arrives!
    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    // 4. Collect the results
    int err = vfs_rpc_registry[slot].error;
    if (err == 0 && stat_out) {
        *stat_out = vfs_rpc_registry[slot].stat;
    }

    // 5. Free the slot
    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return err;
}
EXPORT_SYMBOL(mattx_rpc_vfs_getattr);

// --- API FOR MATTXFS.KO ---
int mattx_rpc_vfs_readdir(int node_id, const char *path, u64 *offset, struct mattx_dirent *entries, u32 *out_count) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_vfs_readdir_req req;

    // --- The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f = filp_open(path, O_RDONLY | O_DIRECTORY, 0);
        if (IS_ERR(f)) return PTR_ERR(f);
        
        // Allocate on the Heap!
        struct mattx_vfs_readdir_reply *local_reply = kzalloc(sizeof(*local_reply), GFP_KERNEL);
        if (!local_reply) { fput(f); return -ENOMEM; }
        
        f->f_pos = *offset; // Fast-forward the kernel's file pointer!
        
        struct mattx_readdir_ctx mctx = {
            .ctx.actor = mattx_filldir,
            .ctx.pos = f->f_pos,
            .reply = local_reply,
        };
        
        iterate_dir(f, &mctx.ctx);
        
        *offset = mctx.ctx.pos; 
        *out_count = local_reply->entry_count;
        memcpy(entries, local_reply->entries, local_reply->entry_count * sizeof(struct mattx_dirent));
        
        kfree(local_reply);
        fput(f);
        return 0;
    }

    if (!cluster_map[node_id]) return -ENOTCONN;

    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            vfs_rpc_registry[i].dirents = entries; // Point to MattXFS's buffer
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) return -EBUSY;

    req.req_id = req_id;
    req.offset = *offset;
    strncpy(req.path, path, sizeof(req.path) - 1);
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_READDIR_REQ, &req, sizeof(req));

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    int err = vfs_rpc_registry[slot].error;
    if (err == 0) {
        *offset = vfs_rpc_registry[slot].new_offset;
        *out_count = vfs_rpc_registry[slot].dirent_count;
    }

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return err;
}
EXPORT_SYMBOL(mattx_rpc_vfs_readdir);


// --- API FOR MATTXFS.KO (OPEN, READ, WRITE, CLOSE, LSEEK, FSYNC) ---

int mattx_rpc_vfs_open(int node_id, const char *path, int flags, int mode, int *remote_fd) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_vfs_open_req req;

    // --- NEW: The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f = filp_open(path, flags, mode);
        if (IS_ERR(f)) return PTR_ERR(f);
        
        spin_lock(&mfs_file_lock);
        for (i = 0; i < MAX_FDS; i++) {
            if (mfs_open_files[i] == NULL) {
                mfs_open_files[i] = f;
                *remote_fd = i;
                slot = i;
                break;
            }
        }
        spin_unlock(&mfs_file_lock);
        
        if (slot == -1) {
            fput(f);
            return -ENFILE;
        }
        return 0;
    }

    if (!cluster_map[node_id]) return -ENOTCONN;

    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) return -EBUSY;

    req.req_id = req_id;
    req.flags = flags;
    req.mode = mode;
    strncpy(req.path, path, sizeof(req.path) - 1);
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_OPEN_REQ, &req, sizeof(req));

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    int err = vfs_rpc_registry[slot].error;
    if (err == 0) *remote_fd = vfs_rpc_registry[slot].remote_fd;

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return err;
}
EXPORT_SYMBOL(mattx_rpc_vfs_open);

ssize_t mattx_rpc_vfs_read(int node_id, int remote_fd, void *buf, size_t count, loff_t *pos) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_vfs_read_req req;
    ssize_t bytes_read = -EIO;

    // --- NEW: The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f = NULL;
        spin_lock(&mfs_file_lock);
        if (remote_fd < MAX_FDS && mfs_open_files[remote_fd]) {
            f = mfs_open_files[remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);
        
        if (!f) return -EBADF;
        bytes_read = kernel_read(f, buf, count, pos);
        fput(f);
        return bytes_read;
    }

    if (!cluster_map[node_id]) return -ENOTCONN;

    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            vfs_rpc_registry[i].data_buf = buf; 
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) return -EBUSY;

    req.req_id = req_id;
    req.remote_fd = remote_fd;
    req.count = count;
    req.pos = *pos;
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_READ_REQ, &req, sizeof(req));

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    bytes_read = vfs_rpc_registry[slot].bytes_rw;
    if (bytes_read > 0) *pos += bytes_read; 

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return bytes_read;
}
EXPORT_SYMBOL(mattx_rpc_vfs_read);

ssize_t mattx_rpc_vfs_write(int node_id, int remote_fd, const void *buf, size_t count, loff_t *pos) {
    int i, slot = -1;
    u64 req_id;
    size_t payload_size = sizeof(struct mattx_vfs_write_req) + count;
    struct mattx_vfs_write_req *req = kmalloc(payload_size, GFP_KERNEL);
    ssize_t bytes_written = -EIO;

    if (!req) return -ENOMEM;

    // --- NEW: The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f = NULL;
        spin_lock(&mfs_file_lock);
        if (remote_fd < MAX_FDS && mfs_open_files[remote_fd]) {
            f = mfs_open_files[remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);
        
        if (!f) { kfree(req); return -EBADF; }
        bytes_written = kernel_write(f, buf, count, pos);
        fput(f);
        kfree(req);
        return bytes_written;
    }

    if (!cluster_map[node_id]) { kfree(req); return -ENOTCONN; }

    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) { kfree(req); return -EBUSY; }

    req->req_id = req_id;
    req->remote_fd = remote_fd;
    req->count = count;
    req->pos = *pos;
    memcpy(req->data, buf, count); 

    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_WRITE_REQ, req, payload_size);
    kfree(req);

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    bytes_written = vfs_rpc_registry[slot].bytes_rw;
    if (bytes_written > 0) *pos += bytes_written;

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return bytes_written;
}
EXPORT_SYMBOL(mattx_rpc_vfs_write);

loff_t mattx_rpc_vfs_llseek(int node_id, int remote_fd, loff_t offset, int whence) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_vfs_lseek_req req;
    loff_t ret_offset = -EIO;

    // --- NEW: The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f = NULL;
        spin_lock(&mfs_file_lock);
        if (remote_fd < MAX_FDS && mfs_open_files[remote_fd]) {
            f = mfs_open_files[remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);
        
        if (!f) return -EBADF;
        ret_offset = vfs_llseek(f, offset, whence);
        fput(f);
        return ret_offset;
    }

    if (!cluster_map[node_id]) return -ENOTCONN;

    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) return -EBUSY;

    req.req_id = req_id;
    req.remote_fd = remote_fd;
    req.offset = offset;
    req.whence = whence;
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_LSEEK_REQ, &req, sizeof(req));

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    if (vfs_rpc_registry[slot].error == 0) {
        ret_offset = vfs_rpc_registry[slot].ret_offset;
    } else {
        ret_offset = vfs_rpc_registry[slot].error;
    }

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return ret_offset;
}
EXPORT_SYMBOL(mattx_rpc_vfs_llseek);

int mattx_rpc_vfs_fsync(int node_id, int remote_fd, loff_t start, loff_t end, int datasync) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_vfs_fsync_req req;
    int err = -EIO;

    // --- NEW: The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f = NULL;
        spin_lock(&mfs_file_lock);
        if (remote_fd < MAX_FDS && mfs_open_files[remote_fd]) {
            f = mfs_open_files[remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);
        
        if (!f) return -EBADF;
        err = vfs_fsync_range(f, start, end, datasync);
        fput(f);
        return err;
    }

    if (!cluster_map[node_id]) return -ENOTCONN;

    spin_lock(&vfs_rpc_lock);
    for (i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) return -EBUSY;

    req.req_id = req_id;
    req.remote_fd = remote_fd;
    req.start = start;
    req.end = end;
    req.datasync = datasync;
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_FSYNC_REQ, &req, sizeof(req));

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    err = vfs_rpc_registry[slot].error;

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return err;
}
EXPORT_SYMBOL(mattx_rpc_vfs_fsync);

void mattx_rpc_vfs_close(int node_id, int remote_fd) {
    // --- NEW: The Local Fast-Path! ---
    if (node_id == my_node_id) {
        spin_lock(&mfs_file_lock);
        if (remote_fd < MAX_FDS && mfs_open_files[remote_fd]) {
            fput(mfs_open_files[remote_fd]);
            mfs_open_files[remote_fd] = NULL;
        }
        spin_unlock(&mfs_file_lock);
        return;
    }

    struct mattx_vfs_close_req req;
    if (cluster_map[node_id]) {
        req.remote_fd = remote_fd;
        mattx_comm_send(cluster_map[node_id], MATTX_MSG_VFS_CLOSE_REQ, &req, sizeof(req));
    }
}
EXPORT_SYMBOL(mattx_rpc_vfs_close);


// --- NETWORK HANDLERS ---

// Node 2 receives the request, reads its local hard drive, and replies!
static void handle_vfs_getattr_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_getattr_req *req = payload;
        struct mattx_vfs_getattr_reply reply;
        struct path path;
        struct kstat stat;
        int err;

        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;

        // Safely look up the file on the local Ext4 disk
        err = kern_path(req->path, LOOKUP_FOLLOW, &path);
        if (!err) {
            err = vfs_getattr(&path, &stat, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
            if (!err) {
                reply.mode = stat.mode;
                reply.size = stat.size;
                reply.blocks = stat.blocks;
                reply.blksize = stat.blksize;
                reply.uid = from_kuid_munged(current_user_ns(), stat.uid);
                reply.gid = from_kgid_munged(current_user_ns(), stat.gid);
                reply.nlink = stat.nlink;
            }
            path_put(&path);
        }
        reply.error = err;

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_GETATTR_REPLY, &reply, sizeof(reply));
        }
    }
}

// Node 1 receives the reply and wakes up the sleeping MattXFS thread!
static void handle_vfs_getattr_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_getattr_reply *reply = payload;
        int i;

        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].error = reply->error;
                if (reply->error == 0) {
                    vfs_rpc_registry[i].stat.mode = reply->mode;
                    vfs_rpc_registry[i].stat.size = reply->size;
                    vfs_rpc_registry[i].stat.blocks = reply->blocks;
                    vfs_rpc_registry[i].stat.blksize = reply->blksize;
                    vfs_rpc_registry[i].stat.uid = make_kuid(current_user_ns(), reply->uid);
                    vfs_rpc_registry[i].stat.gid = make_kgid(current_user_ns(), reply->gid);
                    vfs_rpc_registry[i].stat.nlink = reply->nlink;
                }
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

static void handle_vfs_readdir_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_readdir_req *req = payload;
        // FIXED: Allocate on the Heap!
        struct mattx_vfs_readdir_reply *reply = kzalloc(sizeof(*reply), GFP_KERNEL);
        struct file *f;

        if (!reply) return;
        reply->req_id = req->req_id;

        f = filp_open(req->path, O_RDONLY | O_DIRECTORY, 0);
        if (!IS_ERR(f)) {
            f->f_pos = req->offset; // FIXED: Fast-forward the kernel's file pointer!
            
            struct mattx_readdir_ctx mctx = {
                .ctx.actor = mattx_filldir,
                .ctx.pos = f->f_pos,
                .reply = reply,
            };
            
            iterate_dir(f, &mctx.ctx);
            reply->new_offset = mctx.ctx.pos;
            fput(f);
            reply->error = 0;
        } else {
            reply->error = PTR_ERR(f);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_READDIR_REPLY, reply, sizeof(*reply));
        }
        kfree(reply);
    }
}

static void handle_vfs_readdir_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_readdir_reply *reply = payload;
        int i;

        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].error = reply->error;
                if (reply->error == 0) {
                    vfs_rpc_registry[i].new_offset = reply->new_offset;
                    vfs_rpc_registry[i].dirent_count = reply->entry_count;
                    if (vfs_rpc_registry[i].dirents && reply->entry_count > 0) {
                        memcpy(vfs_rpc_registry[i].dirents, reply->entries, reply->entry_count * sizeof(struct mattx_dirent));
                    }
                }
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

// --- VFS NETWORK HANDLERS ---

static void handle_vfs_open_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_open_req *req = payload;
        struct mattx_vfs_open_reply reply;
        struct file *f;
        int i, fd_slot = -1;

        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;

        f = filp_open(req->path, req->flags, req->mode);
        if (!IS_ERR(f)) {
            spin_lock(&mfs_file_lock);
            for (i = 0; i < MAX_FDS; i++) {
                if (mfs_open_files[i] == NULL) {
                    mfs_open_files[i] = f;
                    fd_slot = i;
                    break;
                }
            }
            spin_unlock(&mfs_file_lock);

            if (fd_slot != -1) {
                reply.remote_fd = fd_slot;
                reply.error = 0;
            } else {
                fput(f);
                reply.error = -ENFILE;
            }
        } else {
            reply.error = PTR_ERR(f);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_OPEN_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_vfs_open_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_open_reply *reply = payload;
        int i;
        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].error = reply->error;
                vfs_rpc_registry[i].remote_fd = reply->remote_fd;
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

static void handle_vfs_read_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_read_req *req = payload;
        struct file *f = NULL;
        
        spin_lock(&mfs_file_lock);
        if (req->remote_fd < MAX_FDS && mfs_open_files[req->remote_fd]) {
            f = mfs_open_files[req->remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);

        if (f) {
            void *read_buf = kmalloc(req->count, GFP_KERNEL);
            if (read_buf) {
                loff_t pos = req->pos;
                ssize_t ret = kernel_read(f, read_buf, req->count, &pos);
                
                size_t reply_size = sizeof(struct mattx_vfs_read_reply) + (ret > 0 ? ret : 0);
                struct mattx_vfs_read_reply *reply = kmalloc(reply_size, GFP_KERNEL);
                if (reply) {
                    reply->req_id = req->req_id;
                    reply->bytes_read = ret;
                    reply->error = (ret < 0) ? ret : 0;
                    if (ret > 0) memcpy(reply->data, read_buf, ret);
                    
                    if (cluster_map[hdr->sender_id]) {
                        mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_READ_REPLY, reply, reply_size);
                    }
                    kfree(reply);
                }
                kfree(read_buf);
            }
            fput(f);
        }
    }
}

static void handle_vfs_read_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_read_reply *reply = payload;
        int i;
        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].bytes_rw = reply->bytes_read;
                if (reply->bytes_read > 0 && vfs_rpc_registry[i].data_buf) {
                    memcpy(vfs_rpc_registry[i].data_buf, reply->data, reply->bytes_read);
                }
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

static void handle_vfs_write_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_write_req *req = payload;
        struct mattx_vfs_write_reply reply;
        struct file *f = NULL;
        
        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;
        reply.bytes_written = -EBADF;

        spin_lock(&mfs_file_lock);
        if (req->remote_fd < MAX_FDS && mfs_open_files[req->remote_fd]) {
            f = mfs_open_files[req->remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);

        if (f) {
            loff_t pos = req->pos;
            reply.bytes_written = kernel_write(f, req->data, req->count, &pos);
            reply.error = (reply.bytes_written < 0) ? reply.bytes_written : 0;
            fput(f);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_WRITE_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_vfs_write_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_write_reply *reply = payload;
        int i;
        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].bytes_rw = reply->bytes_written;
                vfs_rpc_registry[i].error = reply->error;
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

static void handle_vfs_lseek_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_lseek_req *req = payload;
        struct mattx_vfs_lseek_reply reply;
        struct file *f = NULL;

        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;
        reply.error = -EBADF;

        spin_lock(&mfs_file_lock);
        if (req->remote_fd < MAX_FDS && mfs_open_files[req->remote_fd]) {
            f = mfs_open_files[req->remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);

        if (f) {
            loff_t res = vfs_llseek(f, req->offset, req->whence);
            if (res < 0) {
                reply.error = res;
            } else {
                reply.error = 0;
                reply.offset = res;
            }
            fput(f);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_LSEEK_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_vfs_lseek_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_lseek_reply *reply = payload;
        int i;
        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].error = reply->error;
                vfs_rpc_registry[i].ret_offset = reply->offset;
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

static void handle_vfs_fsync_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_fsync_req *req = payload;
        struct mattx_vfs_fsync_reply reply;
        struct file *f = NULL;

        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;
        reply.error = -EBADF;

        spin_lock(&mfs_file_lock);
        if (req->remote_fd < MAX_FDS && mfs_open_files[req->remote_fd]) {
            f = mfs_open_files[req->remote_fd];
            get_file(f);
        }
        spin_unlock(&mfs_file_lock);

        if (f) {
            reply.error = vfs_fsync_range(f, req->start, req->end, req->datasync);
            fput(f);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_VFS_FSYNC_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_vfs_fsync_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_fsync_reply *reply = payload;
        int i;
        spin_lock(&vfs_rpc_lock);
        for (i = 0; i < MAX_VFS_RPC; i++) {
            if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
                vfs_rpc_registry[i].error = reply->error;
                vfs_rpc_registry[i].done = true;
                wake_up_interruptible(&vfs_rpc_registry[i].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
    }
}

static void handle_vfs_close_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_close_req *req = payload;
        spin_lock(&mfs_file_lock);
        if (req->remote_fd < MAX_FDS && mfs_open_files[req->remote_fd]) {
            fput(mfs_open_files[req->remote_fd]);
            mfs_open_files[req->remote_fd] = NULL;
        }
        spin_unlock(&mfs_file_lock);
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
    mattx_register_handler(MATTX_MSG_SYS_POLL_REQ, handle_sys_poll_req);
    mattx_register_handler(MATTX_MSG_SYS_POLL_REPLY, handle_sys_poll_reply);
    mattx_register_handler(MATTX_MSG_VFS_GETATTR_REQ, handle_vfs_getattr_req);
    mattx_register_handler(MATTX_MSG_VFS_GETATTR_REPLY, handle_vfs_getattr_reply);
    mattx_register_handler(MATTX_MSG_VFS_READDIR_REQ, handle_vfs_readdir_req);
    mattx_register_handler(MATTX_MSG_VFS_READDIR_REPLY, handle_vfs_readdir_reply);
    mattx_register_handler(MATTX_MSG_VFS_OPEN_REQ, handle_vfs_open_req);
    mattx_register_handler(MATTX_MSG_VFS_OPEN_REPLY, handle_vfs_open_reply);
    mattx_register_handler(MATTX_MSG_VFS_READ_REQ, handle_vfs_read_req);
    mattx_register_handler(MATTX_MSG_VFS_READ_REPLY, handle_vfs_read_reply);
    mattx_register_handler(MATTX_MSG_VFS_WRITE_REQ, handle_vfs_write_req);
    mattx_register_handler(MATTX_MSG_VFS_WRITE_REPLY, handle_vfs_write_reply);
    mattx_register_handler(MATTX_MSG_VFS_CLOSE_REQ, handle_vfs_close_req);
    mattx_register_handler(MATTX_MSG_VFS_LSEEK_REQ, handle_vfs_lseek_req);
    mattx_register_handler(MATTX_MSG_VFS_LSEEK_REPLY, handle_vfs_lseek_reply);
    mattx_register_handler(MATTX_MSG_VFS_FSYNC_REQ, handle_vfs_fsync_req);
    mattx_register_handler(MATTX_MSG_VFS_FSYNC_REPLY, handle_vfs_fsync_reply);

    printk(KERN_INFO "MattX: [FILEIO] Network handlers registered.\n");
}

