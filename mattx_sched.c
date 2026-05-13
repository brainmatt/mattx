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

 /*
 * MattX - The Modern Single System Image (SSI) Cluster
 * 
 * Copyright (c) 2026 by Matthias Rechenburg
 * All rights reserved.
 */

#include "mattx.h"
#include <linux/cpumask.h> // For num_online_cpus()

// --- Load Balancer Configuration ---
char config_migration_excludes[256] = "top,bash,pvmd,sshd,mattx-discd,mattx-stub,systemd";
u32 config_node_affinity = 0; // 0 means auto-calculate based on CPU cores
static pid_t last_migrated_pid = 0; // Prevents picking the same task twice in a burst
static unsigned long last_migration_jiffies = 0; // Tracks the 5-second cooldown

// --- THE BOUNCER: Check if task is on the VIP Exclude List ---
static bool is_task_excluded(const char *comm) {
    char excludes[256];
    char *token, *rest;
    
    strscpy(excludes, config_migration_excludes, sizeof(excludes));
    rest = excludes;
    
    while ((token = strsep(&rest, ",")) != NULL) {
        while (*token == ' ') token++; // Trim leading spaces
        if (strcmp(comm, token) == 0) return true;
    }
    return false;
}

// --- TRUE LOAD: Count runnable tasks (Scaled by 1000) ---
u32 mattx_calc_local_load(void) {
    struct task_struct *p;
    u32 count = 0;
    
    rcu_read_lock();
    for_each_process(p) {
        if ((p->flags & PF_KTHREAD) || !p->mm) continue;
        if (p->pid <= 1 || (p->flags & PF_EXITING)) continue;
        if (is_guest_process(p->pid)) continue;
        
        // Don't count excluded tasks, otherwise they artificially inflate our load!
        if (is_task_excluded(p->comm)) continue;
        
        // --- DESIGN A: The Observer Blindspot ---
        // Never count the process that is currently asking for the load!
        if (p == current) continue;

        // --- DESIGN B: The Minimum Age Filter ---
        // Only count tasks that have consumed at least 50ms of CPU time (50,000,000 ns).
        // This filters out tiny, transient scripts and commands like 'cat' or 'ls'.
        if (p->se.sum_exec_runtime < 50000000ULL) continue;
                
        if (READ_ONCE(p->__state) == TASK_RUNNING) {
            count++;
        }
    }
    rcu_read_unlock();
    
    return count * 1000; // 1 Task = 1000 Load
}

static struct task_struct* mattx_find_candidate_task(void) {
    struct task_struct *p;
    struct task_struct *best_candidate = NULL;
    u64 max_runtime = 0;

    rcu_read_lock();
    for_each_process(p) {
        if ((p->flags & PF_KTHREAD) || !p->mm) continue;
        if (p->pid <= 1 || (p->flags & PF_EXITING)) continue;
        if (is_guest_process(p->pid)) continue;
        
        if (is_task_excluded(p->comm)) continue;
        if (p->pid == last_migrated_pid) continue;

        if (READ_ONCE(p->__state) != TASK_RUNNING) continue;

        if (p->se.sum_exec_runtime > max_runtime) {
            max_runtime = p->se.sum_exec_runtime;
            best_candidate = p;
        }
    }
    if (best_candidate) get_task_struct(best_candidate);
    rcu_read_unlock();
    return best_candidate;
}

// --- THE TRUE OPENMOSIX NORMALIZED LOAD ALGORITHM ---
static void mattx_evaluate_and_balance(u32 local_load, u32 local_affinity) {
    int i;
    int best_node = -1;
    u32 lowest_remote_norm_load = 0xFFFFFFFF;
    u32 local_norm_load;
    int deficit_tasks = 0;

    if (!balancer_enabled || local_affinity == 0 || local_load == 0) return;

    // --- THE COOLDOWN TIMER ---
    // Prevent network storms by waiting 5 seconds after a migration burst!
    if (last_migration_jiffies && time_before(jiffies, last_migration_jiffies + msecs_to_jiffies(5000))) {
        return; 
    }
        
    // Normalized Load = (Load * 1000) / Affinity
    local_norm_load = (local_load * 1000) / local_affinity;

    for (i = 0; i < MAX_NODES; i++) {
        if (cluster_map[i] && cluster_map[i]->node_id != -1) {
            u32 remote_load = cluster_load_table[i].cpu_load;
            u32 remote_affinity = cluster_load_table[i].affinity;
            if (remote_affinity == 0) remote_affinity = 1000; // Failsafe
            
            u32 remote_norm_load = (remote_load * 1000) / remote_affinity;
            
            if (remote_norm_load < lowest_remote_norm_load) {
                lowest_remote_norm_load = remote_norm_load;
                best_node = i;
            }
        }
    }

if (best_node != -1) {
        // Threshold: 250 means a 25% imbalance of a single CPU core.
        if (local_norm_load > lowest_remote_norm_load && (local_norm_load - lowest_remote_norm_load) >= 250) {
            
            mattx_dbg("[BALANCER] Local Norm: %u, Node %d Norm: %u. Imbalance detected!\n", 
                   local_norm_load, best_node, lowest_remote_norm_load);

            struct task_struct *task = mattx_find_candidate_task();
            if (task) {
                mattx_dbg("[MIGRATE] Selected PID %d (%s) for migration to Node %d!\n", 
                       task->pid, task->comm, best_node);
                
                last_migrated_pid = task->pid;
                mattx_capture_and_send_state(task, best_node);
                put_task_struct(task); 
                
                // --- THE STRICT COOLDOWN ---
                // Start the 5-second cooldown clock immediately after ONE migration!
                last_migration_jiffies = jiffies;
                mattx_dbg("[BALANCER] Migration complete. Entering 5-second cooldown.\n");
            } else {
                mattx_dbg("[BALANCER] Imbalance detected, but no suitable candidate tasks found!\n");
            }
        }
    }
}

int mattx_balancer_loop(void *data) {
    struct mattx_load_info local_load;
    int i;
    struct mattx_guest_info dead_guests[16]; 
    int dead_count;
    struct { u32 orig_pid; int target_node; } dead_exports[16];
    int dead_export_count;

    mattx_dbg(" Balancer thread started\n");

    while (!kthread_should_stop()) {
        
        u32 current_affinity = config_node_affinity ? config_node_affinity : num_online_cpus() * 1000;

        local_load.cpu_load = mattx_calc_local_load(); 
        local_load.affinity = current_affinity;
        local_load.mem_free_mb = (u32)(((u64)si_mem_available() * PAGE_SIZE) / (1024 * 1024));

        for (i = 0; i < MAX_NODES; i++) {
            if (cluster_map[i] && cluster_map[i]->node_id != -1) {
                // Use the Mutex Bypass sender so the balancer never deadlocks!
                mattx_comm_send_ctrl(cluster_map[i], MATTX_MSG_LOAD_UPDATE, &local_load, sizeof(local_load));
            }
        }
        
        mattx_evaluate_and_balance(local_load.cpu_load, local_load.affinity);

        // --- The Guest Watcher (Node 2) ---
        dead_count = 0;
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; ) {
            struct task_struct *task = NULL;
            bool is_dead = false;
            
            rcu_read_lock();
            task = pid_task(find_vpid(guest_registry[i].local_pid), PIDTYPE_PID);
            if (task) get_task_struct(task);
            rcu_read_unlock();
            
            if (!task) {
                is_dead = true; 
            } else {
                if (task->exit_state != 0) {
                    is_dead = true; 
                }
                put_task_struct(task);
            }

            if (is_dead) {
                if (dead_count < 16) dead_guests[dead_count++] = guest_registry[i];
                remove_guest_process(i);
            } else {
                i++;
            }
        }
        spin_unlock(&guest_lock);

        for (i = 0; i < dead_count; i++) {
            struct mattx_process_exit exit_msg;
            exit_msg.orig_pid = dead_guests[i].orig_pid;
            exit_msg.exit_code = 0; 
            
            mattx_dbg(" [WATCHER] Guest PID %d died. Notifying Home Node %d...\n", 
                   dead_guests[i].local_pid, dead_guests[i].home_node);
                   
            if (cluster_map[dead_guests[i].home_node]) {
                mattx_comm_send(cluster_map[dead_guests[i].home_node], MATTX_MSG_PROCESS_EXIT, &exit_msg, sizeof(exit_msg));
            }
        }

        // --- The Home Watcher (Node 1) ---
        dead_export_count = 0;
        spin_lock(&export_lock);
        for (i = 0; i < export_count; ) {
            struct task_struct *task = NULL;
            bool is_dead = false;

            rcu_read_lock();
            task = pid_task(find_vpid(export_registry[i].orig_pid), PIDTYPE_PID);
            if (task) get_task_struct(task);
            rcu_read_unlock();

            if (!task) {
                is_dead = true;
            } else {
                if (task->exit_state != 0) {
                    is_dead = true;
                }
                put_task_struct(task);
            }

            if (is_dead) {
                if (dead_export_count < 16) {
                    dead_exports[dead_export_count].orig_pid = export_registry[i].orig_pid;
                    dead_exports[dead_export_count].target_node = export_registry[i].target_node;
                    dead_export_count++;
                }
                remove_export_process(i);
            } else {
                i++;
            }
        }
        spin_unlock(&export_lock);

        for (i = 0; i < dead_export_count; i++) {
            struct mattx_process_exit kill_msg;
            kill_msg.orig_pid = dead_exports[i].orig_pid;
            kill_msg.exit_code = 0; 

            mattx_dbg(" [WATCHER] Exported PID %d died. Sending Assassination Order to Node %d...\n",
                   dead_exports[i].orig_pid, dead_exports[i].target_node);

            if (cluster_map[dead_exports[i].target_node]) {
                mattx_comm_send(cluster_map[dead_exports[i].target_node], MATTX_MSG_KILL_SURROGATE, &kill_msg, sizeof(kill_msg));
            }
        }

        msleep(BALANCER_INTERVAL_MS);
    }
    return 0;
}

// --- Network Handlers for the Scheduler ---
static void handle_heartbeat(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (link->node_id == -1 && hdr->sender_id < MAX_NODES) {
        link->node_id = hdr->sender_id;
        cluster_map[link->node_id] = link;
        mattx_dbg(" [SCHED] Registered new connection from Node %u\n", hdr->sender_id);
    }
}

static void handle_load_update(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (hdr->sender_id < MAX_NODES && payload) {
        struct mattx_load_info *load = (struct mattx_load_info *)payload;
        cluster_load_table[hdr->sender_id] = *load;
    }
}

void mattx_sched_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_HEARTBEAT, handle_heartbeat);
    mattx_register_handler(MATTX_MSG_LOAD_UPDATE, handle_load_update);
    mattx_dbg(" [SCHED] Network handlers registered.\n");
}
