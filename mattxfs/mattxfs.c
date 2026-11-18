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
 
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/version.h>
#include <linux/falloc.h>
#include <linux/fs_context.h>
#include "../mattx.h" 

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Matthias Rechenburg");
MODULE_DESCRIPTION("MattX SSI - FS Module");

#define MATTXFS_MAGIC 0x4D415454

// Forward declarations for the remote operations
static const struct inode_operations mattxfs_remote_iops;
static const struct inode_operations mattxfs_remote_file_iops;
static const struct inode_operations mattxfs_remote_symlink_iops;
static const struct file_operations mattxfs_remote_dir_fops;

// 1: Path Building Magic ---
static int get_node_id_from_dentry(struct dentry *dentry) {
    struct dentry *parent = dentry;
    int node_id = -1;
    
    while (!IS_ROOT(parent->d_parent)) {
        parent = parent->d_parent;
    }
    
    if (kstrtoint(parent->d_name.name, 10, &node_id) != 0) {
        return -1; 
    }
    
    return node_id;
}

static void get_remote_path_from_dentry(struct dentry *dentry, char *buf, int buflen) {
    char *p = buf + buflen - 1;
    struct dentry *curr = dentry;
    *p = '\0';
    
    while (!IS_ROOT(curr->d_parent)) {
        int len = curr->d_name.len;
        p -= len;
        memcpy(p, curr->d_name.name, len);
        p--;
        *p = '/';
        curr = curr->d_parent;
    }
    if (*p == '\0') { *(--p) = '/'; }
    memmove(buf, p, buf + buflen - p);
}


// 2: The Remote File Operations (Read/Write/Close) ---
struct mattxfs_file_info {
    int node_id;
    int remote_fd;
};

static int mattxfs_remote_file_open(struct inode *inode, struct file *file) {
    struct dentry *dentry = file->f_path.dentry;
    char path_buf[256];
    int node_id = get_node_id_from_dentry(dentry);
    int remote_fd = -1;
    int err;

    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));

    err = mattx_rpc_vfs_open(node_id, path_buf, file->f_flags, inode->i_mode, &remote_fd);
    if (err == 0) {
        struct mattxfs_file_info *fi = kzalloc(sizeof(*fi), GFP_KERNEL);
        if (!fi) {
            mattx_rpc_vfs_close(node_id, remote_fd);
            return -ENOMEM;
        }
        fi->node_id = node_id;
        fi->remote_fd = remote_fd;
        file->private_data = fi;
    }
    return err;
}

static ssize_t mattxfs_remote_file_read(struct file *file, char __user *buf, size_t count, loff_t *pos) {
    struct mattxfs_file_info *fi = file->private_data;
    void *kbuf;
    ssize_t bytes_read;

    if (!fi) return -EIO;

    if (count > MATTX_VFS_READ_MAX)     // one request carries at most that: a short read is fine
        count = MATTX_VFS_READ_MAX;

    kbuf = kvmalloc(count, GFP_KERNEL);
    if (!kbuf) return -ENOMEM;

    bytes_read = mattx_rpc_vfs_read(fi->node_id, fi->remote_fd, kbuf, count, pos);
    
    if (bytes_read > (ssize_t)count)    // never copy more than was asked for (and allocated)
        bytes_read = -EIO;

    if (bytes_read > 0) {
        if (copy_to_user(buf, kbuf, bytes_read)) {
            bytes_read = -EFAULT;
        }
    }
    
    kvfree(kbuf);
    return bytes_read;
}

static ssize_t mattxfs_remote_file_write(struct file *file, const char __user *buf, size_t count, loff_t *pos) {
    struct mattxfs_file_info *fi = file->private_data;
    size_t chunk = min_t(size_t, count, MATTX_VFS_WRITE_MAX);
    size_t done = 0;
    ssize_t err = 0;
    void *kbuf;

    if (!fi) return -EIO;
    if (!count) return 0;

    kbuf = kvmalloc(chunk, GFP_KERNEL);
    if (!kbuf) return -ENOMEM;

    // One request carries at most MATTX_VFS_WRITE_MAX bytes: ask chunk by chunk until everything is written, a short
    // write (non-blocking file, interrupted pipe write) or an error. What was written is reported, not the error.
    while (done < count) {
        size_t n = min_t(size_t, count - done, MATTX_VFS_WRITE_MAX);
        ssize_t w;

        if (copy_from_user(kbuf, buf + done, n)) {
            err = -EFAULT;
            break;
        }
        w = mattx_rpc_vfs_write(fi->node_id, fi->remote_fd, kbuf, n, pos);
        if (w < 0) {
            err = w;
            break;
        }
        done += w;
        if ((size_t)w < n)
            break;
    }

    kvfree(kbuf);

    // The SIGPIPE of the real pipe_write() / sock_write_iter() goes to the writing thread, here the Surrogate's
    if (err == -EPIPE)
        send_sig(SIGPIPE, current, 0);

    return done ? (ssize_t)done : err;
}

static loff_t mattxfs_remote_file_llseek(struct file *file, loff_t offset, int whence) {
    struct mattxfs_file_info *fi = file->private_data;
    loff_t res;

    if (!fi) return -EIO;

    res = mattx_rpc_vfs_llseek(fi->node_id, fi->remote_fd, offset, whence);
    
    // If the seek succeeded on the remote node, update our local VFS pointer!
    if (res >= 0) {
        file->f_pos = res;
    }
    
    return res;
}

static int mattxfs_remote_file_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
    struct mattxfs_file_info *fi = file->private_data;

    if (!fi) return -EIO;

    return mattx_rpc_vfs_fsync(fi->node_id, fi->remote_fd, start, end, datasync);
}

static int mattxfs_remote_file_release(struct inode *inode, struct file *file) {
    struct mattxfs_file_info *fi = file->private_data;
    if (fi) {
        mattx_rpc_vfs_close(fi->node_id, fi->remote_fd);
        kfree(fi);
    }
    return 0;
}

// fcntl() locks and flock() of a remote file: the lock is taken on the home node, on the open file there (issue #67, P3)
static int mattxfs_remote_file_lock(struct file *file, int cmd, struct file_lock *fl) {
    struct mattxfs_file_info *fi = file->private_data;

    if (!fi) return -EIO;
    return mattx_rpc_vfs_lock(fi->node_id, fi->remote_fd, cmd, fl);
}

static int mattxfs_remote_file_flock(struct file *file, int cmd, struct file_lock *fl) {
    struct mattxfs_file_info *fi = file->private_data;

    if (!fi) return -EIO;
    return mattx_rpc_vfs_flock(fi->node_id, fi->remote_fd, cmd, fl);
}

/*
 * fallocate() of a remote file (issue #67, P3, Track M): the home node does it on its open file (the home filesystem
 * decides which modes it knows: EOPNOTSUPP like on a local disk). MattXFS reads and writes go over the wire, there is no
 * page cache here to invalidate; only the cached size follows (fstat asks the home node anyway, see getattr).
 */
static long mattxfs_remote_file_fallocate(struct file *file, int mode, loff_t offset, loff_t len) {
    struct mattxfs_file_info *fi = file->private_data;
    struct inode *inode = file_inode(file);
    loff_t size;
    long err;

    if (!fi) return -EIO;

    mattx_dbg("[FALLOCATE] fd %d mode 0x%x off %lld len %lld on Node %d\n", fi->remote_fd, mode,
              (long long)offset, (long long)len, fi->node_id);
    err = mattx_rpc_vfs_fallocate(fi->node_id, fi->remote_fd, mode, offset, len);
    if (err)
        return err;

    size = i_size_read(inode);
    if (mode & FALLOC_FL_COLLAPSE_RANGE)
        size = size > len ? size - len : 0;
    else if (mode & FALLOC_FL_INSERT_RANGE)
        size += len;
    else if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > size)
        size = offset + len;
    i_size_write(inode, size);
    return 0;
}

// Wire them up!
static const struct file_operations mattxfs_remote_file_fops = {
    .open    = mattxfs_remote_file_open,
    .read    = mattxfs_remote_file_read,
    .write   = mattxfs_remote_file_write,
    .release = mattxfs_remote_file_release,
    .llseek  = mattxfs_remote_file_llseek,
    .fsync   = mattxfs_remote_file_fsync,
    .fallocate = mattxfs_remote_file_fallocate,
    .lock    = mattxfs_remote_file_lock,
    .flock   = mattxfs_remote_file_flock,
};

// ============================================================================
// LEVEL 3: THE REMOTE OPERATIONS (Browsing inside a Node)
// ============================================================================

static int mattxfs_remote_iterate(struct file *file, struct dir_context *ctx) {
    struct dentry *dentry = file->f_path.dentry;
    char path_buf[256];
    struct mattx_dirent *entries; // Pointer instead of array
    u32 count = 0;
    int node_id;
    int err;
    int i;

    node_id = get_node_id_from_dentry(dentry);
    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));

    // Allocate on the Heap to save the stack!
    entries = kvmalloc_array(20, sizeof(struct mattx_dirent), GFP_KERNEL);
    if (!entries) return -ENOMEM;

    u64 offset = ctx->pos;
    err = mattx_rpc_vfs_readdir(node_id, path_buf, &offset, entries, &count);
    
    if (err) {
        kvfree(entries);
        return err;
    }

    for (i = 0; i < count; i++) {
        // Tell 'ls' the exact offset of the file we are emitting!
        ctx->pos = entries[i].offset;
        if (!dir_emit(ctx, entries[i].name, strlen(entries[i].name), entries[i].ino, entries[i].type)) {
            kvfree(entries);
            return 0; // Terminal buffer is full, stop here. ctx->pos is correct!
        }
    }
    
    ctx->pos = offset; // Update to the final offset for the next batch
    kvfree(entries);
    return 0;
}

// The inode of a remote file / directory from the attributes the home node gave us (lookup(), mkdir() ...)
static struct inode *mattxfs_new_inode(struct super_block *sb, const struct kstat *stat) {
    struct inode *inode = new_inode(sb);

    if (!inode) return NULL;

    inode->i_ino = stat->ino;
    inode->i_mode = stat->mode;
    inode->i_size = stat->size;
    inode->i_blocks = stat->blocks;
    inode->i_uid = stat->uid;
    inode->i_gid = stat->gid;
    inode_set_atime_to_ts(inode, stat->atime);
    inode_set_mtime_to_ts(inode, stat->mtime);
    inode_set_ctime_to_ts(inode, stat->ctime);

    if (S_ISDIR(stat->mode)) {
        inode->i_op = &mattxfs_remote_iops;
        inode->i_fop = &mattxfs_remote_dir_fops;
        set_nlink(inode, 2);
    } else if (S_ISLNK(stat->mode)) {
        // A symlink is a symlink here too: the target is fetched when the VFS wants to follow it
        inode->i_op = &mattxfs_remote_symlink_iops;
        set_nlink(inode, 1);
    } else {
        // Give regular files the ability to be read and written! ---
        inode->i_op = &mattxfs_remote_file_iops;
        inode->i_fop = &mattxfs_remote_file_fops;
        set_nlink(inode, 1);
    }
    return inode;
}

static struct dentry *mattxfs_remote_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    char path_buf[256];
    struct kstat stat;
    struct inode *inode;
    int node_id;
    int err;

    node_id = get_node_id_from_dentry(dentry);
    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));

    // Ask mattx.ko to fetch the file metadata over the network!
    // lstat style: a symlink is a symlink (its target is read by ->get_link())
    err = mattx_rpc_vfs_getattr_flags(node_id, path_buf, &stat, MATTX_GETATTR_NOFOLLOW);

    // A signal is no "file does not exist": no negative dentry, let the system call restart
    if (err == -ERESTARTSYS)
        return ERR_PTR(err);

    if (err) {
        // Create a Negative Dentry! ---
        d_add(dentry, NULL);
        return NULL; 
    }

    inode = mattxfs_new_inode(dir->i_sb, &stat);
    if (!inode) return ERR_PTR(-ENOMEM);

    d_add(dentry, inode);
    return NULL;
}

static const struct file_operations mattxfs_remote_dir_fops = {
    .read           = generic_read_dir,
    .iterate_shared = mattxfs_remote_iterate, 
    .llseek         = generic_file_llseek,
};


static int mattxfs_remote_unlink(struct inode *dir, struct dentry *dentry) {
    char path_buf[256];
    int node_id = get_node_id_from_dentry(dentry);
    
    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
    
    mattx_dbg("[UNLINK] Requesting unlink for %s on Node %d\n", path_buf, node_id);
    
    // Actually delete the file over the network! ---
    return mattx_rpc_vfs_unlink(node_id, path_buf);
}

// The Dummy Create Hook ---
// This tricks the VFS into thinking we created the file locally, so it passes 
// the call down to our .open hook, which sends the O_CREAT flag over the network!
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int mattxfs_remote_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
#else
static int mattxfs_remote_create(struct user_namespace *mnt_userns, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
#endif
    struct inode *inode = new_inode(dir->i_sb);
    if (!inode) return -ENOMEM;

    inode->i_ino = get_next_ino(); 
    inode->i_mode = S_IFREG | mode;
    
    // --- FIXED: Assign the file to the user who created it! ---
    inode->i_uid = current_fsuid();
    inode->i_gid = current_fsgid();
    
    simple_inode_init_ts(inode);
    
    inode->i_op = &mattxfs_remote_file_iops;
    inode->i_fop = &mattxfs_remote_file_fops;
    set_nlink(inode, 1);
    
    d_instantiate(dentry, inode);
    return 0;
}

/*
 * stat() / fstat() / statx() of a remote regular file (issue #67, P3). The inode is built once at lookup() (or is empty
 * after create()), so size and times stayed at that moment: after a write (by anybody) and after ftruncate() the
 * attributes were stale. Ask the home node (path based, the existing VFS_GETATTR) and refresh the inode with the answer.
 * statx(AT_STATX_DONT_SYNC) is served from the cache. If the home node cannot answer (link down, file already
 * unlinked while it is still open ...), the cached attributes are better than an error: fstat() keeps working.
 * A signal is passed on (-ERESTARTSYS).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int mattxfs_remote_getattr(struct mnt_idmap *idmap, const struct path *path, struct kstat *stat,
                                  u32 request_mask, unsigned int query_flags) {
#else
static int mattxfs_remote_getattr(struct user_namespace *idmap, const struct path *path, struct kstat *stat,
                                  u32 request_mask, unsigned int query_flags) {
#endif
    struct dentry *dentry = path->dentry;
    struct inode *inode = d_inode(dentry);

    if ((query_flags & AT_STATX_SYNC_TYPE) != AT_STATX_DONT_SYNC) {
        struct kstat remote;
        char path_buf[256];
        int node_id = get_node_id_from_dentry(dentry);
        int err;

        get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
        err = mattx_rpc_vfs_getattr(node_id, path_buf, &remote);
        if (err == -ERESTARTSYS)
            return err;
        if (err) {
            mattx_dbg("[GETATTR] %s on Node %d: home node says %d, cached attributes used\n", path_buf, node_id, err);
        } else {
            mattx_dbg("[GETATTR] %s on Node %d: size %lld (cached %lld)\n", path_buf, node_id,
                      (long long)remote.size, (long long)i_size_read(inode));
            i_size_write(inode, remote.size);
            inode->i_mode = (inode->i_mode & S_IFMT) | (remote.mode & ~S_IFMT);     // chmod of anybody
            inode->i_blocks = remote.blocks;
            inode->i_uid = remote.uid;
            inode->i_gid = remote.gid;
            inode_set_atime_to_ts(inode, remote.atime);
            inode_set_mtime_to_ts(inode, remote.mtime);
            inode_set_ctime_to_ts(inode, remote.ctime);
        }
    }

    generic_fillattr(idmap, request_mask, inode, stat);
    return 0;
}

/*
 * setattr of a remote file (issue #67, P3): truncate() / ftruncate() (0017a), chmod / chown / utimes (0017c).
 * Without a .setattr the kernel's simple_setattr() only changed this node's local inode: the call returned 0 and the
 * file at the home node stayed as it was (and the next stat() brought the old values back). The change is made at
 * the home node FIRST (the permission checks of the caller's identity are done there, the node's own clock gives
 * "now"); only when that worked the local inode follows. ftruncate() comes with the open file (ATTR_FILE): its
 * remote FD is used; everything else is by path.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int mattxfs_remote_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr) {
#else
static int mattxfs_remote_setattr(struct user_namespace *idmap, struct dentry *dentry, struct iattr *attr) {
#endif
    struct inode *inode = d_inode(dentry);
    struct mattxfs_file_info *fi = NULL;
    char path_buf[256];
    int node_id, remote_fd = -1;
    int err;

    err = setattr_prepare(idmap, dentry, attr);
    if (err)
        return err;

    if ((attr->ia_valid & ATTR_FILE) && attr->ia_file && attr->ia_file->f_op == &mattxfs_remote_file_fops)
        fi = attr->ia_file->private_data;
    if (fi) {
        node_id = fi->node_id;
        remote_fd = fi->remote_fd;
        path_buf[0] = '\0';
    } else {
        node_id = get_node_id_from_dentry(dentry);
        get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
    }

    mattx_dbg("[SETATTR] %s valid 0x%x (size %lld) on Node %d\n", fi ? "fd" : path_buf, attr->ia_valid,
              (long long)attr->ia_size, node_id);
    err = mattx_rpc_vfs_setattr(node_id, remote_fd, path_buf, attr);
    if (err)
        return err;

    if (attr->ia_valid & ATTR_SIZE)
        truncate_setsize(inode, attr->ia_size);
    setattr_copy(idmap, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

static const struct inode_operations mattxfs_remote_file_iops = {
    .getattr        = mattxfs_remote_getattr,
    .setattr        = mattxfs_remote_setattr,
};

/*
 * mkdir / rmdir of /mattxfs (issue #67, P3, Track M). Without them the VFS refused both. The home node makes the
 * directory (or removes it) under the identity of the caller; the umask is applied by the VFS before it calls us, so
 * the mode arrives final. The new inode comes from the attributes the home node reports for the new directory.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
typedef struct mnt_idmap mattxfs_idmap_t;
#else
typedef struct user_namespace mattxfs_idmap_t;
#endif

static int mattxfs_do_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
    char path_buf[256];
    struct kstat st = {};
    struct inode *inode;
    int node_id = get_node_id_from_dentry(dentry);
    int err;

    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
    mattx_dbg("[MKDIR] %s mode 0%o on Node %d\n", path_buf, mode, node_id);

    err = mattx_rpc_vfs_mkdir(node_id, path_buf, mode);
    if (err)
        return err;

    if (mattx_rpc_vfs_getattr(node_id, path_buf, &st) || !S_ISDIR(st.mode)) {
        // The home node made it but did not tell us how it looks: our best guess
        st.ino = get_next_ino();
        st.mode = S_IFDIR | (mode & S_IALLUGO);
        st.uid = current_fsuid();
        st.gid = current_fsgid();
        ktime_get_real_ts64(&st.atime);
        st.mtime = st.atime;
        st.ctime = st.atime;
    }
    inode = mattxfs_new_inode(dir->i_sb, &st);
    if (!inode)
        return -ENOMEM;
    d_instantiate(dentry, inode);
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
// since 6.15 ->mkdir() returns a dentry: NULL = the one we were given is instantiated
static struct dentry *mattxfs_remote_mkdir(mattxfs_idmap_t *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
    return ERR_PTR(mattxfs_do_mkdir(dir, dentry, mode));
}
#else
static int mattxfs_remote_mkdir(mattxfs_idmap_t *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
    return mattxfs_do_mkdir(dir, dentry, mode);
}
#endif

static int mattxfs_remote_rmdir(struct inode *dir, struct dentry *dentry) {
    char path_buf[256];
    int node_id = get_node_id_from_dentry(dentry);

    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
    mattx_dbg("[RMDIR] %s on Node %d\n", path_buf, node_id);
    return mattx_rpc_vfs_rmdir(node_id, path_buf);
}

/*
 * rename of /mattxfs (issue #67, P3, Track M). Both names must live on the same node (else EXDEV, like two mounts): the
 * home node does the real rename; the VFS moves the dentries afterwards (d_move / d_exchange) when we return 0.
 * RENAME_NOREPLACE and RENAME_EXCHANGE are carried, RENAME_WHITEOUT is not supported.
 */
static int mattxfs_remote_rename(mattxfs_idmap_t *idmap, struct inode *old_dir, struct dentry *old_dentry,
                                 struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
    char old_buf[256], new_buf[256];
    int old_node = get_node_id_from_dentry(old_dentry);
    int new_node = get_node_id_from_dentry(new_dentry);

    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;
    if (old_node != new_node)
        return -EXDEV;

    get_remote_path_from_dentry(old_dentry, old_buf, sizeof(old_buf));
    get_remote_path_from_dentry(new_dentry, new_buf, sizeof(new_buf));
    mattx_dbg("[RENAME] %s -> %s flags 0x%x on Node %d\n", old_buf, new_buf, flags, old_node);
    return mattx_rpc_vfs_rename(old_node, old_buf, new_buf, flags);
}

/*
 * Symbolic and hard links of /mattxfs (issue #67, P3, Track M). A symlink is a symlink on the Surrogate too (lookup no
 * longer follows it at the home node): ->get_link() asks the home node for the text. The text is NOT translated: an
 * absolute target is resolved by the Surrogate's own namespace, like on NFS. A hard link needs both names on one node.
 */
static const char *mattxfs_remote_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done) {
    char path_buf[256];
    char *target;
    int node_id, len;

    if (!dentry)                    // RCU walk: we sleep
        return ERR_PTR(-ECHILD);

    target = kmalloc(256, GFP_KERNEL);
    if (!target)
        return ERR_PTR(-ENOMEM);

    node_id = get_node_id_from_dentry(dentry);
    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
    len = mattx_rpc_vfs_readlink(node_id, path_buf, target, 256);
    mattx_dbg("[READLINK] %s on Node %d: %d\n", path_buf, node_id, len);
    if (len < 0) {
        kfree(target);
        return ERR_PTR(len);
    }
    target[len] = '\0';
    set_delayed_call(done, kfree_link, target);
    return target;
}

static const struct inode_operations mattxfs_remote_symlink_iops = {
    .get_link       = mattxfs_remote_get_link,
};

static int mattxfs_remote_symlink(mattxfs_idmap_t *idmap, struct inode *dir, struct dentry *dentry, const char *target) {
    char path_buf[256];
    struct kstat st = {};
    struct inode *inode;
    int node_id = get_node_id_from_dentry(dentry);
    int err;

    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));
    mattx_dbg("[SYMLINK] %s -> %s on Node %d\n", path_buf, target, node_id);

    err = mattx_rpc_vfs_symlink(node_id, target, path_buf);
    if (err)
        return err;

    if (mattx_rpc_vfs_getattr_flags(node_id, path_buf, &st, MATTX_GETATTR_NOFOLLOW) || !S_ISLNK(st.mode)) {
        // The home node made it but did not tell us how it looks: our best guess
        st.ino = get_next_ino();
        st.mode = S_IFLNK | 0777;
        st.size = strlen(target);
        st.uid = current_fsuid();
        st.gid = current_fsgid();
        ktime_get_real_ts64(&st.atime);
        st.mtime = st.atime;
        st.ctime = st.atime;
    }
    inode = mattxfs_new_inode(dir->i_sb, &st);
    if (!inode)
        return -ENOMEM;
    d_instantiate(dentry, inode);
    return 0;
}

static int mattxfs_remote_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry) {
    char old_buf[256], new_buf[256];
    struct inode *inode = d_inode(old_dentry);
    int old_node = get_node_id_from_dentry(old_dentry);
    int new_node = get_node_id_from_dentry(new_dentry);
    int err;

    if (old_node != new_node)
        return -EXDEV;

    get_remote_path_from_dentry(old_dentry, old_buf, sizeof(old_buf));
    get_remote_path_from_dentry(new_dentry, new_buf, sizeof(new_buf));
    mattx_dbg("[LINK] %s -> %s on Node %d\n", new_buf, old_buf, old_node);

    err = mattx_rpc_vfs_link(old_node, old_buf, new_buf);
    if (err)
        return err;

    // The new name is the same inode (like any local filesystem)
    inode_set_ctime_current(inode);
    inc_nlink(inode);
    ihold(inode);
    d_instantiate(new_dentry, inode);
    return 0;
}

static const struct inode_operations mattxfs_remote_iops = {
    .lookup         = mattxfs_remote_lookup,  
    .create         = mattxfs_remote_create, // Allow file creation!
    .unlink         = mattxfs_remote_unlink, // NEW: Wire up the rm command!
    .mkdir          = mattxfs_remote_mkdir,
    .rmdir          = mattxfs_remote_rmdir,
    .rename         = mattxfs_remote_rename,
    .symlink        = mattxfs_remote_symlink,
    .link           = mattxfs_remote_link,
};


// ============================================================================
// LEVEL 1: THE ROOT OPERATIONS (Browsing /mattxfs/)
// ============================================================================

static int mattxfs_root_iterate(struct file *file, struct dir_context *ctx) {
    int nodes[64];
    int count, i;
    char name[16];

    if (!dir_emit_dots(file, ctx))
        return 0;

    count = mattx_get_active_nodes(nodes, 64);

    for (i = 0; i < count; i++) {
        if (ctx->pos <= i + 2) {
            snprintf(name, sizeof(name), "%d", nodes[i]);
            
            if (!dir_emit(ctx, name, strlen(name), nodes[i] + 1000, DT_DIR))
                return 0;
            
            ctx->pos++;
        }
    }
    return 0;
}

static struct dentry *mattxfs_root_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    int nodes[64];
    int count, i;
    int target_node;
    bool found = false;
    struct inode *inode;

    if (kstrtoint(dentry->d_name.name, 10, &target_node) != 0)
        return NULL;

    count = mattx_get_active_nodes(nodes, 64);
    for (i = 0; i < count; i++) {
        if (nodes[i] == target_node) {
            found = true;
            break;
        }
    }

    if (!found) {
        // Create a Negative Dentry so the VFS doesn't leak memory! ---
        d_add(dentry, NULL);
        return NULL; 
    }

    inode = new_inode(dir->i_sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    inode->i_ino = target_node + 1000;
    inode->i_mode = S_IFDIR | 0755;
    simple_inode_init_ts(inode);
    
    // When we create a Node folder, we attach the REMOTE operations to it!
    // This means anything inside this folder uses the network.
    inode->i_op = &mattxfs_remote_iops;
    inode->i_fop = &mattxfs_remote_dir_fops;
    set_nlink(inode, 2);

    d_add(dentry, inode);
    return NULL;
}

static const struct file_operations mattxfs_root_fops = {
    .read           = generic_read_dir,
    .iterate_shared = mattxfs_root_iterate, 
    .llseek         = generic_file_llseek,
};

static const struct inode_operations mattxfs_root_iops = {
    .lookup         = mattxfs_root_lookup,  
};

// ============================================================================
// FILESYSTEM MOUNT & INIT
// ============================================================================

// The modern way to force inode deletion on unmount! ---
static int mattxfs_drop_inode(struct inode *inode) {
    return 1; // 1 means "Always delete this inode from memory!"
}

static const struct super_operations mattxfs_s_ops = {
    .statfs         = simple_statfs,
    .drop_inode     = mattxfs_drop_inode, 
};

// --- VFS API EVOLUTION ---
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static int mattxfs_fill_super(struct super_block *sb, struct fs_context *fc)
#else
static int mattxfs_fill_super(struct super_block *sb, void *data, int silent)
#endif
{
    struct inode *inode;

    sb->s_maxbytes      = MAX_LFS_FILESIZE;
    sb->s_blocksize     = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_magic         = MATTXFS_MAGIC;
    sb->s_op            = &mattxfs_s_ops;
    sb->s_time_gran     = 1;

    inode = new_inode(sb);
    if (!inode) return -ENOMEM;

    inode->i_ino = 1; 
    inode->i_mode = S_IFDIR | 0755; 
    simple_inode_init_ts(inode); 

    inode->i_op = &mattxfs_root_iops;
    inode->i_fop = &mattxfs_root_fops;

    set_nlink(inode, 2); 

    sb->s_root = d_make_root(inode);
    if (!sb->s_root) return -ENOMEM;

    return 0;
}

// --- THE MOUNT API SWITCH ---
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)

// The Modern 7.x fs_context API
static int mattxfs_get_tree(struct fs_context *fc) {
    return get_tree_nodev(fc, mattxfs_fill_super);
}

static const struct fs_context_operations mattxfs_context_ops = {
    .get_tree = mattxfs_get_tree,
};

static int mattxfs_init_fs_context(struct fs_context *fc) {
    fc->ops = &mattxfs_context_ops;
    return 0;
}

static struct file_system_type mattxfs_type = {
    .name           = "mattxfs",
    .owner          = THIS_MODULE,
    .init_fs_context = mattxfs_init_fs_context,
    .kill_sb        = kill_anon_super, // kill_litter_super was removed!
};

#else

// The Legacy 6.x mount API
static struct dentry *mattxfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    return mount_nodev(fs_type, flags, data, mattxfs_fill_super);
}

static struct file_system_type mattxfs_type = {
    .name           = "mattxfs",
    .owner          = THIS_MODULE,
    .mount          = mattxfs_mount,
    // Not kill_litter_super(): that d_genocide()s the tree, i.e. drops one reference from every
    // dentry on the assumption that each was pinned by the filesystem (ramfs-style). mattxfs
    // dentries are ordinary lookup dentries with no extra pin, so their refcounts went negative,
    // the umount could not evict them ("VFS: Busy inodes after unmount", BUG at fs/super.c:650)
    // or spun forever in shrink_dcache_parent()/d_walk(). Same as the fs_context path above.
    .kill_sb        = kill_anon_super,
};

#endif

static int __init mattxfs_init(void) {
    int ret = register_filesystem(&mattxfs_type);
    if (ret) printk(KERN_ERR "MattXFS: Failed to register filesystem\n");
    else mattx_dbg(" Filesystem registered successfully.\n");
    return ret;
}

static void __exit mattxfs_exit(void) {
    unregister_filesystem(&mattxfs_type);
    mattx_dbg(" Filesystem unregistered.\n");
}

module_init(mattxfs_init);
module_exit(mattxfs_exit);
