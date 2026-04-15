#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/mman.h> 
#include <signal.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

#define MAX_FDS 64

enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, MATTX_ATTR_BLUEPRINT, MATTX_ATTR_MY_NODE_ID, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)
enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, MATTX_CMD_GET_BLUEPRINT, __MATTX_CMD_MAX };

struct mattx_vma_info {
    uint64_t vm_start;
    uint64_t vm_end;
    uint64_t vm_flags;
};

struct mattx_cpu_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10;
    uint64_t r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
};

struct mattx_migration_req {
    uint32_t orig_pid;
    uint32_t uid; 
    uint32_t gid; 
    uint32_t pad;
    struct mattx_cpu_regs regs;
    uint64_t fsbase; 
    uint64_t gsbase; 
    uint64_t arg_start; 
    uint64_t arg_end;   
    char comm[16]; 
    uint32_t fd_count;          // NEW
    uint32_t open_fds[MAX_FDS]; // NEW
    uint32_t vma_count;
    uint32_t pad2;
    struct mattx_vma_info vmas[]; 
};

static struct mattx_migration_req *received_req = NULL;

static int blueprint_cb(struct nl_msg *msg, void *arg) {
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct nlattr *attrs[MATTX_ATTR_MAX + 1];
    struct nla_policy policy[MATTX_ATTR_MAX + 1];
    
    memset(policy, 0, sizeof(policy));
    policy[MATTX_ATTR_BLUEPRINT].type = NLA_BINARY;

    if (genlmsg_parse(nlh, 0, attrs, MATTX_ATTR_MAX, policy) < 0) return NL_SKIP;

    if (attrs[MATTX_ATTR_BLUEPRINT]) {
        struct mattx_migration_req *req = nla_data(attrs[MATTX_ATTR_BLUEPRINT]);
        int len = nla_len(attrs[MATTX_ATTR_BLUEPRINT]);
        
        received_req = malloc(len);
        if (received_req) memcpy(received_req, req, len);
    }
    return NL_OK;
}

int main() {
    freopen("/tmp/mattx_stub.log", "a", stdout);
    freopen("/tmp/mattx_stub.log", "a", stderr);
    setvbuf(stdout, NULL, _IONBF, 0); 
    setvbuf(stderr, NULL, _IONBF, 0);

    struct nl_sock *sock;
    struct nl_msg *msg;
    int family_id;
    uint32_t my_pid = getpid();

    printf("\n========================================\n");
    printf("MattX-Stub: Waking up! PID %u. Requesting blueprint...\n", my_pid);

    sock = nl_socket_alloc();
    genl_connect(sock);
    family_id = genl_ctrl_resolve(sock, "MATTX");
    
    if (family_id < 0) return -1;

    nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, blueprint_cb, NULL);

    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_GET_BLUEPRINT, 1);
    nl_send_auto(sock, msg);
    nlmsg_free(msg);

    int retries = 10;
    while (!received_req && retries > 0) {
        nl_recvmsgs_default(sock);
        retries--;
    }

    if (!received_req) return -1;

    printf("MattX-Stub: Blueprint received. Original PID: %u, Name: '%s', VMAs: %u, FDs: %u\n", 
           received_req->orig_pid, received_req->comm, received_req->vma_count, received_req->fd_count);

    for (uint32_t i = 0; i < received_req->vma_count; i++) {
        struct mattx_vma_info *v = &received_req->vmas[i];
        size_t size = v->vm_end - v->vm_start;
        int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        mmap((void *)v->vm_start, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    
    // --- NEW: Expand the FD table safely in user-space! ---
    for (int i = 3; i < MAX_FDS; i++) {
        dup2(0, i); // Duplicate stdin to every FD up to 64 to force the kernel to allocate the table
    }
    printf("MattX-Stub: Memory carved and FD table expanded. Ready for hijack.\n");

    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_HIJACK_ME, 1);
    nla_put_u32(msg, MATTX_ATTR_STUB_PID, my_pid);
    nl_send_auto(sock, msg);
    nlmsg_free(msg);
    nl_socket_free(sock);
    free(received_req);

    raise(SIGSTOP); 
    while (1) sleep(1); 
    return 0;
}

