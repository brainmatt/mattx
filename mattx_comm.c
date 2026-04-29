#include "mattx.h"

// --- The Dispatch Table ---
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

// --- NEW: The Bulletproof TCP Reader ---
static int mattx_recv_exact(struct socket *sock, void *buf, size_t len) {
    struct msghdr msg = {0};
    struct kvec iov;
    int received = 0;
    int ret;

    while (received < len) {
        iov.iov_base = buf + received;
        iov.iov_len = len - received;
        
        ret = kernel_recvmsg(sock, &msg, &iov, 1, iov.iov_len, MSG_WAITALL);
        if (ret <= 0) {
            return ret; 
        }
        received += ret;
    }
    return received;
}

static int mattx_receiver_loop(void *data) {
    struct mattx_link *link = (struct mattx_link *)data;
    struct mattx_header hdr;
    int ret;
    void *payload = NULL;

    while (!kthread_should_stop()) {
        // 1. Read exact header
        ret = mattx_recv_exact(link->sock, &hdr, sizeof(struct mattx_header));
        if (ret <= 0) {
            if (ret < 0 && ret != -ERESTARTSYS && ret != -EINTR)
                printk(KERN_ERR "MattX: [COMM] Socket error on Node %d (ret: %d)\n", link->node_id, ret);
            break; 
        }

        // 2. Validate Magic
        if (hdr.magic != MATTX_MAGIC) {
            printk(KERN_ERR "MattX:[COMM] FATAL: Stream out of sync! Invalid magic: 0x%x\n", hdr.magic);
            break;
        }

        // 3. Validate Length
        if (hdr.length > MATTX_MAX_PAYLOAD) {
            printk(KERN_ERR "MattX:[COMM] FATAL: Payload too large (%u bytes).\n", hdr.length);
            break;
        }

        // 4. Read exact payload
        if (hdr.length > 0) {
            payload = kvmalloc(hdr.length, GFP_KERNEL);
            if (!payload) {
                printk(KERN_ERR "MattX: [COMM] FATAL: OOM allocating payload (%u bytes)\n", hdr.length);
                break;
            }
            
            ret = mattx_recv_exact(link->sock, payload, hdr.length);
            if (ret <= 0) {
                printk(KERN_ERR "MattX: [COMM] Socket error during payload read (ret: %d)\n", ret);
                kvfree(payload);
                break;
            }
        }
        
        mattx_handle_message(link, &hdr, payload);
        if (payload) { 
            kvfree(payload); 
            payload = NULL; 
        }
    }
    
    printk(KERN_INFO "MattX: [COMM] Receiver thread exiting for Node %d\n", link->node_id);
    return 0;
}

// --- NEW: Global Mutex to prevent TCP stream collisions! ---
static DEFINE_MUTEX(mattx_send_mutex);

int mattx_comm_send(struct mattx_link *link, u32 type, void *data, u32 len) {
    struct msghdr msg = {0};
    struct kvec iov;
    struct mattx_header *hdr;
    int total_len = sizeof(struct mattx_header) + len;
    int sent = 0;
    int ret;
    char *buf;

    // 1. Allocate a single, contiguous buffer for the entire packet
    buf = kvmalloc(total_len, GFP_KERNEL);
    if (!buf) return -ENOMEM;

    // 2. Pack the header
    hdr = (struct mattx_header *)buf;
    hdr->magic = MATTX_MAGIC;
    hdr->type = type;
    hdr->length = len;
    hdr->sender_id = my_node_id;

    // 3. Pack the payload right behind the header
    if (len > 0 && data) {
        memcpy(buf + sizeof(struct mattx_header), data, len);
    }

    // 4. LOCK THE SOCKET! No other thread can send until we are done.
    mutex_lock(&mattx_send_mutex);
    
    // 5. Loop until every single byte is pushed into the TCP stack
    while (sent < total_len) {
        iov.iov_base = buf + sent;
        iov.iov_len = total_len - sent;
        
        ret = kernel_sendmsg(link->sock, &msg, &iov, 1, iov.iov_len);
        if (ret <= 0) {
            printk(KERN_ERR "MattX:[COMM] Network send failed! (ret: %d)\n", ret);
            mutex_unlock(&mattx_send_mutex);
            kvfree(buf);
            return ret;
        }
        sent += ret;
    }
    
    mutex_unlock(&mattx_send_mutex);
    
    kvfree(buf);
    return sent;
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

