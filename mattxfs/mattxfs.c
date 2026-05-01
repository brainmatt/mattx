#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI Filesystem (MFS)");

// We use our signature magic number for the superblock!
#define MATTXFS_MAGIC 0x4D415454

// Basic superblock operations (we use the kernel's built-in simple helpers for now)
static const struct super_operations mattxfs_s_ops = {
    .statfs         = simple_statfs,
    .drop_inode     = generic_delete_inode,
};

// This function is called when you type 'mount -t mattxfs ...'
static int mattxfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *inode;

    // 1. Configure the Superblock (The "Hard Drive" metadata)
    sb->s_maxbytes      = MAX_LFS_FILESIZE;
    sb->s_blocksize     = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_magic         = MATTXFS_MAGIC;
    sb->s_op            = &mattxfs_s_ops;
    sb->s_time_gran     = 1;

    // 2. Create the Root Inode (The '/' folder of our filesystem)
    inode = new_inode(sb);
    if (!inode)
        return -ENOMEM;

    inode->i_ino = 1; // Inode 1 is traditionally the root directory
    inode->i_mode = S_IFDIR | 0755; // It's a directory, readable by everyone
    
    // Modern 6.x way to initialize timestamps
    simple_inode_init_ts(inode); 

    // We use the kernel's built-in "simple directory" operations for the skeleton
    inode->i_op = &simple_dir_inode_operations;
    inode->i_fop = &simple_dir_operations;

    set_nlink(inode, 2); // Standard for an empty directory (. and ..)

    // 3. Attach the inode to the Superblock root
    sb->s_root = d_make_root(inode);
    if (!sb->s_root)
        return -ENOMEM;

    return 0;
}

// The mount callback
static struct dentry *mattxfs_mount(struct file_system_type *fs_type,
                                    int flags, const char *dev_name,
                                    void *data)
{
    // mount_nodev means "Mount this without a physical block device"
    return mount_nodev(fs_type, flags, data, mattxfs_fill_super);
}

// The Filesystem Definition
static struct file_system_type mattxfs_type = {
    .owner          = THIS_MODULE,
    .name           = "mattxfs",
    .mount          = mattxfs_mount,
    .kill_sb        = kill_litter_super, // Standard cleanup helper
    .fs_flags       = FS_USERNS_MOUNT,   // Allow mounting in namespaces!
};

static int __init mattxfs_init(void)
{
    int ret;
    
    // Register our new filesystem with the Linux VFS!
    ret = register_filesystem(&mattxfs_type);
    if (ret) {
        printk(KERN_ERR "MattXFS: Failed to register filesystem\n");
    } else {
        printk(KERN_INFO "MattXFS: Filesystem registered successfully. Ready to mount!\n");
    }
    
    return ret;
}

static void __exit mattxfs_exit(void)
{
    unregister_filesystem(&mattxfs_type);
    printk(KERN_INFO "MattXFS: Filesystem unregistered.\n");
}

module_init(mattxfs_init);
module_exit(mattxfs_exit);
