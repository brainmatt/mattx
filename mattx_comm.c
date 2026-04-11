#include "mattx.h"

// Global counter for debugging
static int injected_pages_count = 0;

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
                char *argv[] = { "/usr/local/bin/mattx-stub", NULL };
                char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
                
                pending_source_node = hdr->sender_id;

                printk(KERN_INFO "MattX: [INCOMING] Received Blueprint for PID %u. Saving to pending...\n", req->orig_pid);
                if (pending_migration) kvfree(pending_migration);
                
                pending_migration = kvmalloc(hdr->length, GFP_KERNEL);
                if (pending_migration) {
                    memcpy(pending_migration, req, hdr->length);
                }

                if (call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT) != 0) {
                    printk(KERN_ERR "MattX: [INCOMING] Failed to spawn surrogate!\n");
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
                    printk(KERN_ERR "MattX: [INJECT] Failed to inject %u bytes at 0x%lx (res: %d)\n", ph->length, target_addr, res);
                }
            }
            break;
        case MATTX_MSG_MIGRATE_DONE:
            printk(KERN_INFO "MattX: [INCOMING] All memory transferred successfully!\n");
            
            if (hijacked_stub_task && pending_migration) {
                struct pt_regs *regs;
                struct cred *new_cred;
                const struct cred *old_cred;
                int retries = 50;
                unsigned char rip_buf[8] = {0}; 

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
                    printk(KERN_INFO "MattX:[AWAKEN] Renamed stub to '%s'\n", hijacked_stub_task->comm);
                    
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

                    if (access_process_vm(hijacked_stub_task, regs->ip, rip_buf, 8, FOLL_FORCE) == 8) {
                        printk(KERN_INFO "MattX: [DEBUG] Target RIP (0x%lx) contains: %8ph\n", regs->ip, rip_buf);
                    }

                    printk(KERN_INFO "MattX:[AWAKEN] IT'S ALIVE! Sending SIGCONT to PID %d\n", hijacked_stub_task->pid);
                    send_sig(SIGCONT, hijacked_stub_task, 0);
                    
                    // --- FIXED: Add the full origin info to the registry ---
                    add_guest_process(hijacked_stub_task->pid, pending_migration->orig_pid, pending_source_node);
                    printk(KERN_INFO "MattX:[REGISTRY] PID %d registered as a Guest (Orig: %u, Home: %d).\n", 
                           hijacked_stub_task->pid, pending_migration->orig_pid, pending_source_node);
                }

                put_task_struct(hijacked_stub_task);
                hijacked_stub_task = NULL;
                kvfree(pending_migration);
                pending_migration = NULL;
                pending_source_node = -1;
            }
            break;
            
        // --- NEW: The Funeral Director (Node 1 receives this) ---
        case MATTX_MSG_PROCESS_EXIT:
            if (payload) {
                struct mattx_process_exit *exit_msg = (struct mattx_process_exit *)payload;
                struct task_struct *deputy;

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
                } else {
                    printk(KERN_WARNING "MattX: [FUNERAL] Deputy PID %u not found. Already dead?\n", exit_msg->orig_pid);
                }
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
            printk(KERN_ERR "MattX: [COMM] FATAL: Payload too large (%u bytes).\n", hdr.length);
            break;
        }

        if (hdr.length > 0) {
            payload = kvmalloc(hdr.length, GFP_KERNEL);
            if (payload) {
                iov[0].iov_base = payload;
                iov[0].iov_len = hdr.length;
                
                // Read the payload safely
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
    hdr.sender_id = link->node_id;

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
        err = kernel_accept(listen_sock, &client_sock, 0);
        if (err < 0) {
            if (err == -EAGAIN || -EINTR == err) continue;
            continue;
        }
        struct mattx_link *link = kzalloc(sizeof(*link), GFP_KERNEL);
        if (link) {
            link->sock = client_sock;
            link->node_id = -1; 
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

