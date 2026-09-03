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
        
        // --- NEW: Use the absolute address directly! No more VMA index math! ---
        unsigned long target_addr = ph->absolute_addr;
        
        int res;
        struct mm_struct *mm = hijacked_stub_task->mm;
        bool unprotect = false;

        // --- THE W^X SECURITY BYPASS ---
        // Kernel 7.0 refuses to write to Read-Only/Executable memory.
        // We must temporarily force the VMA to be writable!
        if (mm) {
            mmap_write_lock(mm);
            struct vm_area_struct *vma = find_vma(mm, target_addr);
            if (vma && target_addr >= vma->vm_start) {
                if (!(vma->vm_flags & VM_WRITE)) {
                    vm_flags_set(vma, vma->vm_flags | VM_WRITE);
                    unprotect = true;
                }
            }
            mmap_write_unlock(mm);
        }

        // Inject the memory!
        res = access_process_vm(hijacked_stub_task, target_addr, data, ph->length, FOLL_WRITE | FOLL_FORCE);
        
        // Lock the memory back up!
        if (unprotect && mm) {
            mmap_write_lock(mm);
            struct vm_area_struct *vma = find_vma(mm, target_addr);
            if (vma && target_addr >= vma->vm_start) {
                vm_flags_clear(vma, VM_WRITE);
            }
            mmap_write_unlock(mm);
        }

        if (res != ph->length) {
            // Use ratelimited printing to prevent dmesg from freezing the TCP receiver!
            printk_ratelimited(KERN_WARNING "MattX:[IMPORT] ERROR: Failed to inject %u bytes at 0x%lx (res: %d)\n", 
                               ph->length, target_addr, res);
        } else {
            injected_pages_count++;
        }
    }
}


// ==============================================================================
// 🛸 THE vDSO TRANSPLANT (Memory Remapper)
// ==============================================================================
struct mattx_vdso_transplant_ctx {
    struct callback_head cb;
    struct completion done;
    unsigned long old_vdso;
    unsigned long new_vdso;
    unsigned long vdso_size;
};

static void mattx_vdso_transplant_cb(struct callback_head *cb) {
    struct mattx_vdso_transplant_ctx *ctx = container_of(cb, struct mattx_vdso_transplant_ctx, cb);
    struct pt_regs regs;
    
    mattx_dbg("[TRANSPLANT] Moving vDSO from 0x%lx to 0x%lx (Size: %lu)...\n", ctx->old_vdso, ctx->new_vdso, ctx->vdso_size);
    
    memset(&regs, 0, sizeof(regs));
    // sys_mremap(old_addr, old_size, new_size, flags, new_addr)
    regs.di = ctx->old_vdso;
    regs.si = ctx->vdso_size;
    regs.dx = ctx->vdso_size;
    regs.r10 = MREMAP_MAYMOVE | MREMAP_FIXED; // Force it to the exact address!
    regs.r8 = ctx->new_vdso;
    
    if (real_sys_mremap) {
        long ret = real_sys_mremap(&regs);
        if (ret == ctx->new_vdso) {
            current->mm->context.vdso = (void *)ctx->new_vdso;
            mattx_dbg("[TRANSPLANT] vDSO successfully moved!\n");
        } else {
            printk(KERN_ERR "MattX:[TRANSPLANT] Failed to move vDSO! (ret: %ld)\n", ret);
        }
    }
    
    complete(&ctx->done);
}
// ==============================================================================


static void handle_migrate_done(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    mattx_dbg("[IMPORT] All memory transferred! Total pages injected: %d\n", injected_pages_count);
    
    if (hijacked_stub_task && pending_migration) {
        struct cred *new_cred;
        const struct cred *old_cred;
        int retries = 50;
        unsigned char rip_buf[8] = {0}; 
        struct file **fake_files; 
        int i;
        int current_threads = 0;
        struct task_struct *t;

        mattx_dbg("[IMPORT] Commencing full brain transplant on PID %d...\n", hijacked_stub_task->pid);

        // Wait for the Mother to stop
        while (!(READ_ONCE(hijacked_stub_task->__state) & __TASK_STOPPED) && retries > 0) {
            msleep(10);
            retries--;
        }

        // --- WAIT FOR STUB TO SPAWN THREADS ---
        retries = 50;
        while (retries > 0) {
            current_threads = 0;
            rcu_read_lock();
            for_each_thread(hijacked_stub_task, t) { current_threads++; }
            rcu_read_unlock();
            
            if (current_threads >= pending_migration->thread_count) break;
            msleep(10);
            retries--;
        }

        // --- THE vDSO TRANSPLANT ---
        // disabled vDSO transplant for testing
        if (false) {
        // if (hijacked_stub_task->mm && hijacked_stub_task->mm->context.vdso != (void *)pending_migration->vdso_addr) {
            unsigned long old_vdso = (unsigned long)hijacked_stub_task->mm->context.vdso;
            unsigned long new_vdso = pending_migration->vdso_addr;
            unsigned long vdso_size = PAGE_SIZE; // Fallback
            
            mmap_read_lock(hijacked_stub_task->mm);
            struct vm_area_struct *vma = find_vma(hijacked_stub_task->mm, old_vdso);
            if (vma && vma->vm_start <= old_vdso && vma->vm_end > old_vdso) {
                vdso_size = vma->vm_end - vma->vm_start;
            }
            mmap_read_unlock(hijacked_stub_task->mm);
            
            struct mattx_vdso_transplant_ctx *vdso_ctx = kmalloc(sizeof(*vdso_ctx), GFP_KERNEL);
            if (vdso_ctx) {
                vdso_ctx->old_vdso = old_vdso;
                vdso_ctx->new_vdso = new_vdso;
                vdso_ctx->vdso_size = vdso_size;
                init_completion(&vdso_ctx->done);
                init_task_work(&vdso_ctx->cb, mattx_vdso_transplant_cb);
                
                if (real_task_work_add) {
                    real_task_work_add(hijacked_stub_task, &vdso_ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, hijacked_stub_task, 0);
                    wait_for_completion(&vdso_ctx->done);
                    
                    // --- THE TRUE FREEZE ---
                    send_sig(SIGSTOP, hijacked_stub_task, 0);
                    mattx_dbg("[TRANSPLANT] vDSO Transplant finished. Surrogate is freezing...\n");
                    
                    // Ironclad Verification: Wait until the CPU confirms the task is unconscious!
                    int retries = 500; // 5 seconds max
                    while (!(READ_ONCE(hijacked_stub_task->__state) & __TASK_STOPPED) && retries > 0) {
                        msleep(10);
                        retries--;
                    }
                }

                kfree(vdso_ctx);
            }
        }        

        // --- GANG INJECTION ---
        int t_idx = 0;
        rcu_read_lock();
        for_each_thread(hijacked_stub_task, t) {
            if (t_idx < pending_migration->thread_count) {
                struct pt_regs *t_regs = task_pt_regs(t);
                if (t_regs) {
                    memcpy(t_regs, &pending_migration->threads[t_idx].regs, sizeof(struct pt_regs));
                    t->thread.fsbase = pending_migration->threads[t_idx].fsbase;
                    t->thread.gsbase = pending_migration->threads[t_idx].gsbase;
                    
                    // TLS Hardware Sync! ---
                    if (real_x86_fsbase_write_task) real_x86_fsbase_write_task(t, t->thread.fsbase);
                    if (real_x86_gsbase_write_task) real_x86_gsbase_write_task(t, t->thread.gsbase);

                    t->clear_child_tid = (int __user *)pending_migration->threads[t_idx].clear_child_tid;
                    t->set_child_tid   = (int __user *)pending_migration->threads[t_idx].set_child_tid;

                    // Restore FPU/SSE/AVX state! See the matching capture
                    // side in mattx_migr.c for why this is needed. t is
                    // guaranteed frozen here (stopped, about to be
                    // SIGCONT'd below), so its FPU context is safely
                    // resident in memory, not live in hardware registers.
                    if (t->thread.fpu.fpstate && pending_migration->threads[t_idx].fpu_size > 0) {
                        u32 fsize = pending_migration->threads[t_idx].fpu_size;
                        if (fsize > t->thread.fpu.fpstate->size)
                            fsize = t->thread.fpu.fpstate->size;
                        if (fsize > sizeof(pending_migration->threads[t_idx].fpu_state))
                            fsize = sizeof(pending_migration->threads[t_idx].fpu_state);
                        memcpy(&t->thread.fpu.fpstate->regs, pending_migration->threads[t_idx].fpu_state, fsize);
                    }
                }

                // Child Comm Fix! ---
                strscpy(t->comm, pending_migration->comm, sizeof(t->comm));

                t_idx++;
            }
        }
        rcu_read_unlock();

        if (hijacked_stub_task->mm) {
            hijacked_stub_task->mm->arg_start = pending_migration->arg_start;
            hijacked_stub_task->mm->arg_end = pending_migration->arg_end;
            hijacked_stub_task->mm->start_brk = pending_migration->start_brk;
            hijacked_stub_task->mm->brk = pending_migration->brk;
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

        // --- FIXED: Use the Mother's registers from the Blueprint! ---
        if (pending_migration->thread_count > 0) {
            unsigned long mother_rip = pending_migration->threads[0].regs.rip;
            if (access_process_vm(hijacked_stub_task, mother_rip, rip_buf, 8, FOLL_FORCE) == 8) {
                mattx_dbg("[DEBUG] Target RIP (0x%lx) contains: %8ph\n", mother_rip, rip_buf);
            }
        }

        mattx_dbg("[IMPORT] IT'S ALIVE! Waking %d threads in Gang PID %d\n", current_threads, hijacked_stub_task->pid);
        rcu_read_lock();
        for_each_thread(hijacked_stub_task, t) {
            send_sig(SIGCONT, t, 0);
        }
        rcu_read_unlock();
        
        add_guest_process(hijacked_stub_task->pid, pending_migration->orig_pid, pending_source_node);

        // Populate the TID Translation Map ---
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == hijacked_stub_task->pid) {
                guest_registry[i].thread_count = pending_migration->thread_count;
                int map_idx = 0;
                struct task_struct *t_map;
                rcu_read_lock();
                for_each_thread(hijacked_stub_task, t_map) {
                    if (map_idx < pending_migration->thread_count) {
                        guest_registry[i].orig_tids[map_idx] = pending_migration->threads[map_idx].tid;
                        guest_registry[i].local_tids[map_idx] = t_map->pid;
                        map_idx++;
                    }
                }
                rcu_read_unlock();

                // Populate the DSM Translation Map ---
                guest_registry[i].dsm_count = 0;
                for (int v = 0; v < pending_migration->vma_count; v++) {
                    if (pending_migration->vmas[v].is_shm && guest_registry[i].dsm_count < MAX_DSM_SEGMENTS) {
                        int d_idx = guest_registry[i].dsm_count++;
                        guest_registry[i].dsm_map[d_idx].base_addr = pending_migration->vmas[v].vm_start;
                        guest_registry[i].dsm_map[d_idx].size = pending_migration->vmas[v].vm_end - pending_migration->vmas[v].vm_start;
                        guest_registry[i].dsm_map[d_idx].shmid = pending_migration->vmas[v].shmid;
                        mattx_dbg("[IMPORT] Registered DSM Segment: 0x%lx (ID: %u, Size: %lu)\n", 
                                  guest_registry[i].dsm_map[d_idx].base_addr, guest_registry[i].dsm_map[d_idx].shmid, guest_registry[i].dsm_map[d_idx].size);
                    }
                }

                break;
            }
        }
        spin_unlock(&guest_lock);

        // --- CLEANUP ---
        put_task_struct(hijacked_stub_task);
        hijacked_stub_task = NULL;
        kvfree(pending_migration);
        pending_migration = NULL;
        pending_source_node = -1;
    }
}


// ==============================================================================
// 🌱 THE GANG GROWER (Native Thread Spawner)
// ==============================================================================
struct mattx_gang_grower_ctx {
    struct callback_head cb;
    struct completion done;
    int missing_threads;
};

static void mattx_gang_grower_cb(struct callback_head *cb) {
    struct mattx_gang_grower_ctx *ctx = container_of(cb, struct mattx_gang_grower_ctx, cb);
    struct pt_regs regs;
    int i;

    mattx_dbg("[GROWER] Mother %d waking up to spawn %d missing threads...\n", current->pid, ctx->missing_threads);

    for (i = 0; i < ctx->missing_threads; i++) {
        // Carve a tiny 4KB dummy stack in user-space for the new thread
        unsigned long dummy_stack = vm_mmap(NULL, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0);
        if (!IS_ERR_VALUE(dummy_stack)) {
            memset(&regs, 0, sizeof(regs));
            // The exact flags pthread_create uses!
            regs.di = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
            regs.si = dummy_stack + 4096;
            
            if (real_sys_clone) {
                real_sys_clone(&regs); // Birth the thread!
            }
        } else {
            printk(KERN_ERR "MattX:[GROWER] Failed to allocate dummy stack!\n");
        }
    }

    // Clear the growing flag so the Cradle Interceptor stands down
    spin_lock(&export_lock);
    for (i = 0; i < export_count; i++) {
        if (export_registry[i].orig_pid == current->tgid) {
            export_registry[i].is_growing_gang = false;
            break;
        }
    }
    spin_unlock(&export_lock);

    complete(&ctx->done);
    
    // Return normally! Do NOT sleep here, to avoid the SIGCONT ricochet.
}
// ==============================================================================


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

            
            // --- THE GANG GROWER ---
            int local_thread_count = 0;
            struct task_struct *t;
            rcu_read_lock();
            for_each_thread(deputy, t) { local_thread_count++; }
            rcu_read_unlock();

            if (req->thread_count > local_thread_count) {
                int missing = req->thread_count - local_thread_count;
                mattx_dbg("[RECALL] Gang Grower: VM1 has %d threads, Blueprint has %d. Spawning %d dummies...\n", 
                          local_thread_count, req->thread_count, missing);
                
                spin_lock(&export_lock);
                for (int i = 0; i < export_count; i++) {
                    if (export_registry[i].orig_pid == req->orig_pid) {
                        export_registry[i].is_growing_gang = true;
                        break;
                    }
                }
                spin_unlock(&export_lock);

                struct mattx_gang_grower_ctx *grow_ctx = kmalloc(sizeof(*grow_ctx), GFP_KERNEL);
                if (grow_ctx) {
                    grow_ctx->missing_threads = missing;
                    init_completion(&grow_ctx->done);
                    init_task_work(&grow_ctx->cb, mattx_gang_grower_cb);

                    if (real_task_work_add) {
                        real_task_work_add(deputy, &grow_ctx->cb, TWA_SIGNAL);
                        send_sig(SIGCONT, deputy, 0);
                        wait_for_completion(&grow_ctx->done);
                        
                        // --- THE TRUE FREEZE ---
                        send_sig(SIGSTOP, deputy, 0);
                        mattx_dbg("[RECALL] Gang Grower finished. Mother is freezing...\n");
                        
                        // Wait for all newborn threads AND the Mother to reach TASK_STOPPED
                        int retries = 500;
                        while (retries > 0) {
                            bool all_stopped = true;
                            rcu_read_lock();
                            for_each_thread(deputy, t) {
                                if (!(READ_ONCE(t->__state) & __TASK_STOPPED)) {
                                    all_stopped = false;
                                    break;
                                }
                            }
                            rcu_read_unlock();
                            if (all_stopped) break;
                            msleep(10);
                            retries--;
                        }
                    }
                    kfree(grow_ctx);
                }
            }


            // --- THE DYNAMIC BRAIN CARVER (Deadlock-Free Edition) ---
            // The Surrogate may have allocated NEW memory while running on VM2!
            // We must ensure the Deputy has these VMAs mapped before we inject data.
            if (deputy->mm) {
                for (int i = 0; i < req->vma_count; i++) {
                    unsigned long start = req->vmas[i].vm_start;
                    unsigned long size = req->vmas[i].vm_end - start;
                    unsigned long flags = req->vmas[i].vm_flags;
                    


                    // here we check for MPI_SUPPORT and if it's enabled it needs to check if the entire VMA exists, 
                    // not just the start address. This is because MPI applications may have large contiguous memory
                    // regions that need to be preserved during migration. If the entire VMA does not exist, we will
                    // carve it out to ensure the Deputy has the necessary memory mapped before we inject data.
                    // --- THE DYNAMIC BRAIN CARVER (The Hole Carver Edition) ---
                    bool needs_mapping = false;
                    unsigned long carve_start = start;
                    unsigned long carve_size = size;

                    if (config_mpi_support) {
                        // MPI apps might need the whole block checked
                        mmap_read_lock(deputy->mm);
                        struct vm_area_struct *vma = find_vma(deputy->mm, start);
                        if (vma && vma->vm_start <= start && vma->vm_end >= start + size) {
                            needs_mapping = false; 
                        } else {
                            needs_mapping = true;
                        }
                        mmap_read_unlock(deputy->mm);
                    } else {
                        // The Hole Carver: Only carve exactly what is missing!
                        mmap_read_lock(deputy->mm);
                        struct vm_area_struct *vma = find_vma(deputy->mm, start);
                        
                        if (!vma || vma->vm_start >= start + size) {
                            // Completely missing (e.g., new thread stack from VM2)
                            needs_mapping = true;
                        } else if (vma->vm_start > start) {
                            // Missing at the beginning (e.g., stack grew down)
                            needs_mapping = true;
                            carve_size = vma->vm_start - start;
                        } else if (vma->vm_end < start + size) {
                            // Missing at the end (e.g., heap grew up)
                            needs_mapping = true;
                            carve_start = vma->vm_end;
                            carve_size = (start + size) - vma->vm_end;
                        }
                        mmap_read_unlock(deputy->mm);
                    }

                    if (needs_mapping) {
                        mattx_dbg("[RECALL] Carving missing memory hole: 0x%lx (Size: %lu)\n", carve_start, carve_size);
                        
                        kthread_use_mm(deputy->mm);
                        
                        // Dynamically translate VM_FLAGS to PROT_FLAGS to allow VMA merging!
                        unsigned long prot = 0;
                        if (flags & 0x00000001) prot |= PROT_READ;  // VM_READ
                        if (flags & 0x00000002) prot |= PROT_WRITE; // VM_WRITE
                        if (flags & 0x00000004) prot |= PROT_EXEC;  // VM_EXEC
                        
                        unsigned long map_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
                        if (flags & 0x0100) map_flags |= MAP_GROWSDOWN; // Stack protector
                        
                        unsigned long ret = vm_mmap(NULL, carve_start, carve_size, prot, map_flags, 0);
                        if (IS_ERR_VALUE(ret)) {
                            printk(KERN_ERR "MattX:[RECALL] FATAL: Failed to carve memory at 0x%lx (err: %ld)\n", carve_start, ret);
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


// ==============================================================================
// 👻 THE GHOST EXORCIST (Silent Thread Assassin)
// ==============================================================================
struct mattx_exorcist_ctx {
    struct callback_head cb;
};

static void mattx_exorcist_cb(struct callback_head *cb) {
    struct mattx_exorcist_ctx *ctx = container_of(cb, struct mattx_exorcist_ctx, cb);
    struct pt_regs regs;
    
    mattx_dbg("[EXORCIST] Executing clean thread exit for Ghost Thread %d...\n", current->pid);
    kfree(ctx);
    
    // --- THE SILENT ASSASSIN ---
    // Prevent the kernel from writing to clear_child_tid, because this thread 
    // already died on VM2 and the user-space memory might have been reused!
    current->clear_child_tid = NULL;
    current->set_child_tid = NULL;
    
    memset(&regs, 0, sizeof(regs));
    regs.di = 0; // Exit code 0
    
    if (real_sys_exit) {
        real_sys_exit(&regs); // Pull the trigger! (This function never returns)
    }
    
    // Fallback just in case
    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
}
// ==============================================================================


static void handle_return_done(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    mattx_dbg("[IMPORT] Return memory transferred successfully! Pages: %d\n", injected_pages_count);
    
    if (hijacked_stub_task && pending_migration) {
        struct task_struct *t;
        int t_idx = 0;
        int i;

        // --- THE vDSO TRANSPLANT ---
        // disabled vDSO transplant for testing
        if (false) {
        // if (hijacked_stub_task->mm && hijacked_stub_task->mm->context.vdso != (void *)pending_migration->vdso_addr) {
            unsigned long old_vdso = (unsigned long)hijacked_stub_task->mm->context.vdso;
            unsigned long new_vdso = pending_migration->vdso_addr;
            unsigned long vdso_size = PAGE_SIZE; // Fallback
            
            mmap_read_lock(hijacked_stub_task->mm);
            struct vm_area_struct *vma = find_vma(hijacked_stub_task->mm, old_vdso);
            if (vma && vma->vm_start <= old_vdso && vma->vm_end > old_vdso) {
                vdso_size = vma->vm_end - vma->vm_start;
            }
            mmap_read_unlock(hijacked_stub_task->mm);
            
            struct mattx_vdso_transplant_ctx *vdso_ctx = kmalloc(sizeof(*vdso_ctx), GFP_KERNEL);
            if (vdso_ctx) {
                vdso_ctx->old_vdso = old_vdso;
                vdso_ctx->new_vdso = new_vdso;
                vdso_ctx->vdso_size = vdso_size;
                init_completion(&vdso_ctx->done);
                init_task_work(&vdso_ctx->cb, mattx_vdso_transplant_cb);
                
                if (real_task_work_add) {
                    real_task_work_add(hijacked_stub_task, &vdso_ctx->cb, TWA_SIGNAL);
                    send_sig(SIGCONT, hijacked_stub_task, 0);
                    wait_for_completion(&vdso_ctx->done);
                    
                    // --- THE TRUE FREEZE ---
                    send_sig(SIGSTOP, hijacked_stub_task, 0);
                    mattx_dbg("[TRANSPLANT] vDSO Transplant finished. Surrogate is freezing...\n");
                    
                    // Ironclad Verification: Wait until the CPU confirms the task is unconscious!
                    int retries = 500; // 5 seconds max
                    while (!(READ_ONCE(hijacked_stub_task->__state) & __TASK_STOPPED) && retries > 0) {
                        msleep(10);
                        retries--;
                    }
                }

                kfree(vdso_ctx);
            }
        }


        // --- GANG RETURN INJECTION ---
        rcu_read_lock();
        for_each_thread(hijacked_stub_task, t) {
            if (t_idx < pending_migration->thread_count) {
                struct pt_regs *t_regs = task_pt_regs(t);
                if (t_regs) {
                    memcpy(t_regs, &pending_migration->threads[t_idx].regs, sizeof(struct pt_regs));
                    
                    // TLS Hardware Sync on Return! ---
                    t->thread.fsbase = pending_migration->threads[t_idx].fsbase;
                    t->thread.gsbase = pending_migration->threads[t_idx].gsbase;
                    if (real_x86_fsbase_write_task) real_x86_fsbase_write_task(t, t->thread.fsbase);
                    if (real_x86_gsbase_write_task) real_x86_gsbase_write_task(t, t->thread.gsbase);

                    t->clear_child_tid = (int __user *)pending_migration->threads[t_idx].clear_child_tid;
                    t->set_child_tid   = (int __user *)pending_migration->threads[t_idx].set_child_tid;

                    // Restore FPU/SSE/AVX state on return! See the matching
                    // capture side in mattx_migr.c for why this is needed.
                    if (t->thread.fpu.fpstate && pending_migration->threads[t_idx].fpu_size > 0) {
                        u32 fsize = pending_migration->threads[t_idx].fpu_size;
                        if (fsize > t->thread.fpu.fpstate->size)
                            fsize = t->thread.fpu.fpstate->size;
                        if (fsize > sizeof(pending_migration->threads[t_idx].fpu_state))
                            fsize = sizeof(pending_migration->threads[t_idx].fpu_state);
                        memcpy(&t->thread.fpu.fpstate->regs, pending_migration->threads[t_idx].fpu_state, fsize);
                    }
                }

                // Child Comm Fix on Return! ---
                strscpy(t->comm, pending_migration->comm, sizeof(t->comm));
            } else {
                // --- DEPLOY THE GHOST EXORCIST ---
                // This thread exists on VM1 but died on VM2! We must assassinate it cleanly.
                struct mattx_exorcist_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
                if (ctx) {
                    init_task_work(&ctx->cb, mattx_exorcist_cb);
                    if (real_task_work_add) {
                        real_task_work_add(t, &ctx->cb, TWA_SIGNAL);
                    }
                }
            }
            t_idx++;
        }
        rcu_read_unlock();

        // --- Use the Mother's registers from the Blueprint! ---
        if (pending_migration->thread_count > 0) {
            mattx_dbg("[IMPORT] Deputy Brain Restored. New Mother RIP: 0x%lx\n", (unsigned long)pending_migration->threads[0].regs.rip);
        }

        // Restore the Heap boundaries on the Deputy! ---
        if (hijacked_stub_task->mm) {
            hijacked_stub_task->mm->start_brk = pending_migration->start_brk;
            hijacked_stub_task->mm->brk = pending_migration->brk;
        }

        spin_lock(&export_lock);
        for (i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == hijacked_stub_task->pid) {
                remove_export_process(i);
                break;
            }
        }
        spin_unlock(&export_lock);

        mattx_dbg("[IMPORT] Welcome home! Waking Gang Deputy PID %d\n", hijacked_stub_task->pid);
        rcu_read_lock();
        for_each_thread(hijacked_stub_task, t) {
            send_sig(SIGCONT, t, 0);
        }
        rcu_read_unlock();
        
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

