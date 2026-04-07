#include "mattx.h"

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
                
                printk(KERN_INFO "MattX: [INCOMING] Received Blueprint for PID %u. Saving to pending...\n", req->orig_pid);
                if (pending_migration) kfree(pending_migration);
                pending_migration = kmemdup(req, hdr->length, GFP_KERNEL);

                if (call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT) != 0) {
                    printk(KERN_ERR "MattX: [INCOMING] Failed to spawn surrogate!\n");
                }
            }
            break;
        case MATTX_MSG_PAGE_TRANSFER:
            if (payload) {
                struct mattx_page_header *ph = (struct mattx_page_header *)payload;
                if (ph->offset % (PAGE_SIZE * 100) == 0) {
                    printk(KERN_INFO "MattX: [INCOMING] Page data for VMA %u at offset %u\n", ph->vma_index, ph->offset);
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

