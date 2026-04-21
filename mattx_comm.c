#include "mattx.h"
#include <linux/wait.h> // NEW: For wait_event_interruptible

static int injected_pages_count = 0;

// Helper to safely check if the RPC is done while we are sleeping
static bool check_rpc_done(pid_t pid) {
    bool done = false;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            done = guest_registry[i].rpc_done;
            break;
        }
    }
    spin_unlock(&guest_lock);
    return done;
}

// --- NEW: The Fake File Read Operation (Runs on Node 2) ---
static ssize_t mattx_fake_read(struct file *file, char __user *buf, size_t count, loff_t *pos) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    struct mattx_sys_read_req req;
    DECLARE_WAIT_QUEUE_HEAD_ONSTACK(rpc_wq);
    pid_t my_pid = current->pid;
    int i;
    ssize_t ret_bytes = 0;
    void *read_buf = NULL;

    if (!fd_info || !cluster_map[fd_info->home_node]) return -EIO;

    // Cap read size to prevent massive kmallocs on the network
    size_t to_read = min_t(size_t, count, 4096);

    req.orig_pid = fd_info->orig_pid;
    req.fd = fd_info->remote_fd;
    req.count = to_read;

    // 1. Attach our wait queue to the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            guest_registry[i].rpc_wq = &rpc_wq;
            guest_registry[i].rpc_done = false;
            guest_registry[i].rpc_read_buf = NULL;
            guest_registry[i].rpc_read_bytes = 0;
            break;
        }
    }
    spin_unlock(&guest_lock);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d requesting %zu bytes from FD %u. Sleeping...\n", my_pid, to_read, req.fd);

    // 2. Send the request to Node 1
    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_READ_REQ, &req, sizeof(req));

    // 3. Go to sleep until Node 1 replies!
    wait_event_interruptible(rpc_wq, check_rpc_done(my_pid));

    // 4. We woke up! Collect the data from the registry
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == my_pid) {
            read_buf = guest_registry[i].rpc_read_buf;
            ret_bytes = guest_registry[i].rpc_read_bytes;
            
            guest_registry[i].rpc_wq = NULL;
            guest_registry[i].rpc_read_buf = NULL;
            break;
        }
    }
    spin_unlock(&guest_lock);

    // 5. Copy the data to user-space
    if (ret_bytes > 0 && read_buf) {
        if (copy_to_user(buf, read_buf, ret_bytes)) {
            ret_bytes = -EFAULT;
        } else {
            *pos += ret_bytes; // Advance the file position!
        }
    }

    if (read_buf) kfree(read_buf);

    printk(KERN_INFO "MattX:[WORMHOLE] Surrogate %d woke up! Read %zd bytes.\n", my_pid, ret_bytes);
    return ret_bytes;
}

static ssize_t mattx_fake_write(struct file *file, const char __user *buf, size_t count, loff_t *pos) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    size_t to_send;
    size_t packet_size;
    void *packet_buf;
    struct mattx_header *hdr;
    struct mattx_syscall_req *req;

    if (!fd_info || !cluster_map[fd_info->home_node]) return count; 

    to_send = min_t(size_t, count, 4096);

    packet_size = sizeof(struct mattx_header) + sizeof(struct mattx_syscall_req) + to_send;
    packet_buf = kmalloc(packet_size, GFP_KERNEL);
    if (!packet_buf) return -ENOMEM;

    hdr = (struct mattx_header *)packet_buf;
    req = (struct mattx_syscall_req *)(packet_buf + sizeof(struct mattx_header));

    req->orig_pid = fd_info->orig_pid;
    req->fd = fd_info->remote_fd; 
    req->len = to_send;

    if (copy_from_user(req->data, buf, to_send)) {
        kfree(packet_buf);
        return -EFAULT;
    }

    mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYSCALL_FWD, packet_buf + sizeof(struct mattx_header), sizeof(struct mattx_syscall_req) + to_send);

    kfree(packet_buf);
    *pos += to_send;
    return to_send; 
}

static int mattx_fake_release(struct inode *inode, struct file *file) {
    struct mattx_fake_fd_info *fd_info = file->private_data;
    struct mattx_sys_close_req req;

    if (fd_info) {
        if (cluster_map[fd_info->home_node]) {
            req.orig_pid = fd_info->orig_pid;
            req.remote_fd = fd_info->remote_fd;

            printk(KERN_INFO "MattX:[WORMHOLE] Surrogate closed FD %u. Sending CLOSE_REQ to Node %d...\n", req.remote_fd, fd_info->home_node);
            mattx_comm_send(cluster_map[fd_info->home_node], MATTX_MSG_SYS_CLOSE_REQ, &req, sizeof(req));
        }
        kfree(fd_info);
    }
    return 0;
}

const struct file_operations mattx_fops = {
    .read = mattx_fake_read, // NEW: Hook into the VFS read event!
    .write = mattx_fake_write,
    .release = mattx_fake_release, 
};

static char *stub_argv[] = { "/usr/local/bin/mattx-stub", NULL };
static char *stub_envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };

static void mattx_handle_message(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    switch (hdr->type) {
        case MATTX_MSG_HEARTBEAT:
            if (link->node_id == -1 && hdr->sender_id < MAX_NODES) {
                link->node_id = hdr->sender_id;
                cluster_map[link->node_id] = link;
            }
            break;
            
        case MATTX_MSG_LOAD_UPDATE:
            if (hdr->sender_id < MAX_NODES && payload) {
                struct mattx_load_info *load = (struct mattx_load_info *)payload;
                cluster_load_table[hdr->sender_id] = *load;
            }
            break;
            
        case MATTX_MSG_MIGRATE_REQ:
            if (payload) {
                struct mattx_migration_req *req = (struct mattx_migration_req *)payload;
                
                pending_source_node = hdr->sender_id;
                injected_pages_count = 0;

                printk(KERN_INFO "MattX:[INCOMING] Received Blueprint for PID %u. Saving to pending...\n", req->orig_pid);
                if (pending_migration) kvfree(pending_migration);
                
                pending_migration = kvmalloc(hdr->length, GFP_KERNEL);
                if (pending_migration) {
                    memcpy(pending_migration, req, hdr->length);
                }

                if (call_usermodehelper(stub_argv[0], stub_argv, stub_envp, UMH_NO_WAIT) != 0) {
                    printk(KERN_ERR "MattX:[INCOMING] Failed to spawn surrogate!\n");
                }
            }
            break;
            
        case MATTX_MSG_READY_FOR_DATA:
            printk(KERN_INFO "MattX:[COMM] Received READY signal from Node %u. Starting data pump...\n", hdr->sender_id);
            mattx_send_vma_data();
            break;
            
        case MATTX_MSG_PAGE_TRANSFER:
            if (payload && pending_migration && hijacked_stub_task) {
                struct mattx_page_header *ph = (struct mattx_page_header *)payload;
                void *data = (char *)payload + sizeof(struct mattx_page_header);
                unsigned long target_addr = pending_migration->vmas[ph->vma_index].vm_start + ph->offset;
                int res;

                res = access_process_vm(hijacked_stub_task, target_addr, data, ph->length, FOLL_WRITE | FOLL_FORCE);
                
                if (res != ph->length) {
                    if (ph->offset == 0) {
                        printk(KERN_ERR "MattX: [INJECT] Failed to inject %u bytes at 0x%lx (res: %d)\n", ph->length, target_addr, res);
                    }
                } else {
                    injected_pages_count++;
                }
            }
            break;
            
        case MATTX_MSG_MIGRATE_DONE:
            printk(KERN_INFO "MattX:[INCOMING] All memory transferred! Total pages injected: %d\n", injected_pages_count);
            
            if (hijacked_stub_task && pending_migration) {
                struct pt_regs *regs;
                struct cred *new_cred;
                const struct cred *old_cred;
                int retries = 50;
                unsigned char rip_buf[8] = {0}; 
                struct file **fake_files; 
                int i;

                printk(KERN_INFO "MattX:[AWAKEN] Commencing full brain transplant on PID %d...\n", hijacked_stub_task->pid);

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
                            printk(KERN_INFO "MattX:[AWAKEN] Successfully injected %u Fake FDs!\n", pending_migration->fd_count);
                        }
                        kfree(fake_files); 
                    }

                    if (access_process_vm(hijacked_stub_task, regs->ip, rip_buf, 8, FOLL_FORCE) == 8) {
                        printk(KERN_INFO "MattX: [DEBUG] Target RIP (0x%lx) contains: %8ph\n", regs->ip, rip_buf);
                    }

                    printk(KERN_INFO "MattX:[AWAKEN] IT'S ALIVE! Sending SIGCONT to PID %d\n", hijacked_stub_task->pid);
                    send_sig(SIGCONT, hijacked_stub_task, 0);
                    
                    add_guest_process(hijacked_stub_task->pid, pending_migration->orig_pid, pending_source_node);
                }

                put_task_struct(hijacked_stub_task);
                hijacked_stub_task = NULL;
                kvfree(pending_migration);
                pending_migration = NULL;
                pending_source_node = -1;
            }
            break;
            
        case MATTX_MSG_PROCESS_EXIT:
            if (payload) {
                struct mattx_process_exit *exit_msg = (struct mattx_process_exit *)payload;
                struct task_struct *deputy = NULL;
                int i;

                printk(KERN_INFO "MattX: [FUNERAL] Received exit notice for Deputy PID %u from Node %u\n", 
                       exit_msg->orig_pid, hdr->sender_id);

                rcu_read_lock();
                deputy = pid_task(find_vpid(exit_msg->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    printk(KERN_INFO "MattX: [FUNERAL] Laying Deputy PID %u to rest (Sending SIGKILL)...\n", deputy->pid);
                    send_sig(SIGKILL, deputy, 0);
                    put_task_struct(deputy);
                }
                
                spin_lock(&export_lock);
                for (i = 0; i < export_count; i++) {
                    if (export_registry[i].orig_pid == exit_msg->orig_pid) {
                        remove_export_process(i);
                        break;
                    }
                }
                spin_unlock(&export_lock);
            }
            break;

        case MATTX_MSG_KILL_SURROGATE:
            if (payload) {
                struct mattx_process_exit *kill_msg = (struct mattx_process_exit *)payload;
                pid_t local_stub_pid = -1;
                int i;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (guest_registry[i].orig_pid == kill_msg->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                        local_stub_pid = guest_registry[i].local_pid;
                        remove_guest_process(i);
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
                        printk(KERN_INFO "MattX:[ASSASSIN] Executing Surrogate PID %d (Sending SIGKILL)...\n", surrogate->pid);
                        send_sig(SIGKILL, surrogate, 0);
                        put_task_struct(surrogate);
                    }
                }
            }
            break;
            
        case MATTX_MSG_SYSCALL_FWD:
            if (payload) {
                struct mattx_syscall_req *req = (struct mattx_syscall_req *)payload;
                struct task_struct *deputy = NULL;
                struct file *file = NULL;
                int i;
                
                if (req->fd >= 1000) {
                    int slot = req->fd - 1000;
                    spin_lock(&export_lock);
                    for (i = 0; i < export_count; i++) {
                        if (export_registry[i].orig_pid == req->orig_pid) {
                            if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                                file = export_registry[i].remote_files[slot];
                                get_file(file); 
                            }
                            break;
                        }
                    }
                    spin_unlock(&export_lock);
                } else {
                    rcu_read_lock();
                    deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                    if (deputy) get_task_struct(deputy);
                    rcu_read_unlock();
                    
                    if (deputy) {
                        struct files_struct *files = deputy->files;
                        if (files) {
                            spin_lock(&files->file_lock);
                            struct fdtable *fdt = files_fdtable(files);
                            if (req->fd < fdt->max_fds) {
                                file = rcu_dereference_raw(fdt->fd[req->fd]);
                                if (file) get_file(file); 
                            }
                            spin_unlock(&files->file_lock);
                        }
                        put_task_struct(deputy);
                    }
                }
                
                if (file) {
                    loff_t pos = file->f_pos;
                    ssize_t ret;
                    const struct cred *old_cred = NULL;
                    
                    if (deputy) {
                        if (deputy->mm) kthread_use_mm(deputy->mm);
                        old_cred = override_creds(deputy->cred);
                    }
                    
                    ret = kernel_write(file, req->data, req->len, &pos);
                    
                    if (deputy) {
                        revert_creds(old_cred);
                        if (deputy->mm) kthread_unuse_mm(deputy->mm);
                    }
                    
                    if (ret >= 0) {
                        file->f_pos = pos;
                    }
                    fput(file); 
                }
            }
            break;
            
        case MATTX_MSG_SYS_OPEN_REQ:
            if (payload) {
                struct mattx_sys_open_req *req = (struct mattx_sys_open_req *)payload;
                struct mattx_sys_open_reply reply;
                struct file *filp = NULL;
                struct task_struct *deputy = NULL;
                int remote_fd = -1;
                int i, j;

                printk(KERN_INFO "MattX:[RPC] Received OPEN request from Node %u for file: '%s'\n", hdr->sender_id, req->filename);

                rcu_read_lock();
                deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    const struct cred *old_cred;
                    
                    if (deputy->mm) kthread_use_mm(deputy->mm);
                    old_cred = override_creds(deputy->cred);

                    filp = filp_open(req->filename, O_CREAT | O_WRONLY | O_APPEND, 0666);
                    
                    revert_creds(old_cred);
                    if (deputy->mm) kthread_unuse_mm(deputy->mm);

                    if (IS_ERR(filp)) {
                        printk(KERN_ERR "MattX:[RPC] Failed to open file '%s' on Home Node (err: %ld)\n", req->filename, PTR_ERR(filp));
                        reply.error = PTR_ERR(filp);
                    } else {
                        spin_lock(&export_lock);
                        for (i = 0; i < export_count; i++) {
                            if (export_registry[i].orig_pid == req->orig_pid) {
                                for (j = 0; j < MAX_FDS; j++) {
                                    if (export_registry[i].remote_files[j] == NULL) {
                                        export_registry[i].remote_files[j] = filp;
                                        remote_fd = j + 1000; 
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        spin_unlock(&export_lock);
                        
                        if (remote_fd == -1) {
                            fput(filp); 
                            reply.error = -ENFILE;
                        } else {
                            reply.error = 0;
                        }
                    }
                    put_task_struct(deputy);
                } else {
                    reply.error = -ESRCH;
                }

                reply.orig_pid = req->orig_pid;
                reply.remote_fd = remote_fd;

                printk(KERN_INFO "MattX:[RPC] Sending OPEN_REPLY (Remote FD: %d) back to Node %u...\n", remote_fd, hdr->sender_id);
                if (cluster_map[hdr->sender_id]) {
                    mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_OPEN_REPLY, &reply, sizeof(reply));
                }
            }
            break;

        case MATTX_MSG_SYS_CLOSE_REQ:
            if (payload) {
                struct mattx_sys_close_req *req = (struct mattx_sys_close_req *)payload;
                int i;
                
                printk(KERN_INFO "MattX:[RPC] Received CLOSE request for Remote FD %u from Node %u\n", req->remote_fd, hdr->sender_id);

                if (req->remote_fd >= 1000) {
                    int slot = req->remote_fd - 1000;
                    spin_lock(&export_lock);
                    for (i = 0; i < export_count; i++) {
                        if (export_registry[i].orig_pid == req->orig_pid) {
                            if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                                fput(export_registry[i].remote_files[slot]);
                                export_registry[i].remote_files[slot] = NULL;
                                printk(KERN_INFO "MattX:[RPC] Successfully closed Remote FD %u\n", req->remote_fd);
                            }
                            break;
                        }
                    }
                    spin_unlock(&export_lock);
                }
            }
            break;
            
        // --- NEW: Node 1 receives READ_REQ, reads the file, and sends READ_REPLY ---
        case MATTX_MSG_SYS_READ_REQ:
            if (payload) {
                struct mattx_sys_read_req *req = (struct mattx_sys_read_req *)payload;
                struct task_struct *deputy = NULL;
                struct file *file = NULL;
                int i;
                
                printk(KERN_INFO "MattX:[WORMHOLE] Received READ request for FD %u from Node %u. Reading from Deputy...\n", req->fd, hdr->sender_id);

                if (req->fd >= 1000) {
                    int slot = req->fd - 1000;
                    spin_lock(&export_lock);
                    for (i = 0; i < export_count; i++) {
                        if (export_registry[i].orig_pid == req->orig_pid) {
                            if (slot < MAX_FDS && export_registry[i].remote_files[slot]) {
                                file = export_registry[i].remote_files[slot];
                                get_file(file); 
                            }
                            break;
                        }
                    }
                    spin_unlock(&export_lock);
                } else {
                    rcu_read_lock();
                    deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                    if (deputy) get_task_struct(deputy);
                    rcu_read_unlock();
                    
                    if (deputy) {
                        struct files_struct *files = deputy->files;
                        if (files) {
                            spin_lock(&files->file_lock);
                            struct fdtable *fdt = files_fdtable(files);
                            if (req->fd < fdt->max_fds) {
                                file = rcu_dereference_raw(fdt->fd[req->fd]);
                                if (file) get_file(file); 
                            }
                            spin_unlock(&files->file_lock);
                        }
                        put_task_struct(deputy);
                    }
                }
                
                if (file) {
                    loff_t pos = file->f_pos;
                    ssize_t ret;
                    const struct cred *old_cred = NULL;
                    void *read_buf = kmalloc(req->count, GFP_KERNEL);
                    
                    if (read_buf) {
                        if (deputy) {
                            if (deputy->mm) kthread_use_mm(deputy->mm);
                            old_cred = override_creds(deputy->cred);
                        }
                        
                        ret = kernel_read(file, read_buf, req->count, &pos);
                        
                        if (deputy) {
                            revert_creds(old_cred);
                            if (deputy->mm) kthread_unuse_mm(deputy->mm);
                        }
                        
                        if (ret >= 0) {
                            file->f_pos = pos;
                        }

                        // Prepare and send reply
                        size_t reply_size = sizeof(struct mattx_sys_read_reply) + (ret > 0 ? ret : 0);
                        struct mattx_sys_read_reply *reply = kmalloc(reply_size, GFP_KERNEL);
                        if (reply) {
                            reply->orig_pid = req->orig_pid;
                            reply->bytes_read = ret;
                            reply->error = (ret < 0) ? ret : 0;
                            if (ret > 0) {
                                memcpy(reply->data, read_buf, ret);
                            }
                            mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_READ_REPLY, reply, reply_size);
                            kfree(reply);
                        }
                        kfree(read_buf);
                    }
                    fput(file); 
                } else {
                    // Send error reply
                    struct mattx_sys_read_reply reply;
                    reply.orig_pid = req->orig_pid;
                    reply.bytes_read = -EBADF;
                    reply.error = -EBADF;
                    mattx_comm_send(cluster_map[hdr->sender_id], MATTX_MSG_SYS_READ_REPLY, &reply, sizeof(reply));
                }
            }
            break;

        // --- NEW: Node 2 receives the READ_REPLY and wakes up the Surrogate ---
        case MATTX_MSG_SYS_READ_REPLY:
            if (payload) {
                struct mattx_sys_read_reply *reply = (struct mattx_sys_read_reply *)payload;
                int i;

                printk(KERN_INFO "MattX:[RPC] Received READ_REPLY for Orig PID %u. Bytes read: %zd\n", reply->orig_pid, reply->bytes_read);

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (guest_registry[i].orig_pid == reply->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                        
                        guest_registry[i].rpc_read_bytes = reply->bytes_read;
                        
                        if (reply->bytes_read > 0) {
                            guest_registry[i].rpc_read_buf = kmalloc(reply->bytes_read, GFP_ATOMIC);
                            if (guest_registry[i].rpc_read_buf) {
                                memcpy(guest_registry[i].rpc_read_buf, reply->data, reply->bytes_read);
                            } else {
                                guest_registry[i].rpc_read_bytes = -ENOMEM;
                            }
                        } else {
                            guest_registry[i].rpc_read_buf = NULL;
                        }

                        guest_registry[i].rpc_done = true;
                        
                        if (guest_registry[i].rpc_wq) {
                            wake_up_interruptible(guest_registry[i].rpc_wq);
                        }
                        break;
                    }
                }
                spin_unlock(&guest_lock);
            }
            break;

        case MATTX_MSG_RECALL_REQ:
            if (payload) {
                struct mattx_recall_req *req = (struct mattx_recall_req *)payload;
                pid_t local_stub_pid = -1;
                int i;

                printk(KERN_INFO "MattX:[INCOMING] Received RECALL request for Orig PID %u from Node %u\n", 
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
                        printk(KERN_INFO "MattX: [RECALL] Found Surrogate PID %d. Capturing state...\n", surrogate->pid);
                        mattx_capture_and_return_state(surrogate, req->orig_pid, hdr->sender_id);
                        put_task_struct(surrogate);
                    } else {
                        printk(KERN_WARNING "MattX:[RECALL] Surrogate PID %d not found!\n", local_stub_pid);
                    }
                } else {
                    printk(KERN_WARNING "MattX:[RECALL] Could not find guest registry entry for Orig PID %u\n", req->orig_pid);
                }
            }
            break;

        case MATTX_MSG_RETURN_BLUEPRINT:
            if (payload) {
                struct mattx_migration_req *req = (struct mattx_migration_req *)payload;
                struct task_struct *deputy = NULL;
                int i;

                printk(KERN_INFO "MattX:[INCOMING] Received RETURN Blueprint for Deputy PID %u. Saving to pending...\n", req->orig_pid);
                
                pending_source_node = hdr->sender_id;
                injected_pages_count = 0;

                if (pending_migration) kvfree(pending_migration);
                pending_migration = kmemdup(req, hdr->length, GFP_KERNEL);
                
                rcu_read_lock();
                deputy = pid_task(find_vpid(req->orig_pid), PIDTYPE_PID);
                if (deputy) get_task_struct(deputy);
                rcu_read_unlock();

                if (deputy) {
                    printk(KERN_INFO "MattX:[RECALL] Found frozen Deputy PID %d. Preparing memory map...\n", deputy->pid);
                    
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
                                printk(KERN_ERR "MattX:[RECALL] Failed to carve VMA at 0x%lx (err: %ld)\n", start, ret_addr);
                            }
                        }
                        kthread_unuse_mm(deputy->mm);
                        printk(KERN_INFO "MattX:[RECALL] Successfully carved %u VMAs into Deputy!\n", pending_migration->vma_count);
                    }

                    printk(KERN_INFO "MattX: [RECALL] Sending READY_FOR_DATA signal to Node %d...\n", pending_source_node);
                    mattx_comm_send(cluster_map[pending_source_node], MATTX_MSG_READY_FOR_DATA, NULL, 0);
                } else {
                    printk(KERN_ERR "MattX: [RECALL] ERROR: Deputy PID %u not found!\n", req->orig_pid);
                }
            }
            break;

        case MATTX_MSG_RETURN_DONE:
            printk(KERN_INFO "MattX:[INCOMING] Return memory transferred successfully! Pages: %d\n", injected_pages_count);
            
            if (hijacked_stub_task && pending_migration) {
                struct pt_regs *regs = task_pt_regs(hijacked_stub_task);
                int i;

                if (regs) {
                    memcpy(regs, &pending_migration->regs, sizeof(struct pt_regs));
                    
                    printk(KERN_INFO "MattX:[AWAKEN] Deputy Brain Restored. New RIP: 0x%lx\n", regs->ip);
                    
                    spin_lock(&export_lock);
                    for (i = 0; i < export_count; i++) {
                        if (export_registry[i].orig_pid == hijacked_stub_task->pid) {
                            remove_export_process(i);
                            break;
                        }
                    }
                    spin_unlock(&export_lock);

                    printk(KERN_INFO "MattX:[AWAKEN] Welcome home! Sending SIGCONT to Deputy PID %d\n", hijacked_stub_task->pid);
                    send_sig(SIGCONT, hijacked_stub_task, 0);
                }
                
                put_task_struct(hijacked_stub_task);
                hijacked_stub_task = NULL;
                kvfree(pending_migration);
                pending_migration = NULL;
                pending_source_node = -1;
            }
            break;

        default:
            printk(KERN_WARNING "MattX: [COMM] Unknown message type: %u\n", hdr->type);
            break;
    }
}

static int mattx_receiver_loop(void *data) {
    struct mattx_link *link = (struct mattx_link *)data;
    struct msghdr msg = {0};
    struct kvec iov[1];
    struct mattx_header hdr;
    int len;
    void *payload = NULL;

    while (!kthread_should_stop()) {
        iov[0].iov_base = &hdr;
        iov[0].iov_len = sizeof(struct mattx_header);
        
        len = kernel_recvmsg(link->sock, &msg, iov, 1, sizeof(struct mattx_header), 0);
        if (len <= 0) break; 

        if (hdr.magic != MATTX_MAGIC) {
            printk(KERN_ERR "MattX: [COMM] FATAL: Stream out of sync! Invalid magic: 0x%x\n", hdr.magic);
            break;
        }

        if (hdr.length > MATTX_MAX_PAYLOAD) {
            printk(KERN_ERR "MattX:[COMM] FATAL: Payload too large (%u bytes).\n", hdr.length);
            break;
        }

        if (hdr.length > 0) {
            payload = kvmalloc(hdr.length, GFP_KERNEL);
            if (payload) {
                iov[0].iov_base = payload;
                iov[0].iov_len = hdr.length;
                
                int payload_len = kernel_recvmsg(link->sock, &msg, iov, 1, hdr.length, MSG_WAITALL);
                if (payload_len != hdr.length) {
                    printk(KERN_ERR "MattX: [COMM] Short read on payload! Expected %u, got %d\n", hdr.length, payload_len);
                }
            }
        }
        
        mattx_handle_message(link, &hdr, payload);
        if (payload) { 
            kvfree(payload); 
            payload = NULL; 
        }
    }
    return 0;
}

int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len) {
    struct msghdr msg = {0};
    struct kvec iov[2];
    struct mattx_header hdr;
    int err;

    hdr.magic = MATTX_MAGIC;
    hdr.type = type;
    hdr.length = len;
    hdr.sender_id = my_node_id;

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = data;
    iov[1].iov_len = len;

    err = kernel_sendmsg(link->sock, &msg, iov, 2, sizeof(hdr) + len);
    return err;
}

static void mattx_setup_link(struct mattx_link *link) {
    link->sk = link->sock->sk;
    tcp_sock_set_nodelay(link->sk);
    link->receiver_thread = kthread_run(mattx_receiver_loop, link, "mattx_recv_%d", link->node_id);
}

struct mattx_link* mattx_comm_connect(__be32 ip_addr, int node_id) {
    struct mattx_link *link;
    struct sockaddr_in addr;
    int err;

    link = kzalloc(sizeof(*link), GFP_KERNEL);
    if (!link) return NULL;

    err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &link->sock);
    if (err < 0) { kfree(link); return NULL; }

    link->node_id = node_id;
    link->ip_addr = ip_addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MATTX_PORT);
    addr.sin_addr.s_addr = ip_addr;

    err = kernel_connect(link->sock, (struct sockaddr *)&addr, sizeof(addr), 0);
    if (err < 0) {
        sock_release(link->sock);
        kfree(link);
        return NULL;
    }

    mattx_setup_link(link);
    return link;
}

int mattx_listener_loop(void *data) {
    struct socket *listen_sock = NULL;
    struct sockaddr_in addr;
    int err;

    err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &listen_sock);
    if (err < 0) return err;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MATTX_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    err = kernel_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (err < 0) { sock_release(listen_sock); return err; }

    err = kernel_listen(listen_sock, 5);
    if (err < 0) { sock_release(listen_sock); return err; }

    while (!kthread_should_stop()) {
        struct socket *client_sock = NULL;
        struct sockaddr_in peer_addr;
        
        err = kernel_accept(listen_sock, &client_sock, 0);
        if (err < 0) {
            if (err == -EAGAIN || err == -EINTR) continue;
            continue;
        }
        struct mattx_link *link = kzalloc(sizeof(*link), GFP_KERNEL);
        if (link) {
            link->sock = client_sock;
            link->node_id = -1; 
            
            if (kernel_getpeername(client_sock, (struct sockaddr *)&peer_addr) >= 0) {
                link->ip_addr = peer_addr.sin_addr.s_addr;
            }
            
            mattx_setup_link(link);
        } else {
            sock_release(client_sock);
        }
    }
    sock_release(listen_sock);
    return 0;
}

void mattx_comm_disconnect(int node_id) {
    if (node_id < 0 || node_id >= MAX_NODES || !cluster_map[node_id]) return;
    if (cluster_map[node_id]->receiver_thread) kthread_stop(cluster_map[node_id]->receiver_thread);
    sock_release(cluster_map[node_id]->sock);
    kfree(cluster_map[node_id]);
    cluster_map[node_id] = NULL;
}

