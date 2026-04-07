#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

// Must match the kernel definitions
enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, __MATTX_ATTR_MAX };
enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, __MATTX_CMD_MAX };

int main() {
    struct nl_sock *sock;
    struct nl_msg *msg;
    int family_id;
    uint32_t my_pid = getpid();

    printf("MattX-Stub: Waking up! My PID is %u. Offering myself for hijack...\n", my_pid);

    sock = nl_socket_alloc();
    genl_connect(sock);
    family_id = genl_ctrl_resolve(sock, "MATTX");
    
    if (family_id < 0) {
        fprintf(stderr, "MattX-Stub: Kernel module not loaded!\n");
        return -1;
    }

    // Send the HIJACK_ME command to the kernel
    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_HIJACK_ME, 1);
    nla_put_u32(msg, MATTX_ATTR_STUB_PID, my_pid);

    if (nl_send_auto(sock, msg) < 0) {
        fprintf(stderr, "MattX-Stub: Failed to send hijack request.\n");
        return -1;
    }

    printf("MattX-Stub: Request sent. Waiting for brain transplant...\n");
    
    // Go to sleep forever. The kernel will overwrite this memory soon anyway!
    while (1) {
        pause(); 
    }

    return 0;
}

