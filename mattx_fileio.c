/*
 * MattX - The Modern Single System Image (SSI) Cluster
 * 
 * Copyright (c) 2026 by Matthias Rechenburg
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Commercial licensing options are available upon request.
 */
 
#include "mattx.h"
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/namei.h> // For kern_path

// --- The VFS Proxy (Fake FDs) ---

// Helper to safely check if the RPC is done while we are sleeping
bool check_rpc_done(pid_t pid) {
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

// Identity Spoofing Helpers ---
static const struct cred *mattx_override_creds(u32 uid, u32 gid, struct cred **new_cred_out) {
    struct cred *new_cred = prepare_creds();
    if (!new_cred) return NULL;
    
    new_cred->uid = make_kuid(&init_user_ns, uid);
    new_cred->euid = new_cred->uid;
    new_cred->fsuid = new_cred->uid;
    new_cred->gid = make_kgid(&init_user_ns, gid);
    new_cred->egid = new_cred->gid;
    new_cred->fsgid = new_cred->gid;
    
    *new_cred_out = new_cred;
    return override_creds(new_cred);
}

static void mattx_revert_creds(const struct cred *old_cred, struct cred *new_cred) {
    if (old_cred) revert_creds(old_cred);
    if (new_cred) put_cred(new_cred);
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

    mattx_dbg("[WORMHOLE] Surrogate %d requesting GETATTR for FD %u. Sleeping...\n", my_pid, req.fd);

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

    mattx_dbg("[WORMHOLE] Surrogate %d woke up! GETATTR result: %d (Size: %lld).\n", my_pid, ret_error, stat->size);
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

    mattx_dbg("[WORMHOLE] Surrogate %d seeking FD %u (offset %lld, whence %d). Sleeping...\n", my_pid, req.fd, offset, whence);

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

    mattx_dbg("[WORMHOLE] Surrogate %d woke up! Seek result: %lld.\n", my_pid, ret_offset);
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

    mattx_dbg("[WORMHOLE] Surrogate %d requesting FSYNC for FD %u. Sleeping...\n", my_pid, req.fd);

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

    mattx_dbg("[WORMHOLE] Surrogate %d woke up! FSYNC result: %d.\n", my_pid, ret_error);
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

    mattx_dbg("[WORMHOLE] Surrogate %d requesting %zu bytes from FD %u. Sleeping...\n", my_pid, to_read, req.fd);

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

    mattx_dbg("[WORMHOLE] Surrogate %d woke up! Read %zd bytes.\n", my_pid, ret_bytes);
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
        bool is_migrating = false;
        
        // Check if the process is currently migrating (Recall phase)
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->pid) {
                is_migrating = guest_registry[i].is_migrating;
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (cluster_map[fd_info->home_node] && !is_migrating) {
            req.orig_pid = fd_info->orig_pid;
            req.remote_fd = fd_info->remote_fd;

            mattx_dbg("[WORMHOLE] Surrogate closed FD %u. Sending CLOSE_REQ to Node %d...\n", req.remote_fd, fd_info->home_node);
            mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_CLOSE_REQ, &req, sizeof(req));
        } else if (is_migrating) {
            mattx_dbg("[WORMHOLE] Surrogate is migrating! Shielding Remote FD %u from closure.\n", fd_info->remote_fd);
        }
        kfree(fd_info);
    }
    return 0;
}

// "Yes, this Fake FD is always ready for read/write!" - THE PACIFIER!
// When the app actually tries to read/write, our Kprobes will intercept it
// and do the real blocking over the TCP Wormhole!
static __poll_t mattx_fake_poll(struct file *file, struct poll_table_struct *wait) {
    return EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
}


const struct file_operations mattx_fops = {
    .llseek = mattx_fake_llseek,
    .fsync = mattx_fake_fsync,
    .read = mattx_fake_read,
    .write = mattx_fake_write,
    .poll = mattx_fake_poll, // THE PACIFIER!
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

        mattx_dbg("[RPC] Received OPEN request from Node %u for file: '%s'\n", hdr->sender_id, req->filename);

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

        mattx_dbg("[RPC] Sending OPEN_REPLY (Remote FD: %d) back to Node %u...\n", remote_fd, hdr->sender_id);
        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_OPEN_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_open_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_open_reply *reply = (struct mattx_sys_open_reply *)payload;
        int i;

        mattx_dbg("[RPC] Received OPEN_REPLY for Orig PID %u. Remote FD is %d.\n", reply->orig_pid, reply->remote_fd);

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
        struct mattx_sys_close_req *req = payload;
        struct file *f_to_close = NULL;
        
        mattx_dbg("[RPC] Received CLOSE request for Remote FD %u from Node %u\n", req->remote_fd, hdr->sender_id);

        if (req->remote_fd >= 1000) {
            int slot = req->remote_fd - 1000;
            spin_lock(&export_lock);
            for (int i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == req->orig_pid) {
                    if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                        f_to_close = export_registry[i].remote_files[slot];
                        export_registry[i].remote_files[slot] = NULL;
                    }
                    break;
                }
            }
            spin_unlock(&export_lock);
        } else if (req->remote_fd >= 0) {
            // --- THE NATIVE CLOSE UPGRADE ---
            struct task_struct *deputy = NULL;
            rcu_read_lock();
            deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();

            if (deputy && deputy->files) {
                spin_lock(&deputy->files->file_lock);
                struct fdtable *fdt = files_fdtable(deputy->files);
                if (req->remote_fd < fdt->max_fds) {
                    f_to_close = rcu_dereference_raw(fdt->fd[req->remote_fd]);
                    if (f_to_close) {
                        rcu_assign_pointer(fdt->fd[req->remote_fd], NULL);
                        __clear_bit(req->remote_fd, fdt->open_fds);
                    }
                }
                spin_unlock(&deputy->files->file_lock);
                put_task_struct(deputy);
            }
        }
        
        if (f_to_close) {
            fput(f_to_close);
            mattx_dbg("[RPC] Successfully closed Remote FD %u\n", req->remote_fd);
        }
    }
}


static void handle_sys_read_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_read_req *req = (struct mattx_sys_read_req *)payload;
        struct task_struct *deputy = NULL;
        struct file *file = NULL;
        int i;
        
        mattx_dbg("[WORMHOLE] Received READ request for FD %u from Node %u. Reading from Deputy...\n", req->fd, hdr->sender_id);

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

        mattx_dbg("[RPC] Received READ_REPLY for Orig PID %u. Bytes read: %zd\n", reply->orig_pid, reply->bytes_read);

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

        mattx_dbg("[WORMHOLE] Received LSEEK req for FD %u from Node %u (offset %lld, whence %d)\n", req->fd, hdr->sender_id, req->offset, req->whence);

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

        mattx_dbg("[WORMHOLE] Received STATX req for FD %u from Node %u\n", req->fd, hdr->sender_id);

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

        mattx_dbg("[WORMHOLE] Received DUP req for Remote FD %u from Node %u\n", req->old_remote_fd, hdr->sender_id);

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
                                mattx_dbg("[WORMHOLE] Duplicated Exported FD %u to %d\n", req->old_remote_fd, reply.new_remote_fd);
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
                                mattx_dbg("[WORMHOLE] Duplicated Deputy FD %u to %d\n", req->old_remote_fd, reply.new_remote_fd);
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

        mattx_dbg("[WORMHOLE] Received FSYNC req for FD %u from Node %u\n", req->fd, hdr->sender_id);

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



// --- UNIVERSAL REMOTE FILE FETCHER ---
// Safely retrieves a struct file* whether it's a MattXFS file (>= 1000) or a Native Socket (< 1000)
static struct file *mattx_get_remote_file(u32 orig_pid, int remote_fd) {
    struct file *file = NULL;
    
    if (remote_fd >= 1000) {
        int slot = remote_fd - 1000;
        spin_lock(&export_lock);
        for (int i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == orig_pid) {
                if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                    file = export_registry[i].remote_files[slot];
                    get_file(file); 
                }
                break;
            }
        }
        spin_unlock(&export_lock);
    } else if (remote_fd >= 0) {
        // Native Deputy FD!
        struct task_struct *deputy = NULL;
        rcu_read_lock();
        deputy = pid_task(find_vpid(orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            if (deputy->files) {
                spin_lock(&deputy->files->file_lock);
                struct fdtable *fdt = files_fdtable(deputy->files);
                if (remote_fd < fdt->max_fds) {
                    file = rcu_dereference_raw(fdt->fd[remote_fd]);
                    if (file) get_file(file); 
                }
                spin_unlock(&deputy->files->file_lock);
            }
            put_task_struct(deputy);
        }
    }
    return file;
}


// --- THE DEPUTY HIJACK: SOCKET ---
struct mattx_socket_ctx {
    struct callback_head cb;
    struct mattx_sys_socket_req req;
    int target_node;
};

static void mattx_socket_cb(struct callback_head *cb) {
    struct mattx_socket_ctx *ctx = container_of(cb, struct mattx_socket_ctx, cb);
    struct pt_regs regs;
    int ret = -ENOSYS;
    struct mattx_sys_socket_reply reply;

    memset(&regs, 0, sizeof(regs));
    regs.di = ctx->req.domain;
    regs.si = ctx->req.type;
    regs.dx = ctx->req.protocol;

    if (real_sys_socket) {
        ret = real_sys_socket(&regs);
        mattx_dbg("[HIJACK] Deputy executed socket natively. Result FD: %d\n", ret);
    }

    reply.orig_pid = ctx->req.orig_pid;
    reply.remote_fd = (ret >= 0) ? ret : -1;
    reply.error = (ret < 0) ? ret : 0;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SOCKET_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}

static void handle_sys_socket_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_socket_req *req = payload;
        struct task_struct *deputy = NULL;

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_socket_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                memcpy(&ctx->req, req, sizeof(*req));
                ctx->target_node = hdr->sender_id;
                
                init_task_work(&ctx->cb, mattx_socket_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, deputy, 0);
                } else { kfree(ctx); }
            }
            put_task_struct(deputy);
        }
    }
}



// --- THE SMART CONNECTOR: Blocking Waiter ---
struct mattx_connect_wait_ctx {
    struct work_struct work;
    u32 orig_pid;
    int fd; // Native FD on the Deputy
    int target_node;
};

static void mattx_connect_wait_worker(struct work_struct *work) {
    struct mattx_connect_wait_ctx *ctx = container_of(work, struct mattx_connect_wait_ctx, work);
    struct mattx_sys_connect_reply reply;
    struct file *file = NULL;
    struct task_struct *deputy = NULL;
    int err = -EBADF;

    mattx_dbg("[CONNECT_DEBUG] Kworker started for PID %u, Native FD %d\n", ctx->orig_pid, ctx->fd);

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->orig_pid;

    // 1. Safely get the file from the Deputy's Native FD table
    rcu_read_lock();
    deputy = pid_task(find_vpid(ctx->orig_pid), PIDTYPE_PID);
    if (deputy) get_task_struct(deputy);
    rcu_read_unlock();

    if (deputy) {
        if (deputy->files) {
            spin_lock(&deputy->files->file_lock);
            struct fdtable *fdt = files_fdtable(deputy->files);
            if (ctx->fd < fdt->max_fds) {
                file = rcu_dereference_raw(fdt->fd[ctx->fd]);
                if (file) get_file(file);
            }
            spin_unlock(&deputy->files->file_lock);
        }
        put_task_struct(deputy);
    }

    if (file) {
        mattx_dbg("[CONNECT_DEBUG] Successfully retrieved struct file for FD %d. Entering poll loop...\n", ctx->fd);
        
        // 2. Wait for the TCP Handshake to finish (EPOLLOUT)
        while (1) {
            struct poll_wqueues table;
            poll_initwait(&table);
            __poll_t mask = vfs_poll(file, &table.pt);
            
            mattx_dbg("[CONNECT_DEBUG] vfs_poll returned mask: 0x%x\n", mask);

            if (mask & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
                mattx_dbg("[CONNECT_DEBUG] Socket is ready! Breaking poll loop.\n");
                poll_freewait(&table);
                break;
            }
            
            schedule_timeout_interruptible(msecs_to_jiffies(50));
            poll_freewait(&table);
        }

        // 3. Check the socket error state to see if it succeeded!
        struct socket *sock = sock_from_file(file);
        if (sock && sock->sk) {
            err = sock_error(sock->sk);
            mattx_dbg("[CONNECT_DEBUG] sock_error returned: %d\n", err);
            if (err) err = -err; // sock_error returns positive codes, we need negative!
        } else {
            mattx_dbg("[CONNECT_DEBUG] ERROR: Could not extract socket from file!\n");
            err = -ENOTSOCK;
        }
        fput(file);
    } else {
        mattx_dbg("[CONNECT_DEBUG] ERROR: Could not find file for FD %d in Deputy!\n", ctx->fd);
    }

    reply.error = err;
    mattx_dbg("[CONNECT_DEBUG] Sending CONNECT_REPLY to Node %d with Error: %d\n", ctx->target_node, err);
    
    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_CONNECT_REPLY, &reply, sizeof(reply));
    }
    kfree(ctx);
}


// --- THE DEPUTY HIJACK: BIND & CONNECT ---
struct mattx_net_setup_ctx {
    struct callback_head cb;
    u32 orig_pid;
    int fd;
    struct sockaddr_storage addr;
    int addrlen;
    int target_node;
    bool is_connect;
};


static void mattx_net_setup_cb(struct callback_head *cb) {
    struct mattx_net_setup_ctx *ctx = container_of(cb, struct mattx_net_setup_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs;
    int ret = -EFAULT;
    struct mattx_sys_connect_reply reply; 
    bool is_nonblock = false;

    // --- CHECK NATIVE FLAGS ---
    // We check if the socket is non-blocking natively on the Deputy!
    struct file *f = fget(ctx->fd);
    if (f) {
        is_nonblock = (f->f_flags & O_NONBLOCK) != 0;
        fput(f);
    }

    unsigned long user_ptr = task_regs->sp - 128 - ctx->addrlen;

    if (copy_to_user((void __user *)user_ptr, &ctx->addr, ctx->addrlen) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->fd;
        regs.si = user_ptr;
        regs.dx = ctx->addrlen;

        if (ctx->is_connect && real_sys_connect) {
            ret = real_sys_connect(&regs);
            if (ret == -ERESTARTSYS) ret = -EINPROGRESS;
            mattx_dbg("[HIJACK] Deputy executed connect natively. Result: %d (NonBlock: %d)\n", ret, is_nonblock);
        } else if (!ctx->is_connect && real_sys_bind) {
            ret = real_sys_bind(&regs);
            mattx_dbg("[HIJACK] Deputy executed bind natively. Result: %d\n", ret);
        }
    }

    // --- THE SMART CONNECTOR ---
    // If it's a blocking socket and the handshake is in progress, hand it to the Kworker!
    if (ctx->is_connect && ret == -EINPROGRESS && !is_nonblock) {
        mattx_dbg("[CONNECT_DEBUG] Blocking connect returned -EINPROGRESS. Spawning Kworker...\n");
        struct mattx_connect_wait_ctx *wait_ctx = kmalloc(sizeof(*wait_ctx), GFP_KERNEL);
        if (wait_ctx) {
            wait_ctx->orig_pid = ctx->orig_pid;
            wait_ctx->fd = ctx->fd;
            wait_ctx->target_node = ctx->target_node;
            
            INIT_WORK(&wait_ctx->work, mattx_connect_wait_worker);
            schedule_work(&wait_ctx->work);
            
            kfree(ctx);
            set_current_state(TASK_STOPPED);
            schedule();
            return; // Exit early! The Kworker will send the reply!
        } else {
            mattx_dbg("[CONNECT_DEBUG] ERROR: Failed to allocate Kworker context!\n");
            ret = -ENOMEM;
        }
    }

    reply.orig_pid = ctx->orig_pid;
    reply.error = ret;
    if (cluster_map[ctx->target_node]) {
        u32 msg_type = ctx->is_connect ? MATTX_MSG_SYS_CONNECT_REPLY : MATTX_MSG_SYS_BIND_REPLY;
        mattx_comm_send(cluster_map[ctx->target_node], msg_type, &reply, sizeof(reply));
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}


static void handle_sys_bind_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_bind_req *req = payload;
        struct task_struct *deputy = NULL;

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_net_setup_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                ctx->orig_pid = req->orig_pid;
                ctx->fd = req->fd;
                ctx->addrlen = req->addrlen;
                memcpy(&ctx->addr, &req->addr, req->addrlen);
                ctx->target_node = hdr->sender_id;
                ctx->is_connect = false;
                
                init_task_work(&ctx->cb, mattx_net_setup_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, deputy, 0);
                } else { kfree(ctx); }
            }
            put_task_struct(deputy);
        }
    }
}

static void handle_sys_connect_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_connect_req *req = payload;
        struct task_struct *deputy = NULL;

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_net_setup_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                ctx->orig_pid = req->orig_pid;
                ctx->fd = req->fd;
                ctx->addrlen = req->addrlen;
                memcpy(&ctx->addr, &req->addr, req->addrlen);
                ctx->target_node = hdr->sender_id;
                ctx->is_connect = true;
                
                init_task_work(&ctx->cb, mattx_net_setup_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, deputy, 0);
                } else { kfree(ctx); }
            }
            put_task_struct(deputy);
        }
    }
}


static void handle_sys_lseek_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_lseek_reply *reply = (struct mattx_sys_lseek_reply *)payload;
        int i;

        mattx_dbg("[RPC] Received LSEEK_REPLY for Orig PID %u. Result: %lld\n", reply->orig_pid, reply->result_offset);

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

        mattx_dbg("[RPC] Received STATX_REPLY for Orig PID %u. Error: %d\n", reply->orig_pid, reply->error);

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

        mattx_dbg("[RPC] Received DUP_REPLY for Orig PID %u. New Remote FD: %d\n", reply->orig_pid, reply->new_remote_fd);

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

        mattx_dbg("[RPC] Received FSYNC_REPLY for Orig PID %u. Error: %d\n", reply->orig_pid, reply->error);

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

        mattx_dbg("[RPC] Received SOCKET_REPLY for Orig PID %u. Remote FD: %d\n", reply->orig_pid, reply->remote_fd);

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

        mattx_dbg("[RPC] Received CONNECT_REPLY for Orig PID %u. Error: %d\n", reply->orig_pid, reply->error);

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


// --- THE DEPUTY HIJACK: LISTEN ---
struct mattx_listen_ctx {
    struct callback_head cb;
    struct mattx_sys_listen_req req;
    int target_node;
};

static void mattx_listen_cb(struct callback_head *cb) {
    struct mattx_listen_ctx *ctx = container_of(cb, struct mattx_listen_ctx, cb);
    struct pt_regs regs;
    int ret = -ENOSYS;
    struct mattx_sys_listen_reply reply;

    memset(&regs, 0, sizeof(regs));
    regs.di = ctx->req.fd;
    regs.si = ctx->req.backlog;

    if (real_sys_listen) {
        ret = real_sys_listen(&regs);
        mattx_dbg("[HIJACK] Deputy executed listen natively. Result: %d\n", ret);
    }

    reply.orig_pid = ctx->req.orig_pid;
    reply.error = ret;
    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_LISTEN_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}

static void handle_sys_listen_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_listen_req *req = payload;
        struct task_struct *deputy = NULL;

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_listen_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                memcpy(&ctx->req, req, sizeof(*req));
                ctx->target_node = hdr->sender_id;
                
                init_task_work(&ctx->cb, mattx_listen_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, deputy, 0);
                } else { kfree(ctx); }
            }
            put_task_struct(deputy);
        }
    }
}


static void handle_sys_generic_int_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // We can reuse this logic for bind, listen, connect, fsync since they all return an integer
    // The payload structs happen to have 'orig_pid' and 'error' in the exact same memory layout
    if (payload) {
        struct mattx_sys_connect_reply *reply = (struct mattx_sys_connect_reply *)payload;
        int i;

        mattx_dbg("[RPC] Received Integer REPLY (Err: %d) for Orig PID %u\n", reply->error, reply->orig_pid);

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


// === DATA PLANE: SENDTO ===
struct mattx_sendto_kworker_ctx {
    struct work_struct work;
    u32 orig_pid;
    int fd;
    int flags;
    size_t len;
    int target_node;
    char data[];
};

static void mattx_sendto_kworker(struct work_struct *work) {
    struct mattx_sendto_kworker_ctx *ctx = container_of(work, struct mattx_sendto_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->orig_pid, ctx->fd);
    struct mattx_sys_send_reply reply;
    int ret = -EBADF;

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock) {
            struct msghdr msg = {0};
            struct kvec iov;
            iov.iov_base = ctx->data;
            iov.iov_len = ctx->len;
            
            // Execute the send natively in the Kworker! No signals, no interruptions!
            ret = kernel_sendmsg(sock, &msg, &iov, 1, ctx->len);
        } else {
            ret = -ENOTSOCK;
        }
        fput(file);
    }

    reply.orig_pid = ctx->orig_pid;
    reply.bytes_sent = ret;
    reply.error = (ret < 0) ? ret : 0;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SEND_REPLY, &reply, sizeof(reply));
    }
    kfree(ctx);
}

static void handle_sys_send_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_send_req *req = payload;
        struct mattx_sendto_kworker_ctx *ctx = kmalloc(sizeof(*ctx) + req->len, GFP_ATOMIC);
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_sendto_kworker);
            ctx->orig_pid = req->orig_pid;
            ctx->fd = req->fd;
            ctx->flags = req->flags;
            ctx->len = req->len;
            ctx->target_node = hdr->sender_id;
            memcpy(ctx->data, req->data, req->len);
            
            schedule_work(&ctx->work); // Dispatch to the Data Plane!
        }
    }
}


// === DATA PLANE: RECVFROM ===
struct mattx_recvfrom_kworker_ctx {
    struct work_struct work;
    u32 orig_pid;
    int fd;
    size_t size;
    int flags;
    int target_node;
};

static void mattx_recvfrom_kworker(struct work_struct *work) {
    struct mattx_recvfrom_kworker_ctx *ctx = container_of(work, struct mattx_recvfrom_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->orig_pid, ctx->fd);
    int ret = -EBADF;
    void *recv_buf = NULL;

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock) {
            recv_buf = kmalloc(ctx->size, GFP_KERNEL);
            if (recv_buf) {
                struct msghdr msg = {0};
                struct kvec iov;
                iov.iov_base = recv_buf;
                iov.iov_len = ctx->size;
                
                // Execute the recv natively in the Kworker!
                ret = kernel_recvmsg(sock, &msg, &iov, 1, ctx->size, ctx->flags);
            } else {
                ret = -ENOMEM;
            }
        } else {
            ret = -ENOTSOCK;
        }
        fput(file);
    }

    size_t reply_size = sizeof(struct mattx_sys_recv_reply) + (ret > 0 ? ret : 0);
    struct mattx_sys_recv_reply *reply = kzalloc(reply_size, GFP_KERNEL);
    if (reply) {
        reply->orig_pid = ctx->orig_pid;
        reply->bytes_recv = ret;
        reply->error = (ret < 0) ? ret : 0;
        if (ret > 0 && recv_buf) {
            memcpy(reply->data, recv_buf, ret);
        }
        if (cluster_map[ctx->target_node]) {
            mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_RECV_REPLY, reply, reply_size);
        }
        kfree(reply);
    }
    if (recv_buf) kfree(recv_buf);
    kfree(ctx);
}

static void handle_sys_recv_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_recv_req *req = payload;
        struct mattx_recvfrom_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_recvfrom_kworker);
            ctx->orig_pid = req->orig_pid;
            ctx->fd = req->fd;
            ctx->size = req->size;
            ctx->flags = req->flags;
            ctx->target_node = hdr->sender_id;
            
            schedule_work(&ctx->work); // Dispatch to the Data Plane!
        }
    }
}


static void handle_sys_recv_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_recv_reply *reply = (struct mattx_sys_recv_reply *)payload;
        int i;

        mattx_dbg("[RPC] Received RECV_REPLY for Orig PID %u. Bytes recv: %zd\n", reply->orig_pid, reply->bytes_recv);

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


// --- THE DEPUTY HIJACK: ACCEPT4 ---
struct mattx_accept_ctx {
    struct callback_head cb;
    struct mattx_sys_accept_req req;
    int target_node;
};

static void mattx_accept_cb(struct callback_head *cb) {
    struct mattx_accept_ctx *ctx = container_of(cb, struct mattx_accept_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs;
    int ret = -EFAULT;
    struct mattx_sys_accept_reply reply;
    sigset_t old_set, block_set;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;
    reply.remote_fd = -EBADF;
    reply.error = -EBADF;

    // --- THE USER-SPACE SCRATCHPAD HACK ---
    // We need space for a sockaddr_storage (128 bytes) AND an int (4 bytes)
    unsigned long addr_ptr = task_regs->sp - 128 - sizeof(struct sockaddr_storage) - sizeof(int);
    unsigned long len_ptr = addr_ptr + sizeof(struct sockaddr_storage);
    int initial_len = sizeof(struct sockaddr_storage);

    // Write the initial length to the scratchpad
    if (copy_to_user((void __user *)len_ptr, &initial_len, sizeof(int)) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->req.fd;
        regs.si = addr_ptr;
        regs.dx = len_ptr;
        regs.r10 = ctx->req.flags; // accept4 flags

        // Signal shield for non-blocking accept!
        sigfillset(&block_set);
        sigprocmask(SIG_BLOCK, &block_set, &old_set);

        if (real_sys_accept4) {
            ret = real_sys_accept4(&regs);
            if (ret == -ERESTARTSYS) ret = -EAGAIN; // Standard non-blocking behavior
            mattx_dbg("[HIJACK] Deputy executed accept4 natively. Result FD: %d\n", ret);
        }

        sigprocmask(SIG_SETMASK, &old_set, NULL);

        if (ret >= 0) {
            reply.remote_fd = ret;
            reply.error = 0;
            
            // Read back the populated sockaddr and length from the scratchpad!
            int final_len = 0;
            if (copy_from_user(&final_len, (void __user *)len_ptr, sizeof(int)) == 0) {
                if (final_len > 0 && final_len <= sizeof(struct sockaddr_storage)) {
                    if (copy_from_user(&reply.addr, (void __user *)addr_ptr, final_len) == 0) {
                        reply.addrlen = final_len;
                    }
                }
            }
        } else {
            reply.error = ret;
        }
    }

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_ACCEPT_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}

static void handle_sys_accept_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_accept_req *req = payload;
        struct task_struct *deputy = NULL;

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_accept_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                memcpy(&ctx->req, req, sizeof(*req));
                ctx->target_node = hdr->sender_id;
                
                init_task_work(&ctx->cb, mattx_accept_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, deputy, 0);
                } else { kfree(ctx); }
            }
            put_task_struct(deputy);
        }
    }
}


static void handle_sys_accept_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_accept_reply *reply = (struct mattx_sys_accept_reply *)payload;
        int i;

        mattx_dbg("[RPC] Received ACCEPT_REPLY for Orig PID %u. New Remote FD: %d\n", reply->orig_pid, reply->remote_fd);

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


// --- THE TRUE POLL WORKER (Stack Diet Edition) ---
static void mattx_poll_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    struct mattx_sys_poll_reply *reply;
    struct file **poll_files;
    int i;
    unsigned long expire;
    bool has_timeout = (rpc->timeout >= 0);
    
    // --- THE STACK DIET FIX ---
    // Move large structs off the kernel stack and onto the heap!
    reply = kzalloc(sizeof(*reply), GFP_KERNEL);
    poll_files = kcalloc(16, sizeof(struct file *), GFP_KERNEL);

    if (!reply || !poll_files) {
        if (reply) kfree(reply);
        if (poll_files) kfree(poll_files);
        kfree(rpc);
        return;
    }

    reply->orig_pid = rpc->orig_pid;
    reply->nfds = rpc->nfds;
    memcpy(reply->fds, rpc->poll_fds, sizeof(struct mattx_pollfd) * rpc->nfds);

    mattx_dbg("[RPC] Poll Worker started for Deputy PID %u (Timeout: %dms)\n", rpc->orig_pid, rpc->timeout);

    if (has_timeout) {
        expire = jiffies + msecs_to_jiffies(rpc->timeout);
    }

    // --- THE WAIT QUEUE LOOP ---
    while (1) {
        int ready_count = 0;
        bool deputy_alive = false;
        
        // Clear the poll_files array for this iteration
        memset(poll_files, 0, 16 * sizeof(struct file *));

// 1. Check if the Deputy is still alive and grab the files!
        spin_lock(&export_lock);
        for (i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == rpc->orig_pid) {
                deputy_alive = true;
                
                // Standard poll logic (No more epoll Master Watch List!)
                for (int j = 0; j < rpc->nfds; j++) {
                    int remote_fd = reply->fds[j].fd;
                    if (remote_fd >= 1000) {
                        int slot = remote_fd - 1000;
                        if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                            poll_files[j] = export_registry[i].remote_files[slot];
                            get_file(poll_files[j]); 
                        }
                    }
                }
                break;
            }
        }
        spin_unlock(&export_lock);

        if (!deputy_alive) {
            reply->error = -ESRCH;
            break;
        }

        // 2. Do the actual polling SAFELY OUTSIDE the spinlock!
        for (int j = 0; j < rpc->nfds; j++) {
            int remote_fd = reply->fds[j].fd;
            struct file *f = poll_files[j];
            struct task_struct *deputy = NULL;

            if (!f && remote_fd >= 0 && remote_fd < 1000) {
                rcu_read_lock();
                deputy = pid_task(find_vpid(rpc->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    struct files_struct *files = deputy->files;
                    if (files) {
                        spin_lock(&files->file_lock);
                        struct fdtable *fdt = files_fdtable(files);
                        if (remote_fd < fdt->max_fds) {
                            f = rcu_dereference_raw(fdt->fd[remote_fd]);
                            if (f) get_file(f);
                        }
                        spin_unlock(&files->file_lock);
                    }
                    put_task_struct(deputy);
                }
            }

            if (f) {
                struct poll_wqueues table;
                poll_initwait(&table);
                
                __poll_t mask = vfs_poll(f, &table.pt);
                
                if (rpc->is_epoll_wait) {
                    mattx_dbg("[EPOLL_DEBUG] Polled FD %d. Mask returned: %x. Looking for: %x\n", 
                           remote_fd, mask, reply->fds[j].events);
                }
                
                reply->fds[j].revents = mask & reply->fds[j].events;
                if (reply->fds[j].revents) {
                    ready_count++;
                }
                
                poll_freewait(&table);
                fput(f); 
            } else {
                if (rpc->is_epoll_wait) mattx_dbg("[EPOLL_DEBUG] ERROR: Could not find struct file for FD %d!\n", remote_fd);
            }
        }

        if (ready_count > 0) {
            reply->retval = ready_count;
            break;
        }

        if (has_timeout && time_after(jiffies, expire)) {
            reply->retval = 0; 
            break;
        }

        // Sleep for a tiny fraction of a second to let the CPU breathe, 
        // but rely on the poll_table to catch the events!
        schedule_timeout_interruptible(msecs_to_jiffies(10));
    }

    if (cluster_map[rpc->home_node]) {
        mattx_dbg("[RPC] Sending POLL_REPLY (Ready: %d) back to Node %u...\n", reply->retval, rpc->home_node);
        mattx_comm_send_ctrl(cluster_map[rpc->home_node], MATTX_MSG_SYS_POLL_REPLY, reply, sizeof(*reply));
    }
    
    kfree(reply);
    kfree(poll_files);
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

        mattx_dbg("[RPC] Received POLL_REPLY for Orig PID %u. Ready FDs: %d\n", reply->orig_pid, reply->retval);

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


// --- THE DEPUTY HIJACK: Epoll Create ---
struct mattx_epoll_hijack_ctx {
    struct callback_head cb;
    int flags;
    u32 orig_pid;
    int target_node;
};

// This runs INSIDE the Deputy on VM1!
static void mattx_epoll_hijack_cb(struct callback_head *cb) {
    struct mattx_epoll_hijack_ctx *ctx = container_of(cb, struct mattx_epoll_hijack_ctx, cb);
    struct pt_regs regs;
    int fd = -ENOSYS;
    struct mattx_sys_epoll_create_reply reply;

    // Build a dummy pt_regs to pass the flags to the x86_64 syscall wrapper
    memset(&regs, 0, sizeof(regs));
    regs.di = ctx->flags;

    if (real_sys_epoll_create1) {
        fd = real_sys_epoll_create1(&regs);
        mattx_dbg("[HIJACK] Deputy executed epoll_create1 natively. Result FD: %d\n", fd);
    }

    reply.orig_pid = ctx->orig_pid;
    reply.remote_fd = fd;
    reply.error = (fd < 0) ? fd : 0;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_EPOLL_CREATE_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    
    // The Puppet goes back to sleep!
    set_current_state(TASK_STOPPED);
    schedule();
}

static void handle_sys_epoll_create_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_create_req *req = payload;
        struct task_struct *deputy = NULL;

        mattx_dbg("[RPC] Received EPOLL_CREATE_REQ from Node %u for PID %u\n", hdr->sender_id, req->orig_pid);

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_epoll_hijack_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                ctx->flags = req->flags;
                ctx->orig_pid = req->orig_pid;
                ctx->target_node = hdr->sender_id;
                
                init_task_work(&ctx->cb, mattx_epoll_hijack_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    // Nudge the Deputy to wake up and execute our injected code!
                    send_sig(SIGCONT, deputy, 0);
                } else {
                    kfree(ctx);
                }
            }
            put_task_struct(deputy);
        }
    }
}


static void handle_sys_epoll_create_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_create_reply *reply = payload;
        int i;
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                guest_registry[i].rpc_remote_fd = reply->remote_fd; // Actually save the FD!
                guest_registry[i].rpc_fsync_res = reply->error;
                guest_registry[i].rpc_done = true;
                if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq);
                break;
            }
        }
        spin_unlock(&guest_lock);
    }
}


// --- THE DEPUTY HIJACK: Epoll Ctl ---
struct mattx_epoll_ctl_ctx {
    struct callback_head cb;
    struct mattx_sys_epoll_ctl_req req;
    int target_node;
};


static void mattx_epoll_ctl_cb(struct callback_head *cb) {
    struct mattx_epoll_ctl_ctx *ctx = container_of(cb, struct mattx_epoll_ctl_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current); // We are the Deputy!
    struct pt_regs regs;
    int ret = -EFAULT;
    struct mattx_sys_connect_reply reply; 

    // --- THE USER-SPACE SCRATCHPAD HACK ---
    // x86_64 has a 128-byte "red zone" below the stack pointer.
    // We safely allocate space below that to pass our struct!
    unsigned long user_ptr = task_regs->sp - 128 - sizeof(struct epoll_event);

    // Copy the event struct to the Deputy's user-space stack
    if (copy_to_user((void __user *)user_ptr, &ctx->req.event, sizeof(struct epoll_event)) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->req.epfd;
        regs.si = ctx->req.op;
        regs.dx = ctx->req.fd;
        regs.r10 = user_ptr; // Pass the perfectly valid USER pointer!

        if (real_sys_epoll_ctl) {
            ret = real_sys_epoll_ctl(&regs);
            mattx_dbg("[HIJACK] Deputy executed epoll_ctl natively. Result: %d\n", ret);
        }
    }

    reply.orig_pid = ctx->req.orig_pid;
    reply.error = ret;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_EPOLL_CTL_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}


static void handle_sys_epoll_ctl_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_ctl_req *req = payload;
        struct task_struct *deputy = NULL;

        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            struct mattx_epoll_ctl_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
            if (ctx) {
                memcpy(&ctx->req, req, sizeof(*req));
                ctx->target_node = hdr->sender_id;
                
                init_task_work(&ctx->cb, mattx_epoll_ctl_cb);
                if (real_task_work_add) {
                    real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, deputy, 0);
                } else {
                    kfree(ctx);
                }
            }
            put_task_struct(deputy);
        }
    }
}

// --- THE DEPUTY HIJACK: Epoll Wait ---
struct mattx_epoll_wait_ctx {
    struct callback_head cb;
    struct mattx_sys_epoll_wait_req req;
    int target_node;
};


static void mattx_epoll_wait_cb(struct callback_head *cb) {
    struct mattx_epoll_wait_ctx *ctx = container_of(cb, struct mattx_epoll_wait_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs;
    int ret = -EFAULT;
    struct epoll_event *events_buf = NULL;
    
    // --- THE USER-SPACE SCRATCHPAD HACK ---
    size_t alloc_size = ctx->req.maxevents * sizeof(struct epoll_event);
    unsigned long user_ptr = task_regs->sp - 128 - alloc_size;

    memset(&regs, 0, sizeof(regs));
    regs.di = ctx->req.epfd;
    regs.si = user_ptr; // Pass the perfectly valid USER pointer!
    regs.dx = ctx->req.maxevents;
    regs.r10 = ctx->req.timeout;

    if (real_sys_epoll_wait) {
        ret = real_sys_epoll_wait(&regs);
        mattx_dbg("[HIJACK] Deputy executed epoll_wait natively. Result: %d\n", ret);
    }

    size_t reply_size = sizeof(struct mattx_sys_epoll_wait_reply) + (ret > 0 ? ret * sizeof(struct epoll_event) : 0);
    struct mattx_sys_epoll_wait_reply *reply = kzalloc(reply_size, GFP_KERNEL);
    
    if (reply) {
        reply->orig_pid = ctx->req.orig_pid;
        reply->retval = ret;
        
        // Read the results back from the user-space scratchpad!
        if (ret > 0) {
            events_buf = kzalloc(alloc_size, GFP_KERNEL);
            if (events_buf) {
                if (copy_from_user(events_buf, (void __user *)user_ptr, ret * sizeof(struct epoll_event)) == 0) {
                    memcpy(reply->events, events_buf, ret * sizeof(struct epoll_event));
                }
                kfree(events_buf);
            }
        }

        if (cluster_map[ctx->target_node]) {
            mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_EPOLL_WAIT_REPLY, reply, reply_size);
        }
        kfree(reply);
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}


// --- THE HYBRID WAITER: Kworker Wait Queue ---
static void mattx_epoll_wait_kworker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    struct file *epoll_file = mattx_get_remote_file(rpc->orig_pid, rpc->remote_fd);
    unsigned long expire = jiffies + msecs_to_jiffies(rpc->timeout_ms);
    bool has_timeout = (rpc->timeout_ms >= 0);
    bool ready = false;

    // 1. The Kworker sleeps and waits for the epoll instance to trigger!
    if (epoll_file) {

        // --- THE WAIT QUEUE LOOP ---
        while (1) {
            bool abort = false;
            
            // Check the Kill-Switch!
            spin_lock(&export_lock);
            for (int k = 0; k < export_count; k++) {
                if (export_registry[k].orig_pid == rpc->orig_pid) {
                    abort = export_registry[k].abort_rpc;
                    break;
                }
            }
            spin_unlock(&export_lock);

            if (abort) {
                mattx_dbg("[RPC] Kill-Switch activated! Aborting orphaned kworker for PID %d\n", rpc->orig_pid);
                kfree(rpc);
                return; // Exit cleanly without touching the Deputy!
            }

            struct poll_wqueues table;
            poll_initwait(&table);
            __poll_t mask = vfs_poll(epoll_file, &table.pt);
            
            if (mask & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                ready = true;
                poll_freewait(&table);
                break;
            }
            
            if (has_timeout && time_after(jiffies, expire)) {
                poll_freewait(&table);
                break;
            }
            
            schedule_timeout_interruptible(msecs_to_jiffies(10));
            poll_freewait(&table);
        }
        fput(epoll_file);
    }

    // 2. The events are ready!
    // --- THE LATE ABORT SHIELD ---
    // Check the Kill-Switch ONE LAST TIME before injecting the task_work!
    // If a Recall happened while we were breaking out of the loop, we must abort!
    bool late_abort = false;
    spin_lock(&export_lock);
    for (int k = 0; k < export_count; k++) {
        if (export_registry[k].orig_pid == rpc->orig_pid) {
            late_abort = export_registry[k].abort_rpc;
            break;
        }
    }
    spin_unlock(&export_lock);

    if (late_abort) {
        mattx_dbg("[RPC] Late Kill-Switch activated! Aborting orphaned kworker for PID %d\n", rpc->orig_pid);
        kfree(rpc);
        return; // Exit cleanly without touching the Deputy!
    }

    // Inject task_work into the Deputy to grab them instantly!
    struct task_struct *deputy = NULL;
    rcu_read_lock();
    deputy = pid_task(find_vpid(rpc->orig_pid), PIDTYPE_PID);
    if (deputy) get_task_struct(deputy);
    rcu_read_unlock();

    if (deputy) {
        struct mattx_epoll_wait_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            ctx->req.orig_pid = rpc->orig_pid;
            ctx->req.epfd = rpc->remote_fd;
            ctx->req.maxevents = rpc->epoll_maxevents;
            ctx->req.timeout = 0; // INSTANT GRAB! No sleeping, no -EINTR!
            ctx->target_node = rpc->home_node; 
            
            init_task_work(&ctx->cb, mattx_epoll_wait_cb);
            if (real_task_work_add) {
                real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL);
                send_sig(SIGCONT, deputy, 0);
            } else {
                kfree(ctx);
            }
        }
        put_task_struct(deputy);
    } else {
        // Deputy died while we were waiting!
        size_t reply_size = sizeof(struct mattx_sys_epoll_wait_reply);
        struct mattx_sys_epoll_wait_reply *reply = kzalloc(reply_size, GFP_KERNEL);
        if (reply) {
            reply->orig_pid = rpc->orig_pid;
            reply->retval = -ESRCH;
            if (cluster_map[rpc->home_node]) {
                mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_EPOLL_WAIT_REPLY, reply, reply_size);
            }
            kfree(reply);
        }
    }
    kfree(rpc);
}

static void handle_sys_epoll_wait_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_wait_req *req = payload;
        
        // Spawn the Kworker to do the waiting so the Deputy stays free!
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC);
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_epoll_wait_kworker);
            rpc->orig_pid = req->orig_pid;
            rpc->remote_fd = req->epfd;
            rpc->epoll_maxevents = req->maxevents;
            rpc->timeout_ms = req->timeout;
            rpc->home_node = hdr->sender_id; // Target node to send reply to
            schedule_work(&rpc->work);
        }
    }
}


static void handle_sys_epoll_wait_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_wait_reply *reply = payload;
        int i;

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                guest_registry[i].rpc_fsync_res = reply->retval; 
                
                if (reply->retval > 0) {
                    size_t data_size = reply->retval * sizeof(struct epoll_event);
                    guest_registry[i].rpc_read_buf = kmalloc(data_size, GFP_ATOMIC);
                    if (guest_registry[i].rpc_read_buf) {
                        memcpy(guest_registry[i].rpc_read_buf, reply->events, data_size);
                    }
                }
                
                guest_registry[i].rpc_done = true;
                if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq);
                break;
            }
        }
        spin_unlock(&guest_lock);
    }
}


// --- THE DEPUTY HIJACK: GETSOCKNAME & GETPEERNAME ---
struct mattx_sockname_ctx {
    struct callback_head cb;
    u32 orig_pid;
    int fd;
    int target_node;
    bool is_peer;
};

static void mattx_sockname_cb(struct callback_head *cb) {
    struct mattx_sockname_ctx *ctx = container_of(cb, struct mattx_sockname_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs;
    int ret = -EFAULT;
    struct mattx_sys_getsockname_reply reply; // Reuse for both

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->orig_pid;

    unsigned long addr_ptr = task_regs->sp - 128 - sizeof(struct sockaddr_storage) - sizeof(int);
    unsigned long len_ptr = addr_ptr + sizeof(struct sockaddr_storage);
    int initial_len = sizeof(struct sockaddr_storage);

    if (copy_to_user((void __user *)len_ptr, &initial_len, sizeof(int)) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->fd;
        regs.si = addr_ptr;
        regs.dx = len_ptr;

        if (ctx->is_peer && real_sys_getpeername) {
            ret = real_sys_getpeername(&regs);
        } else if (!ctx->is_peer && real_sys_getsockname) {
            ret = real_sys_getsockname(&regs);
        }

        if (ret == 0) {
            int final_len = 0;
            if (copy_from_user(&final_len, (void __user *)len_ptr, sizeof(int)) == 0) {
                if (final_len > 0 && final_len <= sizeof(struct sockaddr_storage)) {
                    if (copy_from_user(&reply.addr, (void __user *)addr_ptr, final_len) == 0) {
                        reply.addrlen = final_len;
                    }
                }
            }
        }
    }

    reply.error = ret;
    if (cluster_map[ctx->target_node]) {
        u32 msg_type = ctx->is_peer ? MATTX_MSG_SYS_GETPEERNAME_REPLY : MATTX_MSG_SYS_GETSOCKNAME_REPLY;
        mattx_comm_send(cluster_map[ctx->target_node], msg_type, &reply, sizeof(reply));
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}

static void handle_sys_getsockname_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getsockname_req *req = payload;
    struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_sockname_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            ctx->orig_pid = req->orig_pid; ctx->fd = req->fd; ctx->target_node = hdr->sender_id; ctx->is_peer = false;
            init_task_work(&ctx->cb, mattx_sockname_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}

static void handle_sys_getpeername_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getsockname_req *req = payload; // Same struct layout
    struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_sockname_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            ctx->orig_pid = req->orig_pid; ctx->fd = req->fd; ctx->target_node = hdr->sender_id; ctx->is_peer = true;
            init_task_work(&ctx->cb, mattx_sockname_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}


// --- THE NATIVE NETWORK REPLIES ---
static void handle_sys_sockname_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getsockname_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            if (reply->error == 0) {
                // Allocate enough space for the sockaddr AND the integer length!
                guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct sockaddr_storage) + sizeof(int), GFP_ATOMIC);
                if (guest_registry[i].rpc_read_buf) {
                    memcpy(guest_registry[i].rpc_read_buf, &reply->addr, sizeof(struct sockaddr_storage));
                    memcpy((char *)guest_registry[i].rpc_read_buf + sizeof(struct sockaddr_storage), &reply->addrlen, sizeof(int));
                }
            }
            guest_registry[i].rpc_done = true;
            break;
        }
    }
    spin_unlock(&guest_lock);
}


// --- THE DEPUTY HIJACK: SETSOCKOPT ---
struct mattx_setsockopt_ctx {
    struct callback_head cb;
    u32 orig_pid;
    int fd;
    int level;
    int optname;
    int optlen;
    int target_node;
    char optval[];
};

static void mattx_setsockopt_cb(struct callback_head *cb) {
    struct mattx_setsockopt_ctx *ctx = container_of(cb, struct mattx_setsockopt_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs;
    int ret = -EFAULT;
    struct mattx_sys_setsockopt_reply reply;

    unsigned long user_ptr = task_regs->sp - 128 - ctx->optlen;

    if (copy_to_user((void __user *)user_ptr, ctx->optval, ctx->optlen) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->fd;
        regs.si = ctx->level;
        regs.dx = ctx->optname;
        regs.r10 = user_ptr;
        regs.r8 = ctx->optlen;

        if (real_sys_setsockopt) ret = real_sys_setsockopt(&regs);
    }

    reply.orig_pid = ctx->orig_pid;
    reply.error = ret;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SETSOCKOPT_REPLY, &reply, sizeof(reply));

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}

static void handle_sys_setsockopt_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_setsockopt_req *req = payload;
    struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_setsockopt_ctx *ctx = kmalloc(sizeof(*ctx) + req->optlen, GFP_KERNEL);
        if (ctx) {
            ctx->orig_pid = req->orig_pid; ctx->fd = req->fd; ctx->level = req->level; ctx->optname = req->optname;
            ctx->optlen = req->optlen; ctx->target_node = hdr->sender_id; memcpy(ctx->optval, req->optval, req->optlen);
            init_task_work(&ctx->cb, mattx_setsockopt_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}

// --- THE DEPUTY HIJACK: GETSOCKOPT ---
struct mattx_getsockopt_ctx {
    struct callback_head cb;
    u32 orig_pid;
    int fd;
    int level;
    int optname;
    int optlen;
    int target_node;
};

static void mattx_getsockopt_cb(struct callback_head *cb) {
    struct mattx_getsockopt_ctx *ctx = container_of(cb, struct mattx_getsockopt_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs;
    int ret = -EFAULT;

    unsigned long val_ptr = task_regs->sp - 128 - ctx->optlen - sizeof(int);
    unsigned long len_ptr = val_ptr + ctx->optlen;
    int initial_len = ctx->optlen;

    if (copy_to_user((void __user *)len_ptr, &initial_len, sizeof(int)) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->fd;
        regs.si = ctx->level;
        regs.dx = ctx->optname;
        regs.r10 = val_ptr;
        regs.r8 = len_ptr;

        if (real_sys_getsockopt) ret = real_sys_getsockopt(&regs);
    }

    size_t reply_size = sizeof(struct mattx_sys_getsockopt_reply) + (ret == 0 ? ctx->optlen : 0);
    struct mattx_sys_getsockopt_reply *reply = kzalloc(reply_size, GFP_KERNEL);
    
    if (reply) {
        reply->orig_pid = ctx->orig_pid;
        reply->error = ret;
        if (ret == 0) {
            int final_len = 0;
            if (copy_from_user(&final_len, (void __user *)len_ptr, sizeof(int)) == 0) {
                reply->optlen = final_len;
                if (final_len > 0 && final_len <= ctx->optlen) {
                    // --- FIXED: Appease the compiler warning! ---
                    if (copy_from_user(reply->optval, (void __user *)val_ptr, final_len)) {
                        mattx_dbg("[HIJACK] Warning: Failed to copy getsockopt val\n");
                    }
                }
            }
        }
        if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_GETSOCKOPT_REPLY, reply, reply_size);
        kfree(reply);
    }

    kfree(ctx);
    set_current_state(TASK_STOPPED);
    schedule();
}


static void handle_sys_getsockopt_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getsockopt_req *req = payload;
    struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_getsockopt_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            ctx->orig_pid = req->orig_pid; ctx->fd = req->fd; ctx->level = req->level; ctx->optname = req->optname;
            ctx->optlen = req->optlen; ctx->target_node = hdr->sender_id;
            init_task_work(&ctx->cb, mattx_getsockopt_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}


static void handle_sys_getsockopt_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getsockopt_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            guest_registry[i].rpc_lseek_res = reply->optlen; // Store the length here!
            if (reply->error == 0 && reply->optlen > 0) {
                guest_registry[i].rpc_read_buf = kmalloc(reply->optlen, GFP_ATOMIC);
                if (guest_registry[i].rpc_read_buf) {
                    memcpy(guest_registry[i].rpc_read_buf, reply->optval, reply->optlen);
                }
            }
            guest_registry[i].rpc_done = true;
            break;
        }
    }
    spin_unlock(&guest_lock);
}


// === DATA PLANE: SENDMSG (Scatter/Gather) ===
struct mattx_sendmsg_kworker_ctx {
    struct work_struct work;
    u32 orig_pid;
    int fd;
    int flags;
    int addrlen;
    size_t datalen;
    struct sockaddr_storage addr;
    int target_node;
    char data[];
};

static void mattx_sendmsg_kworker(struct work_struct *work) {
    struct mattx_sendmsg_kworker_ctx *ctx = container_of(work, struct mattx_sendmsg_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->orig_pid, ctx->fd);
    struct mattx_sys_sendmsg_reply reply;
    int ret = -EBADF;

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock) {
            struct msghdr msg = {0};
            struct kvec iov;
            
            if (ctx->addrlen > 0) {
                msg.msg_name = &ctx->addr;
                msg.msg_namelen = ctx->addrlen;
            }
            
            iov.iov_base = ctx->data;
            iov.iov_len = ctx->datalen;
            
            // The data is already flattened! Send it natively!
            ret = kernel_sendmsg(sock, &msg, &iov, 1, ctx->datalen);
        } else {
            ret = -ENOTSOCK;
        }
        fput(file);
    }

    reply.orig_pid = ctx->orig_pid;
    reply.bytes_sent = ret;
    reply.error = (ret < 0) ? ret : 0;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SENDMSG_REPLY, &reply, sizeof(reply));
    }
    kfree(ctx);
}

static void handle_sys_sendmsg_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_sendmsg_req *req = payload;
        struct mattx_sendmsg_kworker_ctx *ctx = kmalloc(sizeof(*ctx) + req->datalen, GFP_ATOMIC);
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_sendmsg_kworker);
            ctx->orig_pid = req->orig_pid;
            ctx->fd = req->fd;
            ctx->flags = req->flags;
            ctx->addrlen = req->addrlen;
            ctx->datalen = req->datalen;
            ctx->target_node = hdr->sender_id;
            memcpy(&ctx->addr, &req->addr, sizeof(struct sockaddr_storage));
            memcpy(ctx->data, req->data, req->datalen);
            
            schedule_work(&ctx->work); // Dispatch to the Data Plane!
        }
    }
}


static void handle_sys_sendmsg_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_sendmsg_reply *reply = payload;
        int i;
        
        mattx_dbg("[RPC] Received SENDMSG_REPLY for Orig PID %u. Bytes sent: %zd\n", reply->orig_pid, reply->bytes_sent);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                // Store the result (bytes sent if successful, or the negative error code)
                guest_registry[i].rpc_fsync_res = (reply->bytes_sent >= 0) ? reply->bytes_sent : reply->error;
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



// === DATA PLANE: RECVMSG (Scatter/Gather) ===
struct mattx_recvmsg_kworker_ctx {
    struct work_struct work;
    u32 orig_pid;
    int fd;
    int flags;
    int addrlen;
    size_t datalen;
    int target_node;
};

static void mattx_recvmsg_kworker(struct work_struct *work) {
    struct mattx_recvmsg_kworker_ctx *ctx = container_of(work, struct mattx_recvmsg_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->orig_pid, ctx->fd);
    int ret = -EBADF;
    void *recv_buf = NULL;
    struct sockaddr_storage addr;
    int addrlen = 0;

    memset(&addr, 0, sizeof(addr));

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock) {
            recv_buf = kmalloc(ctx->datalen, GFP_KERNEL);
            if (recv_buf) {
                struct msghdr msg = {0};
                struct kvec iov;
                
                if (ctx->addrlen > 0) {
                    msg.msg_name = &addr;
                    msg.msg_namelen = sizeof(addr);
                }
                
                iov.iov_base = recv_buf;
                iov.iov_len = ctx->datalen;
                
                ret = kernel_recvmsg(sock, &msg, &iov, 1, ctx->datalen, ctx->flags);
                addrlen = msg.msg_namelen;
            } else {
                ret = -ENOMEM;
            }
        } else {
            ret = -ENOTSOCK;
        }
        fput(file);
    }

    size_t reply_size = sizeof(struct mattx_sys_recvmsg_reply) + (ret > 0 ? ret : 0);
    struct mattx_sys_recvmsg_reply *reply = kzalloc(reply_size, GFP_KERNEL);
    if (reply) {
        reply->orig_pid = ctx->orig_pid;
        reply->bytes_recv = ret;
        reply->error = (ret < 0) ? ret : 0;
        reply->addrlen = addrlen;
        if (addrlen > 0) {
            memcpy(&reply->addr, &addr, addrlen);
        }
        if (ret > 0 && recv_buf) {
            memcpy(reply->data, recv_buf, ret);
        }
        
        if (cluster_map[ctx->target_node]) {
            mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_RECVMSG_REPLY, reply, reply_size);
        }
        kfree(reply);
    }
    if (recv_buf) kfree(recv_buf);
    kfree(ctx);
}

static void handle_sys_recvmsg_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_recvmsg_req *req = payload;
        struct mattx_recvmsg_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_recvmsg_kworker);
            ctx->orig_pid = req->orig_pid;
            ctx->fd = req->fd;
            ctx->flags = req->flags;
            ctx->addrlen = req->addrlen;
            ctx->datalen = req->datalen;
            ctx->target_node = hdr->sender_id;
            
            schedule_work(&ctx->work); // Dispatch to the Data Plane!
        }
    }
}


static void handle_sys_recvmsg_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_recvmsg_reply *reply = payload;
        int i;

        mattx_dbg("[RPC] Received RECVMSG_REPLY for Orig PID %u. Bytes recv: %zd\n", reply->orig_pid, reply->bytes_recv);

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                
                int retval = (reply->bytes_recv >= 0) ? reply->bytes_recv : reply->error;
                guest_registry[i].rpc_fsync_res = retval;
                
                if (retval > 0) {
                    // --- THE MEMORY PACKING TRICK ---
                    // We allocate one contiguous block for: [DATA] + [SOCKADDR] + [ADDRLEN]
                    size_t buf_size = retval + sizeof(struct sockaddr_storage) + sizeof(int);
                    guest_registry[i].rpc_read_buf = kmalloc(buf_size, GFP_ATOMIC);
                    
                    if (guest_registry[i].rpc_read_buf) {
                        char *ptr = (char *)guest_registry[i].rpc_read_buf;
                        
                        // 1. Copy the flattened data payload
                        memcpy(ptr, reply->data, retval);
                        ptr += retval;
                        
                        // 2. Copy the sockaddr struct
                        memcpy(ptr, &reply->addr, sizeof(struct sockaddr_storage));
                        ptr += sizeof(struct sockaddr_storage);
                        
                        // 3. Copy the addrlen integer
                        memcpy(ptr, &reply->addrlen, sizeof(int));
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
    // Grab the identity of the user typing the command! ---
    req.uid = from_kuid(&init_user_ns, current_fsuid());
    req.gid = from_kgid(&init_user_ns, current_fsgid());
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

int mattx_rpc_vfs_unlink(int node_id, const char *path) {
    int i, slot = -1;
    u64 req_id;
    struct mattx_sys_unlink_req req;
    int err = -EIO;

    if (node_id == my_node_id) {
        // The Perfect VFS Dance (Local) ---
        // The Reverse Lookup (Local) ---
        struct path victim_path;
        err = kern_path(path, 0, &victim_path);
        if (!err) {
            struct dentry *parent = dget_parent(victim_path.dentry);
            struct inode *dir = parent->d_inode;
            
            err = mnt_want_write(victim_path.mnt);
            if (!err) {
                inode_lock_nested(dir, I_MUTEX_PARENT);
                
                // Double check the file wasn't moved while we were locking!
                if (victim_path.dentry->d_parent == parent && d_is_positive(victim_path.dentry)) {
                    err = vfs_unlink(mnt_idmap(victim_path.mnt), dir, victim_path.dentry, NULL);
                    if (err) printk(KERN_ERR "MattXFS:[LOCAL] vfs_unlink failed: %d\n", err);
                } else {
                    err = -ENOENT;
                }
                
                inode_unlock(dir);
                mnt_drop_write(victim_path.mnt);
            } else {
                printk(KERN_ERR "MattXFS:[LOCAL] mnt_want_write failed: %d\n", err);
            }
            dput(parent);
            path_put(&victim_path);
        }
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

    memset(&req, 0, sizeof(req));
    req.req_id = req_id;
    // Grab the identity! ---
    req.uid = from_kuid(&init_user_ns, current_fsuid());
    req.gid = from_kgid(&init_user_ns, current_fsgid());
    strncpy(req.path, path, sizeof(req.path) - 1);
    mattx_comm_send(cluster_map[node_id], MATTX_MSG_SYS_UNLINK_REQ, &req, sizeof(req));

    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    err = vfs_rpc_registry[slot].error;

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    return err;
}
EXPORT_SYMBOL(mattx_rpc_vfs_unlink);


void mattx_rpc_vfs_close(int node_id, int remote_fd) {
    // The Local Fast-Path! ---
    if (node_id == my_node_id) {
        struct file *f_to_close = NULL;
        
        spin_lock(&mfs_file_lock);
        if (remote_fd < MAX_FDS && mfs_open_files[remote_fd]) {
            f_to_close = mfs_open_files[remote_fd];
            mfs_open_files[remote_fd] = NULL;
        }
        spin_unlock(&mfs_file_lock);
        
        // Call fput safely OUTSIDE the spinlock!
        if (f_to_close) {
            fput(f_to_close);
        }
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
        const struct cred *old_cred = NULL;
        struct cred *new_cred = NULL;

        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;

        // Put on the mask of the remote user! ---
        old_cred = mattx_override_creds(req->uid, req->gid, &new_cred);

        f = filp_open(req->path, req->flags, req->mode);
        
        // Take off the mask
        mattx_revert_creds(old_cred, new_cred);

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

static void handle_sys_unlink_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_unlink_req *req = payload;
        struct mattx_sys_unlink_reply reply;
        struct task_struct *deputy = NULL;
        int err = -ENOENT;
        const struct cred *old_cred = NULL;
        struct cred *new_cred = NULL;

        memset(&reply, 0, sizeof(reply));
        reply.req_id = req->req_id;
        reply.orig_pid = req->orig_pid;

        mattx_dbg("[RPC] Received UNLINK request from Node %u for file: '%s'\n", hdr->sender_id, req->path);

        if (req->orig_pid != 0) {
            rcu_read_lock();
            deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();
            
            if (deputy) {
                if (deputy->mm) kthread_use_mm(deputy->mm);
                old_cred = override_creds(deputy->cred);
            }
        } else {
            // It's a MattXFS request! Use the network credentials! ---
            old_cred = mattx_override_creds(req->uid, req->gid, &new_cred);
        }

        // The Perfect VFS Dance (Remote) ---
        // The Reverse Lookup (Remote) ---
        struct path victim_path;
        err = kern_path(req->path, 0, &victim_path);
        if (!err) {
            struct dentry *parent = dget_parent(victim_path.dentry);
            struct inode *dir = parent->d_inode;
            
            err = mnt_want_write(victim_path.mnt);
            if (!err) {
                inode_lock_nested(dir, I_MUTEX_PARENT);
                
                if (victim_path.dentry->d_parent == parent && d_is_positive(victim_path.dentry)) {
                    err = vfs_unlink(mnt_idmap(victim_path.mnt), dir, victim_path.dentry, NULL);
                    if (err) printk(KERN_ERR "MattX:[RPC] vfs_unlink failed: %d\n", err);
                } else {
                    err = -ENOENT;
                }
                
                inode_unlock(dir);
                mnt_drop_write(victim_path.mnt);
            } else {
                printk(KERN_ERR "MattX:[RPC] mnt_want_write failed: %d\n", err);
            }
            dput(parent);
            path_put(&victim_path);
        } else {
            // Only print if it's a weird error, ignore standard "File not found" (-ENOENT)
            if (err != -ENOENT) printk(KERN_ERR "MattX:[RPC] kern_path failed: %d\n", err);
        }
        reply.error = err;

        if (deputy) {
            revert_creds(old_cred);
            if (deputy->mm) kthread_unuse_mm(deputy->mm);
            put_task_struct(deputy);
        } else {
            // Revert MattXFS credentials
            mattx_revert_creds(old_cred, new_cred);
        }

        if (cluster_map[hdr->sender_id]) {
            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_UNLINK_REPLY, &reply, sizeof(reply));
        }
    }
}

static void handle_sys_unlink_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_unlink_reply *reply = payload;
        int i;

        mattx_dbg("[RPC] Received UNLINK_REPLY. Error: %d\n", reply->error);

        // 1. Wake up MattXFS (if req_id is set)
        if (reply->req_id != 0) {
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

        // 2. Wake up Kprobe (if orig_pid is set)
        if (reply->orig_pid != 0) {
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
}

static void handle_vfs_close_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_vfs_close_req *req = payload;
        struct file *f_to_close = NULL;
        
        spin_lock(&mfs_file_lock);
        if (req->remote_fd < MAX_FDS && mfs_open_files[req->remote_fd]) {
            f_to_close = mfs_open_files[req->remote_fd];
            mfs_open_files[req->remote_fd] = NULL;
        }
        spin_unlock(&mfs_file_lock);
        
        // Call fput safely OUTSIDE the spinlock!
        if (f_to_close) {
            fput(f_to_close);
        }
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
    mattx_register_handler(MATTX_MSG_SYS_UNLINK_REQ, handle_sys_unlink_req);
    mattx_register_handler(MATTX_MSG_SYS_UNLINK_REPLY, handle_sys_unlink_reply);
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
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_CREATE_REQ, handle_sys_epoll_create_req);
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_CREATE_REPLY, handle_sys_epoll_create_reply);
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_CTL_REQ, handle_sys_epoll_ctl_req);
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_CTL_REPLY, handle_sys_generic_int_reply);
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_WAIT_REQ, handle_sys_epoll_wait_req);
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_WAIT_REPLY, handle_sys_epoll_wait_reply);
    mattx_register_handler(MATTX_MSG_SYS_GETSOCKNAME_REQ, handle_sys_getsockname_req);
    mattx_register_handler(MATTX_MSG_SYS_GETSOCKNAME_REPLY, handle_sys_sockname_reply);
    mattx_register_handler(MATTX_MSG_SYS_GETPEERNAME_REQ, handle_sys_getpeername_req);
    mattx_register_handler(MATTX_MSG_SYS_GETPEERNAME_REPLY, handle_sys_sockname_reply); // Reuses the same handler!
    mattx_register_handler(MATTX_MSG_SYS_SETSOCKOPT_REQ, handle_sys_setsockopt_req);
    mattx_register_handler(MATTX_MSG_SYS_SETSOCKOPT_REPLY, handle_sys_generic_int_reply);
    mattx_register_handler(MATTX_MSG_SYS_GETSOCKOPT_REQ, handle_sys_getsockopt_req);
    mattx_register_handler(MATTX_MSG_SYS_GETSOCKOPT_REPLY, handle_sys_getsockopt_reply);
    mattx_register_handler(MATTX_MSG_SYS_SENDMSG_REQ, handle_sys_sendmsg_req);
    mattx_register_handler(MATTX_MSG_SYS_SENDMSG_REPLY, handle_sys_sendmsg_reply);
    mattx_register_handler(MATTX_MSG_SYS_RECVMSG_REQ, handle_sys_recvmsg_req);
    mattx_register_handler(MATTX_MSG_SYS_RECVMSG_REPLY, handle_sys_recvmsg_reply);


    mattx_dbg(" [FILEIO] Network handlers registered.\n");
}


// Cleanup all open files to prevent shutdown hangs! ---
// -> Safe cleanup without sleeping inside spinlocks! ---
void mattx_fileio_exit(void) {
    int i, j;
    struct file *f;

    mattx_dbg(" [FILEIO] Purging all open files...\n");

    // 1. Clean up MFS open files
    spin_lock(&mfs_file_lock);
    for (i = 0; i < MAX_FDS; i++) {
        if (mfs_open_files[i]) {
            f = mfs_open_files[i];
            mfs_open_files[i] = NULL;
            spin_unlock(&mfs_file_lock);
            fput(f); // Safe to sleep here!
            spin_lock(&mfs_file_lock);
        }
    }
    spin_unlock(&mfs_file_lock);

    // 2. Clean up Export Registry files
    spin_lock(&export_lock);
    for (i = 0; i < export_count; i++) {
        for (j = 0; j < MAX_FDS; j++) {
            if (export_registry[i].remote_files[j]) {
                f = export_registry[i].remote_files[j];
                export_registry[i].remote_files[j] = NULL;
                spin_unlock(&export_lock);
                fput(f); // Safe to sleep here!
                spin_lock(&export_lock);
            }
        }
    }
    export_count = 0;
    spin_unlock(&export_lock);
    
    mattx_dbg(" [FILEIO] File purge complete.\n");
}

