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

        //Exorcise the ghosts! Clean up dirty memory from previous guests ---
        guest_registry[guest_count].is_migrating = false;
        guest_registry[guest_count].rpc_wq = NULL;
        guest_registry[guest_count].rpc_done = false;
        guest_registry[guest_count].rpc_read_buf = NULL;
        guest_registry[guest_count].rpc_read_bytes = 0;
        guest_registry[guest_count].rpc_lseek_res = 0;
        guest_registry[guest_count].rpc_statx_buf = NULL;
        guest_registry[guest_count].rpc_fsync_res = 0;

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

        mattx_dbg(" [FUNERAL] Received exit notice for Deputy PID %u from Node %u\n", 
               exit_msg->orig_pid, hdr->sender_id);

        rcu_read_lock();
        deputy = pid_task(find_vpid(exit_msg->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            mattx_dbg(" [FUNERAL] Laying Deputy PID %u to rest (Sending SIGKILL)...\n", deputy->pid);
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
                mattx_dbg("[ASSASSIN] Executing Surrogate PID %d (Sending SIGKILL)...\n", surrogate->pid);
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
    mattx_dbg(" [GUEST] Network handlers registered.\n");
}

