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

#include <linux/eventfd.h>
#include "mattx.h"
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/namei.h> // For kern_path
#include <linux/nsproxy.h>
#include <linux/highmem.h> // Required for kmap_local_page
#include <linux/pagemap.h> // Required for read_mapping_page

#define MAX_VFS_RPC 64


// --- VFS RPC Registry ---
// This allows mattxfs.ko to sleep while waiting for network replies
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
    size_t data_cap; // size of data_buf: a reply can never write more than this
    loff_t ret_offset;

    bool in_use;    
};


static struct vfs_rpc_ctx vfs_rpc_registry[MAX_VFS_RPC];
static DEFINE_SPINLOCK(vfs_rpc_lock);
static u64 next_req_id = 1;

/*
 * Wait for the reply of the VFS RPC in @slot.
 *
 * wait_event_interruptible() also returns when a signal arrives, and the freeze that precedes a
 * migration or recall is a TWA_SIGNAL task_work, so a Surrogate blocked in here can be woken
 * with no reply yet. The read/write callers then went on to use bytes_rw, which still held the
 * result of an EARLIER request that had used the slot (it was never reset): a read of 16 bytes
 * "returned" the 30 bytes of the previous reply, and mattxfs_remote_file_read() copied 30 bytes out
 * of its 16-byte buffer. Hardened usercopy turned that into a kernel BUG on the node, which showed
 * up as the intermittent "lost at cycle N" of the pingpong test (about 1 cycle in 100).
 *
 * On an interrupted wait the slot is given back (a reply that arrives later finds no in-use slot
 * with its req_id and is dropped) and -ERESTARTSYS is returned, so the syscall is restarted.
 */
static int mattx_vfs_rpc_wait(int slot)
{
    int rc = wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    if (!rc)
        return 0;

    spin_lock(&vfs_rpc_lock);
    if (vfs_rpc_registry[slot].done) {          // the reply raced in; use it
        spin_unlock(&vfs_rpc_lock);
        return 0;
    }
    vfs_rpc_registry[slot].in_use = false;
    vfs_rpc_registry[slot].data_buf = NULL;
    spin_unlock(&vfs_rpc_lock);
    return rc;
}


// ============================================================================
// THE GLOBAL NODE-LOCAL MESI DIRECTORY (VM1)
// ============================================================================
struct mattx_dsm_master_dir *mattx_global_dsm_dir;
int mattx_dsm_dir_cap;
DEFINE_SPINLOCK(mattx_dsm_lock);

/*
 * The master directory has one entry per SYSV segment, sized by the segment (npages): there is no limit on
 * the size of a segment or on the number of segments. The array of entries grows on demand and its entries
 * never move inside it, so an index stays valid. Everything below needs mattx_dsm_lock unless it says
 * otherwise. The sharers of a page (a bitmap of MAX_NODES bits) are allocated lazily, one small mask per page
 * that really is shared (xarray: page index -> u64[MAX_NODES / 64]); a bigger chunk could not be allocated
 * under the spinlock.
 */
#define MATTX_DSM_MASK_WORDS (MAX_NODES / 64)

// The tables of a segment; an entry that is not in_use may still hold some until the reaper frees them
struct mattx_dsm_dir_tabs {
    u8 *state;
    u32 *owner;
    struct xarray *masks;
};

// Frees the tables of a segment. May sleep: never under a spinlock.
static void mattx_dsm_tabs_release(struct mattx_dsm_dir_tabs *t)
{
    if (t->masks) {
        unsigned long idx;
        void *entry;

        xa_for_each(t->masks, idx, entry)
            kfree(entry);
        xa_destroy(t->masks);
        kfree(t->masks);
    }
    kvfree(t->state);
    kvfree(t->owner);
    memset(t, 0, sizeof(*t));
}

// Takes the tables out of a directory entry (the entry keeps none)
static void mattx_dsm_dir_detach_tabs(struct mattx_dsm_master_dir *dir, struct mattx_dsm_dir_tabs *t)
{
    t->state = dir->page_state;
    t->owner = dir->page_owner_pid;
    t->masks = dir->page_shared_mask;
    dir->page_state = NULL;
    dir->page_owner_pid = NULL;
    dir->page_shared_mask = NULL;
    dir->npages = 0;
}

// The directory of a segment, or NULL if the segment is unknown or the page is beyond its end
struct mattx_dsm_master_dir *mattx_dsm_dir_lookup(u32 shmid, unsigned long page_idx)
{
    int d;

    for (d = 0; d < mattx_dsm_dir_cap; d++) {
        struct mattx_dsm_master_dir *dir = &mattx_global_dsm_dir[d];

        if (dir->in_use && dir->shmid == shmid)
            return page_idx < dir->npages ? dir : NULL;
    }
    return NULL;
}

// The sharers of a page, or NULL if nobody shares it
u64 *mattx_dsm_mask(struct mattx_dsm_master_dir *dir, unsigned long page_idx)
{
    return xa_load(dir->page_shared_mask, page_idx);
}

// The sharers of a page; creates the mask if there is none (GFP_ATOMIC: we hold the spinlock). NULL: out of memory.
u64 *mattx_dsm_mask_get(struct mattx_dsm_master_dir *dir, unsigned long page_idx)
{
    u64 *mask = xa_load(dir->page_shared_mask, page_idx);
    void *old;

    if (mask)
        return mask;
    mask = kzalloc(sizeof(u64) * MATTX_DSM_MASK_WORDS, GFP_ATOMIC);
    if (!mask)
        return NULL;
    old = xa_store(dir->page_shared_mask, page_idx, mask, GFP_ATOMIC);
    if (xa_is_err(old)) {
        kfree(mask);
        return NULL;
    }
    return mask;
}

// Nobody shares the page any more
void mattx_dsm_mask_clear(struct mattx_dsm_master_dir *dir, unsigned long page_idx)
{
    kfree(xa_erase(dir->page_shared_mask, page_idx));
}

// Registers a segment in the directory unless it is there already. May sleep: call it without any lock.
int mattx_dsm_dir_register(u32 shmid, u32 home_pid, unsigned long npages)
{
    struct mattx_dsm_dir_tabs mine = { 0 }, stale = { 0 };
    struct mattx_dsm_master_dir *grown = NULL, *old = NULL;
    int grown_cap = 0, slot = -1, ret = 0, d;
    bool known = false;

    if (!npages)
        return -EINVAL;

    spin_lock(&mattx_dsm_lock);
    for (d = 0; d < mattx_dsm_dir_cap; d++) {
        if (mattx_global_dsm_dir[d].in_use && mattx_global_dsm_dir[d].shmid == shmid) {
            known = true;
            break;
        }
    }
    spin_unlock(&mattx_dsm_lock);
    if (known)
        return 0;

    // The tables are sized by the segment: allocated before we take the spinlock (MATTX_PAGE_INVALID is 0)
    mine.state = kvzalloc(npages, GFP_KERNEL);
    mine.owner = kvcalloc(npages, sizeof(u32), GFP_KERNEL);
    mine.masks = kmalloc(sizeof(*mine.masks), GFP_KERNEL);
    if (!mine.state || !mine.owner || !mine.masks) {
        ret = -ENOMEM;
        goto out;
    }
    xa_init(mine.masks);

    for (;;) {
        int need = 0;

        spin_lock(&mattx_dsm_lock);
        for (d = 0; d < mattx_dsm_dir_cap; d++) {
            if (mattx_global_dsm_dir[d].in_use && mattx_global_dsm_dir[d].shmid == shmid) {
                slot = -2;                  // somebody registered it meanwhile
                break;
            }
        }
        if (slot == -1) {
            for (d = 0; d < mattx_dsm_dir_cap; d++) {
                if (!mattx_global_dsm_dir[d].in_use) {
                    slot = d;
                    break;
                }
            }
        }
        if (slot == -1 && grown && grown_cap > mattx_dsm_dir_cap) {
            // No free entry: grow with the array we allocated outside the lock (the entries keep their index)
            if (mattx_dsm_dir_cap)
                memcpy(grown, mattx_global_dsm_dir, mattx_dsm_dir_cap * sizeof(*grown));
            old = mattx_global_dsm_dir;
            slot = mattx_dsm_dir_cap;
            mattx_global_dsm_dir = grown;
            mattx_dsm_dir_cap = grown_cap;
            grown = NULL;
        }
        if (slot >= 0) {
            struct mattx_dsm_master_dir *dir = &mattx_global_dsm_dir[slot];

            mattx_dsm_dir_detach_tabs(dir, &stale);     // a reaped-later leftover of the entry
            dir->in_use = true;
            dir->shmid = shmid;
            dir->home_pid = home_pid;
            dir->npages = npages;
            dir->sweep_pos = 0;
            dir->page_state = mine.state;
            dir->page_owner_pid = mine.owner;
            dir->page_shared_mask = mine.masks;
            memset(&mine, 0, sizeof(mine));             // the directory owns them now
        } else if (slot == -1) {
            need = mattx_dsm_dir_cap ? mattx_dsm_dir_cap * 2 : 8;
        }
        spin_unlock(&mattx_dsm_lock);

        if (slot >= 0) {
            mattx_dbg("[MESI_VM1] Dynamically registered SHMID %u in Global Directory at slot %d (%lu pages)\n", shmid, slot, npages);
            break;
        }
        if (slot == -2)
            break;

        kvfree(grown);
        grown = kvcalloc(need, sizeof(*grown), GFP_KERNEL);
        grown_cap = need;
        if (!grown) {
            ret = -ENOMEM;
            break;
        }
    }

out:
    kvfree(old);
    kvfree(grown);
    mattx_dsm_tabs_release(&stale);
    mattx_dsm_tabs_release(&mine);      // unused if the segment was known or something failed
    return ret;
}

// Frees the tables of the entries that are not in use any more (a destroyed segment): the garbage collector of
// the shmctl(IPC_RMID) kprobe only clears in_use, because it must not sleep. Called by the sweeper thread.
void mattx_dsm_dir_reap(void)
{
    int rounds;

    for (rounds = 0; rounds < 8; rounds++) {
        struct mattx_dsm_dir_tabs dead = { 0 };
        int d;

        spin_lock(&mattx_dsm_lock);
        for (d = 0; d < mattx_dsm_dir_cap; d++) {
            struct mattx_dsm_master_dir *dir = &mattx_global_dsm_dir[d];

            if (!dir->in_use && dir->page_state) {
                mattx_dsm_dir_detach_tabs(dir, &dead);
                break;
            }
        }
        spin_unlock(&mattx_dsm_lock);

        if (!dead.state)
            return;
        mattx_dsm_tabs_release(&dead);
    }
}

// Module unload: gives the whole directory back
void mattx_dsm_dir_exit(void)
{
    struct mattx_dsm_master_dir *dirs;
    int cap, d;

    spin_lock(&mattx_dsm_lock);
    dirs = mattx_global_dsm_dir;
    cap = mattx_dsm_dir_cap;
    mattx_global_dsm_dir = NULL;
    mattx_dsm_dir_cap = 0;
    spin_unlock(&mattx_dsm_lock);

    for (d = 0; d < cap; d++) {
        struct mattx_dsm_dir_tabs t;

        mattx_dsm_dir_detach_tabs(&dirs[d], &t);
        mattx_dsm_tabs_release(&t);
    }
    kvfree(dirs);
}

// --- THE HPC FAST-PATH HELPER ---
bool is_hpc_local_lib(const char *path) {
    if (!path) return false;
    if (strncmp(path, "/lib/", 5) == 0) return true;
    if (strncmp(path, "/lib64/", 7) == 0) return true;
    if (strncmp(path, "/usr/lib/", 9) == 0) return true;
    if (strncmp(path, "/usr/lib64/", 11) == 0) return true;
    if (strncmp(path, "/etc/ld.so.cache", 16) == 0) return true;
    if (strncmp(path, "/usr/share/locale/", 18) == 0) return true;
    if (strncmp(path, "/proc/", 6) == 0) return true; // Localize /proc!
    if (strncmp(path, "/sys/", 5) == 0) return true;  // Localize /sys!    
    return false;
}
EXPORT_SYMBOL(is_hpc_local_lib);

// --- The VFS Proxy (Fake FDs) ---

// Forward declaration for the universal remote file fetcher
static struct file *mattx_get_remote_file(u32 orig_pid, int remote_fd);

// Helper to safely check if the RPC is done while we are sleeping
bool check_rpc_done(pid_t pid) {
    bool done = false;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (mattx_pid_shares_tgid_with_guest(pid, guest_registry[i].local_pid)) {
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


// --- THE IDENTITY THEFT HELPER ---
static int mattx_execute_as_deputy(struct task_struct *deputy, long (*syscall_fn)(const struct pt_regs *), struct pt_regs *regs, const char *path, int path_reg, void *out_buf, size_t out_size, int buf_reg, int fd_to_map, int fd_reg) {
    int ret = -ESRCH;
    if (!deputy) return ret;

    kthread_use_mm(deputy->mm);
    struct fs_struct *old_fs = current->fs;
    struct files_struct *old_files = current->files;
    const struct cred *old_cred = override_creds(deputy->cred);
    
    current->fs = deputy->fs;
    current->files = deputy->files;

    unsigned long scratchpad = task_pt_regs(deputy)->sp - 8192; 
    unsigned long path_ptr = scratchpad;
    unsigned long buf_ptr = scratchpad + 4096;

    if (path) {
        if (copy_to_user((void __user *)path_ptr, path, strlen(path) + 1)) {
            ret = -EFAULT;
            goto out;
        }
        if (path_reg == 1) regs->di = path_ptr;
        else if (path_reg == 2) regs->si = path_ptr;
    }

    if (out_buf && out_size > 0) {
        if (buf_reg == 1) regs->di = buf_ptr;
        else if (buf_reg == 2) regs->si = buf_ptr;
        else if (buf_reg == 3) regs->dx = buf_ptr;
    }

    int temp_fd = -1;
    struct file *mapped_file = NULL;

    // --- THE MATTXFS FD IMPLANT ---
    // If the FD is >= 1000, it's in the export registry. Raw syscalls can't see it!
    // We temporarily implant it into the Deputy's native FD table.
    if (fd_to_map >= 1000) {
        mapped_file = mattx_get_remote_file(deputy->pid, fd_to_map);
        if (mapped_file) {
            spin_lock(&deputy->files->file_lock);
            struct fdtable *fdt = files_fdtable(deputy->files);
            for (int j = 3; j < fdt->max_fds; j++) {
                if (!rcu_dereference_raw(fdt->fd[j])) {
                    rcu_assign_pointer(fdt->fd[j], mapped_file);
                    __set_bit(j, fdt->open_fds);
                    temp_fd = j;
                    break;
                }
            }
            spin_unlock(&deputy->files->file_lock);
            
            if (temp_fd >= 0) {
                if (fd_reg == 1) regs->di = temp_fd;
                else if (fd_reg == 2) regs->si = temp_fd;
            } else {
                fput(mapped_file);
                ret = -EMFILE;
                goto out;
            }
        } else {
            ret = -EBADF;
            goto out;
        }
    }

    ret = syscall_fn(regs);

    // Clean up the temporary implant!
    if (temp_fd >= 0) {
        spin_lock(&deputy->files->file_lock);
        struct fdtable *fdt = files_fdtable(deputy->files);
        rcu_assign_pointer(fdt->fd[temp_fd], NULL);
        __clear_bit(temp_fd, fdt->open_fds);
        spin_unlock(&deputy->files->file_lock);
        fput(mapped_file); // Release the FD table reference
        fput(mapped_file); // Release the mattx_get_remote_file reference
    }

    if (ret >= 0 && out_buf && out_size > 0) {
        if (copy_from_user(out_buf, (void __user *)buf_ptr, out_size)) {
            mattx_dbg("[KWORKER] Warning: Failed to read out_buf from scratchpad!\n");
        }
    }

out:
    current->fs = old_fs;
    current->files = old_files;
    revert_creds(old_cred);
    kthread_unuse_mm(deputy->mm);

    return ret;
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
        if (mattx_pid_shares_tgid_with_guest(my_pid, guest_registry[i].local_pid)) {
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
        if (mattx_pid_shares_tgid_with_guest(my_pid, guest_registry[i].local_pid)) {
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
        if (mattx_pid_shares_tgid_with_guest(my_pid, guest_registry[i].local_pid)) {
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
        if (mattx_pid_shares_tgid_with_guest(my_pid, guest_registry[i].local_pid)) {
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
        if (mattx_pid_shares_tgid_with_guest(my_pid, guest_registry[i].local_pid)) {
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
        if (mattx_pid_shares_tgid_with_guest(my_pid, guest_registry[i].local_pid)) {
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
    struct mattx_ticket *t;
    struct mattx_link *link;
    pid_t my_pid = current->pid;
    ssize_t ret;

    // Cap read size to prevent massive kmallocs on the network
    size_t to_read = min_t(size_t, count, 4096);

    if (!fd_info || fd_info->home_node < 0 || fd_info->home_node >= MAX_NODES) return -EIO;
    link = READ_ONCE(cluster_map[fd_info->home_node]);
    if (!link) return -EIO;

    // Only THIS thread waits, on its own ticket: no shared per-guest slot, no wake-up mix-ups
    t = mattx_ticket_add(fd_info->orig_pid, fd_info->home_node, to_read);
    if (!t) return -ENOMEM;

    memset(&req, 0, sizeof(req));
    req.orig_pid = fd_info->orig_pid;
    req.fd = fd_info->remote_fd;
    req.count = to_read;
    req.req_id = t->req_id;

    mattx_dbg("[WORMHOLE] Surrogate %d requesting %zu bytes from FD %u (req %u). Sleeping...\n", my_pid, to_read, req.fd, req.req_id);

    if (mattx_comm_send(link, MATTX_MSG_SYS_READ_REQ, &req, sizeof(req)) < 0) {
        ret = -EIO;
        goto out;
    }

    // Sleep until Node 1 replies. If we are interrupted (signal, freeze of a migration or recall)
    // the wait cancels the request and still hands back the bytes if the read had already happened.
    if (mattx_ticket_wait(t, MAX_SCHEDULE_TIMEOUT)) {
        ret = -EINTR;
        goto out;
    }

    ret = t->retval;
    if (ret > 0) {
        if (!t->data || (size_t)ret > t->data_len) {
            ret = -EIO;
        } else if (copy_to_user(buf, t->data, ret)) {
            ret = -EFAULT;
        } else {
            *pos += ret; // Advance the file position!
        }
    }

out:
    mattx_dbg("[WORMHOLE] Surrogate %d woke up! Read %zd bytes (req %u).\n", my_pid, ret, req.req_id);
    mattx_ticket_del(t);
    return ret;
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
            if (guest_registry[i].local_pid == current->tgid) {
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
    .owner = THIS_MODULE,
};

// --- Network Handlers for File I/O ---


/*
 * kernel_write() refuses files that have no ->write_iter, and an eventfd only has a plain
 * ->write (kernel: "kernel write not supported for file [eventfd]", -EINVAL). A Surrogate's
 * write() to an eventfd that lives on the home node was therefore dropped there, the eventfd
 * never became readable, and whoever epoll_wait()ed on it never woke up (mattx#14).
 * Signal the eventfd through its own interface instead.
 */
static ssize_t mattx_wormhole_kernel_write(struct file *file, const void *buf, size_t len, loff_t *pos)
{
    if (file->f_op && file->f_op->write && !file->f_op->write_iter) {
        struct eventfd_ctx *ctx = eventfd_ctx_fileget(file);

        if (!IS_ERR(ctx)) {
            u64 val, n;

            if (len < sizeof(val)) {
                eventfd_ctx_put(ctx);
                return -EINVAL;
            }
            memcpy(&val, buf, sizeof(val));
            if (val == U64_MAX) {
                eventfd_ctx_put(ctx);
                return -EINVAL;
            }
            // eventfd_signal() adds 1; a reader only cares that the counter is non-zero, so
            // a large increment is capped (the exact total is not reproduced).
            for (n = 0; n < min_t(u64, val, 1024); n++)
                eventfd_signal(ctx);
            eventfd_ctx_put(ctx);
            return sizeof(val);
        }
    }
    return kernel_write(file, buf, len, pos);
}

/*
 * FORWARDED WRITES (MATTX_MSG_SYSCALL_FWD)
 *
 * A Surrogate's write() to a pipe/socket/eventfd/... that lives on the home node is
 * fire-and-forget: the Surrogate has already moved on, so these writes must be applied in the
 * order they arrived.
 *
 * They used to run kernel_write() inline in the per-link receiver kthread. A write that blocks
 * (full pipe or socket buffer) froze EVERY message from that node, including the READ_REQ that
 * would have drained the pipe: the receiver-thread stall.
 *
 * Now the receiver only queues the write on one FIFO and returns; a single work item drains the
 * FIFO in order. A blocked write stalls this FIFO only, never the receiver kthread.
 */
struct mattx_fwd_write {
    struct list_head node;
    struct mattx_syscall_req req;   // req.data[] is a flexible array: must stay the LAST member
};

// --- Long-waiting kworkers (poll / epoll_wait proxies) ---
// mattx_poll_worker() and mattx_epoll_wait_kworker() sit in a loop for as long as the Surrogate's
// poll()/epoll_wait() lasts, which can be forever. On the system workqueue nothing could stop them at
// rmmod, so they kept running (or sleeping in schedule_timeout) in module text that rmmod had freed:
// "Oops: int3" in a kworker, a kdump reboot of the node (reproduced every time by unloading a home
// node whose guests run an epoll/poll loop on another node). They now run on a workqueue of their own:
// mattx_exit() sets mattx_unloading (the loops return at their next 10 ms tick) and then
// destroy_workqueue() waits until none is left. Falls back to the system workqueue if the allocation
// fails; that is the old behaviour.
static struct workqueue_struct *mattx_wait_wq;
static bool mattx_unloading;

static void mattx_queue_wait_work(struct work_struct *work) {
    queue_work(mattx_wait_wq ? mattx_wait_wq : system_wq, work);
}

// Called first thing in mattx_exit(): tell the waiters to leave.
void mattx_fileio_waiters_stop(void) {
    WRITE_ONCE(mattx_unloading, true);
}

// Called once nothing can queue a waiter any more (the receiver threads are gone).
void mattx_fileio_waiters_destroy(void) {
    if (mattx_wait_wq) {
        destroy_workqueue(mattx_wait_wq);
        mattx_wait_wq = NULL;
    }
}

static LIST_HEAD(mattx_fwd_write_list);
static DEFINE_SPINLOCK(mattx_fwd_write_lock);
static bool mattx_fwd_write_running;    // a drain work item is queued or running
static bool mattx_fwd_write_stopping;   // set by mattx_fileio_exit(): accept no more writes
static void mattx_fwd_write_drain(struct work_struct *work);
static DECLARE_WORK(mattx_fwd_write_work, mattx_fwd_write_drain);

// Applies ONE forwarded write on the home node. Runs in the drain work item, so it may sleep.
static void mattx_fwd_write_apply(struct mattx_syscall_req *req) {
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

        ret = mattx_wormhole_kernel_write(file, req->data, req->len, &pos);

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

static void mattx_fwd_write_drain(struct work_struct *work) {
    struct mattx_fwd_write *w;

    spin_lock(&mattx_fwd_write_lock);
    while (!list_empty(&mattx_fwd_write_list)) {
        w = list_first_entry(&mattx_fwd_write_list, struct mattx_fwd_write, node);
        list_del(&w->node);
        spin_unlock(&mattx_fwd_write_lock);

        mattx_fwd_write_apply(&w->req);   // may sleep (full pipe/socket): fine, we are not the receiver
        kfree(w);

        spin_lock(&mattx_fwd_write_lock);
    }
    mattx_fwd_write_running = false;
    spin_unlock(&mattx_fwd_write_lock);
}

static void handle_syscall_fwd(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_syscall_req *req = payload;
    struct mattx_fwd_write *w;
    size_t len;
    bool kick = false;
    bool stopping;

    if (!payload || hdr->length < sizeof(*req)) return;

    // Never trust req->len beyond what really arrived in this message
    len = min_t(size_t, req->len, hdr->length - sizeof(*req));

    w = kmalloc(sizeof(*w) + len, GFP_KERNEL);
    if (!w) {
        printk(KERN_ERR "MattX:[WORMHOLE] Forwarded write for FD %u dropped: out of memory\n", req->fd);
        return;
    }
    memcpy(&w->req, req, sizeof(*req));
    w->req.len = len;
    memcpy(w->req.data, req->data, len);

    spin_lock(&mattx_fwd_write_lock);
    stopping = mattx_fwd_write_stopping;
    if (!stopping) {
        list_add_tail(&w->node, &mattx_fwd_write_list);
        if (!mattx_fwd_write_running) {
            mattx_fwd_write_running = true;
            kick = true;
        }
    }
    spin_unlock(&mattx_fwd_write_lock);

    if (stopping) {
        kfree(w);   // module is unloading
        return;
    }
    if (kick) schedule_work(&mattx_fwd_write_work);
}

// --- DATA PLANE: OPENAT (Kworker Native Injector) ---
struct mattx_open_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_open_req req;
    int target_node;
};

static void mattx_open_kworker(struct work_struct *work) {
    struct mattx_open_kworker_ctx *ctx = container_of(work, struct mattx_open_kworker_ctx, work);
    struct mattx_sys_open_reply reply;
    struct file *filp = NULL;
    struct task_struct *deputy = NULL;
    int remote_fd = -1;
    int err = -EBADF;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;

    rcu_read_lock();
    deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID);
    if (deputy) get_task_struct(deputy);
    rcu_read_unlock();

    if (deputy) {
        // Temporarily wear the Deputy's face so file permissions are checked correctly!
        const struct cred *old_cred = override_creds(deputy->cred);

        // Open the file natively in the kernel! (No user-space scratchpad needed!)
        filp = filp_open(ctx->req.filename, ctx->req.flags, ctx->req.mode);
        
        revert_creds(old_cred);

        if (IS_ERR(filp)) {
            err = PTR_ERR(filp);
            mattx_dbg("[KWORKER] Failed to open file '%s' (err: %d)\n", ctx->req.filename, err);
        } else {
            // --- THE NATIVE INJECTOR ---
            // Forcefully inject the open file into the sleeping Deputy's FD table!
            if (deputy->files) {
                spin_lock(&deputy->files->file_lock);
                struct fdtable *fdt = files_fdtable(deputy->files);
                for (int j = 3; j < fdt->max_fds; j++) {
                    if (!rcu_dereference_raw(fdt->fd[j])) {
                        rcu_assign_pointer(fdt->fd[j], filp);
                        __set_bit(j, fdt->open_fds);
                        remote_fd = j;
                        err = 0;
                        break;
                    }
                }
                spin_unlock(&deputy->files->file_lock);
            }
            
            if (remote_fd == -1) {
                fput(filp);
                err = -ENFILE;
            } else {
                mattx_dbg("[KWORKER] Successfully injected '%s' into Deputy Native FD: %d\n", ctx->req.filename, remote_fd);
            }
        }
        put_task_struct(deputy);
    } else {
        err = -ESRCH;
    }

    reply.remote_fd = remote_fd;
    reply.error = err;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_OPEN_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
}

static void handle_sys_open_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_open_req *req = payload;
        struct mattx_open_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_open_kworker);
            memcpy(&ctx->req, req, sizeof(*req));
            ctx->target_node = hdr->sender_id;
            
            mattx_dbg("[RPC] Dispatching OPEN_REQ for '%s' to Kworker...\n", req->filename);
            schedule_work(&ctx->work);
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
            // Legacy Exported FD logic (for MattXFS files)
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
            // --- THE NATIVE CLOSE UPGRADE (Kworker Edition) ---
            // We do NOT wake up the Deputy! The Kworker safely plucks the FD 
            // out of the Deputy's table and closes it!
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


/*
 * READ_REQ (the IPC Wormhole read: pipe / FIFO / socket / eventfd / proxy fds)
 *
 * Runs in its own kworker (a blocking read must never run in the per-link receiver kthread, see
 * the receiver-thread stall) and is CANCELLABLE: the Surrogate thread that asked sleeps on its own
 * ticket (mattx_ticket_wait) and, when it is interrupted (signal, or the freeze of a migration or
 * recall), sends MATTX_MSG_SYS_REQ_CANCEL. Every request gets exactly ONE reply:
 *   - the real result, if the read had already happened (the bytes are never thrown away), or
 *   - -EINTR, if the cancel came first and nothing was consumed.
 * So a recalled Surrogate can no longer leave an orphan kworker behind that swallows the next bytes.
 */
struct mattx_read_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_read_req req;
    int target_node;
};

static atomic_t mattx_cancellable_inflight = ATOMIC_INIT(0);   // cancellable kworkers (read, recvfrom, ...) that still run
static bool mattx_cancellable_stopping;                        // set by mattx_fileio_exit()

static bool mattx_req_take_cancelled(int sender, u32 orig_pid, u32 req_id);

/*
 * Reads up to count bytes from a Deputy file for a Surrogate without ever blocking where the
 * Surrogate's CANCEL cannot reach us: wait for readiness (poll) in short sleeps, checking for a
 * cancel / unload / lost link on every round, then do ONE plain read. Files without ->poll
 * (regular files) and O_NONBLOCK files cannot block for long: they get the plain read at once.
 * Returns the bytes read, 0 (EOF) or -errno; -EINTR means "cancelled, nothing was consumed".
 * *abandoned is set when nobody is left to answer (module unloading, link gone).
 */
static ssize_t mattx_read_cancellable(struct file *file, struct task_struct *deputy, void *buf, size_t count,
                                      loff_t *pos, int sender, u32 orig_pid, u32 req_id, bool *abandoned)
{
    const struct cred *old_cred = NULL;
    unsigned long delay_us = 200;
    unsigned long deadline = 0;
    bool timed = false;
    ssize_t ret;

    if (file->f_op && file->f_op->poll && !(file->f_flags & O_NONBLOCK)) {
        struct socket *sock = sock_from_file(file);

        // SO_RCVTIMEO of a socket is honoured, like the plain read of a socket would
        if (sock && sock->sk && READ_ONCE(sock->sk->sk_rcvtimeo) != MAX_SCHEDULE_TIMEOUT) {
            deadline = jiffies + READ_ONCE(sock->sk->sk_rcvtimeo);
            timed = true;
        }

        for (;;) {
            __poll_t mask;

            if (mattx_req_take_cancelled(sender, orig_pid, req_id)) {
                mattx_dbg("[WORMHOLE] READ for PID %u req %u cancelled by the Surrogate, nothing consumed\n", orig_pid, req_id);
                return -EINTR;
            }
            if (READ_ONCE(mattx_cancellable_stopping) || !READ_ONCE(cluster_map[sender])) {
                *abandoned = true;
                return -EINTR;
            }

            mask = vfs_poll(file, NULL);
            if (mask & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                break;      // data, EOF or an error is waiting: the read below returns at once
            if (timed && time_after(jiffies, deadline))
                return -EAGAIN;

            usleep_range(delay_us, delay_us + delay_us / 2);    // 0.2 ms ... ~10 ms
            if (delay_us < 10000)
                delay_us *= 2;
        }
    }

    if (deputy) {
        if (deputy->mm) kthread_use_mm(deputy->mm);
        old_cred = override_creds(deputy->cred);
    }
    ret = kernel_read(file, buf, count, pos);
    if (deputy) {
        revert_creds(old_cred);
        if (deputy->mm) kthread_unuse_mm(deputy->mm);
    }
    return ret;
}

static void mattx_read_kworker(struct work_struct *work) {
    struct mattx_read_kworker_ctx *ctx = container_of(work, struct mattx_read_kworker_ctx, work);
    struct mattx_sys_read_req *req = &ctx->req;
    struct mattx_sys_read_reply fail = { .orig_pid = req->orig_pid, .req_id = req->req_id };
    struct mattx_sys_read_reply *reply = NULL;
    struct task_struct *deputy = NULL;
    struct file *file = NULL;
    struct mattx_link *link;
    bool abandoned = false;
    // Both senders (mattx_fake_read and the rpc worker) cap a READ_REQ at 4096 bytes as well
    size_t count = min_t(size_t, req->count, 4096);
    ssize_t ret = -EBADF;
    int i;

    mattx_dbg("[WORMHOLE] Received READ request for FD %u (req %u) from Node %d. Reading from Deputy (kworker)...\n", req->fd, req->req_id, ctx->target_node);

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
            // The Deputy reference is kept until we are done with deputy->mm and deputy->cred
        }
    }

    if (file) {
        reply = kmalloc(sizeof(*reply) + count, GFP_KERNEL);
        if (reply) {
            loff_t pos = file->f_pos;

            // Reads straight into the reply buffer; may wait, but can be cancelled
            ret = mattx_read_cancellable(file, deputy, reply->data, count, &pos,
                                         ctx->target_node, req->orig_pid, req->req_id, &abandoned);
            if (ret >= 0) {
                file->f_pos = pos;
            }
        } else {
            ret = -ENOMEM;
        }
        fput(file);
    }
    if (deputy) put_task_struct(deputy);

    link = READ_ONCE(cluster_map[ctx->target_node]);
    if (abandoned || !link) {
        mattx_dbg("[WORMHOLE] READ for FD %u (req %u): nobody left to answer, reply dropped\n", req->fd, req->req_id);
    } else if (reply) {
        reply->orig_pid = req->orig_pid;
        reply->req_id = req->req_id;
        reply->bytes_read = ret;
        reply->error = (ret < 0) ? ret : 0;
        mattx_comm_send(link, MATTX_MSG_SYS_READ_REPLY, reply, sizeof(*reply) + (ret > 0 ? ret : 0));
    } else {
        // No such FD (-EBADF) or out of memory (-ENOMEM): still answer, the Surrogate is waiting!
        fail.bytes_read = ret;
        fail.error = ret;
        mattx_comm_send(link, MATTX_MSG_SYS_READ_REPLY, &fail, sizeof(fail));
    }

    if (reply) kfree(reply);
    kfree(ctx);
    atomic_dec(&mattx_cancellable_inflight);
}

static void handle_sys_read_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_read_kworker_ctx *ctx;

    if (!payload || hdr->length < sizeof(struct mattx_sys_read_req)) return;
    if (hdr->sender_id >= MAX_NODES || READ_ONCE(mattx_cancellable_stopping)) return;

    ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx) {
        printk(KERN_ERR "MattX:[WORMHOLE] READ_REQ dropped: out of memory\n");
        return;
    }
    INIT_WORK(&ctx->work, mattx_read_kworker);
    memcpy(&ctx->req, payload, sizeof(ctx->req));
    ctx->target_node = hdr->sender_id;
    atomic_inc(&mattx_cancellable_inflight);
    schedule_work(&ctx->work);
}

static void handle_sys_read_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // A reply that carries a ticket id belongs to exactly one waiting thread (mattx_ticket_*).
    // req_id 0 is the legacy reply (pread64): it still goes through the per-guest slot below.
    if (payload && hdr->length >= sizeof(struct mattx_sys_read_reply) &&
        ((struct mattx_sys_read_reply *)payload)->req_id) {
        struct mattx_sys_read_reply *reply = (struct mattx_sys_read_reply *)payload;
        size_t datalen = hdr->length - sizeof(*reply);
        long retval = reply->bytes_read;

        if (retval > 0 && (size_t)retval > datalen)
            retval = -EIO;      // the reply claims more bytes than it carries

        mattx_dbg("[RPC] Received READ_REPLY for Orig PID %u req %u. Bytes read: %zd\n",
                  reply->orig_pid, reply->req_id, reply->bytes_read);
        if (!mattx_ticket_complete(hdr->sender_id, reply->orig_pid, reply->req_id, retval,
                                   reply->data, retval > 0 ? retval : 0))
            mattx_dbg("[RPC] READ_REPLY req %u: nobody waits for it any more, dropped\n", reply->req_id);
        return;
    }

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


// === DATA PLANE: LSEEK ===
struct mattx_lseek_kworker_ctx { struct work_struct work; struct mattx_sys_lseek_req req; int target_node; };

static void mattx_lseek_kworker(struct work_struct *work) {
    struct mattx_lseek_kworker_ctx *ctx = container_of(work, struct mattx_lseek_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    struct mattx_sys_lseek_reply reply;
    loff_t ret = -EBADF;

    if (file) {
        ret = vfs_llseek(file, ctx->req.offset, ctx->req.whence);
        fput(file);
    }

    reply.orig_pid = ctx->req.orig_pid;
    reply.result_offset = ret;
    reply.error = (ret < 0) ? ret : 0;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_LSEEK_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}

static void handle_sys_lseek_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_lseek_req *req = payload;
    struct mattx_lseek_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_lseek_kworker);
        memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}

// === DATA PLANE: FSYNC ===
struct mattx_fsync_kworker_ctx { struct work_struct work; struct mattx_sys_fsync_req req; int target_node; };

static void mattx_fsync_kworker(struct work_struct *work) {
    struct mattx_fsync_kworker_ctx *ctx = container_of(work, struct mattx_fsync_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    struct mattx_sys_fsync_reply reply;
    int ret = -EBADF;

    if (file) {
        ret = vfs_fsync_range(file, ctx->req.start, ctx->req.end, ctx->req.datasync);
        fput(file);
    }

    reply.orig_pid = ctx->req.orig_pid;
    reply.error = ret;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_FSYNC_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}

static void handle_sys_fsync_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_fsync_req *req = payload;
    struct mattx_fsync_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_fsync_kworker);
        memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}

// === DATA PLANE: STATX ===
struct mattx_statx_kworker_ctx { struct work_struct work; struct mattx_sys_statx_req req; int target_node; };

static void mattx_statx_kworker(struct work_struct *work) {
    struct mattx_statx_kworker_ctx *ctx = container_of(work, struct mattx_statx_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    struct mattx_sys_statx_reply reply;
    int ret = -EBADF;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;

    if (file) {
        struct kstat stat;
        ret = vfs_getattr(&file->f_path, &stat, ctx->req.mask, ctx->req.flags & AT_STATX_SYNC_TYPE);
        if (ret == 0) {
            reply.statx_buf.stx_mask = stat.result_mask;
            reply.statx_buf.stx_blksize = stat.blksize;
            reply.statx_buf.stx_attributes = stat.attributes;
            reply.statx_buf.stx_nlink = stat.nlink;
            reply.statx_buf.stx_uid = from_kuid_munged(&init_user_ns, stat.uid);
            reply.statx_buf.stx_gid = from_kgid_munged(&init_user_ns, stat.gid);
            reply.statx_buf.stx_mode = stat.mode;
            reply.statx_buf.stx_ino = stat.ino;
            reply.statx_buf.stx_size = stat.size;
            reply.statx_buf.stx_blocks = stat.blocks;
        }
        fput(file);
    }

    reply.error = ret;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_STATX_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}

static void handle_sys_statx_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_statx_req *req = payload;
    struct mattx_statx_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_statx_kworker);
        memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}

// === CONTROL PLANE: DUP (Kworker Native Injector) ===
struct mattx_dup_kworker_ctx { struct work_struct work; struct mattx_sys_dup_req req; int target_node; };

static void mattx_dup_kworker(struct work_struct *work) {
    struct mattx_dup_kworker_ctx *ctx = container_of(work, struct mattx_dup_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.old_remote_fd);
    struct mattx_sys_dup_reply reply;
    int new_fd = -EBADF;
    struct file *file_to_close = NULL;

    if (file) {
        struct task_struct *deputy = NULL;
        rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
        
        if (deputy && deputy->files) {
            spin_lock(&deputy->files->file_lock);
            struct fdtable *fdt = files_fdtable(deputy->files);
            int target_fd = ctx->req.new_local_fd;
            
            if (target_fd < 0) { // Dynamic dup()
                for (int j = 3; j < fdt->max_fds; j++) {
                    if (!rcu_dereference_raw(fdt->fd[j])) { target_fd = j; break; }
                }
            }
            
            if (target_fd >= 0 && target_fd < fdt->max_fds) {
                file_to_close = rcu_dereference_raw(fdt->fd[target_fd]); // In case dup2 overwrites an existing FD!
                rcu_assign_pointer(fdt->fd[target_fd], file);
                __set_bit(target_fd, fdt->open_fds);
                get_file(file); // Increment refcount for the new FD slot!
                new_fd = target_fd;
            }
            spin_unlock(&deputy->files->file_lock);
            
            // Safely close the overwritten file OUTSIDE the spinlock!
            if (file_to_close) fput(file_to_close); 
        }
        if (deputy) put_task_struct(deputy);
        fput(file); // Release the reference we got from mattx_get_remote_file
    }

    reply.orig_pid = ctx->req.orig_pid;
    reply.new_remote_fd = new_fd;
    reply.error = (new_fd < 0) ? new_fd : 0;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_DUP_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}

static void handle_sys_dup_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_dup_req *req = payload;
    struct mattx_dup_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_dup_kworker);
        memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
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



// --- DATA PLANE: SOCKET (Kworker Native Injector) ---
struct mattx_socket_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_socket_req req;
    int target_node;
};

static void mattx_socket_kworker(struct work_struct *work) {
    struct mattx_socket_kworker_ctx *ctx = container_of(work, struct mattx_socket_kworker_ctx, work);
    struct mattx_sys_socket_reply reply;
    struct socket *sock = NULL;
    struct file *file = NULL;
    struct task_struct *deputy = NULL;
    int remote_fd = -1;
    int err;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;

    // Strip non-type flags for sock_create, but keep them for the file!
    int type = ctx->req.type & SOCK_TYPE_MASK;
    int file_flags = ctx->req.type & (O_CLOEXEC | O_NONBLOCK);

    // 1. Create the socket natively in the kernel!
    err = sock_create(ctx->req.domain, type, ctx->req.protocol, &sock);
    
    if (err == 0) {
        // 2. Allocate a file descriptor for the socket
        file = sock_alloc_file(sock, file_flags, NULL);
        
        if (IS_ERR(file)) {
            err = PTR_ERR(file);
            mattx_dbg("[KWORKER] Failed to allocate file for socket (err: %d)\n", err);
        } else {
            // --- THE NATIVE INJECTOR ---
            rcu_read_lock();
            deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();

            if (deputy && deputy->files) {
                spin_lock(&deputy->files->file_lock);
                struct fdtable *fdt = files_fdtable(deputy->files);
                for (int j = 3; j < fdt->max_fds; j++) {
                    if (!rcu_dereference_raw(fdt->fd[j])) {
                        rcu_assign_pointer(fdt->fd[j], file);
                        __set_bit(j, fdt->open_fds);
                        remote_fd = j;
                        err = 0;
                        break;
                    }
                }
                spin_unlock(&deputy->files->file_lock);
            }
            
            if (remote_fd == -1) {
                fput(file);
                err = -ENFILE;
            } else {
                mattx_dbg("[KWORKER] Successfully injected socket into Deputy Native FD: %d\n", remote_fd);
            }
            if (deputy) put_task_struct(deputy);
        }
    } else {
        mattx_dbg("[KWORKER] sock_create failed (err: %d)\n", err);
    }

    reply.remote_fd = remote_fd;
    reply.error = err;

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SOCKET_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
}

static void handle_sys_socket_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_socket_req *req = payload;
        struct mattx_socket_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_socket_kworker);
            memcpy(&ctx->req, req, sizeof(*req));
            ctx->target_node = hdr->sender_id;
            
            mattx_dbg("[RPC] Dispatching SOCKET_REQ to Kworker...\n");
            schedule_work(&ctx->work);
        }
    }
}


// --- DATA PLANE: BIND (Kworker Native Injector) ---
struct mattx_bind_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_bind_req req;
    int target_node;
};

static void mattx_bind_kworker(struct work_struct *work) {
    struct mattx_bind_kworker_ctx *ctx = container_of(work, struct mattx_bind_kworker_ctx, work);
    struct mattx_sys_bind_reply reply;
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    int err = -EBADF;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock) {
            // Execute natively in the Kworker! No signals, no scratchpads!
            err = kernel_bind(sock, MATTX_SA_CAST(&ctx->req.addr), ctx->req.addrlen);
            mattx_dbg("[KWORKER] Executed kernel_bind natively. Result: %d\n", err);
        } else {
            err = -ENOTSOCK;
        }
        fput(file);
    }

    reply.error = err;
    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_BIND_REPLY, &reply, sizeof(reply));
    }
    kfree(ctx);
}

static void handle_sys_bind_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_bind_req *req = payload;
        struct mattx_bind_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_bind_kworker);
            memcpy(&ctx->req, req, sizeof(*req));
            ctx->target_node = hdr->sender_id;
            
            mattx_dbg("[RPC] Dispatching BIND_REQ to Kworker...\n");
            schedule_work(&ctx->work);
        }
    }
}

// --- DATA PLANE: CONNECT (Kworker Native Injector & Smart Waiter) ---
/*
 * CONNECT_REQ (connect() on a socket that lives on the home node)
 *
 * Same contract as the wormhole read, recvfrom and accept: its own kworker, CANCELLABLE, exactly ONE
 * reply (req_id echoed): the real result, or -EINTR when the Surrogate's CANCEL came first.
 *
 * kernel_connect() is only ever called with O_NONBLOCK, so it never sleeps where the CANCEL cannot
 * reach us. The waiting is done here, on the socket's own wait queue (the state change of the
 * handshake wakes it, like sk_state_change of any socket), and the blocking semantics are the
 * application's, like the kernel's own connect():
 *   - an O_NONBLOCK socket returns -EINPROGRESS at once and the handshake runs on,
 *   - SO_SNDTIMEO of the socket is honoured (-EINPROGRESS when it expires, the handshake runs on),
 *   - a cancel returns -EINTR and does NOT abort the handshake: a repeated connect() then answers
 *     -EALREADY / 0 / -EISCONN natively (exactly what the kernel does after a signal).
 * The old 50 ms poll loop (no timeout, no cancel) is gone.
 */
struct mattx_connect_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_connect_req req;
    int target_node;
};

// Connects without ever blocking in kernel_connect(). 0: connected. -EINPROGRESS: non-blocking socket /
// SO_SNDTIMEO expired. -EINTR: cancelled, or nobody is left to answer (*abandoned: unload, link gone).
// Other: the real error of the connect or of the handshake.
static int mattx_connect_cancellable(struct socket *sock, struct file *file, struct sockaddr_storage *addr,
                                     int addrlen, int sender, u32 orig_pid, u32 req_id, bool *abandoned)
{
    bool nonblock = file->f_flags & O_NONBLOCK;
    long sndtimeo = nonblock ? 0 : READ_ONCE(sock->sk->sk_sndtimeo);
    bool timed = !nonblock && sndtimeo != MAX_SCHEDULE_TIMEOUT;
    unsigned long deadline = jiffies + sndtimeo;
    bool connecting = false;
    DEFINE_WAIT_FUNC(wait, woken_wake_function);
    int err;

    // On the wait queue BEFORE the first look, so that no wake-up can slip between look and sleep
    add_wait_queue(sk_sleep(sock->sk), &wait);

    for (;;) {
        // 1. Look: never blocks
        if (!connecting) {
            err = kernel_connect(sock, MATTX_SA_CAST(addr), addrlen, O_NONBLOCK);
            mattx_dbg("[KWORKER] Executed kernel_connect natively. Result: %d (NonBlock: %d)\n", err, nonblock);
            if (err == -EINPROGRESS) {
                // The handshake is under way: from now on we only wait for its end
                connecting = true;
                if (!nonblock)
                    mattx_dbg("[CONNECT_DEBUG] Blocking connect returned -EINPROGRESS. Waiting for the handshake...\n");
            } else if (err == -EAGAIN && !nonblock && sock->sk->sk_family == AF_UNIX) {
                // The listener's backlog is full: a blocking connect() waits for room. Try again below.
            } else {
                break;                          // connected (0) or a real error
            }
        } else {
            __poll_t mask = vfs_poll(file, NULL);

            if (mask & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
                mattx_dbg("[CONNECT_DEBUG] Socket handshake complete! Leaving the wait.\n");
                // Check the final socket error state to see if the handshake succeeded!
                err = -sock_error(sock->sk);    // sock_error() returns positive: we need negative
                mattx_dbg("[CONNECT_DEBUG] Final handshake result: %d\n", err);
                break;
            }
        }

        // 2. Not connected yet
        if (nonblock)
            break;                              // err stays -EINPROGRESS
        if (timed && time_after(jiffies, deadline))
            break;                              // err stays -EINPROGRESS (-EAGAIN for a full AF_UNIX backlog)
        if (mattx_req_take_cancelled(sender, orig_pid, req_id)) {
            mattx_dbg("[WORMHOLE] CONNECT for PID %u req %u cancelled by the Surrogate, handshake keeps running\n", orig_pid, req_id);
            err = -EINTR;
            break;
        }
        if (READ_ONCE(mattx_cancellable_stopping) || !READ_ONCE(cluster_map[sender])) {
            *abandoned = true;
            err = -EINTR;
            break;
        }

        // 3. Sleep until the socket wakes us (or 10 ms, or the SO_SNDTIMEO deadline)
        {
            long to = msecs_to_jiffies(10);

            if (timed && (long)(deadline - jiffies) < to)
                to = (long)(deadline - jiffies) > 0 ? (long)(deadline - jiffies) : 1;
            wait_woken(&wait, TASK_INTERRUPTIBLE, to);
        }
    }

    remove_wait_queue(sk_sleep(sock->sk), &wait);
    return err;
}

static void mattx_connect_kworker(struct work_struct *work) {
    struct mattx_connect_kworker_ctx *ctx = container_of(work, struct mattx_connect_kworker_ctx, work);
    struct mattx_sys_connect_reply reply;
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    struct mattx_link *link;
    bool abandoned = false;
    int err = -EBADF;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;
    reply.req_id = ctx->req.req_id;

    mattx_dbg("[WORMHOLE] Received CONNECT request for FD %u (req %u) from Node %d (kworker)...\n", ctx->req.fd, ctx->req.req_id, ctx->target_node);

    if (file) {
        struct socket *sock = sock_from_file(file);

        if (!sock || !sock->sk) {
            err = -ENOTSOCK;
        } else if (ctx->req.addrlen < 0 || (size_t)ctx->req.addrlen > sizeof(ctx->req.addr)) {
            err = -EINVAL;
        } else {
            err = mattx_connect_cancellable(sock, file, &ctx->req.addr, ctx->req.addrlen,
                                            ctx->target_node, ctx->req.orig_pid, ctx->req.req_id, &abandoned);
        }
        fput(file);
    }

    reply.error = err;

    link = READ_ONCE(cluster_map[ctx->target_node]);
    if (abandoned || !link) {
        mattx_dbg("[WORMHOLE] CONNECT for FD %u (req %u): nobody left to answer, reply dropped\n", ctx->req.fd, ctx->req.req_id);
    } else {
        mattx_comm_send(link, MATTX_MSG_SYS_CONNECT_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    atomic_dec(&mattx_cancellable_inflight);
}

static void handle_sys_connect_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_connect_req *req = payload;
    struct mattx_connect_kworker_ctx *ctx;

    if (!payload || hdr->length < sizeof(*req)) return;
    if (hdr->sender_id >= MAX_NODES || READ_ONCE(mattx_cancellable_stopping)) return;

    ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx) {
        printk(KERN_ERR "MattX:[WORMHOLE] CONNECT_REQ dropped: out of memory\n");
        return;
    }
    INIT_WORK(&ctx->work, mattx_connect_kworker);
    memcpy(&ctx->req, req, sizeof(*req));
    ctx->target_node = hdr->sender_id;

    mattx_dbg("[RPC] Dispatching CONNECT_REQ to Kworker...\n");
    atomic_inc(&mattx_cancellable_inflight);
    schedule_work(&ctx->work);
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
    // A reply that carries a ticket id belongs to exactly one waiting thread (mattx_ticket_*).
    // req_id 0 is the legacy reply of the SIGSTOP fallback path: it goes through the slot below.
    if (payload && hdr->length >= sizeof(struct mattx_sys_connect_reply) &&
        ((struct mattx_sys_connect_reply *)payload)->req_id) {
        struct mattx_sys_connect_reply *reply = (struct mattx_sys_connect_reply *)payload;

        mattx_dbg("[RPC] Received CONNECT_REPLY for Orig PID %u req %u. Error: %d\n", reply->orig_pid, reply->req_id, reply->error);
        // Nothing to clean up if the thread gave up: a connected socket simply stays connected on the home node
        if (!mattx_ticket_complete(hdr->sender_id, reply->orig_pid, reply->req_id, reply->error, NULL, 0))
            mattx_dbg("[RPC] CONNECT_REPLY req %u: nobody waits for it any more, dropped\n", reply->req_id);
        return;
    }

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


// --- DATA PLANE: LISTEN (Kworker Native Injector) ---
struct mattx_listen_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_listen_req req;
    int target_node;
};

static void mattx_listen_kworker(struct work_struct *work) {
    struct mattx_listen_kworker_ctx *ctx = container_of(work, struct mattx_listen_kworker_ctx, work);
    struct mattx_sys_listen_reply reply;
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    int err = -EBADF;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock) {
            // Execute natively in the Kworker!
            err = kernel_listen(sock, ctx->req.backlog);
            mattx_dbg("[KWORKER] Executed kernel_listen natively. Result: %d\n", err);
        } else {
            err = -ENOTSOCK;
        }
        fput(file);
    }

    reply.error = err;
    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_LISTEN_REPLY, &reply, sizeof(reply));
    }
    kfree(ctx);
}

static void handle_sys_listen_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_listen_req *req = payload;
        struct mattx_listen_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        if (ctx) {
            INIT_WORK(&ctx->work, mattx_listen_kworker);
            memcpy(&ctx->req, req, sizeof(*req));
            ctx->target_node = hdr->sender_id;
            
            mattx_dbg("[RPC] Dispatching LISTEN_REQ to Kworker...\n");
            schedule_work(&ctx->work);
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
/*
 * RECV_REQ (recv()/recvfrom() on a socket that lives on the home node)
 *
 * Same contract as the wormhole read: the request runs in its own kworker, is CANCELLABLE, and gets
 * exactly ONE reply (req_id echoed): the real result (bytes that were received are never thrown
 * away) or -EINTR when the Surrogate's CANCEL came first and nothing was consumed.
 *
 * The socket is only ever read with MSG_DONTWAIT; the waiting is done here, so the blocking
 * semantics are the application's, not the kworker's:
 *   - MSG_DONTWAIT, or an O_NONBLOCK socket: -EAGAIN at once when nothing is queued. The kernel's own
 *     recvfrom() adds the O_NONBLOCK of the file, kernel_recvmsg() does not: the old code BLOCKED
 *     the kworker for a non-blocking socket.
 *   - SO_RCVTIMEO is honoured (-EAGAIN when it expires).
 *   - MSG_WAITALL is honoured for stream sockets (a cancel or an error after some bytes returns them).
 * The wait sleeps on the socket's own wait queue (sk_sleep, like sk_wait_data): data, EOF and errors
 * wake us at once, and a 10 ms ceiling looks at cancel / unload / lost link.
 */
struct mattx_recvfrom_kworker_ctx {
    struct work_struct work;
    u32 orig_pid;
    u32 req_id;
    int fd;
    size_t size;
    int flags;
    int target_node;
};

// Returns the bytes received (for MSG_TRUNC on a datagram: its real length), 0 (EOF) or -errno;
// -EINTR: cancelled, nothing consumed. *abandoned: nobody is left to answer (unload, link gone).
static ssize_t mattx_recv_cancellable(struct socket *sock, struct file *file, void *buf, size_t size, int flags,
                                      int sender, u32 orig_pid, u32 req_id, bool *abandoned)
{
    bool nonblock = (flags & MSG_DONTWAIT) || (file->f_flags & O_NONBLOCK);
    bool waitall = (flags & MSG_WAITALL) && !(flags & MSG_PEEK) && sock->type == SOCK_STREAM;
    long rcvtimeo = nonblock ? 0 : READ_ONCE(sock->sk->sk_rcvtimeo);
    bool timed = !nonblock && rcvtimeo != MAX_SCHEDULE_TIMEOUT;
    unsigned long deadline = jiffies + rcvtimeo;
    DEFINE_WAIT_FUNC(wait, woken_wake_function);
    size_t total = 0;
    ssize_t result;
    ssize_t ret;

    // On the wait queue BEFORE the first look, so that no wake-up can slip between look and sleep
    add_wait_queue(sk_sleep(sock->sk), &wait);

    for (;;) {
        struct msghdr msg = {0};
        struct kvec iov = { .iov_base = buf + total, .iov_len = size - total };

        // 1. Look: never blocks
        ret = kernel_recvmsg(sock, &msg, &iov, 1, size - total, (flags & ~MSG_WAITALL) | MSG_DONTWAIT);
        if (ret > 0) {
            if ((size_t)ret > size - total) {   // MSG_TRUNC: the real length of a datagram
                result = ret;
                goto out;
            }
            total += ret;
            if (!waitall || total >= size) {
                result = total;
                goto out;
            }
            continue;                           // MSG_WAITALL: more, right away
        }
        if (ret != -EAGAIN) {                   // EOF (0) or a real error: bytes in hand win
            result = total ? (ssize_t)total : ret;
            goto out;
        }

        // 2. Nothing (more) queued right now
        if (nonblock || (timed && time_after(jiffies, deadline))) {
            result = total ? (ssize_t)total : -EAGAIN;
            goto out;
        }
        if (mattx_req_take_cancelled(sender, orig_pid, req_id)) {
            mattx_dbg("[WORMHOLE] RECV for PID %u req %u cancelled by the Surrogate (%zu bytes in hand)\n", orig_pid, req_id, total);
            result = total ? (ssize_t)total : -EINTR;
            goto out;
        }
        if (READ_ONCE(mattx_cancellable_stopping) || !READ_ONCE(cluster_map[sender])) {
            *abandoned = true;
            result = -EINTR;
            goto out;
        }

        // 3. Sleep until the socket wakes us (or 10 ms, or the SO_RCVTIMEO deadline)
        {
            long to = msecs_to_jiffies(10);

            if (timed && (long)(deadline - jiffies) < to)
                to = (long)(deadline - jiffies) > 0 ? (long)(deadline - jiffies) : 1;
            wait_woken(&wait, TASK_INTERRUPTIBLE, to);
        }
    }

out:
    remove_wait_queue(sk_sleep(sock->sk), &wait);
    return result;
}

static void mattx_recvfrom_kworker(struct work_struct *work) {
    struct mattx_recvfrom_kworker_ctx *ctx = container_of(work, struct mattx_recvfrom_kworker_ctx, work);
    struct mattx_sys_recv_reply fail = { .orig_pid = ctx->orig_pid, .req_id = ctx->req_id };
    struct mattx_sys_recv_reply *reply = NULL;
    struct file *file = mattx_get_remote_file(ctx->orig_pid, ctx->fd);
    struct mattx_link *link;
    // The Surrogate asks for at most 4096 bytes per request as well
    size_t count = min_t(size_t, ctx->size, 4096);
    bool abandoned = false;
    ssize_t ret = -EBADF;

    mattx_dbg("[WORMHOLE] Received RECV request for FD %d (req %u) from Node %d (kworker)...\n", ctx->fd, ctx->req_id, ctx->target_node);

    if (file) {
        struct socket *sock = sock_from_file(file);
        if (sock && sock->sk) {
            reply = kmalloc(sizeof(*reply) + count, GFP_KERNEL);
            if (reply) {
                memset(reply, 0, sizeof(*reply));
                // Receives straight into the reply buffer; may wait, but can be cancelled
                ret = mattx_recv_cancellable(sock, file, reply->data, count, ctx->flags,
                                             ctx->target_node, ctx->orig_pid, ctx->req_id, &abandoned);
            } else {
                ret = -ENOMEM;
            }
        } else {
            ret = -ENOTSOCK;
        }
        fput(file);
    }

    link = READ_ONCE(cluster_map[ctx->target_node]);
    if (abandoned || !link) {
        mattx_dbg("[WORMHOLE] RECV for FD %d (req %u): nobody left to answer, reply dropped\n", ctx->fd, ctx->req_id);
    } else if (reply) {
        // Only what fits travels (bytes_recv may be bigger: MSG_TRUNC)
        size_t n = ret > 0 ? min_t(size_t, ret, count) : 0;

        reply->orig_pid = ctx->orig_pid;
        reply->req_id = ctx->req_id;
        reply->bytes_recv = ret;
        reply->error = (ret < 0) ? ret : 0;
        mattx_comm_send(link, MATTX_MSG_SYS_RECV_REPLY, reply, sizeof(*reply) + n);
    } else {
        // No such socket (-EBADF / -ENOTSOCK) or out of memory: still answer, the Surrogate is waiting!
        fail.bytes_recv = ret;
        fail.error = ret;
        mattx_comm_send(link, MATTX_MSG_SYS_RECV_REPLY, &fail, sizeof(fail));
    }

    kfree(reply);
    kfree(ctx);
    atomic_dec(&mattx_cancellable_inflight);
}

static void handle_sys_recv_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_recv_req *req = payload;
    struct mattx_recvfrom_kworker_ctx *ctx;

    if (!payload || hdr->length < sizeof(*req)) return;
    if (hdr->sender_id >= MAX_NODES || READ_ONCE(mattx_cancellable_stopping)) return;

    ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx) {
        printk(KERN_ERR "MattX:[WORMHOLE] RECV_REQ dropped: out of memory\n");
        return;
    }
    INIT_WORK(&ctx->work, mattx_recvfrom_kworker);
    ctx->orig_pid = req->orig_pid;
    ctx->req_id = req->req_id;
    ctx->fd = req->fd;
    ctx->size = req->size;
    ctx->flags = req->flags;
    ctx->target_node = hdr->sender_id;
    atomic_inc(&mattx_cancellable_inflight);
    schedule_work(&ctx->work); // Dispatch to the Data Plane!
}


static void handle_sys_recv_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // A reply that carries a ticket id belongs to exactly one waiting thread (mattx_ticket_*).
    // req_id 0 is the legacy reply of the SIGSTOP fallback path: it goes through the slot below.
    if (payload && hdr->length >= sizeof(struct mattx_sys_recv_reply) &&
        ((struct mattx_sys_recv_reply *)payload)->req_id) {
        struct mattx_sys_recv_reply *reply = (struct mattx_sys_recv_reply *)payload;
        size_t datalen = hdr->length - sizeof(*reply);
        long retval = reply->bytes_recv;
        // MSG_TRUNC: retval is the real length of a datagram, only what fitted travelled with it
        size_t n = retval > 0 ? min_t(size_t, retval, datalen) : 0;

        mattx_dbg("[RPC] Received RECV_REPLY for Orig PID %u req %u. Bytes recv: %zd\n",
                  reply->orig_pid, reply->req_id, reply->bytes_recv);
        if (!mattx_ticket_complete(hdr->sender_id, reply->orig_pid, reply->req_id, retval, reply->data, n))
            mattx_dbg("[RPC] RECV_REPLY req %u: nobody waits for it any more, dropped\n", reply->req_id);
        return;
    }

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


// --- DATA PLANE: ACCEPT4 (Kworker Native Injector) ---
/*
 * ACCEPT_REQ (accept()/accept4() on a listening socket that lives on the home node)
 *
 * Same contract as the wormhole read and recvfrom: its own kworker, CANCELLABLE, exactly ONE reply
 * (req_id echoed): the real result - an accepted connection is never thrown away - or -EINTR when the
 * Surrogate's CANCEL came first and nothing was accepted.
 *
 * kernel_accept() is only ever called with O_NONBLOCK, the waiting is done here, on the listening
 * socket's own wait queue (a new connection wakes it, like sk_data_ready of any socket), so the
 * blocking semantics are the application's:
 *   - an O_NONBLOCK listening socket returns -EAGAIN at once when no connection is pending,
 *   - SO_RCVTIMEO of the listening socket is honoured (-EAGAIN), like the kernel's own accept().
 * (The SOCK_NONBLOCK / SOCK_CLOEXEC of accept4() belong to the NEW socket, see below.)
 */
struct mattx_accept_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_accept_req req;
    int target_node;
};

// Waits for a pending connection of the listening socket. 0: *newsock is the accepted socket.
// -EAGAIN: non-blocking socket / SO_RCVTIMEO expired. -EINTR: cancelled (nothing was accepted), or
// nobody is left to answer (*abandoned: unload, link gone). Other: a real error of kernel_accept().
static int mattx_accept_cancellable(struct socket *sock, struct file *file, struct socket **newsock,
                                    int sender, u32 orig_pid, u32 req_id, bool *abandoned)
{
    bool nonblock = file->f_flags & O_NONBLOCK;
    long rcvtimeo = nonblock ? 0 : READ_ONCE(sock->sk->sk_rcvtimeo);
    bool timed = !nonblock && rcvtimeo != MAX_SCHEDULE_TIMEOUT;
    unsigned long deadline = jiffies + rcvtimeo;
    DEFINE_WAIT_FUNC(wait, woken_wake_function);
    int err;

    // On the wait queue BEFORE the first look, so that no wake-up can slip between look and sleep
    add_wait_queue(sk_sleep(sock->sk), &wait);

    for (;;) {
        // 1. Look: never blocks
        err = kernel_accept(sock, newsock, O_NONBLOCK);
        if (err != -EAGAIN)
            break;                              // a connection (0) or a real error

        // 2. No connection pending right now
        if (nonblock || (timed && time_after(jiffies, deadline)))
            break;                              // err stays -EAGAIN
        if (mattx_req_take_cancelled(sender, orig_pid, req_id)) {
            mattx_dbg("[WORMHOLE] ACCEPT for PID %u req %u cancelled by the Surrogate, nothing accepted\n", orig_pid, req_id);
            err = -EINTR;
            break;
        }
        if (READ_ONCE(mattx_cancellable_stopping) || !READ_ONCE(cluster_map[sender])) {
            *abandoned = true;
            err = -EINTR;
            break;
        }

        // 3. Sleep until the listening socket wakes us (or 10 ms, or the SO_RCVTIMEO deadline)
        {
            long to = msecs_to_jiffies(10);

            if (timed && (long)(deadline - jiffies) < to)
                to = (long)(deadline - jiffies) > 0 ? (long)(deadline - jiffies) : 1;
            wait_woken(&wait, TASK_INTERRUPTIBLE, to);
        }
    }

    remove_wait_queue(sk_sleep(sock->sk), &wait);
    return err;
}

static void mattx_accept_kworker(struct work_struct *work) {
    struct mattx_accept_kworker_ctx *ctx = container_of(work, struct mattx_accept_kworker_ctx, work);
    struct mattx_sys_accept_reply reply;
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    struct socket *sock = NULL;
    struct socket *newsock = NULL;
    struct file *newfile = NULL;
    struct task_struct *deputy = NULL;
    struct mattx_link *link;
    bool abandoned = false;
    int remote_fd = -1;
    int err = -EBADF;

    memset(&reply, 0, sizeof(reply));
    reply.orig_pid = ctx->req.orig_pid;
    reply.req_id = ctx->req.req_id;

    mattx_dbg("[WORMHOLE] Received ACCEPT request for FD %u (req %u) from Node %d (kworker)...\n", ctx->req.fd, ctx->req.req_id, ctx->target_node);

    if (file) {
        sock = sock_from_file(file);
        if (sock && sock->sk) {
            // 1. Wait for a connection, cancellable. Never blocks inside kernel_accept().
            err = mattx_accept_cancellable(sock, file, &newsock, ctx->target_node,
                                           ctx->req.orig_pid, ctx->req.req_id, &abandoned);

            if (err == 0 && newsock) {
                // 2. Map accept4 flags (SOCK_NONBLOCK, SOCK_CLOEXEC) to file flags
                int file_flags = ctx->req.flags & (O_CLOEXEC | O_NONBLOCK);
                newfile = sock_alloc_file(newsock, file_flags, NULL);

                if (IS_ERR(newfile)) {
                    err = PTR_ERR(newfile);
                    sock_release(newsock);
                } else {
                    // --- THE NATIVE INJECTOR ---
                    rcu_read_lock();
                    deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID);
                    if (deputy) get_task_struct(deputy);
                    rcu_read_unlock();

                    if (deputy && deputy->files) {
                        spin_lock(&deputy->files->file_lock);
                        struct fdtable *fdt = files_fdtable(deputy->files);
                        for (int j = 3; j < fdt->max_fds; j++) {
                            if (!rcu_dereference_raw(fdt->fd[j])) {
                                rcu_assign_pointer(fdt->fd[j], newfile);
                                __set_bit(j, fdt->open_fds);
                                remote_fd = j;
                                break;
                            }
                        }
                        spin_unlock(&deputy->files->file_lock);
                    }

                    if (remote_fd == -1) {
                        fput(newfile);
                        err = -ENFILE;
                    } else {
                        err = 0;
                        mattx_dbg("[KWORKER] Successfully injected accepted socket into Deputy Native FD: %d\n", remote_fd);

                        // 3. Extract the peer name (client IP) natively!
                        if (newsock->ops && newsock->ops->getname) {
                            int len = newsock->ops->getname(newsock, (struct sockaddr *)&reply.addr, 2); // 2 = peer
                            if (len > 0) {
                                reply.addrlen = len;
                            }
                        }
                    }
                    if (deputy) put_task_struct(deputy);
                }
            }
        } else {
            err = -ENOTSOCK;
        }
        fput(file);
    }

    reply.remote_fd = remote_fd;
    reply.error = err;

    link = READ_ONCE(cluster_map[ctx->target_node]);
    if (abandoned || !link) {
        mattx_dbg("[WORMHOLE] ACCEPT for FD %u (req %u): nobody left to answer, reply dropped\n", ctx->req.fd, ctx->req.req_id);
    } else {
        mattx_comm_send(link, MATTX_MSG_SYS_ACCEPT_REPLY, &reply, sizeof(reply));
    }

    kfree(ctx);
    atomic_dec(&mattx_cancellable_inflight);
}

static void handle_sys_accept_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_accept_req *req = payload;
    struct mattx_accept_kworker_ctx *ctx;

    if (!payload || hdr->length < sizeof(*req)) return;
    if (hdr->sender_id >= MAX_NODES || READ_ONCE(mattx_cancellable_stopping)) return;

    ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx) {
        printk(KERN_ERR "MattX:[WORMHOLE] ACCEPT_REQ dropped: out of memory\n");
        return;
    }
    INIT_WORK(&ctx->work, mattx_accept_kworker);
    memcpy(&ctx->req, req, sizeof(*req));
    ctx->target_node = hdr->sender_id;

    mattx_dbg("[RPC] Dispatching ACCEPT_REQ to Kworker...\n");
    atomic_inc(&mattx_cancellable_inflight);
    schedule_work(&ctx->work);
}


static void handle_sys_accept_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // A reply that carries a ticket id belongs to exactly one waiting thread (mattx_ticket_*).
    // req_id 0 is the legacy reply of the SIGSTOP fallback path: it goes through the slot below.
    if (payload && hdr->length >= sizeof(struct mattx_sys_accept_reply) &&
        ((struct mattx_sys_accept_reply *)payload)->req_id) {
        struct mattx_sys_accept_reply *reply = (struct mattx_sys_accept_reply *)payload;
        // The ticket's result is the new remote FD, or -errno; the peer address travels as its data
        long retval = reply->error ? reply->error : reply->remote_fd;
        size_t alen = (reply->error == 0 && reply->addrlen > 0 && reply->addrlen <= sizeof(reply->addr)) ? reply->addrlen : 0;

        mattx_dbg("[RPC] Received ACCEPT_REPLY for Orig PID %u req %u. New Remote FD: %d, error %d\n",
                  reply->orig_pid, reply->req_id, reply->remote_fd, reply->error);
        if (!mattx_ticket_complete(hdr->sender_id, reply->orig_pid, reply->req_id, retval, &reply->addr, alen)) {
            mattx_dbg("[RPC] ACCEPT_REPLY req %u: nobody waits for it any more\n", reply->req_id);
            // The thread gave up for good (dead link, killed) and nobody will ever claim the connection
            // the home node accepted for it: close the orphaned FD there instead of leaking it.
            if (reply->error == 0 && reply->remote_fd >= 0) {
                struct mattx_sys_close_req close_req = { .orig_pid = reply->orig_pid, .remote_fd = reply->remote_fd };

                mattx_comm_send(link, MATTX_MSG_SYS_CLOSE_REQ, &close_req, sizeof(close_req));
            }
        }
        return;
    }

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
/*
 * POLL_REQ (poll(), and select()/pselect6() translated to poll, on files that live on the home node)
 *
 * Same contract as the wormhole read, recvfrom, accept and connect: its own kworker, CANCELLABLE,
 * exactly ONE reply (req_id echoed): the real result, or -EINTR when the Surrogate's CANCEL came first.
 *
 * The wait is event-driven like the kernel's own do_poll(): the files are resolved ONCE and held for the
 * whole wait, the poll table registers this kworker on the wait queue of every file in the first round
 * and the kworker sleeps until one of them wakes it. The sleep is capped at 10 ms so that the cancel, the
 * unload and the lost-link checks still run. The timeout is the application's: 0 is one round, < 0 is
 * infinite, > 0 is a deadline.
 *
 * Requests with a ticket (req_id != 0: poll(), select() and pselect6()) report like native poll(): POLLERR,
 * POLLHUP (and POLLNVAL for an FD that does not exist) are always reported, whether asked for or not; the
 * Surrogate maps them to what its syscall reports. Only the SIGSTOP fallback (req_id 0) keeps the old "only
 * what was asked for" mask, because its events translation expects exactly that.
 */
struct mattx_poll_kworker_ctx {
    struct work_struct work;
    struct mattx_sys_poll_req req;
    int target_node;
};

// The Deputy is alive as long as its entry exists in the export registry
static bool mattx_poll_deputy_alive(u32 orig_pid)
{
    bool alive = false;
    int i;

    spin_lock(&export_lock);
    for (i = 0; i < export_count; i++) {
        if (export_registry[i].orig_pid == orig_pid) {
            alive = true;
            break;
        }
    }
    spin_unlock(&export_lock);
    return alive;
}

/*
 * The state of one poll wait. struct poll_wqueues alone is ~0.8 KB (it carries the inline poll table
 * entries) and the reply is ~0.3 KB: together they must not live on a kworker's stack (frame size).
 */
struct mattx_poll_state {
    struct poll_wqueues table;
    struct file *files[16];
    struct mattx_sys_poll_reply reply;
};

// Always answer: a failed allocation used to leave the Surrogate waiting forever. noinline_for_stack: the
// reply on this function's stack is only paid for in the (rare) case that it is needed.
static noinline_for_stack void mattx_poll_answer_nomem(struct mattx_poll_kworker_ctx *ctx)
{
    struct mattx_sys_poll_reply fail;
    struct mattx_link *link;

    memset(&fail, 0, sizeof(fail));
    fail.orig_pid = ctx->req.orig_pid;
    fail.req_id = ctx->req.req_id;
    fail.error = -ENOMEM;
    link = READ_ONCE(cluster_map[ctx->target_node]);
    if (link)
        mattx_comm_send(link, MATTX_MSG_SYS_POLL_REPLY, &fail, sizeof(fail));
}

// The wait: fills st->reply (revents, retval or error). *abandoned: nobody is left to answer (unload, link gone).
static noinline_for_stack void mattx_poll_wait(struct mattx_poll_kworker_ctx *ctx, struct mattx_poll_state *st,
                                               bool *abandoned)
{
    struct mattx_sys_poll_req *req = &ctx->req;
    struct mattx_sys_poll_reply *reply = &st->reply;
    struct poll_wqueues *table = &st->table;
    bool timed = (req->timeout >= 0);
    unsigned long expire = 0;
    // A ticketed request reports like native poll(): POLLERR/POLLHUP/POLLNVAL are always reported
    __poll_t always = req->req_id ? (EPOLLERR | EPOLLHUP | EPOLLNVAL) : 0;
    int nfds = reply->nfds;
    int j;

    if (timed)
        expire = jiffies + msecs_to_jiffies(req->timeout);

    poll_initwait(table);

    // --- THE WAIT QUEUE LOOP ---
    for (;;) {
        int ready_count = 0;

        if (!mattx_poll_deputy_alive(req->orig_pid)) {
            reply->error = -ESRCH;
            break;
        }

        // 1. Look: never blocks. The first round registers us on every file's wait queue.
        for (j = 0; j < nfds; j++) {
            struct file *f = st->files[j];
            short events = reply->fds[j].events;
            __poll_t mask = 0;

            reply->fds[j].revents = 0;
            if (reply->fds[j].fd < 0)
                continue;                               // a negative FD is ignored, like native poll()

            if (f) {
                // What is reported: what was asked for, plus POLLERR/POLLHUP for a ticketed (native) poll()
                __poll_t keep = (__poll_t)(u16)events | always;

                table->pt._key = (__poll_t)(u16)events | EPOLLERR | EPOLLHUP;
                mask = vfs_poll(f, &table->pt) & keep;
            } else {
                mask = always & EPOLLNVAL;              // no such FD
            }

            reply->fds[j].revents = mask;
            if (mask)
                ready_count++;
        }
        table->pt._qproc = NULL;                        // registered: the next rounds only look

        if (ready_count > 0) {
            reply->retval = ready_count;
            break;
        }
        if (table->error) {
            reply->error = table->error;
            break;
        }

        // 2. Nothing ready
        if (timed && time_after_eq(jiffies, expire)) {
            reply->retval = 0;
            break;
        }
        if (mattx_req_take_cancelled(ctx->target_node, req->orig_pid, req->req_id)) {
            mattx_dbg("[RPC] POLL for PID %u req %u cancelled by the Surrogate\n", req->orig_pid, req->req_id);
            reply->error = -EINTR;
            break;
        }
        if (READ_ONCE(mattx_cancellable_stopping) || !READ_ONCE(cluster_map[ctx->target_node])) {
            *abandoned = true;
            break;
        }

        // 3. Sleep until one of the files wakes us (or 10 ms, or the deadline)
        {
            long to = msecs_to_jiffies(10);

            if (timed && (long)(expire - jiffies) < to)
                to = (long)(expire - jiffies) > 0 ? (long)(expire - jiffies) : 1;

            set_current_state(TASK_INTERRUPTIBLE);
            if (!READ_ONCE(table->triggered))
                schedule_timeout(to);
            __set_current_state(TASK_RUNNING);
            smp_store_mb(table->triggered, 0);
        }
    }

    poll_freewait(table);
}

static void mattx_poll_kworker(struct work_struct *work) {
    struct mattx_poll_kworker_ctx *ctx = container_of(work, struct mattx_poll_kworker_ctx, work);
    struct mattx_sys_poll_req *req = &ctx->req;
    struct mattx_poll_state *st;
    struct mattx_sys_poll_reply *reply;
    struct mattx_link *link;
    bool abandoned = false;
    int nfds = req->nfds;
    int j;

    // --- THE STACK DIET FIX ---
    // Move large structs off the kernel stack and onto the heap!
    st = kzalloc(sizeof(*st), GFP_KERNEL);
    if (!st) {
        mattx_poll_answer_nomem(ctx);
        goto out_ctx;
    }
    reply = &st->reply;

    reply->orig_pid = req->orig_pid;
    reply->req_id = req->req_id;

    if (nfds < 0 || nfds > 16) {
        reply->error = -EINVAL;
        goto answer;
    }
    reply->nfds = nfds;
    memcpy(reply->fds, req->fds, sizeof(struct mattx_pollfd) * nfds);

    mattx_dbg("[RPC] Poll Worker started for Deputy PID %u (req %u, Timeout: %dms)\n", req->orig_pid, req->req_id, req->timeout);

    // The files are resolved once and held for the whole wait, like the fdget() of the native poll()
    for (j = 0; j < nfds; j++) {
        int remote_fd = reply->fds[j].fd;

        reply->fds[j].revents = 0;
        if (remote_fd >= 0)
            st->files[j] = mattx_get_remote_file(req->orig_pid, remote_fd);
    }

    mattx_poll_wait(ctx, st, &abandoned);

    for (j = 0; j < nfds; j++) {
        if (st->files[j])
            fput(st->files[j]);
    }

answer:
    link = READ_ONCE(cluster_map[ctx->target_node]);
    if (abandoned || !link) {
        mattx_dbg("[RPC] POLL for PID %u (req %u): nobody left to answer, reply dropped\n", req->orig_pid, req->req_id);
    } else {
        mattx_dbg("[RPC] Sending POLL_REPLY (Ready: %d, error %d, req %u) back to Node %d...\n", reply->retval, reply->error, req->req_id, ctx->target_node);
        // POLL_REPLY is data, not a droppable control message: mattx_comm_send_ctrl() uses mutex_trylock and
        // silently drops the message when the send mutex is busy. The Surrogate then waits forever (poll
        // with an infinite timeout). A kworker may simply wait for the mutex.
        mattx_comm_send(link, MATTX_MSG_SYS_POLL_REPLY, reply, sizeof(*reply));
    }
    kfree(st);

out_ctx:
    kfree(ctx);
    atomic_dec(&mattx_cancellable_inflight);
}


static void handle_sys_poll_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_poll_req *req = (struct mattx_sys_poll_req *)payload;
    struct mattx_poll_kworker_ctx *ctx;

    if (!payload || hdr->length < sizeof(*req)) return;
    if (hdr->sender_id >= MAX_NODES || READ_ONCE(mattx_cancellable_stopping)) return;

    ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx) {
        printk(KERN_ERR "MattX:[RPC] POLL_REQ dropped: out of memory\n");
        return;
    }
    INIT_WORK(&ctx->work, mattx_poll_kworker);
    memcpy(&ctx->req, req, sizeof(*req));
    ctx->target_node = hdr->sender_id;

    atomic_inc(&mattx_cancellable_inflight);
    schedule_work(&ctx->work);
}

static void handle_sys_poll_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // A reply that carries a ticket id belongs to exactly one waiting thread (mattx_ticket_*).
    // req_id 0 is the legacy reply (the SIGSTOP fallback of poll, select and pselect6): it goes through the slot below.
    if (payload && hdr->length >= sizeof(struct mattx_sys_poll_reply) &&
        ((struct mattx_sys_poll_reply *)payload)->req_id) {
        struct mattx_sys_poll_reply *reply = (struct mattx_sys_poll_reply *)payload;
        // The ticket's result is the number of ready entries, or -errno; the revents travel as its data
        long retval = reply->error ? reply->error : reply->retval;
        size_t dlen = (reply->error == 0 && reply->nfds > 0 && reply->nfds <= 16) ? reply->nfds * sizeof(struct mattx_pollfd) : 0;

        mattx_dbg("[RPC] Received POLL_REPLY for Orig PID %u req %u. Ready FDs: %d, error %d\n",
                  reply->orig_pid, reply->req_id, reply->retval, reply->error);
        if (!mattx_ticket_complete(hdr->sender_id, reply->orig_pid, reply->req_id, retval, reply->fds, dlen))
            mattx_dbg("[RPC] POLL_REPLY req %u: nobody waits for it any more, dropped\n", reply->req_id);
        return;
    }

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
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
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
    struct mattx_sys_bind_reply reply;  // {orig_pid, error} for handle_sys_generic_int_reply (not the connect reply: it carries a req_id now)

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
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
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

// --- Home side: epoll_wait requests the Surrogate gave up on ---
struct mattx_epoll_cancelled {
    struct list_head node;
    int sender; // Surrogate node that gave up (req_id is a per-node counter!)
    u32 orig_pid;
    u32 req_id;
    unsigned long when;
};
static LIST_HEAD(mattx_epoll_cancelled_list);
static DEFINE_SPINLOCK(mattx_epoll_cancelled_lock);
#define MATTX_EPOLL_CANCELLED_MAX 256
static int mattx_epoll_cancelled_count;

#define MATTX_EPOLL_CANCELLED_TTL (60 * HZ)

// Caller holds mattx_epoll_cancelled_lock. The list is in arrival order: oldest entries first.
static void mattx_epoll_cancelled_expire_locked(void)
{
    struct mattx_epoll_cancelled *c;

    while (!list_empty(&mattx_epoll_cancelled_list)) {
        c = list_first_entry(&mattx_epoll_cancelled_list, struct mattx_epoll_cancelled, node);
        if (!time_after(jiffies, c->when + MATTX_EPOLL_CANCELLED_TTL))
            break;
        mattx_dbg("[RPC] Expiring stale epoll cancel (node %d, PID %u, req %u)\n",
                  c->sender, c->orig_pid, c->req_id);
        list_del(&c->node);
        kfree(c);
        mattx_epoll_cancelled_count--;
    }
}

// True (and the entry is consumed) if this request was cancelled.
static bool mattx_epoll_take_cancelled(int sender, u32 orig_pid, u32 req_id)
{
    struct mattx_epoll_cancelled *c, *tmp;
    bool hit = false;

    spin_lock(&mattx_epoll_cancelled_lock);
    mattx_epoll_cancelled_expire_locked();    
    list_for_each_entry_safe(c, tmp, &mattx_epoll_cancelled_list, node) {
        if (c->sender == sender && c->orig_pid == orig_pid && c->req_id == req_id) {
            list_del(&c->node);
            kfree(c);
            mattx_epoll_cancelled_count--;
            hit = true;
            break;
        }
    }
    spin_unlock(&mattx_epoll_cancelled_lock);
    return hit;
}

// Peek without consuming (the kworker polls this every 10 ms).
static bool mattx_epoll_is_cancelled(int sender, u32 orig_pid, u32 req_id)
{
    struct mattx_epoll_cancelled *c;
    bool hit = false;

    spin_lock(&mattx_epoll_cancelled_lock);
    mattx_epoll_cancelled_expire_locked();
    list_for_each_entry(c, &mattx_epoll_cancelled_list, node) {
        if (c->sender == sender && c->orig_pid == orig_pid && c->req_id == req_id) {
            hit = true;
            break;
        }
    }
    spin_unlock(&mattx_epoll_cancelled_lock);
    return hit;
}


// --- Home side: generic CANCEL registry (ticket-based requests, MATTX_MSG_SYS_REQ_CANCEL) ---
// A Surrogate thread that gives up on a proxied request (signal, freeze for a migration or recall)
// names it by (its node, orig_pid, req_id). The kworker serving the request polls this list.
struct mattx_req_cancelled {
    struct list_head node;
    int sender;         // Surrogate node: req_id is a per-node counter
    u32 orig_pid;
    u32 req_id;
    unsigned long when;
};
static LIST_HEAD(mattx_req_cancelled_list);
static DEFINE_SPINLOCK(mattx_req_cancelled_lock);
static int mattx_req_cancelled_count;
#define MATTX_REQ_CANCELLED_MAX 256
#define MATTX_REQ_CANCELLED_TTL (60 * HZ)

// Caller holds mattx_req_cancelled_lock. The list is in arrival order: oldest entries first.
static void mattx_req_cancelled_expire_locked(void)
{
    struct mattx_req_cancelled *c;

    while (!list_empty(&mattx_req_cancelled_list)) {
        c = list_first_entry(&mattx_req_cancelled_list, struct mattx_req_cancelled, node);
        if (mattx_req_cancelled_count <= MATTX_REQ_CANCELLED_MAX &&
            !time_after(jiffies, c->when + MATTX_REQ_CANCELLED_TTL))
            break;
        list_del(&c->node);
        kfree(c);
        mattx_req_cancelled_count--;
    }
}

// True (and the entry is consumed) if the Surrogate cancelled this request
static bool mattx_req_take_cancelled(int sender, u32 orig_pid, u32 req_id)
{
    struct mattx_req_cancelled *c, *tmp;
    bool hit = false;

    spin_lock(&mattx_req_cancelled_lock);
    mattx_req_cancelled_expire_locked();
    list_for_each_entry_safe(c, tmp, &mattx_req_cancelled_list, node) {
        if (c->sender == sender && c->orig_pid == orig_pid && c->req_id == req_id) {
            list_del(&c->node);
            kfree(c);
            mattx_req_cancelled_count--;
            hit = true;
            break;
        }
    }
    spin_unlock(&mattx_req_cancelled_lock);
    return hit;
}

static void handle_sys_req_cancel(struct mattx_link *link, struct mattx_header *hdr, void *payload)
{
    struct mattx_sys_req_cancel *cancel = payload;
    struct mattx_req_cancelled *c;

    if (!payload || hdr->length < sizeof(*cancel)) return;

    c = kmalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        printk(KERN_ERR "MattX:[RPC] CANCEL for PID %u req %u dropped: out of memory\n", cancel->orig_pid, cancel->req_id);
        return;
    }
    c->sender = hdr->sender_id;
    c->orig_pid = cancel->orig_pid;
    c->req_id = cancel->req_id;
    c->when = jiffies;

    spin_lock(&mattx_req_cancelled_lock);
    list_add_tail(&c->node, &mattx_req_cancelled_list);
    mattx_req_cancelled_count++;
    mattx_req_cancelled_expire_locked();
    spin_unlock(&mattx_req_cancelled_lock);

    mattx_dbg("[RPC] CANCEL from Node %d for PID %u req %u\n", hdr->sender_id, cancel->orig_pid, cancel->req_id);
}

static void mattx_req_cancelled_purge(void)
{
    struct mattx_req_cancelled *c, *tmp;

    spin_lock(&mattx_req_cancelled_lock);
    list_for_each_entry_safe(c, tmp, &mattx_req_cancelled_list, node) {
        list_del(&c->node);
        kfree(c);
    }
    mattx_req_cancelled_count = 0;
    spin_unlock(&mattx_req_cancelled_lock);
}


void mattx_epoll_cancelled_purge(void)
{
    struct mattx_epoll_cancelled *c, *tmp;

    mattx_req_cancelled_purge();    // the generic CANCEL registry goes with the epoll one

    spin_lock(&mattx_epoll_cancelled_lock);
    list_for_each_entry_safe(c, tmp, &mattx_epoll_cancelled_list, node) {
        list_del(&c->node);
        kfree(c);
    }
    mattx_epoll_cancelled_count = 0;
    spin_unlock(&mattx_epoll_cancelled_lock);
}


// --- Surrogate side: threads waiting for an epoll_wait reply ---
static LIST_HEAD(mattx_epoll_pending_list);
static DEFINE_SPINLOCK(mattx_epoll_pending_lock);
static atomic_t mattx_epoll_req_seq = ATOMIC_INIT(0);

struct mattx_epoll_pending *mattx_epoll_pending_add(u32 orig_pid, int home_node)
{
    struct mattx_epoll_pending *p = kzalloc(sizeof(*p), GFP_KERNEL);

    if (!p)
        return NULL;
    p->orig_pid = orig_pid;
    p->home_node = home_node;
    p->req_id = (u32)atomic_inc_return(&mattx_epoll_req_seq);
    init_completion(&p->done);
    spin_lock(&mattx_epoll_pending_lock);
    list_add_tail(&p->node, &mattx_epoll_pending_list);
    spin_unlock(&mattx_epoll_pending_lock);
    return p;
}

void mattx_epoll_pending_del(struct mattx_epoll_pending *p)
{
    spin_lock(&mattx_epoll_pending_lock);
    list_del_init(&p->node);
    spin_unlock(&mattx_epoll_pending_lock);
    kfree(p->events);
    kfree(p);
}


/*
 * THE TICKET (Surrogate side): one proxied request that exactly ONE thread waits for.
 *
 * Only the calling thread sleeps (the other threads of the process keep running, which they must:
 * one of them is usually what makes the call return), and the reply finds its waiter by req_id
 * instead of through a slot that is shared by the whole process. The syscall-specific parts stay
 * flat and per syscall (own _req/_reply, task_work function, reply handler and home kworker); only
 * this small skeleton is shared: id, list, completion, result and an optional data buffer.
 */
static LIST_HEAD(mattx_ticket_list);
static DEFINE_SPINLOCK(mattx_ticket_lock);
static atomic_t mattx_ticket_seq = ATOMIC_INIT(0);

// How long an interrupted thread waits for the home node's final answer to its CANCEL. The home
// kworkers look at their cancel flag every few ms, so this only matters if the home node is dead.
#define MATTX_TICKET_CANCEL_WAIT (5 * HZ)

struct mattx_ticket *mattx_ticket_add(u32 orig_pid, int home_node, size_t data_cap)
{
    struct mattx_ticket *t = kzalloc(sizeof(*t), GFP_KERNEL);
    u32 id;

    if (!t)
        return NULL;
    do {
        id = (u32)atomic_inc_return(&mattx_ticket_seq);
    } while (!id);              // 0 is reserved: "no ticket" (legacy replies)
    t->orig_pid = orig_pid;
    t->home_node = home_node;
    t->req_id = id;
    t->data_cap = data_cap;
    init_completion(&t->done);
    spin_lock(&mattx_ticket_lock);
    list_add_tail(&t->node, &mattx_ticket_list);
    spin_unlock(&mattx_ticket_lock);
    return t;
}

void mattx_ticket_del(struct mattx_ticket *t)
{
    if (!t)
        return;
    spin_lock(&mattx_ticket_lock);
    list_del_init(&t->node);
    spin_unlock(&mattx_ticket_lock);
    kfree(t->data);
    kfree(t);
}

// Reply handler side. False if nobody waits for this reply any more (the waiter gave up).
bool mattx_ticket_complete(int home_node, u32 orig_pid, u32 req_id, long retval, const void *data, size_t len)
{
    struct mattx_ticket *t;
    void *copy = NULL;
    bool found = false;

    if (data && len) {
        copy = kmemdup(data, len, GFP_KERNEL);
        if (!copy)
            retval = -ENOMEM;
    }

    spin_lock(&mattx_ticket_lock);
    list_for_each_entry(t, &mattx_ticket_list, node) {
        if (t->req_id == req_id && t->orig_pid == orig_pid && t->home_node == home_node) {
            if (copy && len > t->data_cap) {    // more than the waiter asked for: protocol error
                kfree(copy);
                copy = NULL;
                retval = -EIO;
            }
            kfree(t->data);
            t->data = copy;
            t->data_len = copy ? len : 0;
            copy = NULL;                        // now owned by the ticket
            t->retval = retval;
            complete(&t->done);
            found = true;
            break;
        }
    }
    spin_unlock(&mattx_ticket_lock);

    kfree(copy);                                // nobody waits: drop it
    return found;
}

/*
 * Waits for the reply. 0: the reply is in (t->retval, t->data). The wait is interruptible so that a
 * signal, or the freeze that precedes a migration or recall (a task_work), can always stop the
 * thread. Then the home node is told to finish the request (MATTX_MSG_SYS_REQ_CANCEL) and we wait,
 * bounded and killable, for its FINAL answer: either the real result (the request had already
 * completed, e.g. the bytes a read consumed: they are never thrown away) or -EINTR (cancelled, nothing
 * consumed). -EINTR: no reply could be obtained at all (dying task, lost link, dead home node).
 * A timeout (MAX_SCHEDULE_TIMEOUT = none) that elapses is handled like an interruption.
 */
int mattx_ticket_wait(struct mattx_ticket *t, long timeout)
{
    struct mattx_sys_req_cancel cancel = { .orig_pid = t->orig_pid, .req_id = t->req_id };
    struct mattx_link *link;
    long rc;

    rc = wait_for_completion_interruptible_timeout(&t->done, timeout);
    if (rc > 0)
        return 0;

    if (t->home_node < 0 || t->home_node >= MAX_NODES)
        return -EINTR;
    link = READ_ONCE(cluster_map[t->home_node]);    // we may have slept for a very long time
    if (!link) {
        mattx_dbg("[RPC] PID %u req %u: link to node %d is gone, CANCEL not sent\n", t->orig_pid, t->req_id, t->home_node);
        return -EINTR;
    }
    if (mattx_comm_send(link, MATTX_MSG_SYS_REQ_CANCEL, &cancel, sizeof(cancel)) < 0) {
        mattx_dbg("[RPC] PID %u req %u: CANCEL send failed\n", t->orig_pid, t->req_id);
        return -EINTR;
    }

    rc = wait_for_completion_killable_timeout(&t->done, MATTX_TICKET_CANCEL_WAIT);
    if (rc > 0)
        return 0;
    mattx_dbg("[RPC] PID %u req %u: no answer to the CANCEL, giving up\n", t->orig_pid, t->req_id);
    return -EINTR;
}


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
        reply->req_id = ctx->req.req_id;
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
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
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

            // rmmod mattx is waiting for us (see mattx_wait_wq): leave without replying
            if (READ_ONCE(mattx_unloading)) {
                fput(epoll_file);
                kfree(rpc);
                return;
            }
            
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

            if (mattx_epoll_is_cancelled(rpc->home_node, rpc->orig_pid, rpc->epoll_req_id)) {
                mattx_epoll_take_cancelled(rpc->home_node, rpc->orig_pid, rpc->epoll_req_id);
                mattx_dbg("[RPC] epoll_wait request %u of PID %u cancelled by the Surrogate\n", rpc->epoll_req_id, rpc->orig_pid);
                fput(epoll_file);
                kfree(rpc);
                return;
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

    // The Surrogate may have given up while we were breaking out of the loop.
    if (mattx_epoll_take_cancelled(rpc->home_node, rpc->orig_pid, rpc->epoll_req_id)) {
        kfree(rpc);
        return;
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
            ctx->req.req_id = rpc->epoll_req_id;
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
            reply->req_id = rpc->epoll_req_id;
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
            rpc->epoll_req_id = req->req_id;
            rpc->home_node = hdr->sender_id; // Target node to send reply to
            mattx_queue_wait_work(&rpc->work);
        }
    }
}


static void handle_sys_epoll_wait_cancel(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_wait_cancel *req = payload;
        struct mattx_epoll_cancelled *c = kmalloc(sizeof(*c), GFP_KERNEL);

        if (!c)
            return;
        c->orig_pid = req->orig_pid;
        c->req_id = req->req_id;
        c->when = jiffies;
        c->sender = hdr->sender_id;
        spin_lock(&mattx_epoll_cancelled_lock);
        mattx_epoll_cancelled_expire_locked();
        // A cancel for a request that already finished is never consumed: keep the list bounded.
        if (mattx_epoll_cancelled_count >= MATTX_EPOLL_CANCELLED_MAX) {
            struct mattx_epoll_cancelled *old = list_first_entry(&mattx_epoll_cancelled_list, struct mattx_epoll_cancelled, node);
            list_del(&old->node);
            kfree(old);
            mattx_epoll_cancelled_count--;
        }
        list_add_tail(&c->node, &mattx_epoll_cancelled_list);
        mattx_epoll_cancelled_count++;
        spin_unlock(&mattx_epoll_cancelled_lock);
    }
}

static void handle_sys_epoll_wait_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_sys_epoll_wait_reply *reply = payload;
        struct mattx_epoll_pending *p, *found = NULL;
        struct epoll_event *events = NULL;

        // Copy the events first: kmalloc(GFP_ATOMIC) under the lock below would work too, but
        // this keeps the critical section down to the lookup.
        if (reply->retval > 0) {
            size_t data_size = reply->retval * sizeof(struct epoll_event);
            events = kmalloc(data_size, GFP_KERNEL);
            if (events)
                memcpy(events, reply->events, data_size);
        }

        spin_lock(&mattx_epoll_pending_lock);
        list_for_each_entry(p, &mattx_epoll_pending_list, node) {
            if (p->orig_pid == reply->orig_pid && p->req_id == reply->req_id && p->home_node == hdr->sender_id) {
                found = p;
                break;
            }
        }
        if (found) {
            found->retval = (reply->retval > 0 && !events) ? -ENOMEM : reply->retval;
            found->events = events;
            events = NULL;
            complete(&found->done);
        }
        spin_unlock(&mattx_epoll_pending_lock);

        kfree(events); // nobody is waiting for this reply any more (killed, or an old request)
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
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
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
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
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
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
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



static void handle_sys_uname_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_uname_req *req = payload;
    struct mattx_sys_uname_reply reply;
    reply.orig_pid = req->orig_pid;
    reply.error = 0;
    
    memcpy(&reply.uts, utsname(), sizeof(struct new_utsname));
    
    if (cluster_map[hdr->sender_id]) mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_UNAME_REPLY, &reply, sizeof(reply));
}

static void handle_sys_uname_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_uname_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            if (reply->error == 0) {
                guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct new_utsname), GFP_ATOMIC);
                if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, &reply->uts, sizeof(struct new_utsname));
            }
            guest_registry[i].rpc_done = true;
            if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq);
            break;
        }
    }
    spin_unlock(&guest_lock);
}


// The prlimit64 Deputy Hijack
struct mattx_prlimit_ctx { struct callback_head cb; struct mattx_sys_prlimit64_req req; int target_node; };
static void mattx_prlimit_cb(struct callback_head *cb) {
    struct mattx_prlimit_ctx *ctx = container_of(cb, struct mattx_prlimit_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs; int ret = -EFAULT;
    struct mattx_sys_prlimit64_reply reply;

    unsigned long new_ptr = task_regs->sp - 128 - sizeof(struct rlimit) * 2;
    unsigned long old_ptr = new_ptr + sizeof(struct rlimit);

    if (!ctx->req.has_new || copy_to_user((void __user *)new_ptr, &ctx->req.new_rlim, sizeof(struct rlimit)) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->req.pid; regs.si = ctx->req.resource;
        regs.dx = ctx->req.has_new ? new_ptr : 0; regs.r10 = ctx->req.has_old ? old_ptr : 0;
        if (real_sys_prlimit64) ret = real_sys_prlimit64(&regs);
    }

    reply.orig_pid = ctx->req.orig_pid; reply.error = ret;

    if (ret == 0 && ctx->req.has_old) {
        if (copy_from_user(&reply.old_rlim, (void __user *)old_ptr, sizeof(struct rlimit))) {
            mattx_dbg("[HIJACK] Warning: Failed to read old_rlim from user-space!\n");
        }
    }
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_PRLIMIT64_REPLY, &reply, sizeof(reply));
    kfree(ctx);
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
}
// (Add handle_sys_prlimit64_req to allocate ctx and task_work_add it to the Deputy)


// The prctl Deputy Hijack
struct mattx_prctl_ctx { struct callback_head cb; struct mattx_sys_prctl_req req; int target_node; };
static void mattx_prctl_cb(struct callback_head *cb) {
    struct mattx_prctl_ctx *ctx = container_of(cb, struct mattx_prctl_ctx, cb);
    struct pt_regs regs; int ret = -ENOSYS;
    struct mattx_sys_prctl_reply reply;

    memset(&regs, 0, sizeof(regs));
    regs.di = ctx->req.option; regs.si = ctx->req.arg2; regs.dx = ctx->req.arg3;
    regs.r10 = ctx->req.arg4; regs.r8 = ctx->req.arg5;
    if (real_sys_prctl) ret = real_sys_prctl(&regs);

    reply.orig_pid = ctx->req.orig_pid; reply.error = ret;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_PRCTL_REPLY, &reply, sizeof(reply));
    kfree(ctx);
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
}
// (Add handle_sys_prctl_req to allocate ctx and task_work_add it to the Deputy)


static void handle_sys_prlimit64_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_prlimit64_req *req = payload;
    struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_prlimit_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
            init_task_work(&ctx->cb, mattx_prlimit_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}

static void handle_sys_prctl_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_prctl_req *req = payload;
    struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_prctl_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
            init_task_work(&ctx->cb, mattx_prctl_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}


static void handle_sys_prlimit64_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_prlimit64_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            if (reply->error == 0) {
                guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct rlimit), GFP_ATOMIC);
                if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, &reply->old_rlim, sizeof(struct rlimit));
            }
            guest_registry[i].rpc_done = true;
            if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq);
            break;
        }
    }
    spin_unlock(&guest_lock);
}



// The pread64 kworker
struct mattx_pread64_kworker_ctx { struct work_struct work; struct mattx_sys_pread64_req req; int target_node; };
static void mattx_pread64_kworker(struct work_struct *work) {
    struct mattx_pread64_kworker_ctx *ctx = container_of(work, struct mattx_pread64_kworker_ctx, work);
    struct file *file = mattx_get_remote_file(ctx->req.orig_pid, ctx->req.fd);
    ssize_t ret_bytes = -EBADF; void *read_buf = NULL;

    if (file) {
        read_buf = kmalloc(ctx->req.count, GFP_KERNEL);
        if (read_buf) {
            loff_t pos = ctx->req.pos;
            ret_bytes = kernel_read(file, read_buf, ctx->req.count, &pos);
        } else { ret_bytes = -ENOMEM; }
        fput(file);
    }

    size_t reply_size = sizeof(struct mattx_sys_read_reply) + (ret_bytes > 0 ? ret_bytes : 0);
    struct mattx_sys_read_reply *reply = kmalloc(reply_size, GFP_KERNEL);
    if (reply) {
        reply->orig_pid = ctx->req.orig_pid; reply->req_id = 0; reply->bytes_read = ret_bytes; reply->error = (ret_bytes < 0) ? ret_bytes : 0;
        if (ret_bytes > 0) memcpy(reply->data, read_buf, ret_bytes);
        if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_PREAD64_REPLY, reply, reply_size);
        kfree(reply);
    }
    if (read_buf) kfree(read_buf); 
        kfree(ctx);
}

static void handle_sys_pread64_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_pread64_req *req = payload;
    struct mattx_pread64_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_pread64_kworker);
        memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}


// The fcntl & ioctl Deputy Hijack:
struct mattx_fio_ctx { struct callback_head cb; u32 orig_pid; int fd; int cmd; unsigned long arg; u8 has_ptr; char data[256]; int target_node; bool is_ioctl; };
static void mattx_fio_cb(struct callback_head *cb) {
    struct mattx_fio_ctx *ctx = container_of(cb, struct mattx_fio_ctx, cb);
    struct pt_regs *task_regs = task_pt_regs(current);
    struct pt_regs regs; int ret = -EFAULT;
    struct mattx_sys_fcntl_reply reply; // Reused for ioctl

    unsigned long user_ptr = task_regs->sp - 256 - 128; // 256 byte scratchpad below redzone

    if (!ctx->has_ptr || copy_to_user((void __user *)user_ptr, ctx->data, 256) == 0) {
        memset(&regs, 0, sizeof(regs));
        regs.di = ctx->fd; regs.si = ctx->cmd; regs.dx = ctx->has_ptr ? user_ptr : ctx->arg;
        
        if (ctx->is_ioctl && real_sys_ioctl) ret = real_sys_ioctl(&regs);
        else if (!ctx->is_ioctl && real_sys_fcntl) ret = real_sys_fcntl(&regs);
    }

    reply.orig_pid = ctx->orig_pid; reply.error = ret;
    if (ctx->has_ptr) {
        if (copy_from_user(reply.data, (void __user *)user_ptr, 256)) {
            mattx_dbg("[HIJACK] Warning: Failed to read fcntl/ioctl data from user-space!\n");
        }
    }
    
    u32 msg_type = ctx->is_ioctl ? MATTX_MSG_SYS_IOCTL_REPLY : MATTX_MSG_SYS_FCNTL_REPLY;
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], msg_type, &reply, sizeof(reply));
    
    kfree(ctx);
    send_sig(SIGSTOP, current, 0); // Let the kernel freeze us natively!
}

static void handle_sys_fcntl_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_fcntl_req *req = payload; struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_fio_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            ctx->orig_pid = req->orig_pid; ctx->fd = req->fd; ctx->cmd = req->cmd; ctx->arg = req->arg; ctx->has_ptr = req->has_ptr; memcpy(ctx->data, req->data, 256); ctx->target_node = hdr->sender_id; ctx->is_ioctl = false;
            init_task_work(&ctx->cb, mattx_fio_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}

static void handle_sys_ioctl_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_ioctl_req *req = payload; struct task_struct *deputy = NULL;
    rcu_read_lock(); deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy) {
        struct mattx_fio_ctx *ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
            ctx->orig_pid = req->orig_pid; ctx->fd = req->fd; ctx->cmd = req->cmd; ctx->arg = req->arg; ctx->has_ptr = req->has_ptr; memcpy(ctx->data, req->data, 256); ctx->target_node = hdr->sender_id; ctx->is_ioctl = true;
            init_task_work(&ctx->cb, mattx_fio_cb);
            if (real_task_work_add) { real_task_work_add(deputy, &ctx->cb, TWA_SIGNAL); send_sig(SIGCONT, deputy, 0); } else { kfree(ctx); }
        }
        put_task_struct(deputy);
    }
}


static void handle_sys_fio_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_fcntl_reply *reply = payload; // Reused for ioctl
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            guest_registry[i].rpc_read_buf = kmalloc(256, GFP_ATOMIC);
            if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, reply->data, 256);
            guest_registry[i].rpc_done = true;
            if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq);
            break;
        }
    }
    spin_unlock(&guest_lock);
}


// --- STATFS ---
struct mattx_statfs_kworker_ctx { struct work_struct work; struct mattx_sys_statfs_req req; int target_node; };
static void mattx_statfs_kworker(struct work_struct *work) {
    struct mattx_statfs_kworker_ctx *ctx = container_of(work, struct mattx_statfs_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; void *out_buf = kmalloc(sizeof(struct statfs), GFP_KERNEL);
    struct task_struct *deputy = NULL;

    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_statfs) {
        ret = mattx_execute_as_deputy(deputy, real_sys_statfs, &regs, ctx->req.path, 1, out_buf, sizeof(struct statfs), 2, -1, 0);
    }    
    if (deputy) put_task_struct(deputy);

    struct mattx_sys_statfs_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret };
    if (ret == 0 && out_buf) memcpy(&reply.buf, out_buf, sizeof(struct statfs));
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_STATFS_REPLY, &reply, sizeof(reply));
    if (out_buf) kfree(out_buf); 
        kfree(ctx);
}
static void handle_sys_statfs_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_statfs_req *req = payload; struct mattx_statfs_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_statfs_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_statfs_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_statfs_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error; guest_registry[i].rpc_lseek_res = sizeof(struct statfs);
            if (reply->error == 0) { guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct statfs), GFP_ATOMIC); if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, &reply->buf, sizeof(struct statfs)); }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}

// --- FSTATFS ---
struct mattx_fstatfs_kworker_ctx { struct work_struct work; struct mattx_sys_fstatfs_req req; int target_node; };
static void mattx_fstatfs_kworker(struct work_struct *work) {
    struct mattx_fstatfs_kworker_ctx *ctx = container_of(work, struct mattx_fstatfs_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; void *out_buf = kmalloc(sizeof(struct statfs), GFP_KERNEL);
    struct task_struct *deputy = NULL;

    regs.di = ctx->req.fd;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_fstatfs) {
        ret = mattx_execute_as_deputy(deputy, real_sys_fstatfs, &regs, NULL, 0, out_buf, sizeof(struct statfs), 2, ctx->req.fd, 1);
    }
    if (deputy) put_task_struct(deputy);

    struct mattx_sys_fstatfs_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret };
    if (ret == 0 && out_buf) memcpy(&reply.buf, out_buf, sizeof(struct statfs));
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_FSTATFS_REPLY, &reply, sizeof(reply));
    if (out_buf) kfree(out_buf); 
        kfree(ctx);
}
static void handle_sys_fstatfs_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_fstatfs_req *req = payload; struct mattx_fstatfs_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_fstatfs_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_fstatfs_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_fstatfs_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error; guest_registry[i].rpc_lseek_res = sizeof(struct statfs);
            if (reply->error == 0) { guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct statfs), GFP_ATOMIC); if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, &reply->buf, sizeof(struct statfs)); }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}

// --- NEWFSTATAT ---
struct mattx_newfstatat_kworker_ctx { struct work_struct work; struct mattx_sys_newfstatat_req req; int target_node; };
static void mattx_newfstatat_kworker(struct work_struct *work) {
    struct mattx_newfstatat_kworker_ctx *ctx = container_of(work, struct mattx_newfstatat_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; void *out_buf = kmalloc(sizeof(struct stat), GFP_KERNEL);
    struct task_struct *deputy = NULL;

    regs.di = ctx->req.dfd; regs.r10 = ctx->req.flags;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_newfstatat) {
        ret = mattx_execute_as_deputy(deputy, real_sys_newfstatat, &regs, ctx->req.path, 2, out_buf, sizeof(struct stat), 3, ctx->req.dfd, 1);
    } 
    if (deputy) put_task_struct(deputy);

    struct mattx_sys_newfstatat_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret };
    if (ret == 0 && out_buf) memcpy(&reply.buf, out_buf, sizeof(struct stat));
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_NEWFSTATAT_REPLY, &reply, sizeof(reply));
    if (out_buf) kfree(out_buf); 
        kfree(ctx);
}
static void handle_sys_newfstatat_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_newfstatat_req *req = payload; struct mattx_newfstatat_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_newfstatat_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_newfstatat_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_newfstatat_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error; guest_registry[i].rpc_lseek_res = sizeof(struct stat);
            if (reply->error == 0) { guest_registry[i].rpc_read_buf = kmalloc(sizeof(struct stat), GFP_ATOMIC); if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, &reply->buf, sizeof(struct stat)); }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}

// --- FACCESSAT2 ---
struct mattx_faccessat2_kworker_ctx { struct work_struct work; struct mattx_sys_faccessat2_req req; int target_node; };
static void mattx_faccessat2_kworker(struct work_struct *work) {
    struct mattx_faccessat2_kworker_ctx *ctx = container_of(work, struct mattx_faccessat2_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; struct task_struct *deputy = NULL;

    regs.di = ctx->req.dfd; regs.dx = ctx->req.mode; regs.r10 = ctx->req.flags;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_faccessat2) {
        ret = mattx_execute_as_deputy(deputy, real_sys_faccessat2, &regs, ctx->req.path, 2, NULL, 0, 0, ctx->req.dfd, 1);
    }
    if (deputy) put_task_struct(deputy);

    struct mattx_sys_faccessat2_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret };
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_FACCESSAT2_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}
static void handle_sys_faccessat2_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_faccessat2_req *req = payload; struct mattx_faccessat2_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_faccessat2_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

// --- READLINK ---
struct mattx_readlink_kworker_ctx { struct work_struct work; struct mattx_sys_readlink_req req; int target_node; };
static void mattx_readlink_kworker(struct work_struct *work) {
    struct mattx_readlink_kworker_ctx *ctx = container_of(work, struct mattx_readlink_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; size_t bufsiz = min_t(size_t, ctx->req.bufsiz, 4096);
    void *out_buf = kmalloc(bufsiz, GFP_KERNEL); struct task_struct *deputy = NULL;

    regs.dx = bufsiz;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_readlink) {
        ret = mattx_execute_as_deputy(deputy, real_sys_readlink, &regs, ctx->req.path, 1, out_buf, bufsiz, 2, -1, 0);
    }
    if (deputy) put_task_struct(deputy);

    size_t copy_len = (ret > 0) ? ret : 0;
    size_t rsize = sizeof(struct mattx_sys_readlink_reply) + copy_len;
    struct mattx_sys_readlink_reply *reply = kzalloc(rsize, GFP_KERNEL);
    if (reply) {
        reply->orig_pid = ctx->req.orig_pid; reply->error = ret;
        if (copy_len > 0 && out_buf) memcpy(reply->data, out_buf, copy_len);
        if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_READLINK_REPLY, reply, rsize);
        kfree(reply);
    }
    if (out_buf) kfree(out_buf); 
        kfree(ctx);
}
static void handle_sys_readlink_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_readlink_req *req = payload; struct mattx_readlink_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_readlink_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_readlink_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_readlink_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error; guest_registry[i].rpc_lseek_res = (reply->error > 0) ? reply->error : 0;
            if (reply->error > 0) { guest_registry[i].rpc_read_buf = kmalloc(reply->error, GFP_ATOMIC); if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, reply->data, reply->error); }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}

// --- READLINKAT ---
struct mattx_readlinkat_kworker_ctx { struct work_struct work; struct mattx_sys_readlinkat_req req; int target_node; };
static void mattx_readlinkat_kworker(struct work_struct *work) {
    struct mattx_readlinkat_kworker_ctx *ctx = container_of(work, struct mattx_readlinkat_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; size_t bufsiz = min_t(size_t, ctx->req.bufsiz, 4096);
    void *out_buf = kmalloc(bufsiz, GFP_KERNEL); struct task_struct *deputy = NULL;

    regs.di = ctx->req.dfd; regs.r10 = bufsiz;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_readlinkat) {
        ret = mattx_execute_as_deputy(deputy, real_sys_readlinkat, &regs, ctx->req.path, 2, out_buf, bufsiz, 3, ctx->req.dfd, 1);
    }
    if (deputy) put_task_struct(deputy);

    size_t copy_len = (ret > 0) ? ret : 0;
    size_t rsize = sizeof(struct mattx_sys_readlinkat_reply) + copy_len;
    struct mattx_sys_readlinkat_reply *reply = kzalloc(rsize, GFP_KERNEL);
    if (reply) {
        reply->orig_pid = ctx->req.orig_pid; reply->error = ret;
        if (copy_len > 0 && out_buf) memcpy(reply->data, out_buf, copy_len);
        if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_READLINKAT_REPLY, reply, rsize);
        kfree(reply);
    }
    if (out_buf) kfree(out_buf); 
        kfree(ctx);
}
static void handle_sys_readlinkat_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_readlinkat_req *req = payload; struct mattx_readlinkat_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_readlinkat_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_readlinkat_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_readlinkat_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error; guest_registry[i].rpc_lseek_res = (reply->error > 0) ? reply->error : 0;
            if (reply->error > 0) { guest_registry[i].rpc_read_buf = kmalloc(reply->error, GFP_ATOMIC); if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, reply->data, reply->error); }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}



// --- GETDENTS64 ---
struct mattx_getdents64_kworker_ctx { struct work_struct work; struct mattx_sys_getdents64_req req; int target_node; };
static void mattx_getdents64_kworker(struct work_struct *work) {
    struct mattx_getdents64_kworker_ctx *ctx = container_of(work, struct mattx_getdents64_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; 
    size_t bufsiz = min_t(size_t, ctx->req.count, 32768);
    void *out_buf = kmalloc(bufsiz, GFP_KERNEL);
    struct task_struct *deputy = NULL;

    regs.di = ctx->req.fd; regs.dx = bufsiz;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_getdents64) ret = mattx_execute_as_deputy(deputy, real_sys_getdents64, &regs, NULL, 0, out_buf, bufsiz, 2, ctx->req.fd, 1);
    if (deputy) put_task_struct(deputy);

    size_t copy_len = (ret > 0) ? ret : 0;
    size_t reply_size = sizeof(struct mattx_sys_getdents64_reply) + copy_len;
    struct mattx_sys_getdents64_reply *reply = kzalloc(reply_size, GFP_KERNEL);
    if (reply) {
        reply->orig_pid = ctx->req.orig_pid; reply->error = ret;
        if (copy_len > 0 && out_buf) memcpy(reply->data, out_buf, copy_len);
        if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_GETDENTS64_REPLY, reply, reply_size);
        kfree(reply);
    }
    if (out_buf) kfree(out_buf); 
    kfree(ctx);
}

static void handle_sys_getdents64_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getdents64_req *req = payload; struct mattx_getdents64_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_getdents64_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_getdents64_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_getdents64_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error; guest_registry[i].rpc_lseek_res = (reply->error > 0) ? reply->error : 0;
            if (reply->error > 0) { 
                guest_registry[i].rpc_read_buf = kmalloc(reply->error, GFP_ATOMIC); 
                if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, reply->data, reply->error); 
            }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}


// --- PIPE2 ---
struct mattx_pipe2_kworker_ctx { struct work_struct work; struct mattx_sys_pipe2_req req; int target_node; };
static void mattx_pipe2_kworker(struct work_struct *work) {
    struct mattx_pipe2_kworker_ctx *ctx = container_of(work, struct mattx_pipe2_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; void *out_buf = kmalloc(8, GFP_KERNEL); // 2 integers = 8 bytes
    struct task_struct *deputy = NULL;

    regs.si = ctx->req.flags;
    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (real_sys_pipe2) ret = mattx_execute_as_deputy(deputy, real_sys_pipe2, &regs, NULL, 0, out_buf, 8, 1, -1, 0);
    if (deputy) put_task_struct(deputy);

    struct mattx_sys_pipe2_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret, .fd0 = -1, .fd1 = -1 };
    if (ret == 0 && out_buf) {
        int *fds = (int *)out_buf;
        reply.fd0 = fds[0]; reply.fd1 = fds[1];
    }
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_PIPE2_REPLY, &reply, sizeof(reply));
    if (out_buf) kfree(out_buf); 
    kfree(ctx);
}

static void handle_sys_pipe2_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_pipe2_req *req = payload; struct mattx_pipe2_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_pipe2_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

static void handle_sys_pipe2_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_pipe2_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            if (reply->error == 0) {
                guest_registry[i].rpc_read_buf = kmalloc(8, GFP_ATOMIC); // 2 ints
                if (guest_registry[i].rpc_read_buf) {
                    int *fds = (int *)guest_registry[i].rpc_read_buf;
                    fds[0] = reply->fd0; fds[1] = reply->fd1;
                }
            }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}






// ============================================================================
// THE DSM TRIPWIRE (VM2 PAGE FAULT HANDLER)
// ============================================================================

// VM2: is this offset inside the tables of its mapping (one entry per page of the segment)?
static inline bool mattx_dsm_page_ok(u32 shmid, unsigned long offset, unsigned long npages)
{
    if (likely(offset / PAGE_SIZE < npages))
        return true;
    pr_err_ratelimited("MattX:[DSM] SHMID %u: offset %lu is beyond the end of the mapping (%lu pages): access refused\n",
                       shmid, offset, npages);
    return false;
}

static vm_fault_t mattx_dsm_fault(struct vm_fault *vmf) {
    struct vm_area_struct *vma = vmf->vma;
    unsigned long fault_addr = vmf->address & PAGE_MASK; // Align to 4KB boundary
    int shmid = -1;
    unsigned long offset = 0;
    int home_node = -1;
    u32 orig_pid = 0;
    bool found = false;
    unsigned long npages = 0;

    // 1. TRANSLATION: Find the mapping in the registry
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                unsigned long base = guest_registry[i].dsm_map[d].base_addr;
                unsigned long size = guest_registry[i].dsm_map[d].size;
                
                if (fault_addr >= base && fault_addr < base + size) {
                    shmid = guest_registry[i].dsm_map[d].shmid;
                    offset = fault_addr - base;
                    home_node = guest_registry[i].home_node;
                    orig_pid = guest_registry[i].orig_pid;
                    npages = guest_registry[i].dsm_map[d].npages;
                    found = true;
                    break;
                }
            }
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (!found || home_node == -1 || !cluster_map[home_node]) {
        mattx_dbg("[DSM] SIGSEGV: Unmapped or disconnected DSM access at 0x%lx\n", fault_addr);
        return VM_FAULT_SIGSEGV;
    }


    // 2. CONCURRENCY: Allocate a sterile RPC slot
    // The tables of a mapping hold one entry per page of the segment: never index beyond them
    if (!mattx_dsm_page_ok(shmid, offset, npages))
        return VM_FAULT_SIGBUS;

    unsigned long page_idx = offset / PAGE_SIZE;
    int slot = -1;
    u64 req_id = 0;
    void *page_buf = kmalloc(4096, GFP_KERNEL);
    if (!page_buf) return VM_FAULT_OOM;

    spin_lock(&vfs_rpc_lock);
    for (int i = 0; i < MAX_VFS_RPC; i++) {
        if (!vfs_rpc_registry[i].in_use) {
            slot = i;
            vfs_rpc_registry[i].in_use = true;
            req_id = next_req_id++;
            vfs_rpc_registry[i].req_id = req_id;
            vfs_rpc_registry[i].done = false;
            vfs_rpc_registry[i].data_buf = page_buf;
            init_waitqueue_head(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);

    if (slot == -1) {
        kfree(page_buf);
        return VM_FAULT_SIGSEGV; 
    }

    // 3. THE REQUEST: Ask VM1 for the page
    struct mattx_dsm_page_fault_req req = {
        .req_id = req_id,
        .orig_pid = orig_pid,
        .shmid = shmid,
        .offset = offset
    };
    
    mattx_dbg("[DSM] Page Fault at 0x%lx (SHMID: %d, Offset: %lu). Fetching from Node %d...\n", fault_addr, shmid, offset, home_node);
    mattx_comm_send(cluster_map[home_node], MATTX_MSG_DSM_PAGE_FAULT_REQ, &req, sizeof(req));

    // 4. THE FREEZE: Sleep until the page arrives!
    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);

    int err = vfs_rpc_registry[slot].error;

    spin_lock(&vfs_rpc_lock);
    vfs_rpc_registry[slot].in_use = false;
    spin_unlock(&vfs_rpc_lock);

    if (err != 0) {
        kfree(page_buf);
        return VM_FAULT_SIGSEGV;
    }


    // 5. THE INJECTION: Wire the PFN into the Surrogate's brain!
    // disabled since defined before: unsigned long page_idx = offset / PAGE_SIZE;
    void *kaddr = NULL;
    void *newpage = NULL;   // allocated outside the spinlock: __get_free_page(GFP_KERNEL) may sleep
    struct mattx_dsm_mapping held;  // a reference on the tables: a concurrent shmdt must not free the page under us

    memset(&held, 0, sizeof(held));

    for (;;) {
        bool need_page = false;

        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                    if (guest_registry[i].dsm_map[d].base_addr == (fault_addr - offset) && page_idx < guest_registry[i].dsm_map[d].npages) {
                        // Allocate the physical page if we haven't yet!
                        if (!guest_registry[i].dsm_map[d].pages[page_idx] && newpage) {
                            guest_registry[i].dsm_map[d].pages[page_idx] = newpage;
                            newpage = NULL;
                        }
                        kaddr = guest_registry[i].dsm_map[d].pages[page_idx];
                        need_page = !kaddr;
                        if (kaddr) {
                            // Held until the page is in the page tables: the pages are only freed with the last reference
                            held = guest_registry[i].dsm_map[d];
                            mattx_dsm_map_get(&held);
                        }
                        break;
                    }
                }
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (!need_page)
            break;
        newpage = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
        if (!newpage)
            break;          // kaddr stays NULL: VM_FAULT_OOM below
    }
    if (newpage)
        free_page((unsigned long)newpage);  // somebody else filled the slot meanwhile

    if (!kaddr) {
        kfree(page_buf);
        return VM_FAULT_OOM;
    }

    // Copy the payload into our persistent physical page
    memcpy(kaddr, page_buf, 4096);


    // Surgically insert the Page Frame Number (PFN) into the application's page tables!
    unsigned long pfn = __pa(kaddr) >> PAGE_SHIFT;
    vm_fault_t ret;

    if (config_dsm_mode == 2 && real_vmf_insert_pfn_prot) {
        // --- MODE 2: THE READ-ONLY TRAPDOOR ---
        // We use vm_get_page_prot(VM_READ) to generate a strict Read-Only protection mask.
        // Even if the VMA is RW, the CPU will throw a Write-Protect fault if the app tries to write!
        pgprot_t ro_prot = vm_get_page_prot(VM_READ);
        ret = real_vmf_insert_pfn_prot(vma, fault_addr, pfn, ro_prot);
        
        if (ret == VM_FAULT_NOPAGE) {
            spin_lock(&guest_lock);
            for (int i = 0; i < guest_count; i++) {
                if (guest_registry[i].local_pid == current->tgid) {
                    for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                        if (guest_registry[i].dsm_map[d].base_addr == (fault_addr - offset) && page_idx < guest_registry[i].dsm_map[d].npages) {
                            guest_registry[i].dsm_map[d].page_states[page_idx] = MATTX_PAGE_SHARED;
                            mattx_dbg("[MESI_VM2] Injected Read-Only Trapdoor at 0x%lx. Local State: SHARED\n", fault_addr);
                            break;
                        }
                    }
                    break;
                }
            }
            spin_unlock(&guest_lock);
        }
    } else {
        // Mode 1 or Mode 3: Standard RW injection
        ret = vmf_insert_pfn(vma, fault_addr, pfn);
    }



    // 6. THE LEASH: If we're in DSM_MODE3, set the Trap Flag so we can catch the next instruction!
    // ###################################################################################
    if (config_dsm_mode == 3) {
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                guest_registry[i].dsm_step_pending = true;
                // Check the kernel's fault flags to see if it was a WRITE!
                guest_registry[i].dsm_step_is_write = (vmf->flags & FAULT_FLAG_WRITE) ? true : false;
                guest_registry[i].dsm_step_addr = fault_addr;
                guest_registry[i].dsm_step_shmid = shmid;
                break;
            }
        }
        spin_unlock(&guest_lock);

        // THE LEASH: Set the Trap Flag!
        struct pt_regs *regs = task_pt_regs(current);
        if (regs) regs->flags |= X86_EFLAGS_TF;
        
        mattx_dbg("[DSM_MODE3] Page injected at 0x%lx. Trap Flag SET. Awaiting #DB.\n", fault_addr);
    }

    // ###################################################################################


    kfree(page_buf);

    if (ret != VM_FAULT_NOPAGE) {
        mattx_dsm_map_put(&held);
        return ret;
    }


    // Mark the page as present so the Sweeper knows to clean it!
    if (page_idx < npages) {
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                    if (guest_registry[i].dsm_map[d].base_addr == (fault_addr - offset) && page_idx < guest_registry[i].dsm_map[d].npages) {
                        set_bit(page_idx, guest_registry[i].dsm_map[d].present_pages);
                        break;
                    }
                }
                break;
            }
        }
        spin_unlock(&guest_lock);
    }

    mattx_dsm_map_put(&held);   // the page is wired in: a shmdt may free the tables now (it unmaps the VMA first)
    mattx_dbg("[DSM] Illusion Complete! Page successfully injected at 0x%lx!\n", fault_addr);
    return VM_FAULT_NOPAGE;
}



// ============================================================================
// THE MESI WRITE-PROTECT TRAPDOOR (VM2)
// ============================================================================
static vm_fault_t mattx_dsm_pfn_mkwrite(struct vm_fault *vmf) {
    unsigned long fault_addr = vmf->address & PAGE_MASK;
    int shmid = -1;
    unsigned long offset = 0;
    int home_node = -1;
    u32 orig_pid = 0;
    bool found = false;
    unsigned long npages = 0;

    // 1. TRANSLATION: Find the mapping in the registry
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                unsigned long base = guest_registry[i].dsm_map[d].base_addr;
                unsigned long size = guest_registry[i].dsm_map[d].size;
                
                if (fault_addr >= base && fault_addr < base + size) {
                    shmid = guest_registry[i].dsm_map[d].shmid;
                    offset = fault_addr - base;
                    home_node = guest_registry[i].home_node;
                    orig_pid = guest_registry[i].orig_pid;
                    npages = guest_registry[i].dsm_map[d].npages;
                    found = true;
                    break;
                }
            }
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (!found || home_node == -1 || !cluster_map[home_node]) {
        return VM_FAULT_SIGBUS;
    }

    if (!mattx_dsm_page_ok(shmid, offset, npages))
        return VM_FAULT_SIGBUS;

    if (config_dsm_mode == 2) {
        mattx_dbg("[MESI_VM2] Write-Protect Fault at 0x%lx. Requesting EXCLUSIVE lock...\n", fault_addr);
        
        // 2. CONCURRENCY: Allocate a sterile RPC slot
        int slot = -1; u64 req_id = 0;
        spin_lock(&vfs_rpc_lock);
        for (int j = 0; j < MAX_VFS_RPC; j++) {
            if (!vfs_rpc_registry[j].in_use) {
                slot = j; vfs_rpc_registry[j].in_use = true;
                req_id = next_req_id++; vfs_rpc_registry[j].req_id = req_id;
                vfs_rpc_registry[j].done = false; init_waitqueue_head(&vfs_rpc_registry[j].wq);
                break;
            }
        }
        spin_unlock(&vfs_rpc_lock);
        
        if (slot == -1) return VM_FAULT_SIGBUS;

        // 3. THE REQUEST: Ask VM1 for the EXCLUSIVE lock!
        struct mattx_dsm_write_acquire_req acq_req = { 
            .req_id = req_id, .orig_pid = orig_pid, .shmid = shmid, .offset = offset 
        };
        mattx_comm_send(cluster_map[home_node], MATTX_MSG_DSM_WRITE_ACQUIRE_REQ, &acq_req, sizeof(acq_req));
        
        // 4. THE FREEZE: Sleep until VM1 invalidates the other nodes!
        int wait_ret = wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);
        
        int err = vfs_rpc_registry[slot].error;
        spin_lock(&vfs_rpc_lock); vfs_rpc_registry[slot].in_use = false; spin_unlock(&vfs_rpc_lock);

        // If we were interrupted by a signal (like migration SIGSTOP/SIGKILL), abort!
        if (wait_ret == -ERESTARTSYS) {
            mattx_dbg("[MESI_VM2] Write-Acquire interrupted by signal! Aborting upgrade.\n");
            return VM_FAULT_RETRY; // Tell the kernel to retry the fault later!
        }

        if (err != 0) return VM_FAULT_SIGBUS;

        // 5. THE UPGRADE: Mark local state as EXCLUSIVE
        unsigned long page_idx = offset / PAGE_SIZE;
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                    if (guest_registry[i].dsm_map[d].base_addr == (fault_addr - offset) && page_idx < guest_registry[i].dsm_map[d].npages) {
                        guest_registry[i].dsm_map[d].page_states[page_idx] = MATTX_PAGE_EXCLUSIVE;
                        break;
                    }
                }
                break;
            }
        }
        spin_unlock(&guest_lock);

        mattx_dbg("[MESI_VM2] Lock acquired! Kernel will now upgrade page at 0x%lx to EXCLUSIVE (RW).\n", fault_addr);
    }

    // Return 0 (Success) to tell the Linux kernel: "I handled the lock, you may now make the PTE writable!"
    return 0; 
}



const struct vm_operations_struct mattx_dsm_vm_ops = {
    .fault = mattx_dsm_fault,
    .pfn_mkwrite = mattx_dsm_pfn_mkwrite, // <-- THE WRITE TRAPDOOR!
};
EXPORT_SYMBOL(mattx_dsm_vm_ops);





// ============================================================================
// DSM CONTROL PLANE (VM1 NATIVE EXECUTION)
// ============================================================================

// --- SHMGET KWORKER ---
struct mattx_shmget_kworker_ctx { struct work_struct work; struct mattx_sys_shmget_req req; int target_node; };
static void mattx_shmget_kworker(struct work_struct *work) {
    struct mattx_shmget_kworker_ctx *ctx = container_of(work, struct mattx_shmget_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; struct task_struct *deputy = NULL;

    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy && real_sys_shmget) {
        // THE SILENT NINJA: Steal the IPC Namespace!
        struct nsproxy *old_ns = current->nsproxy;
        current->nsproxy = deputy->nsproxy;
        const struct cred *old_cred = override_creds(deputy->cred);

        regs.di = ctx->req.key; regs.si = ctx->req.size; regs.dx = ctx->req.shmflg;
        ret = real_sys_shmget(&regs);

        revert_creds(old_cred);
        current->nsproxy = old_ns;
        put_task_struct(deputy);
    }

    struct mattx_sys_shmget_reply reply = { .orig_pid = ctx->req.orig_pid, .shmid = ret, .error = (ret < 0) ? ret : 0 };
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SHMGET_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}
static void handle_sys_shmget_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmget_req *req = payload; struct mattx_shmget_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_shmget_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

// --- SHMCTL KWORKER ---
struct mattx_shmctl_kworker_ctx { struct work_struct work; struct mattx_sys_shmctl_req req; int target_node; };
static void mattx_shmctl_kworker(struct work_struct *work) {
    struct mattx_shmctl_kworker_ctx *ctx = container_of(work, struct mattx_shmctl_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; struct task_struct *deputy = NULL;
    void *out_buf = kmalloc(128, GFP_KERNEL);

    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy && real_sys_shmctl) {
        struct nsproxy *old_ns = current->nsproxy;
        current->nsproxy = deputy->nsproxy;
        const struct cred *old_cred = override_creds(deputy->cred);
        kthread_use_mm(deputy->mm);

        unsigned long scratchpad = task_pt_regs(deputy)->sp - 8192;
        regs.di = ctx->req.shmid; regs.si = ctx->req.cmd; regs.dx = scratchpad;
        
        ret = real_sys_shmctl(&regs);

        if (ret == 0 && out_buf) {
            if (copy_from_user(out_buf, (void __user *)scratchpad, 128)) {
                mattx_dbg("[KWORKER] Warning: Failed to read shmid_ds from scratchpad!\n");
                ret = -EFAULT; // Sabotage the return code so VM2 knows it failed!
            }
        }

        kthread_unuse_mm(deputy->mm);
        revert_creds(old_cred);
        current->nsproxy = old_ns;
        put_task_struct(deputy);
    }

    struct mattx_sys_shmctl_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret };
    if (ret == 0 && out_buf) memcpy(reply.data, out_buf, 128);
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SHMCTL_REPLY, &reply, sizeof(reply));
    if (out_buf) kfree(out_buf);
    kfree(ctx);
}
static void handle_sys_shmctl_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmctl_req *req = payload; struct mattx_shmctl_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_shmctl_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

// --- SHMDT KWORKER ---
struct mattx_shmdt_kworker_ctx { struct work_struct work; struct mattx_sys_shmdt_req req; int target_node; };
static void mattx_shmdt_kworker(struct work_struct *work) {
    struct mattx_shmdt_kworker_ctx *ctx = container_of(work, struct mattx_shmdt_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    int ret = -ENOSYS; struct task_struct *deputy = NULL;

    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy && real_sys_shmdt) {
        struct nsproxy *old_ns = current->nsproxy;
        current->nsproxy = deputy->nsproxy;
        const struct cred *old_cred = override_creds(deputy->cred);
        kthread_use_mm(deputy->mm);

        regs.di = ctx->req.shmaddr;
        ret = real_sys_shmdt(&regs);

        kthread_unuse_mm(deputy->mm);
        revert_creds(old_cred);
        current->nsproxy = old_ns;
        put_task_struct(deputy);
    }

    struct mattx_sys_shmdt_reply reply = { .orig_pid = ctx->req.orig_pid, .error = ret };
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SHMDT_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}
static void handle_sys_shmdt_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmdt_req *req = payload; struct mattx_shmdt_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_shmdt_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

// --- VM2 REPLY HANDLERS ---
static void handle_sys_shmget_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmget_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = (reply->error < 0) ? reply->error : reply->shmid;
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}

static void handle_sys_shmctl_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmctl_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            if (reply->error == 0) {
                guest_registry[i].rpc_read_buf = kmalloc(128, GFP_ATOMIC);
                if (guest_registry[i].rpc_read_buf) memcpy(guest_registry[i].rpc_read_buf, reply->data, 128);
            }
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}

static void handle_sys_shmdt_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmdt_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}


// --- SHMAT KWORKER (VM1) ---
struct mattx_shmat_kworker_ctx { struct work_struct work; struct mattx_sys_shmat_req req; int target_node; };
static void mattx_shmat_kworker(struct work_struct *work) {
    struct mattx_shmat_kworker_ctx *ctx = container_of(work, struct mattx_shmat_kworker_ctx, work);
    struct pt_regs regs; memset(&regs, 0, sizeof(regs));
    long ret = -ENOSYS; size_t shm_size = 0; struct task_struct *deputy = NULL;

    rcu_read_lock(); deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
    if (deputy && real_sys_shmat) {
        struct nsproxy *old_ns = current->nsproxy;
        current->nsproxy = deputy->nsproxy;
        const struct cred *old_cred = override_creds(deputy->cred);
        kthread_use_mm(deputy->mm);

        regs.di = ctx->req.shmid; regs.si = ctx->req.shmaddr; regs.dx = ctx->req.shmflg;
        ret = real_sys_shmat(&regs);

        // Intelligence Gathering: Find the size of the newly attached segment!
        if (ret >= 0 && deputy->mm) {
            mmap_read_lock(deputy->mm);
            struct vm_area_struct *vma = find_vma(deputy->mm, ret);
            if (vma && vma->vm_start == ret) {
                shm_size = vma->vm_end - vma->vm_start;
                mattx_dbg("[KWORKER] Native shmat successful on VM1. Addr: 0x%lx, Size: %zu\n", ret, shm_size);
            }
            mmap_read_unlock(deputy->mm);
        }

        kthread_unuse_mm(deputy->mm);
        revert_creds(old_cred);
        current->nsproxy = old_ns;
        put_task_struct(deputy);
    }

    struct mattx_sys_shmat_reply reply = { .orig_pid = ctx->req.orig_pid, .ret_addr = (ret >= 0) ? ret : 0, .size = shm_size, .error = (ret < 0) ? ret : 0 };
    if (cluster_map[ctx->target_node]) mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_SYS_SHMAT_REPLY, &reply, sizeof(reply));
    kfree(ctx);
}
static void handle_sys_shmat_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmat_req *req = payload; struct mattx_shmat_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) { INIT_WORK(&ctx->work, mattx_shmat_kworker); memcpy(&ctx->req, req, sizeof(*req)); ctx->target_node = hdr->sender_id; schedule_work(&ctx->work); }
}

// --- VM2 REPLY HANDLER ---
static void handle_sys_shmat_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_sys_shmat_reply *reply = payload;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
            guest_registry[i].rpc_fsync_res = reply->error;
            guest_registry[i].rpc_lseek_res = reply->size; // Store the size here!
            
            // We need to pass the ret_addr back. Let's allocate a small buffer.
            guest_registry[i].rpc_read_buf = kmalloc(sizeof(unsigned long), GFP_ATOMIC);
            if (guest_registry[i].rpc_read_buf) {
                memcpy(guest_registry[i].rpc_read_buf, &reply->ret_addr, sizeof(unsigned long));
            }
            
            guest_registry[i].rpc_done = true; if (guest_registry[i].rpc_wq) wake_up_interruptible(guest_registry[i].rpc_wq); break;
        }
    }
    spin_unlock(&guest_lock);
}



// --- DSM PAGE FAULT KWORKER (VM1) ---
struct mattx_dsm_fault_kworker_ctx { struct work_struct work; struct mattx_dsm_page_fault_req req; int target_node; };

static void mattx_dsm_fault_kworker(struct work_struct *work) {
    struct mattx_dsm_fault_kworker_ctx *ctx = container_of(work, struct mattx_dsm_fault_kworker_ctx, work);
    struct mattx_dsm_page_fault_reply *reply;
    struct task_struct *deputy = NULL;
    int ret = -EFAULT;
    unsigned long target_addr = 0;
    unsigned long seg_npages = 0;       // size of the segment in pages: the directory of the segment is sized by it

    reply = kzalloc(sizeof(*reply), GFP_KERNEL);
    if (!reply) { kfree(ctx); return; }

    reply->req_id = ctx->req.req_id;

    rcu_read_lock();
    deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID);
    if (deputy) get_task_struct(deputy);
    rcu_read_unlock();

    if (deputy && deputy->mm) {
        // 1. THE HUNT: Grab the lock just long enough to find the address!
        mmap_read_lock(deputy->mm);
        struct vm_area_struct *vma;
        VMA_ITERATOR(vmi, deputy->mm, 0);
        for_each_vma(vmi, vma) {
            if (vma->vm_file && vma->vm_file->f_path.dentry->d_name.name) {
                if (strncmp(vma->vm_file->f_path.dentry->d_name.name, "SYSV", 4) == 0) {
                    if (vma->vm_file->f_inode->i_ino == ctx->req.shmid) {
                        seg_npages = DIV_ROUND_UP(i_size_read(vma->vm_file->f_inode), PAGE_SIZE);
                        if (vma->vm_start + ctx->req.offset < vma->vm_end) {
                            target_addr = vma->vm_start + ctx->req.offset;
                        }
                        break;
                    }
                }
            }
        }
        mmap_read_unlock(deputy->mm); // DROP THE LOCK IMMEDIATELY!

        // 2. THE MESI SYNC & PUMP (Completely Lock-Free!)
        if (target_addr != 0) {
            unsigned long page_idx = ctx->req.offset / PAGE_SIZE;

            // --- MESI FLUSH PIPELINE (MODE 2) ---
            if (config_dsm_mode == 2) {
                u32 owner_pid = 0;
                int owner_node = -1;
                struct mattx_dsm_master_dir *dir;

                spin_lock(&mattx_dsm_lock);
                dir = mattx_dsm_dir_lookup(ctx->req.shmid, page_idx);
                if (dir && dir->page_state[page_idx] == MATTX_PAGE_EXCLUSIVE)
                    owner_pid = dir->page_owner_pid[page_idx];
                spin_unlock(&mattx_dsm_lock);
                
                if (owner_pid != 0 && owner_pid != ctx->req.orig_pid) {
                    spin_lock(&export_lock);
                    for (int e = 0; e < export_count; e++) {
                        if (export_registry[e].orig_pid == owner_pid) {
                            owner_node = export_registry[e].target_node;
                            break;
                        }
                    }
                    spin_unlock(&export_lock);
                }

                // If someone else owns it exclusively, we must FLUSH them first!
                if (owner_pid != 0 && owner_pid != ctx->req.orig_pid && owner_node != -1 && cluster_map[owner_node]) {
                    mattx_dbg("[MESI_VM1] Page %lu is EXCLUSIVE to PID %u (Node %d). Pausing fault to request FLUSH...\n", page_idx, owner_pid, owner_node);
                    
                    int slot = -1; u64 req_id = 0;
                    void *flush_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
                    
                    if (flush_buf) {
                        spin_lock(&vfs_rpc_lock);
                        for (int j = 0; j < MAX_VFS_RPC; j++) {
                            if (!vfs_rpc_registry[j].in_use) {
                                slot = j; vfs_rpc_registry[j].in_use = true;
                                req_id = next_req_id++; vfs_rpc_registry[j].req_id = req_id;
                                vfs_rpc_registry[j].done = false; vfs_rpc_registry[j].data_buf = flush_buf;
                                init_waitqueue_head(&vfs_rpc_registry[j].wq);
                                break;
                            }
                        }
                        spin_unlock(&vfs_rpc_lock);

                        if (slot != -1) {
                            struct mattx_dsm_flush_req flush_req = {
                                .req_id = req_id, .orig_pid = owner_pid, .shmid = ctx->req.shmid, .offset = ctx->req.offset
                            };
                            
                            mattx_comm_send(cluster_map[owner_node], MATTX_MSG_DSM_FLUSH_REQ, &flush_req, sizeof(flush_req));
                            wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);
                            
                            int flush_err = vfs_rpc_registry[slot].error;
                            spin_lock(&vfs_rpc_lock); vfs_rpc_registry[slot].in_use = false; spin_unlock(&vfs_rpc_lock);

                            if (flush_err == 0) {
                                // Write directly to physical RAM bypassing the frozen process's page tables!
                                mmap_read_lock(deputy->mm);
                                VMA_ITERATOR(vmi2, deputy->mm, 0);
                                struct vm_area_struct *vma2;
                                for_each_vma(vmi2, vma2) {
                                    if (vma2->vm_file && vma2->vm_file->f_path.dentry->d_name.name && strncmp(vma2->vm_file->f_path.dentry->d_name.name, "SYSV", 4) == 0) {
                                        if (vma2->vm_file->f_inode->i_ino == ctx->req.shmid) {
                                            struct page *page = read_mapping_page(vma2->vm_file->f_mapping, ctx->req.offset / PAGE_SIZE, NULL);
                                            if (!IS_ERR(page)) {
                                                void *kaddr = kmap_local_page(page);
                                                memcpy(kaddr, flush_buf, PAGE_SIZE);
                                                kunmap_local(kaddr);
                                                set_page_dirty(page); 
                                                put_page(page);
                                            }
                                            break;
                                        }
                                    }
                                }
                                mmap_read_unlock(deputy->mm);
                                mattx_dbg("[MESI_VM1] Successfully flushed dirty data from PID %u to physical RAM.\n", owner_pid);
                            } else {
                                mattx_dbg("[MESI_VM1] Flush failed (PID %u detached). Assuming RAM was updated via Funeral Flush.\n", owner_pid);
                            }

                            // Update Directory: Downgrade to SHARED
                            spin_lock(&mattx_dsm_lock);
                            dir = mattx_dsm_dir_lookup(ctx->req.shmid, page_idx);
                            if (dir) {
                                dir->page_state[page_idx] = MATTX_PAGE_SHARED;
                                dir->page_owner_pid[page_idx] = 0;
                                mattx_dsm_mask_clear(dir, page_idx);
                            }
                            spin_unlock(&mattx_dsm_lock);
                        }
                        kfree(flush_buf);
                    }
                }
            }
            // ---------------------------------------

            mattx_dbg("[DSM_PUMP] Found SYSV segment! Sucking 4096 bytes from physical addr 0x%lx...\n", target_addr); 
            
            // THE PUMP: Safely extract the data because we dropped the mmap_read_lock!
            int bytes = access_process_vm(deputy, target_addr, reply->data, 4096, FOLL_FORCE);
            if (bytes == 4096) {
                ret = 0;
                mattx_dbg("[DSM_PUMP] Successfully extracted 4KB page. Sending to Node %d...\n", ctx->target_node); 

                // --- MODE 2 MASTER DIRECTORY UPDATE ---
                if (config_dsm_mode == 2) {
                    struct mattx_dsm_master_dir *dir;
                    u64 *mask = NULL;

                    // The directory of a segment is sized by the segment: registered once, outside the spinlock
                    if (mattx_dsm_dir_register(ctx->req.shmid, ctx->req.orig_pid, seg_npages) != 0)
                        mattx_dbg("[MESI_VM1] Could not register SHMID %u in the Global Directory\n", ctx->req.shmid);

                    spin_lock(&mattx_dsm_lock);
                    dir = mattx_dsm_dir_lookup(ctx->req.shmid, page_idx);
                    if (dir) {
                        mask = mattx_dsm_mask_get(dir, page_idx);
                        if (mask) {
                            if (dir->page_state[page_idx] == MATTX_PAGE_INVALID)
                                dir->page_state[page_idx] = MATTX_PAGE_SHARED;
                            mask[ctx->target_node / 64] |= (1ULL << (ctx->target_node % 64));

                            mattx_dbg("[MESI_VM1] Page %lu of SHMID %u is now SHARED with Node %d\n", page_idx, ctx->req.shmid, ctx->target_node);
                        }
                    }
                    spin_unlock(&mattx_dsm_lock);

                    // A sharer that cannot be recorded would never be invalidated: fail the fault instead of
                    // handing out a page the directory does not know about
                    if (dir && !mask)
                        ret = -ENOMEM;
                }
            } else {
                mattx_dbg("[DSM_PUMP] ERROR: Failed to extract page! (Read %d bytes)\n", bytes); 
            }
        }
        put_task_struct(deputy);
    }

    reply->error = ret;
    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_DSM_PAGE_FAULT_REPLY, reply, sizeof(*reply));
    }
    kfree(reply);
    kfree(ctx);
}


static void handle_dsm_page_fault_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_page_fault_req *req = payload;

    mattx_dbg("[DSM_PUMP] Received PAGE_FAULT_REQ from Node %u for SHMID %u at Offset %lu\n", 
              hdr->sender_id, req->shmid, req->offset);

    struct mattx_dsm_fault_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_dsm_fault_kworker);
        memcpy(&ctx->req, req, sizeof(*req));
        ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}


// --- DSM PAGE FAULT REPLY HANDLER (VM2) ---
static void handle_dsm_page_fault_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_page_fault_reply *reply = payload;
    
    spin_lock(&vfs_rpc_lock);
    for (int i = 0; i < MAX_VFS_RPC; i++) {
        if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
            vfs_rpc_registry[i].error = reply->error;
            
            // Copy the 4KB payload into the waiting buffer!
            if (reply->error == 0 && vfs_rpc_registry[i].data_buf) {
                memcpy(vfs_rpc_registry[i].data_buf, reply->data, 4096);
            }
            
            vfs_rpc_registry[i].done = true;
            wake_up_interruptible(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);
}


// ============================================================================
// THE 1-WAY DATA BOMB (VM1 RECEIVER)
// ============================================================================
struct mattx_dsm_update_kworker_ctx { struct work_struct work; struct mattx_dsm_page_update_req req; };

static void mattx_dsm_update_kworker(struct work_struct *work) {
    struct mattx_dsm_update_kworker_ctx *ctx = container_of(work, struct mattx_dsm_update_kworker_ctx, work);
    struct task_struct *deputy = NULL;
    struct file *shm_file = NULL;

    rcu_read_lock();
    deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID);
    if (deputy) get_task_struct(deputy);
    rcu_read_unlock();

    if (deputy && deputy->mm) {
        // 1. QUICK GRAB: Find the SYSV file and pin it!
        mmap_read_lock(deputy->mm);
        struct vm_area_struct *vma;
        VMA_ITERATOR(vmi, deputy->mm, 0);
        for_each_vma(vmi, vma) {
            if (vma->vm_file && vma->vm_file->f_path.dentry->d_name.name) {
                if (strncmp(vma->vm_file->f_path.dentry->d_name.name, "SYSV", 4) == 0) {
                    if (vma->vm_file->f_inode->i_ino == ctx->req.shmid) {
                        unsigned long target_addr = vma->vm_start + ctx->req.offset;
                        if (target_addr < vma->vm_end) {
                            shm_file = vma->vm_file;
                            get_file(shm_file); // Safely pin the file so it doesn't vanish!
                        }
                        break;
                    }
                }
            }
        }
        mmap_read_unlock(deputy->mm); // DROP THE LOCK IMMEDIATELY!

        // 2. HEAVY LIFTING: Write directly to physical RAM lock-free!
        if (shm_file) {
            // --- THE SAFE BOMB TRICK ---
            // Use filemap_get_folio instead of read_mapping_page!
            // read_mapping_page blocks if the page is locked (e.g., during IPC_RMID destruction).
            // filemap_get_folio is a non-blocking lookup. If the memory is being destroyed,
            // it just returns an error and we safely drop the bomb!
            struct folio *folio = filemap_get_folio(shm_file->f_mapping, ctx->req.offset / PAGE_SIZE);
            
            if (!IS_ERR(folio)) {
                struct page *page = folio_page(folio, 0);
                void *kaddr = kmap_local_page(page);
                memcpy(kaddr, ctx->req.data, PAGE_SIZE);
                kunmap_local(kaddr);
                
                folio_mark_dirty(folio); // Tell the kernel we modified it!
                folio_put(folio);        // Release our reference
                
                mattx_dbg("[DSM_SYNC] VM1 updated physical RAM for SHMID %u at offset %lu\n", ctx->req.shmid, ctx->req.offset);
            } else {
                mattx_dbg("[DSM_SYNC] SHM segment is gone or locked. Dropping Data Bomb safely.\n");
            }
            // ---------------------------------------
            
            fput(shm_file); // Release the file

            // --- THE FUNERAL DIRECTORY CLEANUP ---
            if (config_dsm_mode == 2) {
                unsigned long page_idx = ctx->req.offset / PAGE_SIZE;
                struct mattx_dsm_master_dir *dir;

                spin_lock(&mattx_dsm_lock);
                dir = mattx_dsm_dir_lookup(ctx->req.shmid, page_idx);
                if (dir) {
                    // If the incoming data bomb is from the EXCLUSIVE owner, downgrade it to SHARED!
                    if (dir->page_state[page_idx] == MATTX_PAGE_EXCLUSIVE &&
                        dir->page_owner_pid[page_idx] == ctx->req.orig_pid) {

                        dir->page_state[page_idx] = MATTX_PAGE_SHARED;
                        dir->page_owner_pid[page_idx] = 0;
                        mattx_dsm_mask_clear(dir, page_idx);
                        mattx_dbg("[MESI_VM1] Directory cleaned up! SHMID %u Offset %lu is now SHARED.\n", ctx->req.shmid, ctx->req.offset);
                    }
                }
                spin_unlock(&mattx_dsm_lock);
            }
            // ------------------------------------------
        }

        put_task_struct(deputy);
    }
    kfree(ctx);
}


static void handle_dsm_page_update(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_page_update_req *req = payload;
    struct mattx_dsm_update_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_dsm_update_kworker);
        memcpy(&ctx->req, req, sizeof(*req));
        schedule_work(&ctx->work);
    }
}


// ============================================================================
// THE DIRTY SWEEPER THREAD (VM2 & VM1)
// ============================================================================
int mattx_dsm_sweeper_loop(void *data) {
    mattx_dbg(" [DSM] Dirty Sweeper thread started\n");

    while (!kthread_should_stop()) {
        
        if (config_dsm_mode == 1) {
            // --- PHASE 1: THE GUEST SWEEPER (VM2 - MODE 1 ONLY) ---
            for (int i = 0; i < MAX_GUESTS; i++) {
                for (int d = 0; ; d++) {
                    struct mattx_dsm_mapping m;
                    pid_t local_pid = -1; u32 orig_pid = 0; int home_node = -1;
                    struct task_struct *surrogate = NULL;

                    // One mapping at a time: the reference keeps its tables alive while guest_lock is dropped
                    spin_lock(&guest_lock);
                    if (i < guest_count && d < guest_registry[i].dsm_count) {
                        local_pid = guest_registry[i].local_pid; orig_pid = guest_registry[i].orig_pid;
                        home_node = guest_registry[i].home_node;
                        m = guest_registry[i].dsm_map[d];
                        mattx_dsm_map_get(&m);
                    }
                    spin_unlock(&guest_lock);

                    if (local_pid == -1)
                        break;

                    rcu_read_lock(); surrogate = pid_task(find_vpid(local_pid), PIDTYPE_PID); if (surrogate) get_task_struct(surrogate); rcu_read_unlock();

                    if (surrogate && surrogate->mm) {
                        unsigned long base = m.base_addr;
                        unsigned long size = m.size;
                        u32 shmid = m.shmid;

                        // 1. Sync present pages (The Data Bomb)
                        for (unsigned long bit = 0; bit < m.npages; bit++) {
                            if (test_and_clear_bit(bit, m.present_pages)) {
                                unsigned long offset = bit * PAGE_SIZE;
                                if (offset >= size) continue;

                                void *kaddr = m.pages[bit];
                                if (kaddr) {
                                    size_t req_size = sizeof(struct mattx_dsm_page_update_req);
                                    struct mattx_dsm_page_update_req *req = kmalloc(req_size, GFP_KERNEL);
                                    if (req) {
                                        req->orig_pid = orig_pid; req->shmid = shmid; req->offset = offset;
                                        memcpy(req->data, kaddr, PAGE_SIZE);
                                        if (cluster_map[home_node]) mattx_comm_send(cluster_map[home_node], MATTX_MSG_DSM_PAGE_UPDATE, req, req_size);
                                        kfree(req);
                                    }
                                }
                            }
                        }

                        // 2. The Zap! (Burn it all down SAFELY)
                        if (real_zap_vma_ptes) {
                            mmap_read_lock(surrogate->mm);
                            struct vm_area_struct *vma = find_vma(surrogate->mm, base);
                            if (vma && vma->vm_start <= base) {
                                real_zap_vma_ptes(vma, base, size);
                                mattx_dbg("[DSM_SWEEPER] Zapped Hollow VMA at 0x%lx. Ready for next fault!\n", base);
                            }
                            mmap_read_unlock(surrogate->mm);
                        }
                        if (surrogate) put_task_struct(surrogate);
                    }
                    if (surrogate)
                        put_task_struct(surrogate);
                    mattx_dsm_map_put(&m);
                }
            }
            
        } else if (config_dsm_mode == 2) {
            // --- PHASE 2: THE NATIVE SWEEPER (VM1 - MODE 2 ONLY) ---
            struct { u32 shmid; u32 home_pid; unsigned long offset; } to_shoot[16];
            int shoot_count = 0;

            spin_lock(&mattx_dsm_lock);
            for (int d = 0; d < mattx_dsm_dir_cap && shoot_count < 16; d++) {
                struct mattx_dsm_master_dir *dir = &mattx_global_dsm_dir[d];
                unsigned long scanned;

                if (!dir->in_use || !dir->npages)
                    continue;

                // Only 16 shootdowns per round: continue where the last round stopped, or the first 16 shared
                // pages would be shot down again and again and the rest never
                for (scanned = 0; scanned < dir->npages && shoot_count < 16; scanned++) {
                    unsigned long p = (dir->sweep_pos + scanned) % dir->npages;

                    // If it's SHARED, remote nodes have copies. We must shoot down native PTEs
                    // to force a fault and catch potential silent native writes!
                    if (dir->page_state[p] == MATTX_PAGE_SHARED) {
                        to_shoot[shoot_count].shmid = dir->shmid;
                        to_shoot[shoot_count].home_pid = dir->home_pid;
                        to_shoot[shoot_count].offset = p * PAGE_SIZE;
                        shoot_count++;
                    }
                }
                dir->sweep_pos = (dir->sweep_pos + scanned) % dir->npages;
            }
            spin_unlock(&mattx_dsm_lock);

            for (int s = 0; s < shoot_count; s++) {
                if (real_unmap_mapping_range) {
                    struct task_struct *deputy = NULL;
                    rcu_read_lock();
                    deputy = pid_task(find_vpid(to_shoot[s].home_pid), PIDTYPE_PID);
                    if (deputy) get_task_struct(deputy);
                    rcu_read_unlock();

                    if (deputy) {
                        // --- THE ZOMBIE SHIELD ---
                        // Never touch a dying process! It is already tearing down its mm_struct
                        // and holding the mmap_write_lock. If we try to read_lock it, we deadlock!
                        // Pin the mm once and use that pointer for lock, walk and unlock: re-reading
                        // deputy->mm after the lock is taken races with exit_mm() (which sets it to NULL)
                        // and ends in up_read(&NULL->mmap_lock), an oops in this kthread.
                        struct mm_struct *mm = (deputy->flags & PF_EXITING) ? NULL : get_task_mm(deputy);
                        if (mm) {
                            mmap_read_lock(mm);
                            struct vm_area_struct *vma;
                            VMA_ITERATOR(vmi, mm, 0);
                            for_each_vma(vmi, vma) {
                                if (vma->vm_file && vma->vm_file->f_path.dentry->d_name.name) {
                                    if (strncmp(vma->vm_file->f_path.dentry->d_name.name, "SYSV", 4) == 0) {
                                        if (vma->vm_file->f_inode->i_ino == to_shoot[s].shmid) {
                                            real_unmap_mapping_range(vma->vm_file->f_mapping, to_shoot[s].offset, PAGE_SIZE, 1);
                                            mattx_dbg("[MESI_VM1] Native Sweeper: Shot down PTE for SHMID %u Offset %lu to catch writes.\n", to_shoot[s].shmid, to_shoot[s].offset);
                                            break;
                                        }
                                    }
                                }
                            }
                            mmap_read_unlock(mm);
                            mmput(mm);
                        }
                        put_task_struct(deputy);
                    }
                }
            }

        }
        
        // Free the tables of the segments that were destroyed (the shmctl kprobe must not sleep: it only clears in_use)
        mattx_dsm_dir_reap();

        // Sleep for 50ms regardless of mode to prevent 100% CPU spin!
        msleep(50); 
    }
    return 0;
}



// ============================================================================
// MODE 3: THE CLEANUP CALLBACK (SYNC & ZAP)
// ============================================================================
struct mattx_dsm_cleanup_ctx {
    struct callback_head cb;
    u32 orig_pid;
    int home_node;
    u32 shmid;
    unsigned long fault_addr;
    bool is_write;
};

static void mattx_dsm_cleanup_cb(struct callback_head *cb) {
    struct mattx_dsm_cleanup_ctx *ctx = container_of(cb, struct mattx_dsm_cleanup_ctx, cb);
    
    // 1. THE SYNC (If it was a WRITE)
    if (ctx->is_write) {
        void *kaddr = NULL;
        unsigned long offset = 0;
        struct mattx_dsm_mapping held;  // keeps the page alive while we copy it outside the spinlock

        memset(&held, 0, sizeof(held));
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                    unsigned long base = guest_registry[i].dsm_map[d].base_addr;
                    unsigned long size = guest_registry[i].dsm_map[d].size;
                    if (ctx->fault_addr >= base && ctx->fault_addr < base + size) {
                        offset = ctx->fault_addr - base;
                        unsigned long page_idx = offset / PAGE_SIZE;
                        kaddr = (page_idx < guest_registry[i].dsm_map[d].npages) ? guest_registry[i].dsm_map[d].pages[page_idx] : NULL;
                        if (kaddr) {
                            held = guest_registry[i].dsm_map[d];
                            mattx_dsm_map_get(&held);
                        }
                        break;
                    }
                }
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (kaddr) {
            size_t req_size = sizeof(struct mattx_dsm_page_update_req);
            struct mattx_dsm_page_update_req *req = kmalloc(req_size, GFP_KERNEL);
            if (req) {
                req->orig_pid = ctx->orig_pid;
                req->shmid = ctx->shmid;
                req->offset = offset;
                // Read the updated data directly from our physical page pool!
                memcpy(req->data, kaddr, PAGE_SIZE);
                
                if (cluster_map[ctx->home_node]) {
                    mattx_comm_send(cluster_map[ctx->home_node], MATTX_MSG_DSM_PAGE_UPDATE, req, req_size);
                }
                kfree(req);
                mattx_dbg("[DSM_MODE3] Synced dirty page at 0x%lx to VM1.\n", ctx->fault_addr);
            }
        }
        mattx_dsm_map_put(&held);
    }

    // 2. THE ZAP (Burn it down!)
    if (real_zap_vma_ptes && current->mm) {
        mmap_read_lock(current->mm);
        struct vm_area_struct *vma = find_vma(current->mm, ctx->fault_addr);
        if (vma && vma->vm_start <= ctx->fault_addr) {
            real_zap_vma_ptes(vma, ctx->fault_addr, PAGE_SIZE);
            mattx_dbg("[DSM_MODE3] Zapped page at 0x%lx. Ready for next fault.\n", ctx->fault_addr);
        }
        mmap_read_unlock(current->mm);
    }
    kfree(ctx);
}

void mattx_inject_dsm_cleanup(u32 orig_pid, int home_node, u32 shmid, unsigned long fault_addr, bool is_write) {
    struct mattx_dsm_cleanup_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        ctx->orig_pid = orig_pid;
        ctx->home_node = home_node;
        ctx->shmid = shmid;
        ctx->fault_addr = fault_addr;
        ctx->is_write = is_write;
        init_task_work(&ctx->cb, mattx_dsm_cleanup_cb);
        
        if (real_task_work_add) {
            real_task_work_add(current, &ctx->cb, TWA_RESUME);
        } else {
            kfree(ctx);
        }
    }
}
EXPORT_SYMBOL(mattx_inject_dsm_cleanup);




// ============================================================================
// MESI PROTOCOL: WRITE ACQUIRE & INVALIDATE (MODE 2)
// ============================================================================

// --- VM1: WRITE ACQUIRE KWORKER ---
struct mattx_dsm_write_acquire_kworker_ctx { struct work_struct work; struct mattx_dsm_write_acquire_req req; int target_node; };

static void mattx_dsm_write_acquire_kworker(struct work_struct *work) {
    struct mattx_dsm_write_acquire_kworker_ctx *ctx = container_of(work, struct mattx_dsm_write_acquire_kworker_ctx, work);
    struct mattx_dsm_write_acquire_reply reply = { .req_id = ctx->req.req_id, .error = 0 };
    
    u64 local_shared_mask[MAX_NODES / 64];
    memset(local_shared_mask, 0, sizeof(local_shared_mask));
    bool found = false;
    unsigned long page_idx = ctx->req.offset / PAGE_SIZE;
    struct mattx_dsm_master_dir *dir;

    // --- MODE 2 GLOBAL DIRECTORY LOOKUP (QUICK GRAB) ---
    spin_lock(&mattx_dsm_lock);
    dir = mattx_dsm_dir_lookup(ctx->req.shmid, page_idx);
    if (dir) {
        u64 *mask = mattx_dsm_mask(dir, page_idx);

        // 1. Copy the mask locally!
        if (mask)
            memcpy(local_shared_mask, mask, sizeof(local_shared_mask));

        // 2. Update Directory to EXCLUSIVE immediately
        dir->page_state[page_idx] = MATTX_PAGE_EXCLUSIVE;
        dir->page_owner_pid[page_idx] = ctx->req.orig_pid;

        // Clear the global mask
        mattx_dsm_mask_clear(dir, page_idx);

        found = true;
    }
    spin_unlock(&mattx_dsm_lock); // DROP THE LOCK!
    
    // --- THE HEAVY LIFTING (LOCK-FREE) ---
    if (found) {
        mattx_dbg("[MESI_VM1] PID %u granted EXCLUSIVE lock for SHMID %u Offset %lu\n", ctx->req.orig_pid, ctx->req.shmid, ctx->req.offset);

        // 1. Send Invalidates to EVERYONE in the local mask!
        for (int node = 0; node < MAX_NODES; node++) {
            int n_idx = node / 64;
            int n_bit = node % 64;
            
            if (local_shared_mask[n_idx] & (1ULL << n_bit)) {
                if (cluster_map[node]) {
                    struct mattx_dsm_invalidate_req inv_req = {
                        .req_id = 0, .orig_pid = ctx->req.orig_pid,
                        .shmid = ctx->req.shmid, .offset = ctx->req.offset
                    };
                    mattx_comm_send(cluster_map[node], MATTX_MSG_DSM_INVALIDATE_REQ, &inv_req, sizeof(inv_req));
                    mattx_dbg("[MESI_VM1] Sent INVALIDATE to Node %d for SHMID %u Offset %lu\n", node, ctx->req.shmid, ctx->req.offset);
                }
            }
        }
        
        // 2. THE PTE SHOOTDOWN (VM1)
        if (real_unmap_mapping_range) {
            struct task_struct *deputy = NULL;
            rcu_read_lock();
            deputy = pid_task(find_vpid(ctx->req.orig_pid), PIDTYPE_PID);
            if (deputy) get_task_struct(deputy);
            rcu_read_unlock();
            
            if (deputy && deputy->mm) {
                mmap_read_lock(deputy->mm); // Safe to sleep here now!
                struct vm_area_struct *vma;
                VMA_ITERATOR(vmi, deputy->mm, 0);
                for_each_vma(vmi, vma) {
                    if (vma->vm_file && vma->vm_file->f_path.dentry->d_name.name) {
                        if (strncmp(vma->vm_file->f_path.dentry->d_name.name, "SYSV", 4) == 0) {
                            if (vma->vm_file->f_inode->i_ino == ctx->req.shmid) {
                                real_unmap_mapping_range(vma->vm_file->f_mapping, ctx->req.offset, PAGE_SIZE, 1);
                                mattx_dbg("[MESI_VM1] Shot down native PTEs for SHMID %u Offset %lu\n", ctx->req.shmid, ctx->req.offset);
                                break;
                            }
                        }
                    }
                }
                mmap_read_unlock(deputy->mm);
                put_task_struct(deputy);
            }
        }
    }
    
    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_DSM_WRITE_ACQUIRE_REPLY, &reply, sizeof(reply));
    }
    kfree(ctx);
}


static void handle_dsm_write_acquire_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_write_acquire_req *req = payload;
    struct mattx_dsm_write_acquire_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_dsm_write_acquire_kworker);
        memcpy(&ctx->req, req, sizeof(*req));
        ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}

// --- VM3: INVALIDATE KWORKER ---
struct mattx_dsm_invalidate_kworker_ctx { struct work_struct work; struct mattx_dsm_invalidate_req req; int target_node; };

// A page of a Surrogate to zap: collected under guest_lock, zapped after it is dropped (mmap_read_lock() sleeps)
struct mattx_zap_target {
    struct task_struct *task;
    unsigned long base;
    unsigned long fault_addr;
};

static void mattx_dsm_invalidate_kworker(struct work_struct *work) {
    struct mattx_dsm_invalidate_kworker_ctx *ctx = container_of(work, struct mattx_dsm_invalidate_kworker_ctx, work);
    struct mattx_zap_target *tg = kvmalloc_array(MAX_GUESTS, sizeof(*tg), GFP_KERNEL);
    int n = 0;

    if (!tg) {
        printk(KERN_ERR "MattX:[MESI_VM2] INVALIDATE dropped: out of memory\n");
        kfree(ctx);
        return;
    }

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        // THE FIX: Skip the process that is currently acquiring the EXCLUSIVE lock!
        if (guest_registry[i].orig_pid == ctx->req.orig_pid) {
            continue; 
        }

        // We must check ALL OTHER guests on this node for the SHMID!
        for (int d = 0; d < guest_registry[i].dsm_count; d++) {
            if (guest_registry[i].dsm_map[d].shmid == ctx->req.shmid && ctx->req.offset / PAGE_SIZE < guest_registry[i].dsm_map[d].npages) {
                unsigned long base = guest_registry[i].dsm_map[d].base_addr;
                unsigned long fault_addr = base + ctx->req.offset;
                unsigned long page_idx = ctx->req.offset / PAGE_SIZE;
                
                // 1. Update local state to INVALID
                guest_registry[i].dsm_map[d].page_states[page_idx] = MATTX_PAGE_INVALID;
                
                // 2. Zap the PTE! Only the task is looked up here, the zapping is done below
                if (n < MAX_GUESTS) {
                    struct task_struct *surrogate = NULL;
                    rcu_read_lock();
                    surrogate = pid_task(find_vpid(guest_registry[i].local_pid), PIDTYPE_PID);
                    if (surrogate) get_task_struct(surrogate);
                    rcu_read_unlock();

                    if (surrogate) {
                        tg[n].task = surrogate;
                        tg[n].base = base;
                        tg[n].fault_addr = fault_addr;
                        n++;
                    }
                }
            }
        }
    }
    spin_unlock(&guest_lock);

    for (int k = 0; k < n; k++) {
        struct task_struct *surrogate = tg[k].task;

        if (surrogate->mm && real_zap_vma_ptes) {
            mmap_read_lock(surrogate->mm);
            struct vm_area_struct *vma = find_vma(surrogate->mm, tg[k].base);
            if (vma && vma->vm_start <= tg[k].base) {
                real_zap_vma_ptes(vma, tg[k].fault_addr, PAGE_SIZE);
                mattx_dbg("[MESI_VM2] Zapped invalidated page at 0x%lx for PID %d\n", tg[k].fault_addr, surrogate->pid);
            }
            mmap_read_unlock(surrogate->mm);
        }
        put_task_struct(surrogate);
    }
    kvfree(tg);
    kfree(ctx);
}

static void handle_dsm_invalidate_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_invalidate_req *req = payload;
    struct mattx_dsm_invalidate_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_dsm_invalidate_kworker);
        memcpy(&ctx->req, req, sizeof(*req));
        ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}


// --- VM2: WRITE ACQUIRE REPLY HANDLER ---
static void handle_dsm_write_acquire_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_write_acquire_reply *reply = payload;
    
    spin_lock(&vfs_rpc_lock);
    for (int i = 0; i < MAX_VFS_RPC; i++) {
        if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
            vfs_rpc_registry[i].error = reply->error;
            vfs_rpc_registry[i].done = true;
            wake_up_interruptible(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);
}


// MESI --- VM2: FLUSH KWORKER ---
struct mattx_dsm_flush_kworker_ctx { struct work_struct work; struct mattx_dsm_flush_req req; int target_node; };

static void mattx_dsm_flush_kworker(struct work_struct *work) {
    struct mattx_dsm_flush_kworker_ctx *ctx = container_of(work, struct mattx_dsm_flush_kworker_ctx, work);
    struct mattx_dsm_flush_reply *reply = kzalloc(sizeof(*reply), GFP_KERNEL);
    
    if (!reply) { kfree(ctx); return; }
    reply->req_id = ctx->req.req_id;
    reply->error = -ENOENT;

    struct task_struct *zap_task = NULL;    // looked up under guest_lock, zapped after it is dropped
    unsigned long zap_base = 0, zap_addr = 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].orig_pid == ctx->req.orig_pid) {
            for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                if (guest_registry[i].dsm_map[d].shmid == ctx->req.shmid && ctx->req.offset / PAGE_SIZE < guest_registry[i].dsm_map[d].npages) {
                    unsigned long page_idx = ctx->req.offset / PAGE_SIZE;
                    unsigned long base = guest_registry[i].dsm_map[d].base_addr;
                    unsigned long fault_addr = base + ctx->req.offset;
                    void *kaddr = guest_registry[i].dsm_map[d].pages[page_idx];

                    if (kaddr && guest_registry[i].dsm_map[d].page_states[page_idx] == MATTX_PAGE_EXCLUSIVE) {
                        // 1. Copy the dirty data into the reply!
                        memcpy(reply->data, kaddr, PAGE_SIZE);
                        reply->error = 0;

                        // 2. Downgrade local state to INVALID
                        guest_registry[i].dsm_map[d].page_states[page_idx] = MATTX_PAGE_INVALID;

                        // 3. Zap the PTE! Only the task is looked up here, the zapping is done below
                        rcu_read_lock();
                        zap_task = pid_task(find_vpid(guest_registry[i].local_pid), PIDTYPE_PID);
                        if (zap_task) get_task_struct(zap_task);
                        rcu_read_unlock();
                        zap_base = base;
                        zap_addr = fault_addr;
                    }
                    break;
                }
            }
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (zap_task) {
        if (zap_task->mm && real_zap_vma_ptes) {
            mmap_read_lock(zap_task->mm);
            struct vm_area_struct *vma = find_vma(zap_task->mm, zap_base);
            if (vma && vma->vm_start <= zap_base) {
                real_zap_vma_ptes(vma, zap_addr, PAGE_SIZE);
                mattx_dbg("[MESI_VM2] Flushed dirty data and zapped page at 0x%lx\n", zap_addr);
            }
            mmap_read_unlock(zap_task->mm);
        }
        put_task_struct(zap_task);
    }

    if (cluster_map[ctx->target_node]) {
        mattx_comm_send(cluster_map[ctx->target_node], MATTX_MSG_DSM_FLUSH_REPLY, reply, sizeof(*reply));
    }
    kfree(reply);
    kfree(ctx);
}

static void handle_dsm_flush_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_flush_req *req = payload;
    struct mattx_dsm_flush_kworker_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (ctx) {
        INIT_WORK(&ctx->work, mattx_dsm_flush_kworker);
        memcpy(&ctx->req, req, sizeof(*req));
        ctx->target_node = hdr->sender_id;
        schedule_work(&ctx->work);
    }
}


// MESI --- VM1: FLUSH REPLY HANDLER ---
static void handle_dsm_flush_reply(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    struct mattx_dsm_flush_reply *reply = payload;
    
    spin_lock(&vfs_rpc_lock);
    for (int i = 0; i < MAX_VFS_RPC; i++) {
        if (vfs_rpc_registry[i].in_use && vfs_rpc_registry[i].req_id == reply->req_id) {
            vfs_rpc_registry[i].error = reply->error;
            if (reply->error == 0 && vfs_rpc_registry[i].data_buf) {
                memcpy(vfs_rpc_registry[i].data_buf, reply->data, PAGE_SIZE);
            }
            vfs_rpc_registry[i].done = true;
            wake_up_interruptible(&vfs_rpc_registry[i].wq);
            break;
        }
    }
    spin_unlock(&vfs_rpc_lock);
}



// ============================================================================
// THE NATIVE HOME NODE SYNC KWORKER (VM1)
// ============================================================================
struct mattx_vm1_sync_work {
    struct work_struct work;
    pid_t local_pid;
    u32 shmid;
    unsigned long offset;
    bool is_write;
};

static void mattx_vm1_shm_sync_kworker(struct work_struct *work) {
    struct mattx_vm1_sync_work *rpc = container_of(work, struct mattx_vm1_sync_work, work);
    u32 shmid = rpc->shmid;
    unsigned long offset = rpc->offset;
    bool is_write = rpc->is_write;
    unsigned long page_idx = offset / PAGE_SIZE;
    
    u32 owner_pid = 0;
    int owner_node = -1;
    bool dir_found = false;
    struct mattx_dsm_master_dir *dir;
    
    spin_lock(&mattx_dsm_lock);
    dir = mattx_dsm_dir_lookup(shmid, page_idx);
    if (dir) {
        dir_found = true;
        if (dir->page_state[page_idx] == MATTX_PAGE_EXCLUSIVE)
            owner_pid = dir->page_owner_pid[page_idx];
    }
    spin_unlock(&mattx_dsm_lock);

    // 1. FLUSH IF EXCLUSIVE
    if (owner_pid != 0) {
        spin_lock(&export_lock);
        for (int e = 0; e < export_count; e++) {
            if (export_registry[e].orig_pid == owner_pid) {
                owner_node = export_registry[e].target_node;
                break;
            }
        }
        spin_unlock(&export_lock);

        if (owner_node != -1 && cluster_map[owner_node]) {
            mattx_dbg("[MESI_VM1] Native Fault: Page %lu is EXCLUSIVE to PID %u. Requesting FLUSH...\n", page_idx, owner_pid);
            
            int slot = -1; u64 req_id = 0;
            void *flush_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
            if (flush_buf) {
                spin_lock(&vfs_rpc_lock);
                for (int j = 0; j < MAX_VFS_RPC; j++) {
                    if (!vfs_rpc_registry[j].in_use) {
                        slot = j; vfs_rpc_registry[j].in_use = true;
                        req_id = next_req_id++; vfs_rpc_registry[j].req_id = req_id;
                        vfs_rpc_registry[j].done = false; vfs_rpc_registry[j].data_buf = flush_buf;
                        init_waitqueue_head(&vfs_rpc_registry[j].wq);
                        break;
                    }
                }
                spin_unlock(&vfs_rpc_lock);

                if (slot != -1) {
                    struct mattx_dsm_flush_req flush_req = {
                        .req_id = req_id, .orig_pid = owner_pid, .shmid = shmid, .offset = offset
                    };
                    mattx_comm_send(cluster_map[owner_node], MATTX_MSG_DSM_FLUSH_REQ, &flush_req, sizeof(flush_req));
                    wait_event_interruptible(vfs_rpc_registry[slot].wq, vfs_rpc_registry[slot].done);
                    
                    int flush_err = vfs_rpc_registry[slot].error;
                    spin_lock(&vfs_rpc_lock); vfs_rpc_registry[slot].in_use = false; spin_unlock(&vfs_rpc_lock);

                    if (flush_err == 0) {
                        // Write directly to physical RAM bypassing the frozen process's page tables!
                        struct task_struct *deputy = NULL;
                        rcu_read_lock(); deputy = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (deputy) get_task_struct(deputy); rcu_read_unlock();
                        if (deputy) {
                            mmap_read_lock(deputy->mm);
                            struct vm_area_struct *vma; VMA_ITERATOR(vmi, deputy->mm, 0);
                            for_each_vma(vmi, vma) {
                                if (vma->vm_file && vma->vm_file->f_path.dentry->d_name.name && strncmp(vma->vm_file->f_path.dentry->d_name.name, "SYSV", 4) == 0) {
                                    if (vma->vm_file->f_inode->i_ino == shmid) {
                                        
                                        // --- THE DIRECT PHYSICAL WRITE TRICK ---
                                        // Grab the physical page directly from the kernel's page cache!
                                        struct page *page = read_mapping_page(vma->vm_file->f_mapping, offset / PAGE_SIZE, NULL);
                                        if (!IS_ERR(page)) {
                                            void *kaddr = kmap_local_page(page);
                                            memcpy(kaddr, flush_buf, PAGE_SIZE);
                                            kunmap_local(kaddr);
                                            set_page_dirty(page); // Tell the kernel we modified it!
                                            put_page(page);
                                        } else {
                                            mattx_dbg("[MESI_VM1] ERROR: Failed to read shmem physical page!\n");
                                        }
                                        // ---------------------------------------
                                        
                                        break;
                                    }
                                }
                            }
                            mmap_read_unlock(deputy->mm);
                            put_task_struct(deputy);
                        }
                        mattx_dbg("[MESI_VM1] Native Fault: Successfully flushed dirty data from PID %u.\n", owner_pid);
                    }

                    spin_lock(&mattx_dsm_lock);
                    dir = mattx_dsm_dir_lookup(shmid, page_idx);
                    if (dir) {
                        dir->page_state[page_idx] = MATTX_PAGE_SHARED;
                        dir->page_owner_pid[page_idx] = 0;
                        mattx_dsm_mask_clear(dir, page_idx);
                    }
                    spin_unlock(&mattx_dsm_lock);
                }
                kfree(flush_buf);
            }
        }
    }

    // 2. INVALIDATE IF WRITE
    if (is_write && dir_found) {
        u64 local_shared_mask[MAX_NODES / 64];

        memset(local_shared_mask, 0, sizeof(local_shared_mask));
        spin_lock(&mattx_dsm_lock);
        dir = mattx_dsm_dir_lookup(shmid, page_idx);
        if (dir) {
            u64 *mask = mattx_dsm_mask(dir, page_idx);

            if (mask)
                memcpy(local_shared_mask, mask, sizeof(local_shared_mask));
            dir->page_state[page_idx] = MATTX_PAGE_EXCLUSIVE;
            dir->page_owner_pid[page_idx] = 0; // 0 means VM1 owns it!
            mattx_dsm_mask_clear(dir, page_idx);
        }
        spin_unlock(&mattx_dsm_lock);

        // The Invalidates go out WITHOUT the spinlock: mattx_comm_send() sleeps on its mutex
        for (int node = 0; node < MAX_NODES; node++) {
            int n_idx = node / 64; int n_bit = node % 64;
            if (local_shared_mask[n_idx] & (1ULL << n_bit)) {
                if (cluster_map[node]) {
                    struct mattx_dsm_invalidate_req inv_req = {
                        .req_id = 0, .orig_pid = 0, .shmid = shmid, .offset = offset
                    };
                    mattx_comm_send(cluster_map[node], MATTX_MSG_DSM_INVALIDATE_REQ, &inv_req, sizeof(inv_req));
                    mattx_dbg("[MESI_VM1] Native Fault: Sent INVALIDATE to Node %d\n", node);
                }
            }
        }
    }

    // Wake up the native process to retry the fault!
    struct task_struct *native_task = NULL;
    rcu_read_lock(); native_task = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (native_task) get_task_struct(native_task); rcu_read_unlock();
    if (native_task) {
        mattx_dbg("[MESI_VM1] Native Fault resolved. Waking PID %d to retry.\n", rpc->local_pid);
        send_sig(SIGCONT, native_task, 0);
        put_task_struct(native_task);
    }
    
    kfree(rpc);
}

void mattx_schedule_vm1_sync(pid_t pid, u32 shmid, unsigned long offset, bool is_write) {
    struct mattx_vm1_sync_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC);
    if (rpc) {
        INIT_WORK(&rpc->work, mattx_vm1_shm_sync_kworker);
        rpc->local_pid = pid;
        rpc->shmid = shmid;
        rpc->offset = offset;
        rpc->is_write = is_write;
        schedule_work(&rpc->work);
    }
}











// ###############################################################################
// MattxFS VFS RPC (VM2)
// ###############################################################################


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

    bool is_local_hpc = (config_hpc_local_libs && is_hpc_local_lib(path));

    // The Local Fast-Path (With Chroot Escape!) ---
    if (node_id == my_node_id || is_local_hpc) {
        struct fs_struct *old_fs = NULL;
        
        if (is_local_hpc && node_id != my_node_id && balancer_thread) {
            old_fs = current->fs;
            task_lock(balancer_thread);
            current->fs = balancer_thread->fs;
            task_unlock(balancer_thread);
        }

        struct path local_path;
        int err = kern_path(path, LOOKUP_FOLLOW, &local_path);
        if (!err) {
            err = vfs_getattr(&local_path, stat_out, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
            path_put(&local_path);
        }
        
        if (old_fs) {
            current->fs = old_fs;
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
    strscpy_pad(req.path, path, sizeof(req.path));
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
    strscpy_pad(req.path, path, sizeof(req.path));
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

    bool is_local_hpc = (config_hpc_local_libs && is_hpc_local_lib(path));

    // The Local Fast-Path (With Chroot Escape!) ---
    if (node_id == my_node_id || is_local_hpc) {
        struct fs_struct *old_fs = NULL;
        
        // If we are on VM2 (chrooted) and using the HPC fast-path, borrow the host's root!
        if (is_local_hpc && node_id != my_node_id && balancer_thread) {
            old_fs = current->fs;
            task_lock(balancer_thread);
            current->fs = balancer_thread->fs;
            task_unlock(balancer_thread);
        }

        struct file *f = filp_open(path, flags, mode);
        
        // Restore the chroot!
        if (old_fs) {
            current->fs = old_fs;
        }

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
    strscpy_pad(req.path, path, sizeof(req.path));
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
            vfs_rpc_registry[i].bytes_rw = 0;
            vfs_rpc_registry[i].data_buf = buf; 
            vfs_rpc_registry[i].data_cap = count;
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

    if (mattx_vfs_rpc_wait(slot))
        return -ERESTARTSYS; // slot already released

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
            vfs_rpc_registry[i].bytes_rw = 0;
            vfs_rpc_registry[i].data_buf = NULL;
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

    if (mattx_vfs_rpc_wait(slot))
        return -ERESTARTSYS; // slot already released

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
    strscpy_pad(req.path, path, sizeof(req.path));
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
                    if ((size_t)reply->bytes_read <= vfs_rpc_registry[i].data_cap)
                        memcpy(vfs_rpc_registry[i].data_buf, reply->data, reply->bytes_read);
                    else
                        vfs_rpc_registry[i].bytes_rw = -EIO; // more than was asked for: never overrun the buffer
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
    mattx_unloading = false;
    mattx_wait_wq = alloc_workqueue("mattx_wait", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!mattx_wait_wq)
        printk(KERN_WARNING "MattX: no private workqueue for poll/epoll waiters, using the system one\n");
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
    mattx_register_handler(MATTX_MSG_SYS_EPOLL_WAIT_CANCEL, handle_sys_epoll_wait_cancel);
    mattx_register_handler(MATTX_MSG_SYS_REQ_CANCEL, handle_sys_req_cancel);
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
    mattx_register_handler(MATTX_MSG_SYS_UNAME_REQ, handle_sys_uname_req);
    mattx_register_handler(MATTX_MSG_SYS_UNAME_REPLY, handle_sys_uname_reply);
    mattx_register_handler(MATTX_MSG_SYS_PRLIMIT64_REQ, handle_sys_prlimit64_req);
    mattx_register_handler(MATTX_MSG_SYS_PRLIMIT64_REPLY, handle_sys_prlimit64_reply);
    mattx_register_handler(MATTX_MSG_SYS_PRCTL_REQ, handle_sys_prctl_req);
    mattx_register_handler(MATTX_MSG_SYS_PRCTL_REPLY, handle_sys_generic_int_reply); // Reusing the generic int reply!
    mattx_register_handler(MATTX_MSG_SYS_FCNTL_REPLY, handle_sys_fio_reply);        // Reusing the sys fio reply for fnctl!
    mattx_register_handler(MATTX_MSG_SYS_FCNTL_REQ, handle_sys_fcntl_req);
    mattx_register_handler(MATTX_MSG_SYS_IOCTL_REPLY, handle_sys_fio_reply);        // Reusing the sys fio reply for ioctl!
    mattx_register_handler(MATTX_MSG_SYS_IOCTL_REQ, handle_sys_ioctl_req);
    mattx_register_handler(MATTX_MSG_SYS_PREAD64_REPLY, handle_sys_read_reply);     // Reusing the generic read reply!
    mattx_register_handler(MATTX_MSG_SYS_PREAD64_REQ, handle_sys_pread64_req);
    mattx_register_handler(MATTX_MSG_SYS_STATFS_REQ, handle_sys_statfs_req);
    mattx_register_handler(MATTX_MSG_SYS_STATFS_REPLY, handle_sys_statfs_reply);
    mattx_register_handler(MATTX_MSG_SYS_FSTATFS_REQ, handle_sys_fstatfs_req);
    mattx_register_handler(MATTX_MSG_SYS_FSTATFS_REPLY, handle_sys_fstatfs_reply);
    mattx_register_handler(MATTX_MSG_SYS_NEWFSTATAT_REQ, handle_sys_newfstatat_req);
    mattx_register_handler(MATTX_MSG_SYS_NEWFSTATAT_REPLY, handle_sys_newfstatat_reply);
    mattx_register_handler(MATTX_MSG_SYS_FACCESSAT2_REQ, handle_sys_faccessat2_req);
    mattx_register_handler(MATTX_MSG_SYS_FACCESSAT2_REPLY, handle_sys_generic_int_reply);   // Reusing the generic int reply for faccessat2!
    mattx_register_handler(MATTX_MSG_SYS_READLINK_REQ, handle_sys_readlink_req);
    mattx_register_handler(MATTX_MSG_SYS_READLINK_REPLY, handle_sys_readlink_reply);
    mattx_register_handler(MATTX_MSG_SYS_READLINKAT_REQ, handle_sys_readlinkat_req);
    mattx_register_handler(MATTX_MSG_SYS_READLINKAT_REPLY, handle_sys_readlinkat_reply);
    mattx_register_handler(MATTX_MSG_SYS_GETDENTS64_REQ, handle_sys_getdents64_req);
    mattx_register_handler(MATTX_MSG_SYS_GETDENTS64_REPLY, handle_sys_getdents64_reply);
    mattx_register_handler(MATTX_MSG_SYS_PIPE2_REQ, handle_sys_pipe2_req);
    mattx_register_handler(MATTX_MSG_SYS_PIPE2_REPLY, handle_sys_pipe2_reply);
    mattx_register_handler(MATTX_MSG_SYS_SHMGET_REQ, handle_sys_shmget_req);
    mattx_register_handler(MATTX_MSG_SYS_SHMGET_REPLY, handle_sys_shmget_reply);
    mattx_register_handler(MATTX_MSG_SYS_SHMCTL_REQ, handle_sys_shmctl_req);
    mattx_register_handler(MATTX_MSG_SYS_SHMCTL_REPLY, handle_sys_shmctl_reply);
    mattx_register_handler(MATTX_MSG_SYS_SHMDT_REQ, handle_sys_shmdt_req);
    mattx_register_handler(MATTX_MSG_SYS_SHMDT_REPLY, handle_sys_shmdt_reply);
    mattx_register_handler(MATTX_MSG_SYS_SHMAT_REQ, handle_sys_shmat_req);
    mattx_register_handler(MATTX_MSG_SYS_SHMAT_REPLY, handle_sys_shmat_reply);
    mattx_register_handler(MATTX_MSG_DSM_PAGE_FAULT_REQ, handle_dsm_page_fault_req);
    mattx_register_handler(MATTX_MSG_DSM_PAGE_FAULT_REPLY, handle_dsm_page_fault_reply);
    mattx_register_handler(MATTX_MSG_DSM_PAGE_UPDATE, handle_dsm_page_update);
    mattx_register_handler(MATTX_MSG_DSM_WRITE_ACQUIRE_REQ, handle_dsm_write_acquire_req);
    mattx_register_handler(MATTX_MSG_DSM_WRITE_ACQUIRE_REPLY, handle_dsm_write_acquire_reply);
    mattx_register_handler(MATTX_MSG_DSM_INVALIDATE_REQ, handle_dsm_invalidate_req);
    mattx_register_handler(MATTX_MSG_DSM_FLUSH_REQ, handle_dsm_flush_req);
    mattx_register_handler(MATTX_MSG_DSM_FLUSH_REPLY, handle_dsm_flush_reply);

    mattx_dbg(" [FILEIO] Network handlers registered.\n");
}


// Cleanup all open files to prevent shutdown hangs! ---
// -> Safe cleanup without sleeping inside spinlocks! ---
void mattx_fileio_exit(void) {
    int i, j;
    struct file *f;

    mattx_dbg(" [FILEIO] Purging all open files...\n");

    // 0. Forwarded writes: accept no more, let the drain work item finish, free what is left over
    {
        struct mattx_fwd_write *w, *tmp;

        spin_lock(&mattx_fwd_write_lock);
        mattx_fwd_write_stopping = true;
        spin_unlock(&mattx_fwd_write_lock);

        flush_work(&mattx_fwd_write_work);

        spin_lock(&mattx_fwd_write_lock);
        list_for_each_entry_safe(w, tmp, &mattx_fwd_write_list, node) {
            list_del(&w->node);
            kfree(w);
        }
        spin_unlock(&mattx_fwd_write_lock);
    }

    // 0b. Cancellable kworkers (read, recvfrom, ...): tell them to stop and wait (bounded) until the last one is gone
    {
        int waited;

        WRITE_ONCE(mattx_cancellable_stopping, true);
        for (waited = 0; atomic_read(&mattx_cancellable_inflight) > 0 && waited < 500; waited++)
            msleep(10);
        if (atomic_read(&mattx_cancellable_inflight) > 0)
            printk(KERN_WARNING "MattX:[FILEIO] %d cancellable kworker(s) still running at unload\n", atomic_read(&mattx_cancellable_inflight));
    }

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

    // 3. The master directory of the DSM
    mattx_dsm_dir_exit();
    
    mattx_dbg(" [FILEIO] File purge complete.\n");
}

