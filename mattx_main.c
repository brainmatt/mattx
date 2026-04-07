#include "mattx.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Rechenburg & AI Copilot");
MODULE_DESCRIPTION("MattX SSI - Core Module");

// Global State Definitions
struct mattx_load_info cluster_load_table[MAX_NODES];
struct mattx_link *cluster_map[MAX_NODES];
struct mattx_migration_req *pending_migration = NULL;

static struct task_struct *balancer_thread;
static struct task_struct *listener_thread;

// --- Netlink Interface ---
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
    void *msg_head;
    int len;

    if (!pending_migration) return -ENOENT;

    len = sizeof(struct mattx_migration_req) + (pending_migration->vma_count * sizeof(struct mattx_vma_info));
    
    // FIXED: Using info->snd_portid and info->snd_seq so libnl accepts the reply
    msg_head = genlmsg_put(skb, info->snd_portid, info->snd_seq, &mattx_genl_family, 0, MATTX_CMD_GET_BLUEPRINT);
    if (!msg_head) return -ENOMEM;

    nla_put(msg_head, MATTX_ATTR_BLUEPRINT, len, pending_migration);
    return 0; 
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

    printk(KERN_INFO "MattX: [HIJACK] Starting Memory Injection for Stub PID %u...\n", stub_pid);

    mmap_write_lock(stub_task->mm);
    for (int i = 0; i < pending_migration->vma_count; i++) {
        if (mattx_inject_vma_data(stub_task->mm, &pending_migration->vmas[i]) < 0) {
            printk(KERN_ERR "MattX: [HIJACK] Failed to inject VMA %d\n", i);
        }
    }
    mmap_write_unlock(stub_task->mm);

    printk(KERN_INFO "MattX: [HIJACK] SUCCESS! Memory injected into Stub PID %u\n", stub_pid);

    put_task_struct(stub_task);
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


