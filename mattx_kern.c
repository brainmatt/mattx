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
#include <linux/sched/loadavg.h> // For avenrun
#include <linux/mm.h>            // For si_mem_available

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI - Load Monitoring Phase");

#define MATTX_PORT 7226
#define MAX_NODES 64
#define LOAD_UPDATE_INTERVAL_MS 2000 

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

struct mattx_link {
    int node_id;
    struct socket *sock;
    struct sock *sk;
    struct task_struct *receiver_thread;
};

static struct mattx_load_info cluster_load_table[MAX_NODES];
static struct mattx_link *cluster_map[MAX_NODES];
static struct task_struct *broadcaster_thread;

// --- Receiver Logic ---

static void mattx_handle_message(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    switch (hdr->type) {
        case MATTX_MSG_HEARTBEAT:
            printk(KERN_INFO "MattX: Heartbeat received from Node %u\n", hdr->sender_id);
            break;
        case MATTX_MSG_LOAD_UPDATE:
            if (hdr->sender_id < MAX_NODES && payload) {
                struct mattx_load_info *load = (struct mattx_load_info *)payload;
                cluster_load_table[hdr->sender_id] = *load;
                printk(KERN_INFO "MattX: Node %u Load Update -> CPU: %u, MemFree: %u MB\n", 
                       hdr->sender_id, load->cpu_load, load->mem_free_mb);
            }
            break;
        default:
            printk(KERN_WARNING "MattX: Unknown message type %u from Node %u\n", hdr->type, hdr->sender_id);
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

int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len) {
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

// --- Broadcaster Logic ---

static int mattx_broadcaster_loop(void *data) {
    struct mattx_load_info local_load;
    int i;

    printk(KERN_INFO "MattX: Broadcaster thread started\n");

    while (!kthread_should_stop()) {
        // FIX 1: avenrun[0] is the 1-minute load average (unsigned long)
        local_load.cpu_load = (u32)avenrun[0]; 
        
        // FIX 2: si_mem_available is a function in modern kernels
        local_load.mem_free_mb = (u32)(si_mem_available() >> 20);

        for (i = 0; i < MAX_NODES; i++) {
            if (cluster_map[i]) {
                mattx_comm_send(cluster_map[i], MATTX_MSG_LOAD_UPDATE, &local_load, sizeof(local_load));
            }
        }

        msleep(LOAD_UPDATE_INTERVAL_MS);
    }
    return 0;
}

static struct mattx_link* mattx_comm_connect(__be32 ip_addr, int node_id) {
    struct mattx_link *link;
    struct sockaddr_in addr;
    int err;

    link = kzalloc(sizeof(*link), GFP_KERNEL);
    if (!link) return NULL;

    err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &link->sock);
    if (err < 0) { kfree(link); return NULL; }

    link->sk = link->sock->sk;
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

    tcp_sock_set_nodelay(link->sk);
    link->receiver_thread = kthread_run(mattx_receiver_loop, link, "mattx_recv_%d", node_id);

    return link;
}

static void mattx_comm_disconnect(int node_id) {
    if (node_id < 0 || node_id >= MAX_NODES || !cluster_map[node_id]) return;
    if (cluster_map[node_id]->receiver_thread) kthread_stop(cluster_map[node_id]->receiver_thread);
    sock_release(cluster_map[node_id]->sock);
    kfree(cluster_map[node_id]);
    cluster_map[node_id] = NULL;
}

// --- Netlink Interface ---

enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)
enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, __MATTX_CMD_MAX };
#define MATTX_CMD_MAX (__MATTX_CMD_MAX - 1)

static const struct nla_policy mattx_genl_policy[MATTX_ATTR_MAX + 1] = {
    [MATTX_ATTR_NODE_ID] = { .type = NLA_U32 }, [MATTX_ATTR_IPV4_ADDR] = { .type = NLA_U32 },
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

static const struct genl_ops mattx_genl_ops[] = {
    { .cmd = MATTX_CMD_NODE_JOIN, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_node_join },
    { .cmd = MATTX_CMD_NODE_LEAVE, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_node_leave },
};

static struct genl_family mattx_genl_family = {
    .name = "MATTX", .version = 1, .maxattr = MATTX_ATTR_MAX, .ops = mattx_genl_ops, .n_ops = ARRAY_SIZE(mattx_genl_ops),
};

static int __init mattx_init(void) { 
    int rc = genl_register_family(&mattx_genl_family);
    if (rc) return rc;

    broadcaster_thread = kthread_run(mattx_broadcaster_loop, NULL, "mattx_broadcaster");
    if (IS_ERR(broadcaster_thread)) {
        genl_unregister_family(&mattx_genl_family);
        return PTR_ERR(broadcaster_thread);
    }

    return 0; 
}

static void __exit mattx_exit(void) {
    int i;
    if (broadcaster_thread) kthread_stop(broadcaster_thread);
    for (i = 0; i < MAX_NODES; i++) mattx_comm_disconnect(i);
    genl_unregister_family(&mattx_genl_family);
}

module_init(mattx_init);
module_exit(mattx_exit);