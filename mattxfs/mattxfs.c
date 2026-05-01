#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "../mattx.h" // Include the shared header to get the API!

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI Filesystem (MFS) - Phase 19");

#define MATTXFS_MAGIC 0x4D415454

// --- 1. The 'ls' Command (Directory Iteration) ---
static int mattxfs_root_iterate(struct file *file, struct dir_context *ctx) {
    int nodes[64];
    int count, i;
    char name[16];

    // Emit the standard "." and ".." directories
    if (!dir_emit_dots(file, ctx))
        return 0;

    // Ask mattx.ko who is currently connected!
    count = mattx_get_active_nodes(nodes, 64);

    for (i = 0; i < count; i++) {
        // ctx->pos keeps track of where 'ls' is in the list. 
        // 0 is ".", 1 is "..", so our nodes start at pos 2.
        if (ctx->pos <= i + 2) {
            snprintf(name, sizeof(name), "%d", nodes[i]);
            
            // Emit the folder name to user-space! 
            // We use (nodes[i] + 1000) as a dummy inode number for now.
            if (!dir_emit(ctx, name, strlen(name), nodes[i] + 1000, DT_DIR))
                return 0;
            
            ctx->pos++;
        }
    }
    return 0;
}

// --- 2. The 'cd' Command (Inode Lookup) ---
// Path Building Magic ---
static int get_node_id_from_dentry(struct dentry *dentry) {
    struct dentry *parent = dentry;
    int node_id = -1;
    
    // Walk up the tree until we find the folder right below the root (e.g., "709")
    while (!IS_ROOT(parent->d_parent)) {
        parent = parent->d_parent;
    }
    kstrtoint(parent->d_name.name, 10, &node_id);
    return node_id;
}

static void get_remote_path_from_dentry(struct dentry *dentry, char *buf, int buflen) {
    char *p = buf + buflen - 1;
    struct dentry *curr = dentry;
    *p = '\0';
    
    // Walk up the tree and prepend folder names until we hit the Node ID folder
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

// Forward declaration for the remote directory operations
static const struct inode_operations mattxfs_remote_iops;

// The Remote Lookup (The RPC Bridge) ---
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
        // File doesn't exist on the remote node
        return NULL; 
    }

    // The file exists! Create a virtual inode for it.
    inode = new_inode(dir->i_sb);
    if (!inode) return ERR_PTR(-ENOMEM);

    inode->i_ino = stat.ino;
    inode->i_mode = stat.mode;
    inode->i_size = stat.size;
    inode->i_blocks = stat.blocks;
    inode->i_uid = stat.uid;
    inode->i_gid = stat.gid;
    simple_inode_init_ts(inode);

    // If it's a directory, attach our lookup operations so we can keep exploring!
    if (S_ISDIR(stat.mode)) {
        inode->i_op = &mattxfs_remote_iops;
        inode->i_fop = &simple_dir_operations; // We will replace this with remote readdir later
        set_nlink(inode, 2);
    } else {
        // It's a regular file
        set_nlink(inode, 1);
    }

    d_add(dentry, inode);
    return NULL;
}

static const struct inode_operations mattxfs_remote_iops = {
    .lookup = mattxfs_remote_lookup,
};

// the Root Lookup ---
static struct dentry *mattxfs_root_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    int nodes[64];
    int count, i;
    int target_node;
    bool found = false;
    struct inode *inode;

    if (kstrtoint(dentry->d_name.name, 10, &target_node) != 0) return NULL;

    count = mattx_get_active_nodes(nodes, 64);
    for (i = 0; i < count; i++) {
        if (nodes[i] == target_node) {
            found = true;
            break;
        }
    }

    if (!found) return NULL; 

    inode = new_inode(dir->i_sb);
    if (!inode) return ERR_PTR(-ENOMEM);

    inode->i_ino = target_node + 1000;
    inode->i_mode = S_IFDIR | 0755;
    simple_inode_init_ts(inode);
    
    // Attach the REMOTE operations to the Node folder! ---
    inode->i_op = &mattxfs_remote_iops;
    inode->i_fop = &simple_dir_operations;
    set_nlink(inode, 2);

    d_add(dentry, inode);
    return NULL;
}

// --- 3. Wire up the Custom Operations ---
static const struct file_operations mattxfs_root_fops = {
    .read           = generic_read_dir,
    .iterate_shared = mattxfs_root_iterate, // Hook 'ls'
    .llseek         = generic_file_llseek,
};

static const struct inode_operations mattxfs_root_iops = {
    .lookup         = mattxfs_root_lookup,  // Hook 'cd'
};

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

    // --- FIXED: Apply our custom operations to the Root Inode! ---
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
