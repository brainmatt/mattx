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
static struct dentry *mattxfs_root_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    int nodes[64];
    int count, i;
    int target_node;
    bool found = false;
    struct inode *inode;

    // Convert the requested folder name (e.g., "709") into an integer
    if (kstrtoint(dentry->d_name.name, 10, &target_node) != 0)
        return NULL;

    // Check if that node actually exists in the cluster
    count = mattx_get_active_nodes(nodes, 64);
    for (i = 0; i < count; i++) {
        if (nodes[i] == target_node) {
            found = true;
            break;
        }
    }

    if (!found)
        return NULL; // Folder doesn't exist!

    // The node exists! Create a new inode in memory to represent this folder.
    inode = new_inode(dir->i_sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    inode->i_ino = target_node + 1000;
    inode->i_mode = S_IFDIR | 0755;
    simple_inode_init_ts(inode);
    
    // For now, the node folders are just empty directories
    inode->i_op = &simple_dir_inode_operations;
    inode->i_fop = &simple_dir_operations;
    set_nlink(inode, 2);

    // Attach the new inode to the dentry
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
