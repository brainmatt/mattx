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

        printk(KERN_INFO "MattX:[IMPORT] Received Blueprint for PID %u. Saving to pending...\n", req->orig_pid);
        if (pending_migration) kvfree(pending_migration);
        
        pending_migration = kvmalloc(hdr->length, GFP_KERNEL);
        if (pending_migration) {
            memcpy(pending_migration, req, hdr->length);
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
        unsigned long target_addr = pending_migration->vmas[ph->vma_index].vm_start + ph->offset;
        int res;

        // If we are injecting into the Deputy (Return Migration), we must force the VMA to be writable!
        if (hijacked_stub_task->mm) {
            struct vm_area_struct *vma;
            mmap_write_lock(hijacked_stub_task->mm);
            vma = find_vma(hijacked_stub_task->mm, target_addr);
            if (vma && target_addr >= vma->vm_start) {
                vm_flags_set(vma, VM_WRITE); 
            }
            mmap_write_unlock(hijacked_stub_task->mm);
        }

        res = access_process_vm(hijacked_stub_task, target_addr, data, ph->length, FOLL_WRITE | FOLL_FORCE);
        
        if (res != ph->length) {
            if (ph->offset == 0) {
                printk(KERN_ERR "MattX:[IMPORT] Failed to inject %u bytes at 0x%lx (res: %d)\n", ph->length, target_addr, res);
            }
        } else {
            injected_pages_count++;
        }
    }
}

static void handle_migrate_done(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    printk(KERN_INFO "MattX:[IMPORT] All memory transferred! Total pages injected: %d\n", injected_pages_count);
    
    if (hijacked_stub_task && pending_migration) {
        struct pt_regs *regs;
        struct cred *new_cred;
        const struct cred *old_cred;
        int retries = 50;
        unsigned char rip_buf[8] = {0}; 
        struct file **fake_files; 
        int i;

        printk(KERN_INFO "MattX:[IMPORT] Commencing full brain transplant on PID %d...\n", hijacked_stub_task->pid);

        while (!(READ_ONCE(hijacked_stub_task->__state) & __TASK_STOPPED) && retries > 0) {
            msleep(10);
            retries--;
        }

        regs = task_pt_regs(hijacked_stub_task);
        if (regs) {
            memcpy(regs, &pending_migration->regs, sizeof(struct pt_regs));
            regs->ax = 0; 
            
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
                        }
                    }
                    spin_unlock(&hijacked_stub_task->files->file_lock);
                    printk(KERN_INFO "MattX:[IMPORT] Successfully injected %u Fake FDs!\n", pending_migration->fd_count);
                }
                kfree(fake_files); 
            }

            if (access_process_vm(hijacked_stub_task, regs->ip, rip_buf, 8, FOLL_FORCE) == 8) {
                printk(KERN_INFO "MattX:[DEBUG] Target RIP (0x%lx) contains: %8ph\n", regs->ip, rip_buf);
            }

            printk(KERN_INFO "MattX:[IMPORT] IT'S ALIVE! Sending SIGCONT to PID %d\n", hijacked_stub_task->pid);
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
        int i;

        printk(KERN_INFO "MattX:[IMPORT] Received RETURN Blueprint for Deputy PID %u. Saving to pending...\n", req->orig_pid);
        
        pending_source_node = hdr->sender_id;
        injected_pages_count = 0;

        if (pending_migration) kvfree(pending_migration);
        pending_migration = kmemdup(req, hdr->length, GFP_KERNEL);
        
        rcu_read_lock();
        deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            printk(KERN_INFO "MattX:[IMPORT] Found frozen Deputy PID %d. Preparing memory map...\n", deputy->pid);
            
            if (hijacked_stub_task) put_task_struct(hijacked_stub_task);
            hijacked_stub_task = deputy; 

            if (deputy->mm) {
                kthread_use_mm(deputy->mm);
                for (i = 0; i < pending_migration->vma_count; i++) {
                    unsigned long start = pending_migration->vmas[i].vm_start;
                    unsigned long len = pending_migration->vmas[i].vm_end - start;
                    unsigned long prot = PROT_READ | PROT_WRITE | PROT_EXEC;
                    unsigned long flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
                    unsigned long ret_addr;

                    ret_addr = vm_mmap(NULL, start, len, prot, flags, 0);
                    if (IS_ERR_VALUE(ret_addr)) {
                        printk(KERN_ERR "MattX:[IMPORT] Failed to carve VMA at 0x%lx (err: %ld)\n", start, ret_addr);
                    }
                }
                kthread_unuse_mm(deputy->mm);
                printk(KERN_INFO "MattX:[IMPORT] Successfully carved %u VMAs into Deputy!\n", pending_migration->vma_count);
            }

            printk(KERN_INFO "MattX:[IMPORT] Sending READY_FOR_DATA signal to Node %d...\n", pending_source_node);
            mattx_comm_send(cluster_map[pending_source_node], MATTX_MSG_READY_FOR_DATA, NULL, 0);
        } else {
            printk(KERN_ERR "MattX:[IMPORT] ERROR: Deputy PID %u not found!\n", req->orig_pid);
        }
    }
}

static void handle_return_done(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    printk(KERN_INFO "MattX:[IMPORT] Return memory transferred successfully! Pages: %d\n", injected_pages_count);
    
    if (hijacked_stub_task && pending_migration) {
        struct pt_regs *regs = task_pt_regs(hijacked_stub_task);
        int i;

        if (regs) {
            memcpy(regs, &pending_migration->regs, sizeof(struct pt_regs));
            
            printk(KERN_INFO "MattX:[IMPORT] Deputy Brain Restored. New RIP: 0x%lx\n", regs->ip);
            
            spin_lock(&export_lock);
            for (i = 0; i < export_count; i++) {
                if (export_registry[i].orig_pid == hijacked_stub_task->pid) {
                    remove_export_process(i);
                    break;
                }
            }
            spin_unlock(&export_lock);

            printk(KERN_INFO "MattX:[IMPORT] Welcome home! Sending SIGCONT to Deputy PID %d\n", hijacked_stub_task->pid);
            send_sig(SIGCONT, hijacked_stub_task, 0);
        }
        
        put_task_struct(hijacked_stub_task);
        hijacked_stub_task = NULL;
        kvfree(pending_migration);
        pending_migration = NULL;
        pending_source_node = -1;
    }
}

void mattx_import_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_MIGRATE_REQ, handle_migrate_req);
    mattx_register_handler(MATTX_MSG_PAGE_TRANSFER, handle_page_transfer);
    mattx_register_handler(MATTX_MSG_MIGRATE_DONE, handle_migrate_done);
    mattx_register_handler(MATTX_MSG_RETURN_BLUEPRINT, handle_return_blueprint);
    mattx_register_handler(MATTX_MSG_RETURN_DONE, handle_return_done);
    printk(KERN_INFO "MattX: [IMPORT] Network handlers registered.\n");
}

