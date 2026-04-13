#include "mattx.h"
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

static struct proc_dir_entry *mattx_proc_dir;

// --- 1. The Status File (/proc/mattx/nodes) ---
static int nodes_show(struct seq_file *m, void *v) {
    int i;
    
    // --- NEW: Calculate Local Load on-the-fly ---
    u32 local_cpu = (u32)avenrun[0];
    u32 local_mem = (u32)(((u64)si_mem_available() * PAGE_SIZE) / (1024 * 1024));

    seq_printf(m, "MattX Cluster Nodes:\n");
    seq_printf(m, "------------------------------------------------------------\n");
    seq_printf(m, "Node ID\t\tIP Address\tCPU Load\tMem Free (MB)\n");
    seq_printf(m, "------------------------------------------------------------\n");

    // --- NEW: Print the Local Node First ---
    // We use 127.0.0.1 for the local IP to keep the formatting clean
    seq_printf(m, "%d (Local)\t127.0.0.1\t%u\t\t%u\n", 
               my_node_id, local_cpu, local_mem);

    // --- Print the Remote Nodes ---
    for (i = 0; i < MAX_NODES; i++) {
        if (cluster_map[i] && cluster_map[i]->node_id != -1) {
            seq_printf(m, "%d\t\t%pI4\t%u\t\t%u\n", 
                       cluster_map[i]->node_id, 
                       &cluster_map[i]->ip_addr, 
                       cluster_load_table[i].cpu_load, 
                       cluster_load_table[i].mem_free_mb);
        }
    }
    
    seq_printf(m, "\nBalancer Enabled: %s\n", balancer_enabled ? "YES" : "NO");
    return 0;
}

static int nodes_open(struct inode *inode, struct file *file) {
    return single_open(file, nodes_show, NULL);
}

// In kernel 6.x, we must use proc_ops instead of file_operations
static const struct proc_ops nodes_proc_ops = {
    .proc_open = nodes_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

// --- 2. The Control File (/proc/mattx/admin) ---
static ssize_t admin_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[64];
    char cmd[32];
    int arg1 = -1, arg2 = -1;
    size_t len = min(count, sizeof(buf) - 1);

    if (copy_from_user(buf, ubuf, len)) return -EFAULT;
    buf[len] = '\0';

    // Parse the command string (e.g., "balancer 0" or "migrate 1234 709")
    if (sscanf(buf, "%31s %d %d", cmd, &arg1, &arg2) >= 1) {
        
        if (strcmp(cmd, "balancer") == 0 && arg1 != -1) {
            balancer_enabled = (arg1 != 0);
            printk(KERN_INFO "MattX: [ADMIN] Automatic Load Balancer set to: %d\n", balancer_enabled);
        } 
        else if (strcmp(cmd, "migrate") == 0 && arg1 != -1 && arg2 != -1) {
            struct task_struct *task;
            
            printk(KERN_INFO "MattX: [ADMIN] Manual migration requested: PID %d to Node %d\n", arg1, arg2);
            
            if (arg2 < 0 || arg2 >= MAX_NODES || !cluster_map[arg2]) {
                printk(KERN_ERR "MattX:[ADMIN] Target Node %d is not connected!\n", arg2);
                return count;
            }

            // Safely look up the requested PID
            rcu_read_lock();
            task = pid_task(find_vpid(arg1), PIDTYPE_PID);
            if (task) get_task_struct(task);
            rcu_read_unlock();

            if (task) {
                // Trigger the exact same pipeline the load balancer uses!
                mattx_capture_and_send_state(task, arg2);
                put_task_struct(task);
            } else {
                printk(KERN_ERR "MattX:[ADMIN] PID %d not found!\n", arg1);
            }
        } else {
            printk(KERN_WARNING "MattX: [ADMIN] Unknown command or missing arguments.\n");
        }
    }
    return count;
}

static const struct proc_ops admin_proc_ops = {
    .proc_write = admin_write,
};

// --- Init & Exit ---
int mattx_proc_init(void) {
    mattx_proc_dir = proc_mkdir("mattx", NULL);
    if (!mattx_proc_dir) return -ENOMEM;

    proc_create("nodes", 0444, mattx_proc_dir, &nodes_proc_ops);
    proc_create("admin", 0666, mattx_proc_dir, &admin_proc_ops); // Write-only for everyone (for testing)

    printk(KERN_INFO "MattX: /proc/mattx interface created successfully.\n");
    return 0;
}

void mattx_proc_exit(void) {
    if (mattx_proc_dir) {
        remove_proc_entry("nodes", mattx_proc_dir);
        remove_proc_entry("admin", mattx_proc_dir);
        remove_proc_entry("mattx", NULL);
    }
}

