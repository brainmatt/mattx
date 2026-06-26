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

// Local state for the import pipeline
static int injected_pages_count = 0;
static char *stub_argv[] = { "/usr/local/bin/mattx-stub", NULL };
static char *stub_envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };

static void handle_migrate_req(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_migration_req *req = (struct mattx_migration_req *)payload;
        
        pending_source_node = hdr->sender_id;
        injected_pages_count = 0;

        mattx_dbg("[IMPORT] Received Blueprint for PID %u. Saving to pending...\n", req->orig_pid);
        if (pending_migration) kvfree(pending_migration);
        
        // FIX: Use kvmalloc to prevent fragmentation failures!
        pending_migration = kvmalloc(hdr->length, GFP_KERNEL);
        if (pending_migration) {
            memcpy(pending_migration, req, hdr->length);
        } else {
            printk(KERN_ERR "MattX:[IMPORT] FATAL: Failed to allocate memory for blueprint!\n");
            return;
        }

        if (call_usermodehelper(stub_argv[0], stub_argv, stub_envp, UMH_NO_WAIT) != 0) {
            printk(KERN_ERR "MattX:[IMPORT] Failed to spawn surrogate!\n");
        }
    }
}


static void handle_page_transfer(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload && pending_migration && hijacked_stub_task) {
        struct mattx_page_header *ph = (struct mattx_page_header *)payload;
        void *data = (char *)payload + sizeof(struct mattx_page_header);
        unsigned long target_addr = ph->absolute_addr;
        int res;
        struct mm_struct *mm = hijacked_stub_task->mm;
        bool unprotect = false;

        // 1. Temporarily unprotect the memory (W^X Bypass)
        if (mm) {
            mmap_write_lock(mm);
            struct vm_area_struct *vma = find_vma(mm, target_addr);
            if (vma && target_addr >= vma->vm_start) {
                if (!(vma->vm_flags & VM_WRITE)) {
                    vm_flags_set(vma, vma->vm_flags | VM_WRITE);
                    unprotect = true;
                }
            }
            mmap_write_unlock(mm); // DROP THE LOCK SO PAGE FAULTS CAN HAPPEN!
        }

        // 2. Inject the memory! (Page faults can now safely trigger and allocate RAM)
        res = access_process_vm(hijacked_stub_task, target_addr, data, ph->length, FOLL_WRITE | FOLL_FORCE);
        
        // 3. Lock the memory back up!
        if (unprotect && mm) {
            mmap_write_lock(mm);
            struct vm_area_struct *vma = find_vma(mm, target_addr);
            if (vma && target_addr >= vma->vm_start) {
                vm_flags_clear(vma, VM_WRITE);
            }
            mmap_write_unlock(mm);
        }

        if (res != ph->length) {
            printk_ratelimited(KERN_WARNING "MattX:[IMPORT] ERROR: Failed to inject %u bytes at 0x%lx (res: %d)\n", 
                               ph->length, target_addr, res);
        } else {
            injected_pages_count++;
        }
    }
}


static void handle_migrate_done(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    mattx_dbg("[IMPORT] All memory transferred! Total pages injected: %d\n", injected_pages_count);
    
    if (hijacked_stub_task && pending_migration) {
        struct pt_regs *regs;
        struct cred *new_cred;
        const struct cred *old_cred;
        int retries = 50;
        unsigned char rip_buf[8] = {0}; 
        struct file **fake_files; 
        int i;

        mattx_dbg("[IMPORT] Commencing full brain transplant on PID %d...\n", hijacked_stub_task->pid);

        while (!(READ_ONCE(hijacked_stub_task->__state) & __TASK_STOPPED) && retries > 0) {
            msleep(10);
            retries--;
        }

        regs = task_pt_regs(hijacked_stub_task);
        if (regs) {
            memcpy(regs, &pending_migration->regs, sizeof(struct pt_regs));
            
            hijacked_stub_task->thread.fsbase = pending_migration->fsbase;
            hijacked_stub_task->thread.gsbase = pending_migration->gsbase;
            
            strscpy(hijacked_stub_task->comm, pending_migration->comm, sizeof(hijacked_stub_task->comm));
            
            if (hijacked_stub_task->mm) {
                hijacked_stub_task->mm->arg_start = pending_migration->arg_start;
                hijacked_stub_task->mm->arg_end = pending_migration->arg_end;
            }
            
            new_cred = prepare_creds();
            if (new_cred) {
                new_cred->uid = make_kuid(&init_user_ns, pending_migration->uid);
                new_cred->euid = new_cred->uid;
                new_cred->suid = new_cred->uid;
                new_cred->fsuid = new_cred->uid;
                
                new_cred->gid = make_kgid(&init_user_ns, pending_migration->gid);
                new_cred->egid = new_cred->gid;
                new_cred->sgid = new_cred->gid;
                new_cred->fsgid = new_cred->gid;

                rcu_read_lock();
                old_cred = rcu_dereference(hijacked_stub_task->cred);
                rcu_assign_pointer(hijacked_stub_task->real_cred, get_cred(new_cred));
                rcu_assign_pointer(hijacked_stub_task->cred, get_cred(new_cred));
                rcu_read_unlock();

                put_cred(old_cred);
                put_cred(old_cred);
                put_cred(new_cred);
            }

            fake_files = kmalloc_array(pending_migration->fd_count, sizeof(struct file *), GFP_KERNEL);
            if (fake_files) {
                memset(fake_files, 0, pending_migration->fd_count * sizeof(struct file *));
                
                for (i = 0; i < pending_migration->fd_count; i++) {
                    u32 fd_num = pending_migration->open_fds[i];
                    
                    struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_KERNEL);
                    if (fd_info) {
                        fd_info->home_node = pending_source_node;
                        fd_info->orig_pid = pending_migration->orig_pid;
                        fd_info->remote_fd = fd_num;
                        fake_files[i] = anon_inode_getfile("mattx_vfs_proxy", &mattx_fops, fd_info, O_WRONLY);
                    }
                }

                if (hijacked_stub_task->files) {
                    spin_lock(&hijacked_stub_task->files->file_lock);
                    struct fdtable *fdt = files_fdtable(hijacked_stub_task->files);
                    
                    for (i = 0; i < pending_migration->fd_count; i++) {
                        u32 fd_num = pending_migration->open_fds[i];
                        if (fd_num < fdt->max_fds && fake_files[i] && !IS_ERR(fake_files[i])) {
                            rcu_assign_pointer(fdt->fd[fd_num], fake_files[i]);
                            __set_bit(fd_num, fdt->open_fds);                        
                        }
                    }
                    spin_unlock(&hijacked_stub_task->files->file_lock);
                    mattx_dbg("[IMPORT] Successfully injected %u Fake FDs!\n", pending_migration->fd_count);
                }
                kfree(fake_files); 
            }

            if (access_process_vm(hijacked_stub_task, regs->ip, rip_buf, 8, FOLL_FORCE) == 8) {
                mattx_dbg("[DEBUG] Target RIP (0x%lx) contains: %8ph\n", regs->ip, rip_buf);
            }

            mattx_dbg("[IMPORT] IT'S ALIVE! Sending SIGCONT to PID %d\n", hijacked_stub_task->pid);
            send_sig(SIGCONT, hijacked_stub_task, 0);
            
            add_guest_process(hijacked_stub_task->pid, pending_migration->orig_pid, pending_source_node);
        }

        put_task_struct(hijacked_stub_task);
        hijacked_stub_task = NULL;
        kvfree(pending_migration);
        pending_migration = NULL;
        pending_source_node = -1;
    }
}

static void handle_return_blueprint(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_migration_req *req = (struct mattx_migration_req *)payload;
        struct task_struct *deputy = NULL;

        mattx_dbg("[IMPORT] Received RETURN Blueprint for Deputy PID %u. Saving to pending...\n", req->orig_pid);
        
        pending_source_node = hdr->sender_id;
        injected_pages_count = 0;

        if (pending_migration) kvfree(pending_migration);
        
        pending_migration = kvmalloc(hdr->length, GFP_KERNEL);
        if (pending_migration) {
            memcpy(pending_migration, req, hdr->length);
        } else {
            printk(KERN_ERR "MattX: [RECALL] FATAL: Failed to allocate memory for return blueprint!\n");
            return;
        }
        
        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            mattx_dbg("[RECALL] Found frozen Deputy PID %d. Preparing for injection...\n", deputy->pid);
            
            // --- THE KWORKER KILL-SWITCH ---
            spin_lock(&export_lock);
            for (int i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == req->orig_pid) {
                    export_registry[i].abort_rpc = true;
                    break;
                }
            }
            spin_unlock(&export_lock);

            // --- THE DYNAMIC BRAIN CARVER (Deadlock-Free Edition) ---
            // The Surrogate may have allocated NEW memory while running on VM2!
            // We must ensure the Deputy has these VMAs mapped before we inject data.
            if (deputy->mm) {
                for (int i = 0; i < req->vma_count; i++) {
                    unsigned long start = req->vmas[i].vm_start;
                    unsigned long size = req->vmas[i].vm_end - start;
                    unsigned long flags = req->vmas[i].vm_flags;

                    // 1. Take the READ lock to check if the VMA exists
                    mmap_read_lock(deputy->mm);
                    struct vm_area_struct *vma = find_vma(deputy->mm, start);
                    
                    // ONLY carve if there is literally no memory mapped at this starting address!
                    // If vma->vm_start > start, it means there is a hole in the memory map.
                    bool needs_mapping = (!vma || vma->vm_start > start);
                    mmap_read_unlock(deputy->mm); // DROP THE LOCK!

                    
                    // 2. If it's missing, carve it out safely!
                    if (needs_mapping) {
                        mattx_dbg("[RECALL] Carving NEW memory for Deputy: 0x%lx (Size: %lu)\n", start, size);
                        
                        // We must temporarily adopt the Deputy's memory context to call vm_mmap
                        kthread_use_mm(deputy->mm);
                        
                        unsigned long prot = PROT_READ | PROT_WRITE | PROT_EXEC;
                        unsigned long map_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
                        
                        if (flags & 0x0100) map_flags |= MAP_GROWSDOWN; // Stack protector
                        
                        // vm_mmap handles its own locking internally!
                        unsigned long ret = vm_mmap(NULL, start, size, prot, map_flags, 0);
                        if (IS_ERR_VALUE(ret)) {
                            printk(KERN_ERR "MattX:[RECALL] FATAL: Failed to carve memory at 0x%lx (err: %ld)\n", start, ret);
                        }
                        
                        kthread_unuse_mm(deputy->mm);
                    }
                }
            }

            if (hijacked_stub_task) put_task_struct(hijacked_stub_task);
            hijacked_stub_task = deputy;

            mattx_dbg(" [RECALL] Sending READY_FOR_DATA signal to Node %d...\n", pending_source_node);
            mattx_comm_send(cluster_map[pending_source_node], MATTX_MSG_READY_FOR_DATA, NULL, 0);
        } else {
            printk(KERN_ERR "MattX: [RECALL] ERROR: Deputy PID %u not found!\n", req->orig_pid);
        }
    }
}


static void handle_return_done(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    mattx_dbg("[IMPORT] Return memory transferred successfully! Pages: %d\n", injected_pages_count);
    
    if (hijacked_stub_task && pending_migration) {
        struct pt_regs *regs = task_pt_regs(hijacked_stub_task);
        int i;

        if (regs) {
            memcpy(regs, &pending_migration->regs, sizeof(struct pt_regs));
            
            mattx_dbg("[IMPORT] Deputy Brain Restored. New RIP: 0x%lx\n", regs->ip);

            spin_lock(&export_lock);
            for (i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == hijacked_stub_task->pid) {
                    remove_export_process(i);
                    break;
                }
            }
            spin_unlock(&export_lock);

            mattx_dbg("[IMPORT] Welcome home! Sending SIGCONT to Deputy PID %d\n", hijacked_stub_task->pid);
            send_sig(SIGCONT, hijacked_stub_task, 0);
        }
        
        put_task_struct(hijacked_stub_task);
        hijacked_stub_task = NULL;
        kvfree(pending_migration);
        pending_migration = NULL;
        pending_source_node = -1;
    } else {
        printk(KERN_ERR "MattX: [RECALL] FATAL: Missing stub task or blueprint in return_done!\n");
    }
}

void mattx_import_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_MIGRATE_REQ, handle_migrate_req);
    mattx_register_handler(MATTX_MSG_PAGE_TRANSFER, handle_page_transfer);
    mattx_register_handler(MATTX_MSG_MIGRATE_DONE, handle_migrate_done);
    mattx_register_handler(MATTX_MSG_RETURN_BLUEPRINT, handle_return_blueprint);
    mattx_register_handler(MATTX_MSG_RETURN_DONE, handle_return_done);
    mattx_dbg(" [IMPORT] Network handlers registered.\n");
}

