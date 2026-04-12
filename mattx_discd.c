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
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

#define CONFIG_FILE "/etc/mattx.conf"
#define DEFAULT_GROUP "239.0.0.1"
#define DEFAULT_PORT 7225
#define DEFAULT_IFACE "eth0"

// Must match the kernel definitions
// FIXED: Added MATTX_ATTR_MY_NODE_ID here!
enum { MATTX_ATTR_UNSPEC, MATTX_ATTR_NODE_ID, MATTX_ATTR_IPV4_ADDR, MATTX_ATTR_STUB_PID, MATTX_ATTR_BLUEPRINT, MATTX_ATTR_MY_NODE_ID, __MATTX_ATTR_MAX };
#define MATTX_ATTR_MAX (__MATTX_ATTR_MAX - 1)

enum { MATTX_CMD_UNSPEC, MATTX_CMD_NODE_JOIN, MATTX_CMD_NODE_LEAVE, MATTX_CMD_HIJACK_ME, MATTX_CMD_GET_BLUEPRINT, __MATTX_CMD_MAX };
#define MATTX_CMD_MAX (__MATTX_CMD_MAX - 1)

struct mattx_config {
    char interface[IFNAMSIZ];
    char group[INET_ADDRSTRLEN];
    int port;
    uint32_t node_id;
};

struct mattx_beacon {
    uint32_t node_id;
    uint32_t ip_addr;
};

struct mattx_config config;
struct nl_sock *nl_sock = NULL;
int mattx_family_id = -1;

// Simple list to track known nodes to avoid spamming the kernel
uint32_t known_nodes[64];
int known_nodes_count = 0;

void load_config();
uint32_t generate_node_id(const char *iface);
uint32_t get_interface_ip(const char *iface);

// --- Netlink Logic ---

int init_netlink() {
    nl_sock = nl_socket_alloc();
    if (!nl_sock) return -1;
    
    if (genl_connect(nl_sock) < 0) return -1;
    
    mattx_family_id = genl_ctrl_resolve(nl_sock, "MATTX");
    if (mattx_family_id < 0) return -1;
    
    printf("Netlink initialized. Family ID: %d\n", mattx_family_id);
    fflush(stdout);
    return 0;
}

void trigger_kernel_join(uint32_t node_id, uint32_t ip_addr) {
    struct nl_sock *sock = nl_socket_alloc();
    struct nl_msg *msg;
    int family_id;

    genl_connect(sock);
    family_id = genl_ctrl_resolve(sock, "MATTX");
    if (family_id < 0) {
        fprintf(stderr, "Kernel module not loaded!\n");
        nl_socket_free(sock);
        return;
    }

    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family_id, 0, 0, MATTX_CMD_NODE_JOIN, 1);
    nla_put_u32(msg, MATTX_ATTR_NODE_ID, node_id);
    nla_put_u32(msg, MATTX_ATTR_IPV4_ADDR, ip_addr);
    
    // NEW: Tell the kernel who we are!
    nla_put_u32(msg, MATTX_ATTR_MY_NODE_ID, config.node_id);

    if (nl_send_auto(sock, msg) < 0) {
        fprintf(stderr, "Failed to send JOIN to kernel\n");
    } else {
        printf("Triggered kernel JOIN for Node %u (%s)\n", node_id, inet_ntoa(*(struct in_addr*)&ip_addr));
    }

    nlmsg_free(msg);
    nl_socket_free(sock);
}

// --- Helper Functions ---

uint32_t generate_node_id(const char *iface) {
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    strncpy(ifr.ifr_name, iface, IFNAMSIZ);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) { close(s); return rand() % 1000; }
    close(s);
    uint32_t id = 0;
    for(int i=0; i<6; i++) id = (id << 8) | ((unsigned char*)ifr.ifr_hwaddr.sa_data)[i];
    return id % 1000;
}

uint32_t get_interface_ip(const char *iface) {
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    strncpy(ifr.ifr_name, iface, IFNAMSIZ);
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) { close(s); return 0; }
    close(s);
    return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
}

void load_config() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        strcpy(config.interface, DEFAULT_IFACE);
        strcpy(config.group, DEFAULT_GROUP);
        config.port = DEFAULT_PORT;
        config.node_id = 0;
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "INTERFACE=%s", config.interface)) continue;
        if (sscanf(line, "MULTICAST_GROUP=%s", config.group)) continue;
        if (sscanf(line, "PORT=%d", &config.port)) continue;
        if (sscanf(line, "NODE_ID=%u", &config.node_id)) continue;
    }
    fclose(fp);
    if (config.node_id == 0) config.node_id = generate_node_id(config.interface);
}

// --- Thread: Send Beacons ---

void* beacon_sender(void* arg) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    struct mattx_beacon beacon;
    uint32_t my_ip = get_interface_ip(config.interface);
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = inet_addr(config.group);
    
    beacon.node_id = config.node_id;
    beacon.ip_addr = my_ip;
    
    while (1) {
        sendto(s, &beacon, sizeof(beacon), 0, (struct sockaddr*)&addr, sizeof(addr));
        sleep(5);
    }
    return NULL;
}

// --- Main: Listen for Beacons ---

int main() {
    int s;
    struct sockaddr_in addr, group_addr;
    struct ip_mreq mreq;
    struct mattx_beacon beacon;
    pthread_t tid;

    load_config();
    
    if (init_netlink() < 0) {
        fprintf(stderr, "Critical Error: Could not initialize Netlink. Exiting.\n");
        return -1;
    }

    s = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr*)&addr, sizeof(addr));

    memset(&group_addr, 0, sizeof(group_addr));
    group_addr.sin_family = AF_INET;
    group_addr.sin_addr.s_addr = inet_addr(config.group);
    
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = group_addr.sin_addr.s_addr;
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    pthread_create(&tid, NULL, beacon_sender, NULL);

    printf("MattX Discovery Daemon running. My Node ID: %u\n", config.node_id);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        if (recvfrom(s, &beacon, sizeof(beacon), 0, (struct sockaddr*)&client_addr, &addr_len) > 0) {
            if (beacon.node_id != config.node_id) {
                printf("DEBUG: Node %u is different from mine (%u). Triggering JOIN...\n", beacon.node_id, config.node_id);
                fflush(stdout);
                trigger_kernel_join(beacon.node_id, beacon.ip_addr);
            }
        }
    }
    return 0;
}

