#include "mattx.h"

static struct mattx_migration_req *local_migration_req = NULL;
static struct task_struct *migrating_task = NULL;
static int migrating_target_node = -1;

void mattx_capture_and_send_state(struct task_struct *task, int target_node) {
    struct pt_regs *regs;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct mattx_migration_req *req;
    const struct cred *cred; 
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
        printk(KERN_WARNING "MattX:[EXTRACT] Warning: Task %d took too long to stop!\n", task->pid);
    }

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
        unsigned long curr = start;
        
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
    printk(KERN_INFO "MattX:[MIGRATE] Data pipeline complete. Sending DONE signal.\n");
    
    mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_MIGRATE_DONE, NULL, 0);
    
    // --- NEW: Add to Export Registry ---
    add_export_process(migrating_task->pid, migrating_target_node);
    printk(KERN_INFO "MattX:[REGISTRY] PID %d registered as Exported to Node %d.\n", migrating_task->pid, migrating_target_node);
    
    put_task_struct(migrating_task);
    migrating_task = NULL;
    kfree(local_migration_req);
    local_migration_req = NULL;
}


