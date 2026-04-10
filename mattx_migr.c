#include "mattx.h"

static struct mattx_migration_req *local_migration_req = NULL;
static struct task_struct *migrating_task = NULL;
static int migrating_target_node = -1;

void mattx_capture_and_send_state(struct task_struct *task, int target_node) {
    struct pt_regs *regs;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct mattx_migration_req *req;
    size_t max_payload_size;
    size_t actual_payload_size;
    int vma_count = 0;
    int retries = 50;
    unsigned char rip_buf[8] = {0}; 

    max_payload_size = sizeof(struct mattx_migration_req) + (MAX_VMAS * sizeof(struct mattx_vma_info));
    req = kzalloc(max_payload_size, GFP_KERNEL);
    if (!req) return;

    printk(KERN_INFO "MattX:[EXTRACT] Initiating state capture for PID %d (%s)...\n", task->pid, task->comm);

    send_sig(SIGSTOP, task, 0);
    
    while (!(READ_ONCE(task->__state) & __TASK_STOPPED) && retries > 0) {
        msleep(10);
        retries--;
    }
    if (retries == 0) {
        printk(KERN_WARNING "MattX: [EXTRACT] Warning: Task %d took too long to stop!\n", task->pid);
    }

    req->orig_pid = task->pid;

    regs = task_pt_regs(task);
    if (regs) {
        memcpy(&req->regs, regs, sizeof(struct pt_regs));
        
        // Extract the TLS Bases (The Soul)
        req->fsbase = task->thread.fsbase;
        req->gsbase = task->thread.gsbase;
        
        if (access_process_vm(task, req->regs.rip, rip_buf, 8, FOLL_FORCE) == 8) {
            printk(KERN_INFO "MattX: [DEBUG] Source RIP (0x%lx) contains: %8ph\n", (unsigned long)req->regs.rip, rip_buf);
        } else {
            printk(KERN_WARNING "MattX: [DEBUG] Failed to read Source RIP!\n");
        }
    }

    mm = task->mm;
    if (mm) {
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

void mattx_send_vma_data(void) {
    if (!local_migration_req || !migrating_task || migrating_target_node == -1) return;
    if (!cluster_map[migrating_target_node]) return;

    printk(KERN_INFO "MattX: [MIGRATE] Starting data pipeline to Node %d...\n", migrating_target_node);

    for (int i = 0; i < local_migration_req->vma_count; i++) {
        unsigned long start = local_migration_req->vmas[i].vm_start;
        unsigned long end = local_migration_req->vmas[i].vm_end;
        unsigned long curr = start;
        
        while (curr < end) {
            u32 chunk_size = PAGE_SIZE;
            if (curr + chunk_size > end) chunk_size = end - curr;

            void *page_buf = kmalloc(chunk_size, GFP_KERNEL);
            if (page_buf) {
                int bytes_read = access_process_vm(migrating_task, curr, page_buf, chunk_size, FOLL_FORCE);
                if (bytes_read > 0) {
                    
                    // FIXED: Only allocate space for the Page Header + Data. 
                    // mattx_comm_send will prepend the main header!
                    size_t payload_size = sizeof(struct mattx_page_header) + bytes_read;
                    void *payload_buf = kmalloc(payload_size, GFP_KERNEL);
                    
                    if (payload_buf) {
                        struct mattx_page_header *p_page_hdr = (struct mattx_page_header *)payload_buf;
                        
                        p_page_hdr->vma_index = i;
                        p_page_hdr->offset = curr - start;
                        p_page_hdr->length = bytes_read;

                        // Copy the memory data right after the page header
                        memcpy(payload_buf + sizeof(struct mattx_page_header), page_buf, bytes_read);
                        
                        // Send it!
                        mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_PAGE_TRANSFER, payload_buf, payload_size);
                        kfree(payload_buf);
                    }
                }
                kfree(page_buf);
            }
            curr += chunk_size;
        }
    }
    
    printk(KERN_INFO "MattX:[MIGRATE] Data pipeline complete. Sending DONE signal.\n");
    mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_MIGRATE_DONE, NULL, 0);
    
    put_task_struct(migrating_task);
    migrating_task = NULL;
    kfree(local_migration_req);
    local_migration_req = NULL;
}

