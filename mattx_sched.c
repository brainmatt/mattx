#include "mattx.h"

static struct task_struct* mattx_find_candidate_task(void) {
    struct task_struct *p;
    struct task_struct *best_candidate = NULL;
    u64 max_runtime = 0;

    rcu_read_lock();
    for_each_process(p) {
        if ((p->flags & PF_KTHREAD) || !p->mm) continue;
        if (p->pid <= 1 || (p->flags & PF_EXITING)) continue;
        if (strcmp(p->comm, "mattx-discd") == 0) continue;
        if (strcmp(p->comm, "mattx-stub") == 0) continue;
        
        if (is_guest_process(p->pid)) continue;

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

static void mattx_evaluate_and_balance(u32 local_cpu_load) {
    int i;
    int best_node = -1;
    u32 lowest_remote_load = 0xFFFFFFFF;

    for (i = 0; i < MAX_NODES; i++) {
        if (cluster_map[i] && cluster_map[i]->node_id != -1) {
            u32 remote_load = cluster_load_table[i].cpu_load;
            if (remote_load < lowest_remote_load) {
                lowest_remote_load = remote_load;
                best_node = i;
            }
        }
    }

    if (best_node != -1) {
        int cond_a = (local_cpu_load > FIXED_LOAD_1_0 && lowest_remote_load < FIXED_LOAD_1_0);
        int cond_b = (local_cpu_load > lowest_remote_load && (local_cpu_load - lowest_remote_load) > FIXED_LOAD_0_2);

        if (cond_a || cond_b) {
            struct task_struct *task = mattx_find_candidate_task();
            if (task) {
                printk(KERN_INFO "MattX: [MIGRATE] Selected PID %d (%s) for migration to Node %d!\n", 
                       task->pid, task->comm, best_node);
                mattx_capture_and_send_state(task, best_node);
                put_task_struct(task); 
                msleep(10000); 
            }
        }
    }
}

int mattx_balancer_loop(void *data) {
    struct mattx_load_info local_load;
    int i;
    
    struct mattx_guest_info dead_guests[16]; 
    int dead_count;

    // NEW: Array for dead exports
    struct mattx_export_info dead_exports[16];
    int dead_export_count;

    printk(KERN_INFO "MattX: Balancer thread started\n");

    while (!kthread_should_stop()) {
        local_load.cpu_load = (u32)avenrun[0]; 
        local_load.mem_free_mb = (u32)(si_mem_available() >> 20);
        
        for (i = 0; i < MAX_NODES; i++) {
            if (cluster_map[i] && cluster_map[i]->node_id != -1) {
                mattx_comm_send(cluster_map[i], MATTX_MSG_LOAD_UPDATE, &local_load, sizeof(local_load));
            }
        }
        
        mattx_evaluate_and_balance(local_load.cpu_load);

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
            
            printk(KERN_INFO "MattX: [WATCHER] Guest PID %d died. Notifying Home Node %d...\n", 
                   dead_guests[i].local_pid, dead_guests[i].home_node);
                   
            if (cluster_map[dead_guests[i].home_node]) {
                mattx_comm_send(cluster_map[dead_guests[i].home_node], MATTX_MSG_PROCESS_EXIT, &exit_msg, sizeof(exit_msg));
            }
        }

        // --- NEW: The Home Watcher (Node 1) ---
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
                if (dead_export_count < 16) dead_exports[dead_export_count++] = export_registry[i];
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

            printk(KERN_INFO "MattX: [WATCHER] Exported PID %d died. Sending Assassination Order to Node %d...\n",
                   dead_exports[i].orig_pid, dead_exports[i].target_node);

            if (cluster_map[dead_exports[i].target_node]) {
                mattx_comm_send(cluster_map[dead_exports[i].target_node], MATTX_MSG_KILL_SURROGATE, &kill_msg, sizeof(kill_msg));
            }
        }

        msleep(BALANCER_INTERVAL_MS);
    }
    return 0;
}

