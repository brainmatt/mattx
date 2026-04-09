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
    unsigned char rip_buf[8] = {0}; // For the Hex Dump

    max_payload_size = sizeof(struct mattx_migration_req) + (MAX_VMAS * sizeof(struct mattx_vma_info));
    req = kzalloc(max_payload_size, GFP_KERNEL);
    if (!req) return;

    send_sig(SIGSTOP, task, 0);
    
    while (!(READ_ONCE(task->__state) & __TASK_STOPPED) && retries > 0) {
        msleep(10);
        retries--;
    }

    req->orig_pid = task->pid;

    regs = task_pt_regs(task);
    if (regs) {
        memcpy(&req->regs, regs, sizeof(struct pt_regs));
        
        // --- NEW: Source Hex Dump ---
        if (access_process_vm(task, req->regs.ip, rip_buf, 8, 0) == 8) {
            printk(KERN_INFO "MattX: [DEBUG] Source RIP (0x%lx) contains: %8ph\n", req->regs.ip, rip_buf);
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
                // --- NEW: Handle partial reads ---
                int bytes_read = access_process_vm(migrating_task, curr, page_buf, chunk_size, 0);
                if (bytes_read > 0) {
                    size_t packet_size = sizeof(struct mattx_header) + sizeof(struct mattx_page_header) + bytes_read;
                    void *packet_buf = kmalloc(packet_size, GFP_KERNEL);
                    
                    if (packet_buf) {
                        struct mattx_header *p_hdr = (struct mattx_header *)packet_buf;
                        struct mattx_page_header *p_page_hdr = (struct mattx_page_header *)(packet_buf + sizeof(struct mattx_header));
                        
                        p_hdr->type = MATTX_MSG_PAGE_TRANSFER;
                        p_hdr->length = sizeof(struct mattx_page_header) + bytes_read;
                        p_hdr->sender_id = migrating_task->pid;

                        p_page_hdr->vma_index = i;
                        p_page_hdr->offset = curr - start;
                        p_page_hdr->length = bytes_read;

                        memcpy(packet_buf + sizeof(struct mattx_header) + sizeof(struct mattx_page_header), page_buf, bytes_read);
                        mattx_comm_send(cluster_map[migrating_target_node], MATTX_MSG_PAGE_TRANSFER, packet_buf, packet_size);
                        kfree(packet_buf);
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

