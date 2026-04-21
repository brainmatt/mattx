#include "mattx.h"

// An array of function pointers, indexed by the message type (up to 256 types)
static mattx_msg_handler_fn msg_handlers[256] = {NULL};

void mattx_register_handler(u32 type, mattx_msg_handler_fn handler) {
    if (type < 256) {
        msg_handlers[type] = handler;
    } else {
        printk(KERN_ERR "MattX: [COMM] Failed to register handler for type %u (out of bounds)\n", type);
    }
}
EXPORT_SYMBOL(mattx_register_handler);

static void mattx_handle_message(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    // 1. Check if the message type is valid and if a handler is registered
    if (hdr->type < 256 && msg_handlers[hdr->type] != NULL) {
        // 2. Call the registered function!
        msg_handlers[hdr->type](link, hdr, payload);
    } else {
        printk(KERN_WARNING "MattX: [COMM] Unhandled or unknown message type: %u\n", hdr->type);
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

