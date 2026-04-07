#include "mattx.h"

void mattx_capture_and_send_state(struct task_struct *task, int target_node) {
    struct pt_regs *regs;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct mattx_migration_req *req;
    size_t max_payload_size;
    size_t actual_payload_size;
    int vma_count = 0;

    max_payload_size = sizeof(struct mattx_migration_req) + (MAX_VMAS * sizeof(struct mattx_vma_info));
    req = kzalloc(max_payload_size, GFP_KERNEL);
    if (!req) return;

    send_sig(SIGSTOP, task, 0);
    req->orig_pid = task->pid;

    regs = task_pt_regs(task);
    if (regs) {
        req->rip = regs->ip;
        req->rsp = regs->sp;
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

    if (cluster_map[target_node]) {
        mattx_comm_send(cluster_map[target_node], MATTX_MSG_MIGRATE_REQ, req, actual_payload_size);

        for (int i = 0; i < vma_count; i++) {
            unsigned long start = req->vmas[i].vm_start;
            unsigned long end = req->vmas[i].vm_end;
            unsigned long curr = start;
            
            while (curr < end) {
                u32 chunk_size = PAGE_SIZE;
                if (curr + chunk_size > end) chunk_size = end - curr;

                void *page_buf = kmalloc(chunk_size, GFP_KERNEL);
                if (page_buf) {
                    if (access_process_vm(task, curr, page_buf, chunk_size, 0) == 0) {
                        size_t packet_size = sizeof(struct mattx_header) + sizeof(struct mattx_page_header) + chunk_size;
                        void *packet_buf = kmalloc(packet_size, GFP_KERNEL);
                        
                        if (packet_buf) {
                            struct mattx_header *p_hdr = (struct mattx_header *)packet_buf;
                            struct mattx_page_header *p_page_hdr = (struct mattx_page_header *)(packet_buf + sizeof(struct mattx_header));
                            
                            p_hdr->type = MATTX_MSG_PAGE_TRANSFER;
                            p_hdr->length = sizeof(struct mattx_page_header) + chunk_size;
                            p_hdr->sender_id = task->pid;

                            p_page_hdr->vma_index = i;
                            p_page_hdr->offset = curr - start;
                            p_page_hdr->length = chunk_size;

                            memcpy(packet_buf + sizeof(struct mattx_header) + sizeof(struct mattx_page_header), page_buf, chunk_size);
                            mattx_comm_send(cluster_map[target_node], MATTX_MSG_PAGE_TRANSFER, packet_buf, packet_size);
                            kfree(packet_buf);
                        }
                    }
                    kfree(page_buf);
                }
                curr += chunk_size;
            }
        }
    }
    kfree(req);
}

int mattx_inject_vma_data(struct mm_struct *mm, struct mattx_vma_info *vma_info) {
    unsigned long addr = vma_info->vm_start;
    unsigned long end = vma_info->vm_end;
    struct vm_area_struct *vma;
    
    printk(KERN_INFO "MattX: [INJECT] Mapping VMA 0x%lx - 0x%lx\n", addr, end);

    while (addr < end) {
        struct page *page = alloc_page(GFP_KERNEL);
        if (!page) return -ENOMEM;

        vma = find_vma(mm, addr);
        if (!vma) {
            __free_page(page);
            return -EFAULT;
        }

        if (vm_insert_page(vma, addr, page) < 0) {
            __free_page(page);
            return -EFAULT;
        }
        addr += PAGE_SIZE;
    }
    return 0;
}

