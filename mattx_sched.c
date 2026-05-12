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
#include <linux/cpumask.h> // For num_online_cpus()

// --- NEW: Load Balancer Configuration ---
char config_migration_excludes[256] = "top,bash,pvmd,sshd,mattx-discd,mattx-stub,systemd";
u32 config_node_affinity = 0; // 0 means auto-calculate based on CPU cores
static pid_t last_migrated_pid = 0; // Prevents picking the same task twice in a burst

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

// --- INSTANTANEOUS LOAD: Count running threads ---
u32 mattx_calc_local_load(void) {
    struct task_struct *p;
    u32 load = 0;

    rcu_read_lock();
    for_each_process(p) {
        if ((p->flags & PF_KTHREAD) || !p->mm) continue;
        if (p->pid <= 1 || (p->flags & PF_EXITING)) continue;
        if (is_guest_process(p->pid)) continue;
        
        // Count threads that are actively running or waiting for CPU
        if (READ_ONCE(p->__state) == TASK_RUNNING) {
            load++;
        }
    }
    rcu_read_unlock();
    return load;
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
        
        // The Bouncer Check!
        if (is_task_excluded(p->comm)) continue;
        
        // Prevent picking the same task twice during a burst!
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

// --- THE OPENMOSIX NORMALIZED LOAD ALGORITHM (V2: Fixed Math!) ---
static void mattx_evaluate_and_balance(u32 local_load, u32 local_affinity) {
    int i;
    int best_node = -1;
    u64 lowest_remote_norm_load = 0xFFFFFFFFFFFFFFFFULL;
    u64 local_norm_load;
    int deficit = 0;

    if (!balancer_enabled || local_affinity == 0) return;

    // Scale by 1,000,000 so that 1 process per CPU equals 1,000,000
    local_norm_load = ((u64)local_load * 1000000ULL) / local_affinity;

    for (i = 0; i < MAX_NODES; i++) {
        if (cluster_map[i] && cluster_map[i]->node_id != -1) {
            u32 remote_load = cluster_load_table[i].cpu_load;
            u32 remote_affinity = cluster_load_table[i].affinity;
            if (remote_affinity == 0) remote_affinity = 1000; // Failsafe
            
            u64 remote_norm_load = ((u64)remote_load * 1000000ULL) / remote_affinity;
            
            if (remote_norm_load < lowest_remote_norm_load) {
                lowest_remote_norm_load = remote_norm_load;
                best_node = i;
            }
        }
    }

    if (best_node != -1) {
        // A difference of 500,000 means a difference of 0.5 processes per CPU
        if (local_norm_load > lowest_remote_norm_load && (local_norm_load - lowest_remote_norm_load) >= 500000ULL) {
            
            // Calculate how many processes we need to shed to equalize!
            deficit = (((local_norm_load - lowest_remote_norm_load) * local_affinity) / 1000000ULL) / 2;
            
            if (deficit < 1) deficit = 1;
            if (deficit > 5) deficit = 5; // Cap burst to 5 per cycle to avoid network storms

            mattx_dbg("[BALANCER] Local Norm: %llu, Node %d Norm: %llu. Deficit: %d. Bursting!\n", 
                   local_norm_load, best_node, lowest_remote_norm_load, deficit);

            for (i = 0; i < deficit; i++) {
                struct task_struct *task = mattx_find_candidate_task();
                if (task) {
                    mattx_dbg("[MIGRATE] Selected PID %d (%s) for migration to Node %d!\n", 
                           task->pid, task->comm, best_node);
                    
                    last_migrated_pid = task->pid;
                    mattx_capture_and_send_state(task, best_node);
                    put_task_struct(task); 
                    
                    msleep(200); // Small delay between burst migrations to let the network breathe
                } else {
                    mattx_dbg("[BALANCER] Deficit is %d, but no suitable candidate tasks found! (Excluded or Sleeping?)\n", deficit);
                    break; // No more candidates found
                }
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
        
        // Auto-calculate affinity if not set by admin
        if (config_node_affinity == 0) {
            config_node_affinity = num_online_cpus() * 1000;
        }

        local_load.cpu_load = mattx_calc_local_load(); // Instantaneous Load!
        local_load.affinity = config_node_affinity;
        local_load.mem_free_mb = (u32)(((u64)si_mem_available() * PAGE_SIZE) / (1024 * 1024));

        for (i = 0; i < MAX_NODES; i++) {
            if (cluster_map[i] && cluster_map[i]->node_id != -1) {
                mattx_comm_send(cluster_map[i], MATTX_MSG_LOAD_UPDATE, &local_load, sizeof(local_load));
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

