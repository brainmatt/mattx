#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

// Must match the kernel module definitions
enum {
    MATTX_ATTR_UNSPEC,
    MATTX_ATTR_NODE_ID,
    MATTX_ATTR_IPV4_ADDR,
    __MATTX_ATTR_MAX,
};

enum {
    MATTX_CMD_UNSPEC,
    MATTX_CMD_NODE_JOIN,
    __MATTX_CMD_MAX,
};

int main() {
    struct nl_sock *sock;
    struct nl_msg *msg;
    int family_id, err;
    u_int32_t fake_ip;

    // 1. Setup socket
    sock = nl_socket_alloc();
    genl_connect(sock);

    // 2. Find the MattX family ID assigned by the kernel
    family_id = genl_ctrl_resolve(sock, "MATTX");
    if (family_id < 0) {
        fprintf(stderr, "Error: MattX kernel module not loaded!\n");
        return -1;
    }

    // 3. Create a new Netlink message
    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_NODE_JOIN, 1);

    // 4. Add our payload (Node ID: 1, IP: 192.168.88.114)
    inet_pton(AF_INET, "192.168.88.114", &fake_ip);
    nla_put_u32(msg, MATTX_ATTR_NODE_ID, 1);
    nla_put_u32(msg, MATTX_ATTR_IPV4_ADDR, fake_ip);

    // 5. Send the message to the kernel
    printf("Sending JOIN command to MattX kernel module...\n");
    err = nl_send_auto(sock, msg);
    if (err < 0) {
        fprintf(stderr, "Failed to send message.\n");
    } else {
        printf("Message sent successfully!\n");
    }

    nlmsg_free(msg);
    nl_socket_free(sock);
    return 0;
}
