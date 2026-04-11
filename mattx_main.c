#include "mattx.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI - Core Module");

struct mattx_load_info cluster_load_table[MAX_NODES];
struct mattx_link *cluster_map[MAX_NODES];
struct mattx_migration_req *pending_migration = NULL;

int pending_source_node = -1;
struct task_struct *hijacked_stub_task = NULL;

static struct task_struct *balancer_thread;
static struct task_struct *listener_thread;

// --- Guest Registry Implementation ---
struct mattx_guest_info guest_registry[MAX_GUESTS];
int guest_count = 0;
DEFINE_SPINLOCK(guest_lock);

bool is_guest_process(pid_t pid) {
    int i;
    bool found = false;
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            found = true;
            break;
        }
    }
    spin_unlock(&guest_lock);
    return found;
}

void add_guest_process(pid_t local_pid, u32 orig_pid, int home_node) {
    spin_lock(&guest_lock);
    if (guest_count < MAX_GUESTS) {
        guest_registry[guest_count].local_pid = local_pid;
        guest_registry[guest_count].orig_pid = orig_pid;
        guest_registry[guest_count].home_node = home_node;
        guest_count++;
    } else {
        printk(KERN_WARNING "MattX: [REGISTRY] Guest registry is full!\n");
    }
    spin_unlock(&guest_lock);
}

void remove_guest_process(int index) {
    // Note: Must be called with guest_lock held!
    if (index < 0 || index >= guest_count) return;
    // Swap with the last element to keep array contiguous
    guest_registry[index] = guest_registry[guest_count - 1];
    guest_count--;
}
// ------------------------------------------

enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, MATTX_ATTR_BLUEPRINT, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)
enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, MATTX_CMD_GET_BLUEPRINT, __MATTX_CMD_MAX };
#define MATTX_CMD_MAX (__MATTX_CMD_MAX - 1)

static const struct nla_policy mattx_genl_policy[MATTX_ATTR_MAX + 1] = {
    [MATTX_ATTR_NODE_ID] = { .type = NLA_U32 },[MATTX_ATTR_IPV4_ADDR] = { .type = NLA_U32 },[MATTX_ATTR_STUB_PID] = { .type = NLA_U32 },[MATTX_ATTR_BLUEPRINT] = { .type = NLA_BINARY },
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

static int mattx_nl_cmd_get_blueprint(struct sk_buff *skb, struct genl_info *info) {
    struct sk_buff *reply_skb;
    void *msg_head;
    int len;

    if (!pending_migration) {
        printk(KERN_ERR "MattX: [NL] Stub requested blueprint, but pending_migration is NULL!\n");
        return -ENOENT;
    }

    len = sizeof(struct mattx_migration_req) + (pending_migration->vma_count * sizeof(struct mattx_vma_info));
    
    reply_skb = nlmsg_new(nla_total_size(len), GFP_KERNEL);
    if (!reply_skb) return -ENOMEM;

    msg_head = genlmsg_put_reply(reply_skb, info, &mattx_genl_family, 0, MATTX_CMD_GET_BLUEPRINT);
    if (!msg_head) {
        nlmsg_free(reply_skb);
        return -ENOMEM;
    }

    if (nla_put(reply_skb, MATTX_ATTR_BLUEPRINT, len, pending_migration)) {
        genlmsg_cancel(reply_skb, msg_head);
        nlmsg_free(reply_skb);
        return -EMSGSIZE;
    }

    genlmsg_end(reply_skb, msg_head);
    printk(KERN_INFO "MattX: [NL] Sending Blueprint (%d bytes) back to Stub PID %u\n", len, info->snd_portid);
    return genlmsg_reply(reply_skb, info);
}

static int mattx_nl_cmd_hijack_me(struct sk_buff *skb, struct genl_info *info) {
    u32 stub_pid = nla_get_u32(info->attrs[MATTX_ATTR_STUB_PID]);
    struct task_struct *stub_task = NULL;

    if (!pending_migration) return -ENOENT;

    rcu_read_lock();
    stub_task = pid_task(find_vpid(stub_pid), PIDTYPE_PID);
    if (stub_task) get_task_struct(stub_task);
    rcu_read_unlock();

    if (!stub_task) return -ESRCH;

    if (hijacked_stub_task) put_task_struct(hijacked_stub_task);
    hijacked_stub_task = stub_task;

    printk(KERN_INFO "MattX: [HIJACK] SUCCESS! Stub PID %u is carved and ready.\n", stub_pid);

    if (pending_source_node != -1 && cluster_map[pending_source_node]) {
        printk(KERN_INFO "MattX: [HIJACK] Sending READY_FOR_DATA signal to Node %d...\n", pending_source_node);
        mattx_comm_send(cluster_map[pending_source_node], MATTX_MSG_READY_FOR_DATA, NULL, 0);
    }

    return 0;
}

static const struct genl_ops mattx_genl_ops[] = {
    { .cmd = MATTX_CMD_NODE_JOIN, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_node_join },
    { .cmd = MATTX_CMD_NODE_LEAVE, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_node_leave },
    { .cmd = MATTX_CMD_GET_BLUEPRINT, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_get_blueprint },
    { .cmd = MATTX_CMD_HIJACK_ME, .policy = mattx_genl_policy, .doit = mattx_nl_cmd_hijack_me },
};

struct genl_family mattx_genl_family = {
    .name = "MATTX", .version = 1, .maxattr = MATTX_ATTR_MAX, .ops = mattx_genl_ops, .n_ops = ARRAY_SIZE(mattx_genl_ops),
};

static int __init mattx_init(void) { 
    int rc = genl_register_family(&mattx_genl_family);
    if (rc) return rc;
    
    spin_lock_init(&guest_lock); 
    
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
    if (pending_migration) kvfree(pending_migration);
    if (hijacked_stub_task) put_task_struct(hijacked_stub_task);
    genl_unregister_family(&mattx_genl_family);
}

module_init(mattx_init);
module_exit(mattx_exit);

