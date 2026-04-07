#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <net/genetlink.h>
#include <net/sock.h>
#include <linux/in.h>
#include <net/tcp.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/loadavg.h> 
#include <linux/mm.h>            
#include <linux/sched/signal.h>  
#include <linux/sched/task.h>    
#include <linux/rcupdate.h>      
#include <linux/mmap_lock.h>     
#include <asm/ptrace.h>          
#include <linux/umh.h>           // NEW: For call_usermodehelper

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI - Phase 7.3: The Surrogate");

#define MATTX_PORT 7226
#define MAX_NODES 1024 
#define BALANCER_INTERVAL_MS 2000 

#define FIXED_LOAD_1_0 2048
#define FIXED_LOAD_0_2 409
#define MAX_VMAS 256 

// --- The MattX Protocol ---

enum mattx_msg_type {
    MATTX_MSG_HEARTBEAT = 1,
    MATTX_MSG_LOAD_UPDATE,
    MATTX_MSG_MIGRATE_REQ,
    MATTX_MSG_SYSCALL_FWD,
};

struct mattx_header {
    u32 type;
    u32 length;
    u32 sender_id;
};

struct mattx_load_info {
    u32 cpu_load;    
    u32 mem_free_mb; 
};

struct mattx_vma_info {
    unsigned long vm_start;
    unsigned long vm_end;
    unsigned long vm_flags;
};

struct mattx_migration_req {
    u32 orig_pid;
    unsigned long rip;
    unsigned long rsp;
    u32 vma_count;
    struct mattx_vma_info vmas[]; 
};

struct mattx_link {
    int node_id;
    struct socket *sock;
    struct sock *sk;
    struct task_struct *receiver_thread;
};

static struct mattx_load_info cluster_load_table[MAX_NODES];
static struct mattx_link *cluster_map[MAX_NODES];
static struct task_struct *balancer_thread;
static struct task_struct *listener_thread;

// NEW: Global pointer to hold the blueprint while we wait for the stub
static struct mattx_migration_req *pending_migration = NULL;

// --- Receiver Logic ---

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
                int ret;

                printk(KERN_INFO "MattX: [INCOMING] Received Blueprint for PID %u. Saving to pending...\n", req->orig_pid);
                
                // Save the blueprint globally (For now, we assume 1 migration at a time)
                if (pending_migration) kfree(pending_migration);
                pending_migration = kmemdup(req, hdr->length, GFP_KERNEL);

                // Spawn the Surrogate!
                printk(KERN_INFO "MattX: [INCOMING] Spawning surrogate (/usr/local/bin/mattx-stub)...\n");
                ret = call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
                if (ret != 0) {
                    printk(KERN_ERR "MattX: [INCOMING] Failed to spawn surrogate! Error: %d\n", ret);
                }
            }
            break;
        default:
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

        if (hdr.length > 0) {
            payload = kmalloc(hdr.length, GFP_KERNEL);
            if (payload) {
                iov[0].iov_base = payload;
                iov[0].iov_len = hdr.length;
                kernel_recvmsg(link->sock, &msg, iov, 1, hdr.length, 0);
            }
        }
        mattx_handle_message(link, &hdr, payload);
        if (payload) { kfree(payload); payload = NULL; }
    }
    return 0;
}

// --- Communication Logic ---

static int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len) {
    struct msghdr msg = {0};
    struct kvec iov[2];
    struct mattx_header hdr = { .type = type, .length = len, .sender_id = link->node_id };
    int err;

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

static struct mattx_link* mattx_comm_connect(__be32 ip_addr, int node_id) {
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

// --- Phase 6.1: The SMART Process Selector ---

static struct task_struct* mattx_find_candidate_task(void) {
    struct task_struct *p;
    struct task_struct *best_candidate = NULL;
    u64 max_runtime = 0;

    rcu_read_lock();
    for_each_process(p) {
        if ((p->flags & PF_KTHREAD) || !p->mm) continue;
        if (p->pid <= 1 || (p->flags & PF_EXITING)) continue;
        if (strcmp(p->comm, "mattx-discd") == 0) continue;
        if (strcmp(p->comm, "mattx-stub") == 0) continue; // Don't migrate the stub!
        if (READ_ONCE(p->__state) != TASK_RUNNING) continue;

        if (p->se.sum_exec_runtime > max_runtime) {
            max_runtime = p->se.sum_exec_runtime;
            best_candidate = p;
        }
    }
    if (best_candidate) get_task_struct(best_candidate);
    rcu_read_unlock();
    return best_candidate;
}

// --- Phase 7.2: The Blueprint Teleporter ---

static void mattx_capture_and_send_state(struct task_struct *task, int target_node) {
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
    }
    kfree(req);
}

// --- The Decision Engine (Load Balancer) ---

static void mattx_evaluate_and_balance(u32 local_cpu_load) {
    int i;
    int best_node = -1;
    u32 lowest_remote_load = 0xFFFFFFFF;

    for (i = 0; i < MAX_NODES; i++) {
        if (cluster_map[i] && cluster_map[i]->node_id != -1) {
            u32 remote_load = cluster_load_table[i].cpu_load;
            if (remote_load < lowest_remote_load) {
                lowest_remote_load = remote_load;
                best_node = i;
            }
        }
    }

    if (best_node != -1) {
        int cond_a = (local_cpu_load > FIXED_LOAD_1_0 && lowest_remote_load < FIXED_LOAD_1_0);
        int cond_b = (local_cpu_load > lowest_remote_load && (local_cpu_load - lowest_remote_load) > FIXED_LOAD_0_2);

        if (cond_a || cond_b) {
            struct task_struct *task = mattx_find_candidate_task();
            if (task) {
                printk(KERN_INFO "MattX:[MIGRATE] Selected PID %d (%s) for migration to Node %d!\n", 
                       task->pid, task->comm, best_node);
                mattx_capture_and_send_state(task, best_node);
                put_task_struct(task); 
                msleep(10000); 
            }
        }
    }
}

static int mattx_balancer_loop(void *data) {
    struct mattx_load_info local_load;
    int i;
    while (!kthread_should_stop()) {
        local_load.cpu_load = (u32)avenrun[0]; 
        local_load.mem_free_mb = (u32)(si_mem_available() >> 20);
        for (i = 0; i < MAX_NODES; i++) {
            if (cluster_map[i] && cluster_map[i]->node_id != -1) {
                mattx_comm_send(cluster_map[i], MATTX_MSG_LOAD_UPDATE, &local_load, sizeof(local_load));
            }
        }
        mattx_evaluate_and_balance(local_load.cpu_load);
        msleep(BALANCER_INTERVAL_MS);
    }
    return 0;
}

// --- The Listener (Server) Logic ---

static int mattx_listener_loop(void *data) {
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
            if (err == -EAGAIN || err == -EINTR) continue;
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

static void mattx_comm_disconnect(int node_id) {
    if (node_id < 0 || node_id >= MAX_NODES || !cluster_map[node_id]) return;
    if (cluster_map[node_id]->receiver_thread) kthread_stop(cluster_map[node_id]->receiver_thread);
    sock_release(cluster_map[node_id]->sock);
    kfree(cluster_map[node_id]);
    cluster_map[node_id] = NULL;
}

// --- Netlink Interface ---

enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)
enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, __MATTX_CMD_MAX };
#define MATTX_CMD_MAX (__MATTX_CMD_MAX - 1)

static const struct nla_policy mattx_genl_policy[MATTX_ATTR_MAX + 1] = {
    [MATTX_ATTR_NODE_ID] = { .type = NLA_U32 }, 
    [MATTX_ATTR_IPV4_ADDR] = { .type = NLA_U32 },
    [MATTX_ATTR_STUB_PID] = { .type = NLA_U32 },
};

static int mattx_nl_cmd_node_join(struct sk_buff *skb, struct genl_info *info) {
    u32 node_id = nla_get_u32(info->attrs[MATTX_ATTR_NODE_ID]);
    u32 ip_addr = nla_get_u32(info->attrs[MATTX_ATTR_IPV4_ADDR]);
    struct mattx_link *link = NULL;

    if (node_id >= MAX_NODES) return -EINVAL;
    if (cluster_map[node_id]) mattx_comm_disconnect(node_id);
    
    link = mattx_comm_connect(ip_addr, node_id);
    if (link) {
        cluster_map[node_id] = link;
        mattx_comm_send(link, MATTX_MSG_HEARTBEAT, NULL, 0);
        return 0;
    }
    return -ECONNREFUSED;
}

static int mattx_nl_cmd_node_leave(struct sk_buff *skb, struct genl_info *info) {
    mattx_comm_disconnect(nla_get_u32(info->attrs[MATTX_ATTR_NODE_ID]));
    return 0;
}

// NEW: The Hijack Handler
static int mattx_nl_cmd_hijack_me(struct sk_buff *skb, struct genl_info *info) {
    u32 stub_pid;
    struct task_struct *stub_task;

    if (!info->attrs[MATTX_ATTR_STUB_PID]) return -EINVAL;
    stub_pid = nla_get_u32(info->attrs[MATTX_ATTR_STUB_PID]);

    printk(KERN_INFO "MattX: [HIJACK] Received hijack request from Stub PID %u\n", stub_pid);

    if (!pending_migration) {
        printk(KERN_ERR "MattX: [HIJACK] Error: No pending migration blueprint found!\n");
        return -ENOENT;
    }

    // Find the stub task in the kernel
    rcu_read_lock();
    stub_task = pid_task(find_vpid(stub_pid), PIDTYPE_PID);
    if (stub_task) get_task_struct(stub_task);
    rcu_read_unlock();

    if (!stub_task) {
        printk(KERN_ERR "MattX: [HIJACK] Error: Could not find task for PID %u\n", stub_pid);
        return -ESRCH;
    }

    printk(KERN_INFO "MattX: [HIJACK] SUCCESS! Stub PID %u is ready to become the new process (Original PID: %u, Target RIP: 0x%lx)\n", 
           stub_pid, pending_migration->orig_pid, pending_migration->rip);

    // TODO Phase 7.4: Overwrite the stub's memory and registers!

    put_task_struct(stub_task);
    return 0;
}

static const struct genl_ops mattx_genl_ops[] = {
    { .cmd = MATTX_CMD_NODE_JOIN, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_node_join },
    { .cmd = MATTX_CMD_NODE_LEAVE, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_node_leave },
    { .cmd = MATTX_CMD_HIJACK_ME, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_hijack_me }, // NEW
};

static struct genl_family mattx_genl_family = {
    .name = "MATTX", .version = 1, .maxattr = MATTX_ATTR_MAX, .ops = mattx_genl_ops, .n_ops = ARRAY_SIZE(mattx_genl_ops),
};

static int __init mattx_init(void) { 
    int rc = genl_register_family(&mattx_genl_family);
    if (rc) return rc;

    balancer_thread = kthread_run(mattx_balancer_loop, NULL, "mattx_balancer");
    listener_thread = kthread_run(mattx_listener_loop, NULL, "mattx_listener");
    
    if (IS_ERR(balancer_thread) || IS_ERR(listener_thread)) {
        genl_unregister_family(&mattx_genl_family);
        return -ENOMEM;
    }

    return 0; 
}

static void __exit mattx_exit(void) {
    int i;
    if (balancer_thread) kthread_stop(balancer_thread);
    if (listener_thread) kthread_stop(listener_thread);
    for (i = 0; i < MAX_NODES; i++) mattx_comm_disconnect(i);
    if (pending_migration) kfree(pending_migration);
    genl_unregister_family(&mattx_genl_family);
}

module_init(mattx_init);
module_exit(mattx_exit);


