#include "mattx.h"
#include <linux/task_work.h>   // For task_work_add
#include <linux/completion.h>  // For wait_for_completion

static struct mattx_migration_req *local_migration_req = NULL;
static struct task_struct *migrating_task = NULL;
static int migrating_target_node = -1;
static bool is_returning = false; 

// --- NEW: The Syscall Drainer ---

struct mattx_drain_ctx {
    struct callback_head cb;
    struct completion done;
};

// This function runs INSIDE the target process, right at the user-space boundary!
static void mattx_drain_callback(struct callback_head *cb) {
    struct mattx_drain_ctx *ctx = container_of(cb, struct mattx_drain_ctx, cb);
    
    // 1. Tell the migrator thread that we have safely reached the boundary
    complete(&ctx->done);
    
    // 2. Put ourselves into a deep, stable freeze
    set_current_state(TASK_STOPPED);
    schedule();
}

static void mattx_freeze_task_safely(struct task_struct *task) {
    struct mattx_drain_ctx ctx;
    int ret;

    // If the task is already stopped (e.g., waiting in our RPC Wormhole), it's already stable!
    if (READ_ONCE(task->__state) & __TASK_STOPPED) {
        printk(KERN_INFO "MattX:[DRAIN] PID %d is already stopped and stable.\n", task->pid);
        return;
    }

    init_completion(&ctx.done);
    init_task_work(&ctx.cb, mattx_drain_callback);

    // TWA_SIGNAL safely interrupts sleeping syscalls and forces the callback to run
    ret = task_work_add(task, &ctx.cb, TWA_SIGNAL);
    if (ret == 0) {
        printk(KERN_INFO "MattX:[DRAIN] Injected Task Work into PID %d. Waiting for stable state...\n", task->pid);
        wait_for_completion(&ctx.done);
        printk(KERN_INFO "MattX:[DRAIN] PID %d is now stable and frozen at the user-space boundary!\n", task->pid);
    } else {
        // Fallback just in case the process is exiting or refusing task work
        printk(KERN_WARNING "MattX:[DRAIN] task_work_add failed for PID %d. Falling back to SIGSTOP.\n", task->pid);
        send_sig(SIGSTOP, task, 0);
        
        int retries = 50;
        while (!(READ_ONCE(task->__state) & __TASK_STOPPED) && retries > 0) {
            msleep(10);
            retries--;
        }
    }
}

void mattx_capture_and_send_state(struct task_struct *task, int target_node) {
    struct pt_regs *regs;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct mattx_migration_req *req;
    const struct cred *cred; 
    size_t max_payload_size;
    size_t actual_payload_size;
    int vma_count = 0;
    unsigned char rip_buf[8] = {0}; 

    is_returning = false; 

    max_payload_size = sizeof(struct mattx_migration_req) + (MAX_VMAS * sizeof(struct mattx_vma_info));
    req = kzalloc(max_payload_size, GFP_KERNEL);
    if (!req) return;

    printk(KERN_INFO "MattX:[EXTRACT] Initiating state capture for PID %d (%s)...\n", task->pid, task->comm);

    // Use the Jedi Master Pivot!
    mattx_freeze_task_safely(task);

    req->orig_pid = task->pid;
    
    cred = get_task_cred(task);
    if (cred) {
        req->uid = from_kuid(&init_user_ns, cred->uid);
        req->gid = from_kgid(&init_user_ns, cred->gid);
        put_cred(cred); 
        printk(KERN_INFO "MattX:[EXTRACT] Captured Identity -> UID: %u, GID: %u\n", req->uid, req->gid);
    }

    get_task_comm(req->comm, task);
    printk(KERN_INFO "MattX:[EXTRACT] Captured process name: '%s'\n", req->comm);

    // --- RESTORED: Extract Open File Descriptors ---
    req->fd_count = 0;
    if (task->files) {
        spin_lock(&task->files->file_lock);
        struct fdtable *fdt = files_fdtable(task->files);
        int i;
        for (i = 0; i < fdt->max_fds && req->fd_count < MAX_FDS; i++) {
            if (rcu_dereference_raw(fdt->fd[i]) != NULL) {
                req->open_fds[req->fd_count++] = i;
            }
        }
        spin_unlock(&task->files->file_lock);
        printk(KERN_INFO "MattX:[EXTRACT] Captured %u open File Descriptors.\n", req->fd_count);
    }

    regs = task_pt_regs(task);
    if (regs) {
        memcpy(&req->regs, regs, sizeof(struct pt_regs));
        req->fsbase = task->thread.fsbase;
        req->gsbase = task->thread.gsbase;
        
        if (access_process_vm(task, req->regs.rip, rip_buf, 8, FOLL_FORCE) == 8) {
            printk(KERN_INFO "MattX: [DEBUG] Source RIP (0x%lx) contains: %8ph\n", (unsigned long)req->regs.rip, rip_buf);
        }
    }

    mm = task->mm;
    if (mm) {
        req->arg_start = mm->arg_start;
        req->arg_end = mm->arg_end;

        mmap_read_lock(mm);
        VMA_ITERATOR(vmi, mm, 0);
        for_each_vma(vmi, vma) {
            if (vma_count >= MAX_VMAS) break;
            req->vmas[vma_count].vm_start = vma->vm_start;
            req->vmas[vma_count].vm_end = vma->vm_end;
            req->vmas[vma_count].vm_flags = vma->vm_flags;
            vma_count++;
        }
        mmap_read_unlock(mm);
    }
    
    req->vma_count = vma_count;
    actual_payload_size = sizeof(struct mattx_migration_req) + (vma_count * sizeof(struct mattx_vma_info));

    if (local_migration_req) kfree(local_migration_req);
    local_migration_req = kmemdup(req, actual_payload_size, GFP_KERNEL);
    
    if (migrating_task) put_task_struct(migrating_task);
    get_task_struct(task);
    migrating_task = task;
    migrating_target_node = target_node;

    if (cluster_map[target_node]) {
        printk(KERN_INFO "MattX:[MIGRATE] Sending blueprint to Node %d. Waiting for READY signal...\n", target_node);
        mattx_comm_send(cluster_map[target_node], MATTX_MSG_MIGRATE_REQ, req, actual_payload_size);
    }
    kfree(req);
}

void mattx_capture_and_return_state(struct task_struct *task, u32 orig_pid, int target_node) {
    struct pt_regs *regs;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct mattx_migration_req *req;
    size_t max_payload_size;
    size_t actual_payload_size;
    int vma_count = 0;

    is_returning = true; 

    max_payload_size = sizeof(struct mattx_migration_req) + (MAX_VMAS * sizeof(struct mattx_vma_info));
    req = kzalloc(max_payload_size, GFP_KERNEL);
    if (!req) return;

    printk(KERN_INFO "MattX:[EXTRACT] Initiating RETURN state capture for Surrogate PID %d...\n", task->pid);

    // Use the Jedi Master Pivot!
    mattx_freeze_task_safely(task);

    req->orig_pid = orig_pid;

    regs = task_pt_regs(task);
    if (regs) {
        memcpy(&req->regs, regs, sizeof(struct pt_regs));
    }

    mm = task->mm;
    if (mm) {
        req->arg_start = mm->arg_start;
        req->arg_end = mm->arg_end;

        mmap_read_lock(mm);
        VMA_ITERATOR(vmi, mm, 0);
        for_each_vma(vmi, vma) {
            if (vma_count >= MAX_VMAS) break;
            req->vmas[vma_count].vm_start = vma->vm_start;
            req->vmas[vma_count].vm_end = vma->vm_end;
            req->vmas[vma_count].vm_flags = vma->vm_flags;
            vma_count++;
        }
        mmap_read_unlock(mm);
    }
    
    req->vma_count = vma_count;
    actual_payload_size = sizeof(struct mattx_migration_req) + (vma_count * sizeof(struct mattx_vma_info));

    if (local_migration_req) kfree(local_migration_req);
    local_migration_req = kmemdup(req, actual_payload_size, GFP_KERNEL);
    
    if (migrating_task) put_task_struct(migrating_task);
    get_task_struct(task);
    migrating_task = task;
    migrating_target_node = target_node;

    if (cluster_map[target_node]) {
        printk(KERN_INFO "MattX:[MIGRATE] Sending RETURN blueprint to Node %d. Waiting for READY signal...\n", target_node);
        mattx_comm_send(cluster_map[target_node], MATTX_MSG_RETURN_BLUEPRINT, req, actual_payload_size);
    }
    kfree(req);
}

void mattx_send_vma_data(void) {
    int total_pages = 0;
    int sent_pages = 0;
    int skipped_pages = 0;
    int network_errors = 0;

    if (!local_migration_req || !migrating_task || migrating_target_node == -1) return;
    if (!cluster_map[migrating_target_node]) return;

    printk(KERN_INFO "MattX: [MIGRATE] Starting data pipeline to Node %d...\n", migrating_target_node);

    for (int i = 0; i < local_migration_req->vma_count; i++) {
        unsigned long start = local_migration_req->vmas[i].vm_start;
        unsigned long end = local_migration_req->vmas[i].vm_end;
        unsigned long vma_flags = local_migration_req->vmas[i].vm_flags; // Declared here!
        unsigned long curr = start;
        
        // --- The Smart Return Optimization ---
        // If we are returning to the Home Node, the Deputy already has the Read-Only code!
        // We only need to send back the memory that could have changed (VM_WRITE).
        if (is_returning && !(vma_flags & VM_WRITE)) {
            continue; // Skip this VMA entirely!
        }
        
        while (curr < end) {
            u32 chunk_size = PAGE_SIZE;
            if (curr + chunk_size > end) chunk_size = end - curr;
            total_pages++;

            void *page_buf = kmalloc(chunk_size, GFP_KERNEL);
            if (page_buf) {
                int bytes_read = access_process_vm(migrating_task, curr, page_buf, chunk_size, FOLL_FORCE);
                if (bytes_read > 0) {
                    size_t payload_size = sizeof(struct mattx_page_header) + bytes_read;
                    void *payload_buf = kmalloc(payload_size, GFP_KERNEL);
                    
                    if (payload_buf) {
                        struct mattx_page_header *p_page_hdr = (struct mattx_page_header *)payload_buf;
                        
                        p_page_hdr->vma_index = i;
                        p_page_hdr->offset = curr - start;
                        p_page_hdr->length = bytes_read;

                        memcpy(payload_buf + sizeof(struct mattx_page_header), page_buf, bytes_read);
                        
                        int send_res = mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_PAGE_TRANSFER, payload_buf, payload_size);
                        if (send_res < payload_size) {
                            network_errors++;
                        } else {
                            sent_pages++;
                        }
                        
                        kfree(payload_buf);
                    }
                } else {
                    skipped_pages++;
                }
                kfree(page_buf);
            }
            curr += chunk_size;
        }
    }
    
    printk(KERN_INFO "MattX:[MIGRATE] Pipeline stats: %d total, %d sent, %d skipped, %d net errors\n", 
           total_pages, sent_pages, skipped_pages, network_errors);
           
    if (is_returning) {
        printk(KERN_INFO "MattX:[MIGRATE] Return pipeline complete. Sending RETURN_DONE signal.\n");
        mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_RETURN_DONE, NULL, 0);
        
        printk(KERN_INFO "MattX:[RECALL] Executing local Surrogate PID %d...\n", migrating_task->pid);
        send_sig(SIGKILL, migrating_task, 0);
        
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == migrating_task->pid) {
                remove_guest_process(i);
                break;
            }
        }
        spin_unlock(&guest_lock);
    } else {
        printk(KERN_INFO "MattX:[MIGRATE] Data pipeline complete. Sending DONE signal.\n");
        mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_MIGRATE_DONE, NULL, 0);
        
        add_export_process(migrating_task->pid, migrating_target_node);
        printk(KERN_INFO "MattX:[REGISTRY] PID %d registered as Exported to Node %d.\n", migrating_task->pid, migrating_target_node);
    }
    
    put_task_struct(migrating_task);
    migrating_task = NULL;
    kfree(local_migration_req);
    local_migration_req = NULL;
}

void mattx_trigger_recall(pid_t orig_pid) {
    int target_node = get_export_target(orig_pid);
    struct mattx_recall_req req;

    if (target_node == -1) {
        printk(KERN_WARNING "MattX: [RECALL] PID %d is not in the export registry. Cannot recall.\n", orig_pid);
        return;
    }

    if (!cluster_map[target_node]) {
        printk(KERN_ERR "MattX:[RECALL] Target Node %d is disconnected. Cannot recall PID %d.\n", target_node, orig_pid);
        return;
    }

    req.orig_pid = orig_pid;

    printk(KERN_INFO "MattX: [RECALL] Sending RECALL_REQ for PID %d to Node %d...\n", orig_pid, target_node);
    mattx_comm_send(cluster_map[target_node], MATTX_MSG_RECALL_REQ, &req, sizeof(req));
}

static void handle_ready_for_data(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    printk(KERN_INFO "MattX:[EXPORT] Received READY signal from Node %u. Starting data pump...\n", hdr->sender_id);
    mattx_send_vma_data();
}

static void handle_recall_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_recall_req *req = (struct mattx_recall_req *)payload;
        pid_t local_stub_pid = -1;
        int i;

        printk(KERN_INFO "MattX:[EXPORT] Received RECALL request for Orig PID %u from Node %u\n", 
               req->orig_pid, hdr->sender_id);
        
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == req->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                local_stub_pid = guest_registry[i].local_pid;
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
                printk(KERN_INFO "MattX:[EXPORT] Found Surrogate PID %d. Capturing state...\n", surrogate->pid);
                mattx_capture_and_return_state(surrogate, req->orig_pid, hdr->sender_id);
                put_task_struct(surrogate);
            } else {
                printk(KERN_WARNING "MattX:[EXPORT] Surrogate PID %d not found!\n", local_stub_pid);
            }
        } else {
            printk(KERN_WARNING "MattX:[EXPORT] Could not find guest registry entry for Orig PID %u\n", req->orig_pid);
        }
    }
}

void mattx_migr_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_READY_FOR_DATA, handle_ready_for_data);
    mattx_register_handler(MATTX_MSG_RECALL_REQ, handle_recall_req);
    printk(KERN_INFO "MattX: [EXPORT] Network handlers registered.\n");
}

