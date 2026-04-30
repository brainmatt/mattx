#include "mattx.h"

// --- Guest Registry Implementation ---
struct mattx_guest_info guest_registry[MAX_GUESTS];
int guest_count = 0;
DEFINE_SPINLOCK(guest_lock);

bool is_guest_process(pid_t pid) {
    int i;
    bool found = false;
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            found = true;
            break;
        }
    }
    spin_unlock(&guest_lock);
    return found;
}

void add_guest_process(pid_t local_pid, u32 orig_pid, int home_node) {
    spin_lock(&guest_lock);
    if (guest_count < MAX_GUESTS) {
        guest_registry[guest_count].local_pid = local_pid;
        guest_registry[guest_count].orig_pid = orig_pid;
        guest_registry[guest_count].home_node = home_node;
        guest_count++;
    } else {
        printk(KERN_WARNING "MattX: [REGISTRY] Guest registry is full!\n");
    }
    spin_unlock(&guest_lock);
}

void remove_guest_process(int index) {
    if (index < 0 || index >= guest_count) return;
    guest_registry[index] = guest_registry[guest_count - 1];
    guest_count--;
}

// --- Export Registry Implementation ---
struct mattx_export_info export_registry[MAX_GUESTS];
int export_count = 0;
DEFINE_SPINLOCK(export_lock);

void add_export_process(pid_t orig_pid, int target_node) {
    spin_lock(&export_lock);
    if (export_count < MAX_GUESTS) {
        export_registry[export_count].orig_pid = orig_pid;
        export_registry[export_count].target_node = target_node;
        export_count++;
    } else {
        printk(KERN_WARNING "MattX: [REGISTRY] Export registry is full!\n");
    }
    spin_unlock(&export_lock);
}

void remove_export_process(int index) {
    if (index < 0 || index >= export_count) return;
    export_registry[index] = export_registry[export_count - 1];
    export_count--;
}

int get_export_target(pid_t orig_pid) {
    int target = -1;
    int i;
    spin_lock(&export_lock);
    for (i = 0; i < export_count; i++) {
        if (export_registry[i].orig_pid == orig_pid) {
            target = export_registry[i].target_node;
            break;
        }
    }
    spin_unlock(&export_lock);
    return target;
}

// --- Network Handlers for Lifecycle Management ---

static void handle_process_exit(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_process_exit *exit_msg = (struct mattx_process_exit *)payload;
        struct task_struct *deputy = NULL;
        int i;

        printk(KERN_INFO "MattX: [FUNERAL] Received exit notice for Deputy PID %u from Node %u\n", 
               exit_msg->orig_pid, hdr->sender_id);

        rcu_read_lock();
        deputy = pid_task(find_vpid(exit_msg->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            printk(KERN_INFO "MattX: [FUNERAL] Laying Deputy PID %u to rest (Sending SIGKILL)...\n", deputy->pid);
            send_sig(SIGKILL, deputy, 0);
            put_task_struct(deputy);
        }
        
        spin_lock(&export_lock);
        for (i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == exit_msg->orig_pid) {
                remove_export_process(i);
                break;
            }
        }
        spin_unlock(&export_lock);
    }
}

static void handle_kill_surrogate(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_process_exit *kill_msg = (struct mattx_process_exit *)payload;
        pid_t local_stub_pid = -1;
        int i;

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == kill_msg->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                local_stub_pid = guest_registry[i].local_pid;
                remove_guest_process(i);
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (local_stub_pid != -1) {
            struct task_struct *surrogate = NULL;
            rcu_read_lock();
            surrogate = pid_task(find_vpid(local_stub_pid), PIDTYPE_PID);
            if (surrogate) get_task_struct(surrogate);
            rcu_read_unlock();

            if (surrogate) {
                printk(KERN_INFO "MattX:[ASSASSIN] Executing Surrogate PID %d (Sending SIGKILL)...\n", surrogate->pid);
                send_sig(SIGKILL, surrogate, 0);
                put_task_struct(surrogate);
            }
        }
    }
}

bool is_rpc_pending(pid_t pid) {
    bool pending = false;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            // If rpc_wq is not NULL, the process is currently waiting for Node 1!
            pending = (guest_registry[i].rpc_wq != NULL);
            break;
        }
    }
    spin_unlock(&guest_lock);
    return pending;
}

void mattx_guest_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_PROCESS_EXIT, handle_process_exit);
    mattx_register_handler(MATTX_MSG_KILL_SURROGATE, handle_kill_surrogate);
    printk(KERN_INFO "MattX: [GUEST] Network handlers registered.\n");
}

