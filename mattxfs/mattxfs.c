#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "../mattx.h" 

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI Filesystem (MFS) - Phase 20");

#define MATTXFS_MAGIC 0x4D415454

// Forward declarations for the remote operations
static const struct inode_operations mattxfs_remote_iops;
static const struct file_operations mattxfs_remote_dir_fops;

// --- Path Building Magic ---
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

// ============================================================================
// LEVEL 2: THE REMOTE OPERATIONS (Browsing inside a Node)
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

static struct dentry *mattxfs_remote_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    char path_buf[256];
    struct kstat stat;
    struct inode *inode;
    int node_id;
    int err;

    node_id = get_node_id_from_dentry(dentry);
    get_remote_path_from_dentry(dentry, path_buf, sizeof(path_buf));

    // Ask mattx.ko to fetch the file metadata over the network!
    err = mattx_rpc_vfs_getattr(node_id, path_buf, &stat);
    
    if (err) {
        return NULL; // File doesn't exist on the remote node
    }

    inode = new_inode(dir->i_sb);
    if (!inode) return ERR_PTR(-ENOMEM);

    inode->i_ino = stat.ino;
    inode->i_mode = stat.mode;
    inode->i_size = stat.size;
    inode->i_blocks = stat.blocks;
    inode->i_uid = stat.uid;
    inode->i_gid = stat.gid;
    simple_inode_init_ts(inode);

    if (S_ISDIR(stat.mode)) {
        inode->i_op = &mattxfs_remote_iops;
        inode->i_fop = &mattxfs_remote_dir_fops; 
        set_nlink(inode, 2);
    } else {
        set_nlink(inode, 1);
    }

    d_add(dentry, inode);
    return NULL;
}

static const struct file_operations mattxfs_remote_dir_fops = {
    .read           = generic_read_dir,
    .iterate_shared = mattxfs_remote_iterate, 
    .llseek         = generic_file_llseek,
};

static const struct inode_operations mattxfs_remote_iops = {
    .lookup         = mattxfs_remote_lookup,  
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

    if (!found)
        return NULL; 

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

static const struct super_operations mattxfs_s_ops = {
    .statfs         = simple_statfs,
    .drop_inode     = generic_delete_inode,
};

static int mattxfs_fill_super(struct super_block *sb, void *data, int silent) {
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

static struct dentry *mattxfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    return mount_nodev(fs_type, flags, data, mattxfs_fill_super);
}

static struct file_system_type mattxfs_type = {
    .owner          = THIS_MODULE,
    .name           = "mattxfs",
    .mount          = mattxfs_mount,
    .kill_sb        = kill_litter_super, 
    .fs_flags       = FS_USERNS_MOUNT,   
};

static int __init mattxfs_init(void) {
    int ret = register_filesystem(&mattxfs_type);
    if (ret) printk(KERN_ERR "MattXFS: Failed to register filesystem\n");
    else printk(KERN_INFO "MattXFS: Filesystem registered successfully.\n");
    return ret;
}

static void __exit mattxfs_exit(void) {
    unregister_filesystem(&mattxfs_type);
    printk(KERN_INFO "MattXFS: Filesystem unregistered.\n");
}

module_init(mattxfs_init);
module_exit(mattxfs_exit);
