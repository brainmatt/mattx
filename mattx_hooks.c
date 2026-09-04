/*
 * MattX - The Modern Single System Image (SSI) Cluster
 * 
 * Copyright (c) 2026 by Matthias Rechenburg
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Commercial licensing options are available upon request.
 */
 
#include "mattx.h"
#include <linux/wait.h> 
#include <linux/poll.h>
#include <linux/eventpoll.h>

// Helper for modern x86_64 syscall wrappers (__x64_sys_*)
// The first argument (regs->di) is a pointer to the real pt_regs!
#define SYSCALL_REGS(regs) ((struct pt_regs *)(regs)->di)

extern char config_migration_excludes[512];
extern char config_migration_includes[512];


static struct kretprobe openat_kprobe;

// --- IPC WORMHOLE: Read/Write Kprobes ---
struct rw_kretprobe_data {
    int fd;
    void __user *buf;
    size_t count;
    bool is_wormhole;
};

static struct kretprobe read_kprobe;
static struct kretprobe write_kprobe;


static void mattx_rpc_worker(struct work_struct *work) {
    struct mattx_rpc_work *rpc = container_of(work, struct mattx_rpc_work, work);
    int i;
    int remote_fd = -1;
    
    // LSEEK WORKER ---
    if (rpc->is_lseek) {
        struct mattx_sys_lseek_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->remote_fd, .offset = rpc->offset, .whence = rpc->whence };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_LSEEK_REQ, &req, sizeof(req));

    // FSYNC WORKER ---
    } else if (rpc->is_fsync) {
        struct mattx_sys_fsync_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->remote_fd, .datasync = rpc->datasync };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_FSYNC_REQ, &req, sizeof(req));

    // STATX WORKER ---
    } else if (rpc->is_statx) {
        struct mattx_sys_statx_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->remote_fd, .mask = rpc->mask, .flags = rpc->flags };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_STATX_REQ, &req, sizeof(req));
        
    } else if (rpc->is_dup) {
        struct mattx_sys_dup_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.old_remote_fd = rpc->remote_fd;
        req.new_local_fd = rpc->new_local_fd;

        mattx_dbg("[RPC] Worker started for PID %d. Sending DUP_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_DUP_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_unlink) {
        struct mattx_sys_unlink_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.req_id = 0; // 0 means this is a Kprobe request, not a VFS request!
        strncpy(req.path, rpc->filename, sizeof(req.path) - 1);

        mattx_dbg("[RPC] Worker started for PID %d. Sending UNLINK_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_UNLINK_REQ, &req, sizeof(req));
        }        
    } else if (rpc->is_socket) {
        struct mattx_sys_socket_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.domain = rpc->domain;
        req.type = rpc->type;
        req.protocol = rpc->protocol;

        mattx_dbg("[RPC] Worker started for PID %d. Sending SOCKET_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SOCKET_REQ, &req, sizeof(req));
        }

    // EPOLL_CREATE WORKER ---
    } else if (rpc->is_epoll_create) {
        struct mattx_sys_epoll_create_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.flags = rpc->epoll_flags;

        mattx_dbg("[RPC] Worker sending EPOLL_CREATE_REQ to Node %d...\n", rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_EPOLL_CREATE_REQ, &req, sizeof(req));
        }

    // EPOLL_CTL WORKER ---
    } else if (rpc->is_epoll_ctl) {
        struct mattx_sys_epoll_ctl_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.epfd = rpc->remote_fd;
        req.op = rpc->epoll_op;
        req.fd = rpc->new_local_fd;

        if (rpc->epoll_op == EPOLL_CTL_ADD || rpc->epoll_op == EPOLL_CTL_MOD) {
            struct task_struct *s = NULL;
            rcu_read_lock();
            s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
            if (s) get_task_struct(s);
            rcu_read_unlock();

            if (s) {
                if (access_process_vm(s, (unsigned long)rpc->epoll_events_ptr, &req.event, sizeof(struct epoll_event), FOLL_FORCE) != sizeof(struct epoll_event)) {
                    mattx_dbg("[RPC] Warning: Failed to read epoll_event from Surrogate!\n");
                }
                put_task_struct(s);
            }
        }

        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_EPOLL_CTL_REQ, &req, sizeof(req));
        }

    // EPOLL_WAIT WORKER ---
    } else if (rpc->is_epoll_wait) {
        struct mattx_sys_epoll_wait_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.epfd = rpc->remote_fd;
        req.maxevents = rpc->epoll_maxevents;
        req.timeout = rpc->timeout_ms;

        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_EPOLL_WAIT_REQ, &req, sizeof(req));
        }

    } else if (rpc->is_connect) {
        struct mattx_sys_connect_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.addrlen = rpc->addrlen;
        memcpy(&req.addr, &rpc->addr, rpc->addrlen);

        mattx_dbg("[RPC] Worker started for PID %d. Sending CONNECT_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_CONNECT_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_bind) {
        struct mattx_sys_bind_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.addrlen = rpc->addrlen;
        memcpy(&req.addr, &rpc->addr, rpc->addrlen);

        mattx_dbg("[RPC] Worker started for PID %d. Sending BIND_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_BIND_REQ, &req, sizeof(req));
        }
    } else if (rpc->is_listen) {
        struct mattx_sys_listen_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.backlog = rpc->backlog;

        mattx_dbg("[RPC] Worker started for PID %d. Sending LISTEN_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_LISTEN_REQ, &req, sizeof(req));
        }

    // SENDTO WORKER ---
    } else if (rpc->is_sendto) {
        size_t to_send = min_t(size_t, rpc->len, 4096);
        size_t payload_size = sizeof(struct mattx_sys_send_req) + to_send;
        void *payload_buf = kmalloc(payload_size, GFP_KERNEL);
        
        if (payload_buf) {
            struct mattx_sys_send_req *req = (struct mattx_sys_send_req *)payload_buf;
            req->orig_pid = rpc->orig_pid;
            req->fd = rpc->remote_fd;
            req->flags = rpc->flags;
            req->len = to_send;
            
            // FIX: Use access_process_vm to read Surrogate memory from the Kworker!
            struct task_struct *s = NULL;
            rcu_read_lock();
            s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
            if (s) get_task_struct(s);
            rcu_read_unlock();
            
            if (s) {
                if (access_process_vm(s, (unsigned long)rpc->buff, req->data, to_send, FOLL_FORCE) == to_send) {
                    mattx_dbg("[RPC] Worker sending SEND_REQ to Node %d...\n", rpc->home_node);
                    if (cluster_map[rpc->home_node]) {
                        mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SEND_REQ, payload_buf, payload_size);
                    }
                }
                put_task_struct(s);
            }
            kfree(payload_buf);
        }
    // RECVFROM WORKER ---
    } else if (rpc->is_recvfrom) {
        struct mattx_sys_recv_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.flags = rpc->flags;
        req.size = min_t(size_t, rpc->size, 4096);
        
        mattx_dbg("[RPC] Worker sending RECV_REQ to Node %d...\n", rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_RECV_REQ, &req, sizeof(req));
        }

    // IPC WORMHOLE: READ WORKER ---
    } else if (rpc->is_read) {
        struct mattx_sys_read_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.count = min_t(size_t, rpc->len, 4096);

        mattx_dbg("[RPC] Worker sending READ_REQ for Ghost FD %d to Node %d...\n", rpc->remote_fd, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_READ_REQ, &req, sizeof(req));
        }

    // IPC WORMHOLE: WRITE WORKER ---
    } else if (rpc->is_write) {
        size_t to_send = min_t(size_t, rpc->len, 4096);
        size_t payload_size = sizeof(struct mattx_syscall_req) + to_send;
        void *payload_buf = kmalloc(payload_size, GFP_KERNEL);
        
        if (payload_buf) {
            struct mattx_syscall_req *req = (struct mattx_syscall_req *)payload_buf;
            req->orig_pid = rpc->orig_pid;
            req->fd = rpc->remote_fd;
            req->len = to_send;
            
            // FIX: Use access_process_vm!
            struct task_struct *s = NULL;
            rcu_read_lock();
            s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
            if (s) get_task_struct(s);
            rcu_read_unlock();
            
            if (s) {
                if (access_process_vm(s, (unsigned long)rpc->buff, req->data, to_send, FOLL_FORCE) == to_send) {
                    mattx_dbg("[RPC] Worker sending SYSCALL_FWD (write) for Ghost FD %d to Node %d...\n", rpc->remote_fd, rpc->home_node);
                    if (cluster_map[rpc->home_node]) {
                        mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYSCALL_FWD, payload_buf, payload_size);
                    }
                }
                put_task_struct(s);
            }
            kfree(payload_buf);
        }
        
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                guest_registry[i].rpc_fsync_res = to_send; 
                guest_registry[i].rpc_done = true;
                break;
            }
        }
        spin_unlock(&guest_lock);

    // ACCEPT WORKER
    } else if (rpc->is_accept) {
        struct mattx_sys_accept_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        req.flags = rpc->flags;

        mattx_dbg("[RPC] Worker started for PID %d. Sending ACCEPT_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_ACCEPT_REQ, &req, sizeof(req));
        }
    // POLL WORKER
    } else if (rpc->is_poll) {
        struct mattx_sys_poll_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.nfds = rpc->nfds;
        req.timeout = rpc->timeout;
        memcpy(req.fds, rpc->poll_fds, sizeof(struct mattx_pollfd) * rpc->nfds);

        mattx_dbg("[RPC] Worker started for PID %d. Sending POLL_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_POLL_REQ, &req, sizeof(req));
        }

    // SELECT & PSELECT6 WORKER
    } else if (rpc->is_select || rpc->is_pselect6) {
        struct mattx_sys_poll_req req;
        int poll_idx = 0;
        int fd;
        struct task_struct *surrogate = NULL;

        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.timeout = rpc->timeout_ms; // Use pre-calculated timeout!

        rcu_read_lock();
        surrogate = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
        if (surrogate) get_task_struct(surrogate);
        rcu_read_unlock();

        if (surrogate) {
            struct files_struct *files = surrogate->files;
            if (files) {
                spin_lock(&files->file_lock);
                struct fdtable *fdt = files_fdtable(files);
                
                for (fd = 0; fd < rpc->select_nfds && poll_idx < 16; fd++) {
                    short events = 0;
                    // Read from the pre-copied bitmaps!
                    if (rpc->select_readfds_ptr && test_bit(fd, (unsigned long *)&rpc->in_fds)) events |= POLLIN;
                    if (rpc->select_writefds_ptr && test_bit(fd, (unsigned long *)&rpc->out_fds)) events |= POLLOUT;
                    if (rpc->select_exceptfds_ptr && test_bit(fd, (unsigned long *)&rpc->ex_fds)) events |= POLLPRI;

                    if (events && fd < fdt->max_fds) {
                        struct file *f = rcu_dereference_raw(fdt->fd[fd]);
                        if (f && f->f_op == &mattx_fops) {
                            struct mattx_fake_fd_info *fd_info = f->private_data;
                            if (fd_info) {
                                req.fds[poll_idx].fd = fd_info->remote_fd;
                                req.fds[poll_idx].events = events;
                                rpc->poll_fds[poll_idx].fd = fd; 
                                poll_idx++;
                            }
                        }
                    }
                }
                spin_unlock(&files->file_lock);
            }
            put_task_struct(surrogate);
        }

        req.nfds = poll_idx;
        rpc->nfds = poll_idx; 

        mattx_dbg("[RPC] Translated select() to poll(). Sending POLL_REQ to Node %d...\n", rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_POLL_REQ, &req, sizeof(req));
        }

    // SOCKNAME WORKER ---
    } else if (rpc->is_getsockname || rpc->is_getpeername) {
        struct mattx_sys_getsockname_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        req.fd = rpc->remote_fd;
        u32 msg_type = rpc->is_getsockname ? MATTX_MSG_SYS_GETSOCKNAME_REQ : MATTX_MSG_SYS_GETPEERNAME_REQ;
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], msg_type, &req, sizeof(req));

    // SETSOCKOPT WORKER ---
    } else if (rpc->is_setsockopt) {
        size_t req_size = sizeof(struct mattx_sys_setsockopt_req) + rpc->sock_optlen;
        struct mattx_sys_setsockopt_req *req = kzalloc(req_size, GFP_KERNEL);
        if (req) {
            req->orig_pid = rpc->orig_pid; req->fd = rpc->remote_fd; req->level = rpc->sock_level;
            req->optname = rpc->sock_optname; req->optlen = rpc->sock_optlen;
            
            // Safely read the optval from user-space!
            struct task_struct *s = NULL; rcu_read_lock(); s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (s) get_task_struct(s); rcu_read_unlock();
            if (s) { access_process_vm(s, (unsigned long)rpc->buff, req->optval, rpc->sock_optlen, FOLL_FORCE); put_task_struct(s); }
            
            if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SETSOCKOPT_REQ, req, req_size);
            kfree(req);
        }

    // GETSOCKOPT WORKER ---
    } else if (rpc->is_getsockopt) {
        struct mattx_sys_getsockopt_req req;
        memset(&req, 0, sizeof(req));
        // FIXED: Use dots instead of arrows!
        req.orig_pid = rpc->orig_pid; req.fd = rpc->remote_fd; req.level = rpc->sock_level; req.optname = rpc->sock_optname;
        
        struct task_struct *s = NULL; rcu_read_lock(); s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (s) get_task_struct(s); rcu_read_unlock();
        if (s) { access_process_vm(s, (unsigned long)rpc->size, &req.optlen, sizeof(int), FOLL_FORCE); put_task_struct(s); }
        
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_GETSOCKOPT_REQ, &req, sizeof(req));

    // SENDMSG WORKER ---
    } else if (rpc->is_sendmsg) {
        struct user_msghdr msg; 
        struct task_struct *s = NULL; rcu_read_lock(); s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (s) get_task_struct(s); rcu_read_unlock();
        
        mattx_dbg("[MSG_DEBUG] Starting SENDMSG flattening for PID %d\n", rpc->local_pid);
        
        if (s) {
            if (access_process_vm(s, (unsigned long)rpc->msg_ptr, &msg, sizeof(msg), FOLL_FORCE) == sizeof(msg)) {
                mattx_dbg("[MSG_DEBUG] Successfully read user_msghdr. iovlen: %zu\n", (size_t)msg.msg_iovlen);
                
                struct iovec *iovs = kmalloc_array(msg.msg_iovlen, sizeof(struct iovec), GFP_KERNEL);
                if (iovs) {
                    if (access_process_vm(s, (unsigned long)msg.msg_iov, iovs, msg.msg_iovlen * sizeof(struct iovec), FOLL_FORCE) > 0) {
                        mattx_dbg("[MSG_DEBUG] Successfully read iovec array.\n");
                        
                        size_t total_len = 0;
                        for (int i = 0; i < msg.msg_iovlen; i++) total_len += iovs[i].iov_len;
                        size_t to_send = min_t(size_t, total_len, 4096);
                        mattx_dbg("[MSG_DEBUG] Total data length to send: %zu (capped at %zu)\n", total_len, to_send);
                        
                        size_t req_size = sizeof(struct mattx_sys_sendmsg_req) + to_send;
                        struct mattx_sys_sendmsg_req *req = kzalloc(req_size, GFP_KERNEL);
                        if (req) {
                            req->orig_pid = rpc->orig_pid; req->fd = rpc->remote_fd; req->flags = rpc->flags;
                            
                            if (msg.msg_name && msg.msg_namelen > 0) {
                                if (access_process_vm(s, (unsigned long)msg.msg_name, &req->addr, msg.msg_namelen, FOLL_FORCE) == msg.msg_namelen) {
                                    req->addrlen = msg.msg_namelen;
                                    mattx_dbg("[MSG_DEBUG] Successfully read msg_name (addrlen: %d)\n", req->addrlen);
                                } else {
                                    mattx_dbg("[MSG_DEBUG] ERROR: Failed to read msg_name!\n");
                                }
                            }
                            
                            size_t copied = 0;
                            for (int i = 0; i < msg.msg_iovlen && copied < to_send; i++) {
                                size_t chunk = min_t(size_t, iovs[i].iov_len, to_send - copied);
                                if (access_process_vm(s, (unsigned long)iovs[i].iov_base, req->data + copied, chunk, FOLL_FORCE) != chunk) {
                                    mattx_dbg("[MSG_DEBUG] ERROR: Failed to read iov_base at index %d!\n", i);
                                }
                                copied += chunk;
                            }
                            req->datalen = copied;
                            mattx_dbg("[MSG_DEBUG] Flattened %zu bytes of data. Sending RPC...\n", copied);
                            
                            if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SENDMSG_REQ, req, req_size);
                            kfree(req);
                        } else { mattx_dbg("[MSG_DEBUG] ERROR: Failed to allocate SENDMSG_REQ!\n"); }
                    } else { mattx_dbg("[MSG_DEBUG] ERROR: Failed to read iovec array from 0x%lx!\n", (unsigned long)msg.msg_iov); }
                    kfree(iovs);
                } else { mattx_dbg("[MSG_DEBUG] ERROR: Failed to allocate iovs array!\n"); }
            } else { mattx_dbg("[MSG_DEBUG] ERROR: Failed to read user_msghdr from 0x%lx!\n", (unsigned long)rpc->msg_ptr); }
            put_task_struct(s);
        } else { mattx_dbg("[MSG_DEBUG] ERROR: Surrogate task not found!\n"); }

    // RECVMSG WORKER ---
    } else if (rpc->is_recvmsg) {
        struct user_msghdr msg; 
        struct task_struct *s = NULL; rcu_read_lock(); s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (s) get_task_struct(s); rcu_read_unlock();
        
        mattx_dbg("[MSG_DEBUG] Starting RECVMSG flattening for PID %d\n", rpc->local_pid);

        if (s) {
            if (access_process_vm(s, (unsigned long)rpc->msg_ptr, &msg, sizeof(msg), FOLL_FORCE) == sizeof(msg)) {
                struct iovec *iovs = kmalloc_array(msg.msg_iovlen, sizeof(struct iovec), GFP_KERNEL);
                if (iovs) {
                    if (access_process_vm(s, (unsigned long)msg.msg_iov, iovs, msg.msg_iovlen * sizeof(struct iovec), FOLL_FORCE) > 0) {
                        size_t total_len = 0;
                        for (int i = 0; i < msg.msg_iovlen; i++) total_len += iovs[i].iov_len;
                        
                        mattx_dbg("[MSG_DEBUG] RECVMSG expects up to %zu bytes across %zu iovecs.\n", total_len, (size_t)msg.msg_iovlen);

                        struct mattx_sys_recvmsg_req req;
                        memset(&req, 0, sizeof(req));
                        req.orig_pid = rpc->orig_pid; req.fd = rpc->remote_fd; req.flags = rpc->flags;
                        req.addrlen = msg.msg_namelen;
                        req.datalen = min_t(size_t, total_len, 4096);
                        
                        mattx_dbg("[MSG_DEBUG] Sending RECVMSG_REQ to Node %d...\n", rpc->home_node);
                        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_RECVMSG_REQ, &req, sizeof(req));
                    } else { mattx_dbg("[MSG_DEBUG] ERROR: Failed to read RECVMSG iovec array!\n"); }
                    kfree(iovs);
                }
            } else { mattx_dbg("[MSG_DEBUG] ERROR: Failed to read RECVMSG user_msghdr!\n"); }
            put_task_struct(s);
        }

    } else if (rpc->is_uname) {
        struct mattx_sys_uname_req req = { .orig_pid = rpc->orig_pid };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_UNAME_REQ, &req, sizeof(req));

    } else if (rpc->is_prlimit64) {
        struct mattx_sys_prlimit64_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid; 
        req.pid = rpc->prlimit_pid; 
        req.resource = rpc->prlimit_resource;
        req.has_new = rpc->prlimit_has_new; 
        req.has_old = rpc->prlimit_has_old;
        
        if (req.has_new) {
            struct task_struct *s = NULL; rcu_read_lock(); s = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID); if (s) get_task_struct(s); rcu_read_unlock();
            if (s) { 
                access_process_vm(s, (unsigned long)rpc->prlimit_new_rlim_ptr, &req.new_rlim, sizeof(struct rlimit), FOLL_FORCE); put_task_struct(s); 
            }
        }
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_PRLIMIT64_REQ, &req, sizeof(req));

    } else if (rpc->is_prctl) {
        struct mattx_sys_prctl_req req = { .orig_pid = rpc->orig_pid, .option = rpc->prctl_option, .arg2 = rpc->prctl_arg2, .arg3 = rpc->prctl_arg3, .arg4 = rpc->prctl_arg4, .arg5 = rpc->prctl_arg5 };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_PRCTL_REQ, &req, sizeof(req));

    } else if (rpc->is_fcntl || rpc->is_ioctl) {
        if (rpc->is_fcntl) {
            struct mattx_sys_fcntl_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->remote_fd, .cmd = rpc->fcntl_cmd, .arg = rpc->ioctl_arg, .has_ptr = rpc->ioctl_has_ptr };
            if (req.has_ptr) memcpy(req.data, rpc->ioctl_data, 256);
            if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_FCNTL_REQ, &req, sizeof(req));
        } else {
            struct mattx_sys_ioctl_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->remote_fd, .cmd = rpc->ioctl_cmd, .arg = rpc->ioctl_arg, .has_ptr = rpc->ioctl_has_ptr };
            if (req.has_ptr) memcpy(req.data, rpc->ioctl_data, 256);
            if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_IOCTL_REQ, &req, sizeof(req));
        }

    } else if (rpc->is_pread64) {
        struct mattx_sys_pread64_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->remote_fd, .count = min_t(size_t, rpc->len, 4096), .pos = rpc->pread64_pos };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_PREAD64_REQ, &req, sizeof(req));

    } else if (rpc->is_statfs) {
        struct mattx_sys_statfs_req req = { .orig_pid = rpc->orig_pid }; strncpy(req.path, rpc->meta_path, 255);
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_STATFS_REQ, &req, sizeof(req));
    
    } else if (rpc->is_fstatfs) {
        struct mattx_sys_fstatfs_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->meta_dfd };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_FSTATFS_REQ, &req, sizeof(req));
    
    } else if (rpc->is_newfstatat) {
        struct mattx_sys_newfstatat_req req = { .orig_pid = rpc->orig_pid, .dfd = rpc->meta_dfd, .flags = rpc->meta_flags }; strncpy(req.path, rpc->meta_path, 255);
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_NEWFSTATAT_REQ, &req, sizeof(req));
    
    } else if (rpc->is_faccessat2) {
        struct mattx_sys_faccessat2_req req = { .orig_pid = rpc->orig_pid, .dfd = rpc->meta_dfd, .mode = rpc->meta_mode, .flags = rpc->meta_flags }; strncpy(req.path, rpc->meta_path, 255);
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_FACCESSAT2_REQ, &req, sizeof(req));
    
    } else if (rpc->is_readlink) {
        struct mattx_sys_readlink_req req = { .orig_pid = rpc->orig_pid, .bufsiz = rpc->meta_bufsiz }; strncpy(req.path, rpc->meta_path, 255);
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_READLINK_REQ, &req, sizeof(req));
    
    } else if (rpc->is_readlinkat) {
        struct mattx_sys_readlinkat_req req = { .orig_pid = rpc->orig_pid, .dfd = rpc->meta_dfd, .bufsiz = rpc->meta_bufsiz }; strncpy(req.path, rpc->meta_path, 255);
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_READLINKAT_REQ, &req, sizeof(req));

    } else if (rpc->is_getdents64) {
        struct mattx_sys_getdents64_req req = { .orig_pid = rpc->orig_pid, .fd = rpc->getdents64_fd, .count = min_t(u32, rpc->getdents64_count, 32768) };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_GETDENTS64_REQ, &req, sizeof(req));
    } else if (rpc->is_pipe2) {
        struct mattx_sys_pipe2_req req = { .orig_pid = rpc->orig_pid, .flags = rpc->pipe2_flags };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_PIPE2_REQ, &req, sizeof(req));

    // --- DSM WORKERS ---
    } else if (rpc->is_shmget) {
        struct mattx_sys_shmget_req req = { .orig_pid = rpc->orig_pid, .key = rpc->shm_key, .size = rpc->shm_size, .shmflg = rpc->shm_flg };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SHMGET_REQ, &req, sizeof(req));

    } else if (rpc->is_shmctl) {
        struct mattx_sys_shmctl_req req = { .orig_pid = rpc->orig_pid, .shmid = rpc->shm_id, .cmd = rpc->shm_cmd };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SHMCTL_REQ, &req, sizeof(req));

    } else if (rpc->is_shmdt) {
        struct mattx_sys_shmdt_req req = { .orig_pid = rpc->orig_pid, .shmaddr = rpc->shm_addr };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SHMDT_REQ, &req, sizeof(req));

    } else if (rpc->is_shmat) {
        struct mattx_sys_shmat_req req = { .orig_pid = rpc->orig_pid, .shmid = rpc->shm_id, .shmaddr = rpc->shm_addr, .shmflg = rpc->shm_flg };
        if (cluster_map[rpc->home_node]) mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_SHMAT_REQ, &req, sizeof(req));



    // OPEN WORKER (DEFAULT FALLBACK)
    } else {
        struct mattx_sys_open_req req;
        memset(&req, 0, sizeof(req));
        req.orig_pid = rpc->orig_pid;
        strncpy(req.filename, rpc->filename, sizeof(req.filename) - 1);
        req.flags = rpc->flags;
        req.mode = rpc->mode;

        mattx_dbg("[RPC] Worker started for PID %d. Sending OPEN_REQ to Node %d...\n", rpc->local_pid, rpc->home_node);
        if (cluster_map[rpc->home_node]) {
            mattx_comm_send(cluster_map[rpc->home_node], MATTX_MSG_SYS_OPEN_REQ, &req, sizeof(req));
        }
    }


    // The Ghost Exorcist
    bool done = false;
    bool is_alive = true; // Track if our Surrogate still exists!
    bool aborted = false; // Track if we were interrupted by a Migration!

    while (!done && is_alive) {
        msleep(100);
        
        bool found = false;
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                found = true;
                done = guest_registry[i].rpc_done;
                if (done) remote_fd = guest_registry[i].rpc_remote_fd;
                
                // --- THE TIME MACHINE FIX ---
                // If a Return Migration was triggered, we MUST abort the RPC!
                if (guest_registry[i].is_migrating) {
                    aborted = true;
                    done = true;
                    guest_registry[i].rpc_done = true; // Clear the pending flag!
                }
                break;
            }
        }
        spin_unlock(&guest_lock);
        
        // If we looped through the whole registry and didn't find our PID,
        // it means the Assassin killed it during a migration!
        if (!found) {
            is_alive = false;
        }
    }

    // If the Surrogate was assassinated, we just quietly exit and free our memory!
    if (!is_alive) {
        mattx_dbg("[RPC] Surrogate %d disappeared! Kworker aborting cleanly.\n", rpc->local_pid);
        kfree(rpc);
        return; 
    }

    struct task_struct *surrogate = NULL;
    rcu_read_lock();
    surrogate = pid_task(find_vpid(rpc->local_pid), PIDTYPE_PID);
    if (surrogate) get_task_struct(surrogate);
    rcu_read_unlock();

    if (surrogate) {
        // --- THE QUANTUM RACE CONDITION FIX (V2: With Dead-Task Protection) ---
        int wait_loops = 0;
        bool is_stopped = false;
        
        while (1) {
            if (task_is_stopped_or_traced(surrogate)) {
                is_stopped = true;
                break;
            }
            // If the task is exiting, or its stack was already freed, ABORT!
            if ((surrogate->flags & PF_EXITING) || !surrogate->stack) {
                mattx_dbg("[RPC] Surrogate %d is exiting or dead. Aborting illusion.\n", rpc->local_pid);
                break;
            }
            msleep(1); // Sleep 1 millisecond
            wait_loops++;
            if (wait_loops > 2000) {
                mattx_dbg("[RPC] WARNING: Surrogate %d took too long to stop!\n", rpc->local_pid);
                break; // 2 second timeout
            }
        }
        
        // ONLY apply the illusion if the task is safely stopped and has a stack!
        if (is_stopped && surrogate->stack) {
            
            // If we aborted due to migration, inject -EINTR and skip the rest!
            if (aborted) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                if (regs) regs->ax = -EINTR; // Interrupted System Call
                mattx_dbg("[RPC] Migration initiated! Aborted RPC for Surrogate %d\n", rpc->local_pid);

            // NOW it is safe to apply the Illusion!
            // --- LSEEK AWAKENING ---
            } else if (rpc->is_lseek) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                loff_t res = -EINTR;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) res = guest_registry[i].rpc_lseek_res; 
                        break;
                    }
                }
                spin_unlock(&guest_lock);
                if (regs) regs->ax = res;

            // --- FSYNC AWAKENING ---
            } else if (rpc->is_fsync) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) error = guest_registry[i].rpc_fsync_res; 
                        break;
                    }
                }
                spin_unlock(&guest_lock);
                if (regs) regs->ax = error;

            // --- STATX AWAKENING ---
            } else if (rpc->is_statx) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res; 
                            read_buf = guest_registry[i].rpc_statx_buf; 
                        }
                        guest_registry[i].rpc_statx_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (error == 0 && read_buf) {
                    if (access_process_vm(surrogate, (unsigned long)rpc->statx_buffer, read_buf, sizeof(struct statx), FOLL_WRITE | FOLL_FORCE) != sizeof(struct statx)) {
                        error = -EFAULT;
                    }
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            // --- Group ALL FD-Operating Syscalls together! ---
            } else if (rpc->is_unlink || rpc->is_connect || rpc->is_bind || rpc->is_listen || rpc->is_sendto) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -1;
                bool success = false;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        // We reused rpc_fsync_res to store the generic integer reply from VM1
                        error = guest_registry[i].rpc_fsync_res; 
                        success = true;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (success) {
                    // Shove the return code (e.g., 0) directly into RAX! No Fake FDs allocated!
                    regs->ax = error;
                    mattx_dbg("[RPC] Illusion Complete! Networking syscall returned %d to Surrogate %d\n", error, rpc->local_pid);
                } else {
                    regs->ax = -EBADF;
                }

            // IPC WORMHOLE: READ AWAKENING ---
            } else if (rpc->is_read) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                ssize_t ret_bytes = -1;
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        ret_bytes = guest_registry[i].rpc_read_bytes;
                        read_buf = guest_registry[i].rpc_read_buf;
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (ret_bytes > 0 && read_buf) {
                    // FIX: Use access_process_vm!
                    if (access_process_vm(surrogate, (unsigned long)rpc->buff, read_buf, ret_bytes, FOLL_WRITE | FOLL_FORCE) != ret_bytes) {
                        ret_bytes = -EFAULT;
                    }
                }
                
                if (read_buf) kfree(read_buf);
                
                if (regs) {
                    regs->ax = ret_bytes;
                    mattx_dbg("[RPC] Illusion Complete! read() on Ghost FD returned %zd\n", ret_bytes);
                }

            // IPC WORMHOLE: WRITE AWAKENING ---
            } else if (rpc->is_write) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int bytes_written = -1;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        bytes_written = guest_registry[i].rpc_fsync_res;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (regs) {
                    regs->ax = bytes_written;
                    mattx_dbg("[RPC] Illusion Complete! write() on Ghost FD returned %d\n", bytes_written);
                }

            // RECVFROM AWAKENING ---
            } else if (rpc->is_recvfrom) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                ssize_t ret_bytes = -1;
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        ret_bytes = guest_registry[i].rpc_read_bytes;
                        read_buf = guest_registry[i].rpc_read_buf;
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (ret_bytes > 0 && read_buf) {
                    // FIX: Use access_process_vm!
                    if (access_process_vm(surrogate, (unsigned long)rpc->buff, read_buf, ret_bytes, FOLL_WRITE | FOLL_FORCE) != ret_bytes) {
                        ret_bytes = -EFAULT;
                    }
                }
                
                if (read_buf) kfree(read_buf);
                
                if (regs) {
                    regs->ax = ret_bytes;
                    mattx_dbg("[RPC] Illusion Complete! recvfrom returned %zd\n", ret_bytes);
                }

            // ACCEPT AWAKENING
            } else if (rpc->is_accept) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int local_fd = -1;
                
                // If remote_fd is >= 0, VM1 successfully accepted a connection!
                if (remote_fd >= 0) {
                    struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_KERNEL);
                    if (fd_info) {
                        fd_info->home_node = rpc->home_node;
                        fd_info->orig_pid = rpc->orig_pid;
                        fd_info->remote_fd = remote_fd;
                        
                        struct file *fake_file = anon_inode_getfile("mattx_vfs_proxy", &mattx_fops, fd_info, O_RDWR);
                        
                        if (!IS_ERR(fake_file) && surrogate->files) {
                            spin_lock(&surrogate->files->file_lock);
                            struct fdtable *fdt = files_fdtable(surrogate->files);
                            for (int j = 3; j < fdt->max_fds; j++) {
                                if (!rcu_dereference_raw(fdt->fd[j])) {
                                    rcu_assign_pointer(fdt->fd[j], fake_file);
                                    __set_bit(j, fdt->open_fds);
                                    local_fd = j;
                                    break;
                                }
                            }
                            spin_unlock(&surrogate->files->file_lock);
                            
                            if (local_fd >= 0) {
                                regs->ax = local_fd; // Return the new Local FD!

                                // Copy the client's IP address back to user-space if requested
                                if (rpc->buff && rpc->size) {
                                    int __user *ulen = (int __user *)rpc->size;
                                    int len, g_idx;
                                    void *addr_buf = NULL;
                                    int addr_len = 0;

                                    spin_lock(&guest_lock);
                                    for (g_idx = 0; g_idx < guest_count; g_idx++) {
                                        if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[g_idx].local_pid)) {
                                            addr_len = guest_registry[g_idx].rpc_fsync_res;
                                            addr_buf = guest_registry[g_idx].rpc_read_buf;
                                            guest_registry[g_idx].rpc_read_buf = NULL; 
                                            break;
                                        }
                                    }
                                    spin_unlock(&guest_lock);

                                    // Ensure addr_len is > 0 before copying!
                                    if (addr_buf && addr_len > 0 && get_user(len, ulen) == 0) {
                                        len = min_t(int, len, addr_len);
                                        if (copy_to_user(rpc->buff, addr_buf, len)) {
                                            printk(KERN_WARNING "MattX:[RPC] Failed to copy sockaddr to user!\n");
                                        } else {
                                            put_user(len, ulen); 
                                        }
                                    }
                                    if (addr_buf) kfree(addr_buf);
                                }

                                mattx_dbg("[RPC] Illusion Complete! Mapped New Remote FD %d to Local FD %d\n", remote_fd, local_fd);
                            } else {
                                fput(fake_file);
                                regs->ax = -EMFILE;
                            }
                        } else {
                            kfree(fd_info);
                            regs->ax = -ENFILE;
                        }
                    }
                } else {
                    // VM1 returned an error (e.g., -EAGAIN for non-blocking sockets)
                    regs->ax = remote_fd; 
                }            

            // POLL AWAKENING
            } else if (rpc->is_poll) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int retval = -EBADF;
                bool success = false;
                struct mattx_sys_poll_reply *reply_data = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        retval = guest_registry[i].rpc_fsync_res; 
                        reply_data = guest_registry[i].rpc_read_buf; 
                        guest_registry[i].rpc_read_buf = NULL;
                        success = true;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (success && reply_data) {
                    // FIX: Use access_process_vm!
                    if (access_process_vm(surrogate, (unsigned long)rpc->poll_ufds, reply_data->fds, sizeof(struct pollfd) * rpc->nfds, FOLL_WRITE | FOLL_FORCE) != (sizeof(struct pollfd) * rpc->nfds)) {
                        regs->ax = -EFAULT;
                    } else {
                        regs->ax = retval; 
                        mattx_dbg("[RPC] Illusion Complete! poll returned %d\n", retval);
                    }
                    kfree(reply_data);
                } else {
                    regs->ax = -EBADF;
                }

            // --- SELECT & PSELECT6 AWAKENING ---
            } else if (rpc->is_select || rpc->is_pselect6) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int retval = -EBADF;
                bool success = false;
                struct mattx_sys_poll_reply *reply_data = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        retval = guest_registry[i].rpc_fsync_res; 
                        reply_data = guest_registry[i].rpc_read_buf; 
                        guest_registry[i].rpc_read_buf = NULL;
                        success = true;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (success && reply_data) {
                    memset(&rpc->in_fds, 0, sizeof(fd_set));
                    memset(&rpc->out_fds, 0, sizeof(fd_set));
                    memset(&rpc->ex_fds, 0, sizeof(fd_set));

                    for (int j = 0; j < reply_data->nfds; j++) {
                        int local_fd = rpc->poll_fds[j].fd; 
                        short revents = reply_data->fds[j].revents;

                        if (revents & (POLLIN | POLLERR | POLLHUP)) __set_bit(local_fd, (unsigned long *)&rpc->in_fds);
                        if (revents & (POLLOUT | POLLERR | POLLHUP)) __set_bit(local_fd, (unsigned long *)&rpc->out_fds);
                        if (revents & (POLLPRI | POLLERR | POLLHUP)) __set_bit(local_fd, (unsigned long *)&rpc->ex_fds);
                    }

                    // --- THE STACK SMASH FIX (V2: Kernel 7.0 Guard) ---
                    // Only write the exact number of bytes the user allocated!
                    size_t copy_size = (rpc->select_nfds + 7) / 8;
                    if (copy_size > sizeof(fd_set)) copy_size = sizeof(fd_set);

                    mattx_dbg("[RPC] Injecting %zu bytes into Surrogate %d. RIP: 0x%lx\n", 
                           copy_size, rpc->local_pid, regs->ip);

                    if (rpc->select_readfds_ptr) {
                        mattx_dbg("[RPC] Writing readfds to 0x%lx\n", (unsigned long)rpc->select_readfds_ptr);
                        if (access_process_vm(surrogate, (unsigned long)rpc->select_readfds_ptr, &rpc->in_fds, copy_size, FOLL_WRITE | FOLL_FORCE) != copy_size) {
                            mattx_dbg("[RPC] ERROR: Failed to write readfds!\n");
                        }
                    }
                    if (rpc->select_writefds_ptr) {
                        mattx_dbg("[RPC] Writing writefds to 0x%lx\n", (unsigned long)rpc->select_writefds_ptr);
                        if (access_process_vm(surrogate, (unsigned long)rpc->select_writefds_ptr, &rpc->out_fds, copy_size, FOLL_WRITE | FOLL_FORCE) != copy_size) {
                            mattx_dbg("[RPC] ERROR: Failed to write writefds!\n");
                        }
                    }
                    if (rpc->select_exceptfds_ptr) {
                        mattx_dbg("[RPC] Writing exceptfds to 0x%lx\n", (unsigned long)rpc->select_exceptfds_ptr);
                        if (access_process_vm(surrogate, (unsigned long)rpc->select_exceptfds_ptr, &rpc->ex_fds, copy_size, FOLL_WRITE | FOLL_FORCE) != copy_size) {
                            mattx_dbg("[RPC] ERROR: Failed to write exceptfds!\n");
                        }
                    }

                    regs->ax = retval;
                    mattx_dbg("[RPC] Illusion Complete! select() returned %d\n", retval);
                    kfree(reply_data);
                } else {
                    regs->ax = -EBADF;
                }

            // --- EPOLL_CREATE AWAKENING ---
            } else if (rpc->is_epoll_create) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int local_fd = -1;
                
                if (remote_fd >= 0) {
                    struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_KERNEL);
                    if (fd_info) {
                        fd_info->home_node = rpc->home_node;
                        fd_info->orig_pid = rpc->orig_pid;
                        fd_info->remote_fd = remote_fd;
                        
                        struct file *fake_file = anon_inode_getfile("mattx_epoll_proxy", &mattx_fops, fd_info, O_RDWR);
                        
                        if (!IS_ERR(fake_file) && surrogate->files) {
                            spin_lock(&surrogate->files->file_lock);
                            struct fdtable *fdt = files_fdtable(surrogate->files);
                            for (int j = 3; j < fdt->max_fds; j++) {
                                if (!rcu_dereference_raw(fdt->fd[j])) {
                                    rcu_assign_pointer(fdt->fd[j], fake_file);
                                    __set_bit(j, fdt->open_fds);
                                    local_fd = j;
                                    break;
                                }
                            }
                            spin_unlock(&surrogate->files->file_lock);
                            
                            if (local_fd >= 0) {
                                regs->ax = local_fd; 
                                mattx_dbg("[RPC] Illusion Complete! Mapped Remote Epoll FD %d to Local FD %d\n", remote_fd, local_fd);
                            } else {
                                fput(fake_file);
                                regs->ax = -EMFILE;
                            }
                        } else {
                            kfree(fd_info);
                            regs->ax = -ENFILE;
                        }
                    }
                } else {
                    regs->ax = remote_fd; // Pass the error code (e.g., -ENOSYS)
                }

            // --- EPOLL_CTL AWAKENING ---
            } else if (rpc->is_epoll_ctl) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR; // Default to clean interruption!

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res; 
                        }
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                // Inject the result back into the Surrogate's Brain!
                if (regs) regs->ax = error;

            // --- EPOLL_WAIT AWAKENING ---
            } else if (rpc->is_epoll_wait) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int retval = -EINTR; // Default to clean interruption!
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            retval = guest_registry[i].rpc_fsync_res; 
                            read_buf = guest_registry[i].rpc_read_buf; 
                        }
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (retval > 0 && read_buf) {
                    size_t copy_size = retval * sizeof(struct epoll_event);
                    // Safely inject the events into user-space memory!
                    if (access_process_vm(surrogate, (unsigned long)rpc->epoll_events_ptr, read_buf, copy_size, FOLL_WRITE | FOLL_FORCE) != copy_size) {
                        retval = -EFAULT;
                    }
                }
                
                if (read_buf) kfree(read_buf);
                
                // Inject the result back into the Surrogate's Brain!
                if (regs) regs->ax = retval;

            // --- SOCKNAME AWAKENING ---
            } else if (rpc->is_getsockname || rpc->is_getpeername) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res; 
                            read_buf = guest_registry[i].rpc_read_buf; 
                        }
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (error == 0 && read_buf) {
                    struct sockaddr_storage *addr = read_buf;
                    int *addrlen = (int *)((char *)read_buf + sizeof(struct sockaddr_storage));
                    // Inject the struct and the length back into user-space!
                    access_process_vm(surrogate, (unsigned long)rpc->buff, addr, *addrlen, FOLL_WRITE | FOLL_FORCE);
                    access_process_vm(surrogate, (unsigned long)rpc->size, addrlen, sizeof(int), FOLL_WRITE | FOLL_FORCE);
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            // --- SETSOCKOPT AWAKENING ---
            } else if (rpc->is_setsockopt) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) error = guest_registry[i].rpc_fsync_res; 
                        break;
                    }
                }
                spin_unlock(&guest_lock);
                if (regs) regs->ax = error;

            // --- GETSOCKOPT AWAKENING ---
            } else if (rpc->is_getsockopt) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                int optlen = 0;
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res; 
                            optlen = guest_registry[i].rpc_lseek_res; // We stored the length here!
                            read_buf = guest_registry[i].rpc_read_buf; 
                        }
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (error == 0) {
                    if (read_buf && optlen > 0) {
                        access_process_vm(surrogate, (unsigned long)rpc->buff, read_buf, optlen, FOLL_WRITE | FOLL_FORCE);
                    }
                    access_process_vm(surrogate, (unsigned long)rpc->size, &optlen, sizeof(int), FOLL_WRITE | FOLL_FORCE);
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            // --- SENDMSG AWAKENING ---
            } else if (rpc->is_sendmsg) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) error = guest_registry[i].rpc_fsync_res; 
                        break;
                    }
                }
                spin_unlock(&guest_lock);
                if (regs) regs->ax = error;

            // --- RECVMSG AWAKENING ---
            } else if (rpc->is_recvmsg) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int retval = -EINTR;
                void *read_buf = NULL;
                struct sockaddr_storage addr;
                int addrlen = 0;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            retval = guest_registry[i].rpc_fsync_res; 
                            read_buf = guest_registry[i].rpc_read_buf; 
                            if (read_buf && retval > 0) {
                                // Extract the sockaddr from the end of the buffer!
                                memcpy(&addr, (char *)read_buf + retval, sizeof(struct sockaddr_storage));
                                memcpy(&addrlen, (char *)read_buf + retval + sizeof(struct sockaddr_storage), sizeof(int));
                            }
                        }
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (retval > 0 && read_buf) {
                    // --- FIXED: Use user_msghdr to match the user-space layout! ---
                    struct user_msghdr msg; 
                    if (access_process_vm(surrogate, (unsigned long)rpc->msg_ptr, &msg, sizeof(msg), FOLL_FORCE) == sizeof(msg)) {
                        
                        // Un-flatten the data back into the iovecs!
                        struct iovec *iovs = kmalloc_array(msg.msg_iovlen, sizeof(struct iovec), GFP_KERNEL);
                        if (iovs && access_process_vm(surrogate, (unsigned long)msg.msg_iov, iovs, msg.msg_iovlen * sizeof(struct iovec), FOLL_FORCE) > 0) {
                            size_t copied = 0;
                            for (int j = 0; j < msg.msg_iovlen && copied < retval; j++) {
                                size_t chunk = min_t(size_t, iovs[j].iov_len, retval - copied);
                                access_process_vm(surrogate, (unsigned long)iovs[j].iov_base, (char *)read_buf + copied, chunk, FOLL_WRITE | FOLL_FORCE);
                                copied += chunk;
                            }
                        }
                        if (iovs) kfree(iovs);

                        // Inject the sockaddr back!
                        if (msg.msg_name && addrlen > 0) {
                            access_process_vm(surrogate, (unsigned long)msg.msg_name, &addr, addrlen, FOLL_WRITE | FOLL_FORCE);
                            msg.msg_namelen = addrlen;
                            access_process_vm(surrogate, (unsigned long)rpc->msg_ptr, &msg, sizeof(msg), FOLL_WRITE | FOLL_FORCE);
                        }
                    }
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = retval;
                
            // --- UNAME AWAKENING ---
            } else if (rpc->is_uname) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR; void *read_buf = NULL;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { error = guest_registry[i].rpc_fsync_res; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                if (error == 0 && read_buf) access_process_vm(surrogate, (unsigned long)rpc->buff, read_buf, sizeof(struct new_utsname), FOLL_WRITE | FOLL_FORCE);
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            // --- PRLIMIT64 AWAKENING ---
            } else if (rpc->is_prlimit64) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR; void *read_buf = NULL;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { error = guest_registry[i].rpc_fsync_res; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                if (error == 0 && rpc->prlimit_has_old && read_buf) access_process_vm(surrogate, (unsigned long)rpc->prlimit_old_rlim_ptr, read_buf, sizeof(struct rlimit), FOLL_WRITE | FOLL_FORCE);
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            // --- PRCTL AWAKENING ---
            } else if (rpc->is_prctl) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res;
                        }
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (regs) regs->ax = error;                

            // --- FCNTL & IOCTL AWAKENING ---
            } else if (rpc->is_fcntl || rpc->is_ioctl) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR; void *read_buf = NULL;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { error = guest_registry[i].rpc_fsync_res; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                
                if (rpc->ioctl_has_ptr && read_buf) {
                    access_process_vm(surrogate, rpc->ioctl_arg, read_buf, 256, FOLL_WRITE | FOLL_FORCE);
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            // --- PREAD64 AWAKENING ---
            } else if (rpc->is_pread64) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                ssize_t ret_bytes = -1; void *read_buf = NULL;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { ret_bytes = guest_registry[i].rpc_read_bytes; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                if (ret_bytes > 0 && read_buf) {
                    if (access_process_vm(surrogate, (unsigned long)rpc->buff, read_buf, ret_bytes, FOLL_WRITE | FOLL_FORCE) != ret_bytes) ret_bytes = -EFAULT;
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = ret_bytes;

            // --- META AWAKENING ---
            } else if (rpc->is_statfs || rpc->is_fstatfs || rpc->is_newfstatat || rpc->is_faccessat2 || rpc->is_readlink || rpc->is_readlinkat) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR; void *read_buf = NULL; size_t datalen = 0;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { 
                            error = guest_registry[i].rpc_fsync_res; 
                            datalen = guest_registry[i].rpc_lseek_res; // Reused for datalen
                            read_buf = guest_registry[i].rpc_read_buf; 
                        }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                
                if (error >= 0 && read_buf && datalen > 0 && rpc->meta_buf_ptr) {
                    if (access_process_vm(surrogate, (unsigned long)rpc->meta_buf_ptr, read_buf, datalen, FOLL_WRITE | FOLL_FORCE) != datalen) error = -EFAULT;
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;


            // --- GETDENTS64 AWAKENING ---
            } else if (rpc->is_getdents64) {
                struct pt_regs *regs = task_pt_regs(surrogate); int error = -EINTR; void *read_buf = NULL; size_t datalen = 0;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { error = guest_registry[i].rpc_fsync_res; datalen = guest_registry[i].rpc_lseek_res; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                if (error >= 0 && read_buf && datalen > 0 && rpc->getdents64_dirp) {
                    if (access_process_vm(surrogate, (unsigned long)rpc->getdents64_dirp, read_buf, datalen, FOLL_WRITE | FOLL_FORCE) != datalen) error = -EFAULT;
                }
                if (read_buf) {
                    kfree(read_buf);
                }
                if (regs) {
                    regs->ax = error;
                }

            // --- PIPE2 AWAKENING (THE TWIN INJECTOR) ---
            } else if (rpc->is_pipe2) {
                struct pt_regs *regs = task_pt_regs(surrogate); int error = -EINTR; void *read_buf = NULL;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { error = guest_registry[i].rpc_fsync_res; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);

                if (error == 0 && read_buf) {
                    int *remote_fds = (int *)read_buf;
                    int local_fds[2] = {-1, -1};
                    
                    // Inject BOTH FDs into the Surrogate's table!
                    if (surrogate->files) {
                        spin_lock(&surrogate->files->file_lock);
                        struct fdtable *fdt = files_fdtable(surrogate->files);
                        int injected = 0;
                        for (int j = 3; j < fdt->max_fds && injected < 2; j++) {
                            if (!rcu_dereference_raw(fdt->fd[j])) {
                                struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_ATOMIC);
                                if (fd_info) {
                                    fd_info->home_node = rpc->home_node; fd_info->orig_pid = rpc->orig_pid; fd_info->remote_fd = remote_fds[injected];
                                    struct file *fake_file = anon_inode_getfile("mattx_pipe_proxy", &mattx_fops, fd_info, O_RDWR);
                                    if (!IS_ERR(fake_file)) {
                                        rcu_assign_pointer(fdt->fd[j], fake_file);
                                        __set_bit(j, fdt->open_fds);
                                        local_fds[injected] = j;
                                        injected++;
                                    } else { kfree(fd_info); }
                                }
                            }
                        }
                        spin_unlock(&surrogate->files->file_lock);
                        
                        if (injected == 2) {
                            // Write the two local FDs into the user's pipefd[2] array!
                            if (access_process_vm(surrogate, (unsigned long)rpc->pipe2_pipefd, local_fds, sizeof(local_fds), FOLL_WRITE | FOLL_FORCE) == sizeof(local_fds)) {
                                error = 0;
                                mattx_dbg("[RPC] Twin Injector Complete! Mapped Remote FDs [%d, %d] to Local FDs [%d, %d]\n", remote_fds[0], remote_fds[1], local_fds[0], local_fds[1]);
                            } else { error = -EFAULT; }
                        } else { error = -EMFILE; }
                    } else { error = -EBADF; }
                }
                if (read_buf) {
                    kfree(read_buf);
                }
                if (regs) {
                    regs->ax = error;
                }

            // --- DSM AWAKENING ---
            } else if (rpc->is_shmget) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) error = guest_registry[i].rpc_fsync_res; 
                        break;
                    }
                }
                spin_unlock(&guest_lock);
                if (regs) regs->ax = error;

            } else if (rpc->is_shmctl) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR; void *read_buf = NULL;
                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) { error = guest_registry[i].rpc_fsync_res; read_buf = guest_registry[i].rpc_read_buf; }
                        guest_registry[i].rpc_read_buf = NULL; break;
                    }
                }
                spin_unlock(&guest_lock);
                
                if (error == 0 && read_buf && rpc->buff) {
                    access_process_vm(surrogate, (unsigned long)rpc->buff, read_buf, 128, FOLL_WRITE | FOLL_FORCE);
                }
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;

            } else if (rpc->is_shmdt) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                unsigned long size_to_unmap = 0;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res; 
                            // Find the Hollow VMA size and remove it from the map!
                            for (int d = 0; d < guest_registry[i].dsm_count; d++) {
                                if (guest_registry[i].dsm_map[d].base_addr == rpc->shm_addr) {
                                    size_to_unmap = guest_registry[i].dsm_map[d].size;
                                    guest_registry[i].dsm_map[d] = guest_registry[i].dsm_map[--guest_registry[i].dsm_count];
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                // The Magic Trick: Destroy the Hollow VMA on VM2!
                if (error == 0 && size_to_unmap > 0) {
                    vm_munmap(rpc->shm_addr, size_to_unmap);
                    mattx_dbg("[DSM] Hollow VMA at 0x%lx (Size: %lu) destroyed on VM2.\n", rpc->shm_addr, size_to_unmap);
                }

                if (regs) regs->ax = error;

            // --- SHMAT AWAKENING (THE HOLLOW CARVING) ---
            } else if (rpc->is_shmat) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int error = -EINTR;
                size_t shm_size = 0;
                unsigned long ret_addr = 0;
                void *read_buf = NULL;

                spin_lock(&guest_lock);
                for (i = 0; i < guest_count; i++) {
                    if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                        if (guest_registry[i].rpc_done) {
                            error = guest_registry[i].rpc_fsync_res;
                            shm_size = guest_registry[i].rpc_lseek_res; // We stored the size here!
                            read_buf = guest_registry[i].rpc_read_buf;
                        }
                        guest_registry[i].rpc_read_buf = NULL;
                        break;
                    }
                }
                spin_unlock(&guest_lock);

                if (error == 0 && read_buf && shm_size > 0) {
                    memcpy(&ret_addr, read_buf, sizeof(unsigned long));
                    
                    // --- THE HOLLOW CARVING ---
                    kthread_use_mm(surrogate->mm);
                    
                    unsigned long prot = PROT_READ | PROT_WRITE;
                    if (rpc->shm_flg & SHM_RDONLY) prot = PROT_READ;
                    
                    unsigned long map_flags = MAP_PRIVATE | MAP_ANONYMOUS;
                    if (rpc->shm_addr) map_flags |= MAP_FIXED; // If user requested specific address

                    // 1. Carve the blank VMA
                    unsigned long hollow_addr = vm_mmap(NULL, ret_addr, shm_size, prot, map_flags, 0);
                    
                    if (!IS_ERR_VALUE(hollow_addr)) {
                        // 2. The Tripwire Injection!
                        mmap_write_lock(surrogate->mm);
                        struct vm_area_struct *vma = find_vma(surrogate->mm, hollow_addr);
                        if (vma && vma->vm_start == hollow_addr) {
                            
                            vma->vm_ops = &mattx_dsm_vm_ops; // LAY THE TRAP!
                            
                            // 3. Register it in the DSM Map
                            spin_lock(&guest_lock);
                            for (i = 0; i < guest_count; i++) {
                                if (guest_registry[i].local_pid == surrogate->tgid) {
                                    if (guest_registry[i].dsm_count < MAX_DSM_SEGMENTS) {
                                        int d_idx = guest_registry[i].dsm_count++;
                                        guest_registry[i].dsm_map[d_idx].base_addr = hollow_addr;
                                        guest_registry[i].dsm_map[d_idx].size = shm_size;
                                        guest_registry[i].dsm_map[d_idx].shmid = rpc->shm_id;
                                        mattx_dbg("[DSM] Hollow VMA carved at 0x%lx (Size: %zu, SHMID: %d). Tripwire armed!\n", hollow_addr, shm_size, rpc->shm_id);
                                    }
                                    break;
                                }
                            }
                            spin_unlock(&guest_lock);
                        }
                        mmap_write_unlock(surrogate->mm);
                        error = hollow_addr; // Return the mapped address in RAX!
                    } else {
                        error = hollow_addr; // Return the mmap error
                    }
                    
                    kthread_unuse_mm(surrogate->mm);
                }
                
                if (read_buf) kfree(read_buf);
                if (regs) regs->ax = error;




                


            // --- FD-Creating Syscalls (open, socket, dup) fall through here! ---
            } else if (remote_fd >= 0) {
                struct pt_regs *regs = task_pt_regs(surrogate);
                int local_fd = regs ? regs->ax : -1; 

                mattx_dbg("[DEBUG] Remote FD from VM1: %d. Local regs->ax: %d\n", remote_fd, local_fd);

                if (local_fd < 0) {
                    mattx_dbg("[DEBUG] Local syscall failed (expected)! Searching for a free FD slot...\n");
                    
                    if (surrogate->files) {
                        spin_lock(&surrogate->files->file_lock);
                        struct fdtable *fdt = files_fdtable(surrogate->files);
                        int fd;
                        for (fd = 3; fd < fdt->max_fds; fd++) {
                            if (!rcu_dereference_raw(fdt->fd[fd])) {
                                local_fd = fd;
                                __set_bit(fd, fdt->open_fds);
                                break;
                            }
                        }
                        spin_unlock(&surrogate->files->file_lock);
                    }
                }

                if (local_fd >= 0) {
                    struct mattx_fake_fd_info *fd_info = kmalloc(sizeof(*fd_info), GFP_KERNEL);
                    if (fd_info) {
                        fd_info->home_node = rpc->home_node;
                        fd_info->orig_pid = rpc->orig_pid;
                        fd_info->remote_fd = remote_fd;
                        
                        // Use the original flags (filtered to file access modes) to create the fake file
                        struct file *fake_file = anon_inode_getfile("mattx_vfs_proxy", &mattx_fops, fd_info, rpc->flags & O_ACCMODE);
                        struct file *old_file = NULL;

                        if (!IS_ERR(fake_file) && surrogate->files) {
                            // Inject our custom inode_operations so we can catch fstat/getattr!
                            if (fake_file->f_inode) {
                                fake_file->f_inode->i_op = &mattx_iops;
                            }

                            spin_lock(&surrogate->files->file_lock);
                            struct fdtable *fdt = files_fdtable(surrogate->files);
                            
                            if (local_fd < fdt->max_fds) {
                                old_file = rcu_dereference_raw(fdt->fd[local_fd]);
                                rcu_assign_pointer(fdt->fd[local_fd], fake_file);
                            }
                            spin_unlock(&surrogate->files->file_lock);
                            
                            if (old_file) fput(old_file); 
                            
                            // --- RESTORED: Hijack the return value! ---
                            regs->ax = local_fd; 
                            
                            mattx_dbg("[RPC] Illusion Complete! Mapped Remote FD %d to Local FD %d\n", remote_fd, local_fd);
                        } else {
                            if (!IS_ERR(fake_file)) fput(fake_file);
                            kfree(fd_info);
                        }
                    }
                }
            }

            // The Migration Lock Check ---
            bool safe_to_wake = true;
            spin_lock(&guest_lock);
            for (i = 0; i < guest_count; i++) {
                if (mattx_pid_shares_tgid_with_guest(rpc->local_pid, guest_registry[i].local_pid)) {
                    if (guest_registry[i].is_migrating) {
                        safe_to_wake = false;
                    }
                    break;
                }
            }
            spin_unlock(&guest_lock);

            if (safe_to_wake) {
                mattx_dbg("[RPC] Worker finished for PID %d. Waking Surrogate...\n", rpc->local_pid);
                send_sig(SIGCONT, surrogate, 0);
            } else {
                mattx_dbg("[RPC] Worker finished, but Surrogate %d is migrating! Leaving it frozen.\n", rpc->local_pid);
            }
        }
        put_task_struct(surrogate);
    }

    kfree(rpc);
}

// --- UNIVERSAL GHOST FD DETECTOR ---
static bool is_wormhole_fd(int fd, int *remote_fd_out) {
    bool wormhole = false;
    if (fd >= 0) {
        struct file *f = fget(fd);
        if (f) {
            if (f->f_op == &mattx_fops) {
                struct mattx_fake_fd_info *fd_info = f->private_data;
                if (fd_info && remote_fd_out) *remote_fd_out = fd_info->remote_fd;
                wormhole = true;
            } else {
                struct inode *inode = file_inode(f);
                if (S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
                    if (remote_fd_out) *remote_fd_out = fd; // 1:1 Mapping!
                    wormhole = true;
                }
            }
            fput(f);
        } else {
            if (remote_fd_out) *remote_fd_out = fd; // Ghost FD! 1:1 Mapping!
            wormhole = true;
        }
    }
    return wormhole;
}


struct kretprobe_data {
    const char __user *filename_ptr;
    int flags;
    int mode;
    bool is_hpc_fastpath; // <-- NEW
};

static int entry_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct kretprobe_data *data = (struct kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid)) return 0; 
    if (config_mattxfs_enabled) return 0; 

    data->filename_ptr = (const char __user *)regs->si;
    
    struct open_how *how = (struct open_how *)regs->dx;
    if (how) {
        data->flags = how->flags;
        data->mode = how->mode;
    } else {
        data->flags = O_RDONLY;
        data->mode = 0;
    }

    // --- THE HPC FAST-PATH EVALUATION ---
    data->is_hpc_fastpath = false;
    if (config_hpc_local_libs && data->filename_ptr) {
        char tmp_path[256] = {0};
        if (strncpy_from_user(tmp_path, data->filename_ptr, sizeof(tmp_path) - 1) > 0) {
            if (is_hpc_local_lib(tmp_path)) {
                data->is_hpc_fastpath = true;
                mattx_dbg("[HOOK] HPC Fast-Path: Allowing native openat('%s')\n", tmp_path);
            }
        }
    }

    // Only sabotage if it's NOT a local library!
    if (!data->is_hpc_fastpath) {
        regs->si = 0; // Sabotage the syscall!
    }

    return 0;
}

static int ret_handler_openat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct kretprobe_data *data = (struct kretprobe_data *)ri->data;
    char filename[256] = {0};
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid)) return 0;
    if (config_mattxfs_enabled) return 0;

    // --- THE HPC FAST-PATH BYPASS ---
    if (data->is_hpc_fastpath) return 0; // Let the native result flow back to the app!

    if (strncpy_from_user(filename, data->filename_ptr, sizeof(filename) - 1) > 0) {
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                home_node = guest_registry[i].home_node;
                orig_pid = guest_registry[i].orig_pid;
                guest_registry[i].rpc_done = false; 
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (home_node != -1) {
            if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

            if (!config_migrate_file_io) {
                // Local Breakout! 🏎️
                // User disabled File I/O Wormhole routing. Let the Surrogate execute
                // openat locally on the remote node's native filesystem!
                return 0;
            }

            // --- THE ATOMIC SHIELD ---
            // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
            // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
            if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
                mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
                return 0;
            }

            struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
            if (rpc) {
                INIT_WORK(&rpc->work, mattx_rpc_worker);
                rpc->local_pid = my_pid;
                rpc->orig_pid = orig_pid;
                rpc->home_node = home_node;
                rpc->flags = data->flags;
                rpc->mode = data->mode;
                strncpy(rpc->filename, filename, sizeof(rpc->filename) - 1);

                mattx_dbg("[HOOK] Intercepted open('%s'). Freezing Surrogate %d...\n", filename, my_pid);
                send_sig(SIGSTOP, current, 0);
                schedule_work(&rpc->work);
            }
        }
    }
    return 0;
}


// Removed statx hooks

// --- DUP HOOK ---

struct dup_kretprobe_data {
    int oldfd;
    int newfd;
    bool is_wormhole_fd;
    int old_remote_fd;
};

static struct kretprobe dup_kprobe;
static struct kretprobe dup2_kprobe;

static int entry_handler_dup(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct dup_kretprobe_data *data = (struct dup_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs); // Unwrap!

    if (!is_guest_process(my_pid)) return 0;
    if (config_mattxfs_enabled) return 0; 

    data->oldfd = (int)sys_regs->di;
    
    if (get_kretprobe(ri) == &dup2_kprobe) {
        data->newfd = (int)sys_regs->si;
    } else {
        data->newfd = -1;
    }
    
    data->is_wormhole_fd = false;
    data->old_remote_fd = -1;

    if (data->oldfd >= 0) {
        struct file *f = fget(data->oldfd);
        if (f) {
            if (f->f_op == &mattx_fops) {
                struct mattx_fake_fd_info *fd_info = f->private_data;
                if (fd_info) {
                    data->is_wormhole_fd = true;
                    data->old_remote_fd = fd_info->remote_fd;
                }
            }
            fput(f);
        }
    }

    if (data->is_wormhole_fd) {
        sys_regs->di = -1; // Sabotage inner FD!
    }
    return 0;
}

static int ret_handler_dup(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct dup_kretprobe_data *data = (struct dup_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_dup = true;
            rpc->remote_fd = data->old_remote_fd;
            rpc->new_local_fd = data->newfd;
            
            // We need to pass valid flags/mode to open a new fake file proxy later in mattx_rpc_worker
            struct file *f = fget(data->oldfd);
            if (f) {
                rpc->flags = f->f_flags;
                fput(f);
            } else {
                rpc->flags = O_RDWR;
            }
            rpc->mode = 0666;

            mattx_dbg("[HOOK] Intercepted dup(fd=%d). Freezing Surrogate %d and escaping to Workqueue...\n", data->oldfd, my_pid);
            
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- UNLINK HOOK ---
struct unlinkat_kretprobe_data {
    const char __user *pathname;
};

static struct kretprobe unlinkat_kprobe;

static int entry_handler_unlinkat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct unlinkat_kretprobe_data *data = (struct unlinkat_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs); // Unwrap!

    if (!is_guest_process(my_pid)) return 0; 
    if (config_mattxfs_enabled) return 0; 

    data->pathname = (const char __user *)sys_regs->si;
    sys_regs->si = 0; // Sabotage inner pathname!

    return 0;
}

static int ret_handler_unlinkat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct unlinkat_kretprobe_data *data = (struct unlinkat_kretprobe_data *)ri->data;
    char filename[256] = {0};
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid)) return 0;
    if (config_mattxfs_enabled) return 0; // Bypass!

    if (strncpy_from_user(filename, data->pathname, sizeof(filename) - 1) > 0) {
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                home_node = guest_registry[i].home_node;
                orig_pid = guest_registry[i].orig_pid;
                guest_registry[i].rpc_done = false; 
                break;
            }
        }
        spin_unlock(&guest_lock);

        if (home_node != -1) {
            // --- THE ATOMIC SHIELD ---
            // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
            // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
            if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
                mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
                return 0;
            }

            struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
            if (rpc) {
                INIT_WORK(&rpc->work, mattx_rpc_worker);
                rpc->local_pid = my_pid;
                rpc->orig_pid = orig_pid;
                rpc->home_node = home_node;
                rpc->is_unlink = true;
                strncpy(rpc->filename, filename, sizeof(rpc->filename) - 1);

                mattx_dbg("[HOOK] Intercepted unlinkat('%s'). Freezing Surrogate %d...\n", filename, my_pid);
                send_sig(SIGSTOP, current, 0);
                schedule_work(&rpc->work);
            }
        }
    }
    return 0;
}


// --- SOCKET HOOK ---
struct socket_kretprobe_data {
    int domain;
    int type;
    int protocol;
};

static struct kretprobe socket_kprobe;

static int entry_handler_socket(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct socket_kretprobe_data *data = (struct socket_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->domain = (int)regs->di;
    data->type = (int)regs->si;
    data->protocol = (int)regs->dx;

    regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_socket(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct socket_kretprobe_data *data = (struct socket_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_socket = true;
            rpc->domain = data->domain;
            rpc->type = data->type;
            rpc->protocol = data->protocol;

            // Fake an O_RDWR open so mattx_vfs_proxy is fully featured
            rpc->flags = O_RDWR;

            mattx_dbg("[HOOK] Intercepted socket(domain=%d). Freezing Surrogate %d and escaping to Workqueue...\n", data->domain, my_pid);
            
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- CONNECT HOOK ---
struct connect_kretprobe_data {
    int fd;
    struct sockaddr_storage addr;
    int addrlen;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe connect_kprobe;

static int entry_handler_connect(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->fd = (int)regs->di;
    data->addrlen = (int)regs->dx;
    data->remote_fd = -1;

    if (data->addrlen > 0 && data->addrlen <= sizeof(struct sockaddr_storage)) {
        if (copy_from_user(&data->addr, (void __user *)regs->si, data->addrlen)) data->addrlen = 0;
    } else {
        data->addrlen = 0;
    }

    data->is_wormhole_fd = is_wormhole_fd(data->fd, &data->remote_fd);
    if (data->is_wormhole_fd) regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_connect(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_connect = true;
            rpc->remote_fd = data->remote_fd;
            rpc->addrlen = data->addrlen;
            memcpy(&rpc->addr, &data->addr, data->addrlen);

            mattx_dbg("[HOOK] Intercepted connect(fd=%d). Freezing Surrogate %d and escaping to Workqueue...\n", data->fd, my_pid);
            
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- BIND HOOK ---
static struct kretprobe bind_kprobe;

static int entry_handler_bind(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->fd = (int)regs->di;
    data->addrlen = (int)regs->dx;
    data->remote_fd = -1;

    if (data->addrlen > 0 && data->addrlen <= sizeof(struct sockaddr_storage)) {
        if (copy_from_user(&data->addr, (void __user *)regs->si, data->addrlen)) data->addrlen = 0;
    } else {
        data->addrlen = 0;
    }

    data->is_wormhole_fd = is_wormhole_fd(data->fd, &data->remote_fd);
    if (data->is_wormhole_fd) regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_bind(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct connect_kretprobe_data *data = (struct connect_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_bind = true;
            rpc->remote_fd = data->remote_fd;
            rpc->addrlen = data->addrlen;
            memcpy(&rpc->addr, &data->addr, data->addrlen);

            mattx_dbg("[HOOK] Intercepted bind(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- LISTEN HOOK ---
struct listen_kretprobe_data {
    int fd;
    int backlog;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe listen_kprobe;

static int entry_handler_listen(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct listen_kretprobe_data *data = (struct listen_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->fd = (int)regs->di;
    data->backlog = (int)regs->si;
    data->remote_fd = -1;

    data->is_wormhole_fd = is_wormhole_fd(data->fd, &data->remote_fd);
    if (data->is_wormhole_fd) regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_listen(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct listen_kretprobe_data *data = (struct listen_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_listen = true;
            rpc->remote_fd = data->remote_fd;
            rpc->backlog = data->backlog;

            mattx_dbg("[HOOK] Intercepted listen(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// SENDTO HOOK ---
struct sendto_kretprobe_data {
    int fd;
    void __user *buff;
    size_t len;
    unsigned int flags;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe sendto_kprobe;

static int entry_handler_sendto(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct sendto_kretprobe_data *data = (struct sendto_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->fd = (int)regs->di;
    data->buff = (void __user *)regs->si;
    data->len = (size_t)regs->dx;
    data->flags = (unsigned int)regs->r10;
    data->remote_fd = -1;
    
    data->is_wormhole_fd = is_wormhole_fd(data->fd, &data->remote_fd);
    if (data->is_wormhole_fd) regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_sendto(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct sendto_kretprobe_data *data = (struct sendto_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_sendto = true;
            rpc->remote_fd = data->remote_fd;
            rpc->buff = data->buff;
            rpc->len = data->len;
            rpc->flags = data->flags;

            mattx_dbg("[HOOK] Intercepted sendto(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// RECVFROM HOOK ---
struct recvfrom_kretprobe_data {
    int fd;
    void __user *ubuf;
    size_t size;
    unsigned int flags;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe recvfrom_kprobe;

static int entry_handler_recvfrom(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct recvfrom_kretprobe_data *data = (struct recvfrom_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->fd = (int)regs->di;
    data->ubuf = (void __user *)regs->si;
    data->size = (size_t)regs->dx;
    data->flags = (unsigned int)regs->r10;
    data->remote_fd = -1;

    data->is_wormhole_fd = is_wormhole_fd(data->fd, &data->remote_fd);
    if (data->is_wormhole_fd) regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_recvfrom(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct recvfrom_kretprobe_data *data = (struct recvfrom_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_recvfrom = true;
            rpc->remote_fd = data->remote_fd;
            rpc->buff = data->ubuf;
            rpc->size = data->size;
            rpc->flags = data->flags;

            mattx_dbg("[HOOK] Intercepted recvfrom(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- ACCEPT HOOK ---
struct accept_kretprobe_data {
    int fd;
    struct sockaddr __user *upeer_sockaddr;
    int __user *upeer_addrlen;
    int flags;
    bool is_wormhole_fd;
    int remote_fd;
};

static struct kretprobe accept_kprobe;

static int entry_handler_accept(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct accept_kretprobe_data *data = (struct accept_kretprobe_data *)ri->data;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0; 

    data->fd = (int)regs->di;
    data->upeer_sockaddr = (struct sockaddr __user *)regs->si;
    data->upeer_addrlen = (int __user *)regs->dx;
    data->flags = (int)regs->r10;
    data->remote_fd = -1;

    data->is_wormhole_fd = is_wormhole_fd(data->fd, &data->remote_fd);
    if (data->is_wormhole_fd) regs->di = -1; // Sabotage!
    return 0;
}

static int ret_handler_accept(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct accept_kretprobe_data *data = (struct accept_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole_fd) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_accept = true;
            rpc->remote_fd = data->remote_fd;
            rpc->flags = data->flags;
            
            // We save the user-space pointers so we can copy the client IP later!
            rpc->buff = data->upeer_sockaddr; 
            rpc->size = (size_t)data->upeer_addrlen; 

            mattx_dbg("[HOOK] Intercepted accept(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- POLL HOOK ---
struct poll_kretprobe_data {
    void __user *ufds;
    int nfds;
    int timeout;
    bool is_wormhole;
    struct mattx_pollfd fds[16];
};

static struct kretprobe poll_kprobe;

static int entry_handler_poll(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct poll_kretprobe_data *data = (struct poll_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0;

    data->ufds = (void __user *)sys_regs->di;
    data->nfds = (int)sys_regs->si;
    data->timeout = (int)sys_regs->dx;
    data->is_wormhole = false;

    if (data->nfds > 0 && data->nfds <= 16) {
        if (copy_from_user(data->fds, data->ufds, data->nfds * sizeof(struct pollfd)) == 0) {
            for (i = 0; i < data->nfds; i++) {
                int remote_fd = -1;
                if (is_wormhole_fd(data->fds[i].fd, &remote_fd)) {
                    if (remote_fd != -1) data->fds[i].fd = remote_fd; // Translate if known!
                    data->is_wormhole = true;
                }
            }
        }
    }

    if (data->is_wormhole) sys_regs->di = 0; // Sabotage!
    return 0;
}

static int ret_handler_poll(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct poll_kretprobe_data *data = (struct poll_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io || !data->is_wormhole) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_poll = true;
            rpc->nfds = data->nfds;
            rpc->timeout = data->timeout;
            rpc->poll_ufds = data->ufds;
            memcpy(rpc->poll_fds, data->fds, sizeof(struct mattx_pollfd) * data->nfds);

            mattx_dbg("[HOOK] Intercepted poll(). Freezing Surrogate %d...\n", my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- SELECT HOOK ---
struct select_kretprobe_data {
    int n;
    void __user *inp;
    void __user *outp;
    void __user *exp;
    void __user *tvp;
};

static struct kretprobe select_kprobe;

static int entry_handler_select(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct select_kretprobe_data *data = (struct select_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs); // Unwrap!

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    data->n = (int)sys_regs->di;
    data->inp = (void __user *)sys_regs->si;
    data->outp = (void __user *)sys_regs->dx;
    data->exp = (void __user *)sys_regs->r10;
    data->tvp = (void __user *)sys_regs->r8;

    sys_regs->di = -1; // Sabotage inner FD count!
    return 0;
}

static int ret_handler_select(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct select_kretprobe_data *data = (struct select_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_select = true;
            rpc->select_nfds = data->n;
            rpc->select_readfds_ptr = data->inp;
            rpc->select_writefds_ptr = data->outp;
            rpc->select_exceptfds_ptr = data->exp;
            
            size_t copy_size = (data->n + 7) / 8;
            if (copy_size > sizeof(fd_set)) copy_size = sizeof(fd_set);

            memset(&rpc->in_fds, 0, sizeof(fd_set));
            memset(&rpc->out_fds, 0, sizeof(fd_set));
            memset(&rpc->ex_fds, 0, sizeof(fd_set));
            
            // COPY FROM USER IN THE CORRECT CONTEXT!
            if (data->inp && copy_from_user(&rpc->in_fds, data->inp, copy_size)) {
                mattx_dbg("[HOOK] Warning: Failed to copy readfds from user!\n");
            }
            if (data->outp && copy_from_user(&rpc->out_fds, data->outp, copy_size)) {
                mattx_dbg("[HOOK] Warning: Failed to copy writefds from user!\n");
            }
            if (data->exp && copy_from_user(&rpc->ex_fds, data->exp, copy_size)) {
                mattx_dbg("[HOOK] Warning: Failed to copy exceptfds from user!\n");
            }

            if (data->tvp) {
                struct __kernel_old_timeval tv;
                if (copy_from_user(&tv, data->tvp, sizeof(tv)) == 0) {
                    rpc->timeout_ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
                } else {
                    rpc->timeout_ms = -1;
                }
            } else {
                rpc->timeout_ms = -1;
            }

            mattx_dbg("[HOOK] Intercepted select(). Freezing Surrogate %d...\n", my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }

    return 0;
}

// --- PSELECT6 HOOK ---
struct pselect6_kretprobe_data {
    int n;
    void __user *inp;
    void __user *outp;
    void __user *exp;
    void __user *tsp;
    void __user *sig;
};

static struct kretprobe pselect6_kprobe;

static int entry_handler_pselect6(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct pselect6_kretprobe_data *data = (struct pselect6_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs); // Unwrap!

    if (!is_guest_process(my_pid)) return 0; 
    if (!config_migrate_network_io) return 0;

    data->n = (int)sys_regs->di;
    data->inp = (void __user *)sys_regs->si;
    data->outp = (void __user *)sys_regs->dx;
    data->exp = (void __user *)sys_regs->r10;
    data->tsp = (void __user *)sys_regs->r8;
    data->sig = (void __user *)sys_regs->r9;

    sys_regs->di = -1; // Sabotage inner FD count!
    return 0;
}

static int ret_handler_pselect6(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct pselect6_kretprobe_data *data = (struct pselect6_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !config_migrate_network_io) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_pselect6 = true;
            rpc->select_nfds = data->n;
            rpc->select_readfds_ptr = data->inp;
            rpc->select_writefds_ptr = data->outp;
            rpc->select_exceptfds_ptr = data->exp;

            size_t copy_size = (data->n + 7) / 8;
            if (copy_size > sizeof(fd_set)) copy_size = sizeof(fd_set);

            memset(&rpc->in_fds, 0, sizeof(fd_set));
            memset(&rpc->out_fds, 0, sizeof(fd_set));
            memset(&rpc->ex_fds, 0, sizeof(fd_set));
            
            // COPY FROM USER IN THE CORRECT CONTEXT!
            if (data->inp && copy_from_user(&rpc->in_fds, data->inp, copy_size)) {
                mattx_dbg("[HOOK] Warning: Failed to copy readfds from user!\n");
            }
            if (data->outp && copy_from_user(&rpc->out_fds, data->outp, copy_size)) {
                mattx_dbg("[HOOK] Warning: Failed to copy writefds from user!\n");
            }
            if (data->exp && copy_from_user(&rpc->ex_fds, data->exp, copy_size)) {
                mattx_dbg("[HOOK] Warning: Failed to copy exceptfds from user!\n");
            }

            if (data->tsp) {
                struct __kernel_timespec ts;
                if (copy_from_user(&ts, data->tsp, sizeof(ts)) == 0) {
                    rpc->timeout_ms = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
                } else {
                    rpc->timeout_ms = -1;
                }
            } else {
                rpc->timeout_ms = -1;
            }

            mattx_dbg("[HOOK] Intercepted pselect6(). Freezing Surrogate %d...\n", my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}

// --- IPC WORMHOLE: Read Handlers ---
static int entry_handler_read(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct rw_kretprobe_data *data = (struct rw_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs); // Unwrap!

    if (!is_guest_process(my_pid)) return 0;

    data->fd = (int)sys_regs->di;
    data->buf = (void __user *)sys_regs->si;
    data->count = (size_t)sys_regs->dx;
    data->is_wormhole = false;

    if (data->fd >= 0) {
        struct file *f = fget(data->fd);
        if (f) {
            if (f->f_op != &mattx_fops) {
                struct inode *inode = file_inode(f);
                if (S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
                    data->is_wormhole = true;
                }
            }
            fput(f);
        } else {
            data->is_wormhole = true;
        }
    }

    if (data->is_wormhole) {
        sys_regs->di = -1; // Sabotage the inner FD, NOT the wrapper pointer!
    }
    return 0;
}

static int ret_handler_read(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct rw_kretprobe_data *data = (struct rw_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !data->is_wormhole) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            rpc->is_read = true;
            rpc->remote_fd = data->fd; 
            rpc->buff = data->buf;
            rpc->len = data->count;

            mattx_dbg("[HOOK] Intercepted read(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}

// --- IPC WORMHOLE: Write Handlers ---
static int entry_handler_write(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct rw_kretprobe_data *data = (struct rw_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs); // Unwrap!

    if (!is_guest_process(my_pid)) return 0;

    data->fd = (int)sys_regs->di;
    data->buf = (void __user *)sys_regs->si;
    data->count = (size_t)sys_regs->dx;
    data->is_wormhole = false;

    if (data->fd >= 0) {
        struct file *f = fget(data->fd);
        if (f) {
            if (f->f_op != &mattx_fops) {
                struct inode *inode = file_inode(f);
                if (S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
                    data->is_wormhole = true;
                }
            }
            fput(f);
        } else {
            data->is_wormhole = true; 
        }
    }

    if (data->is_wormhole) {
        sys_regs->di = -1; // Sabotage the inner FD!
    }
    return 0;
}

static int ret_handler_write(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct rw_kretprobe_data *data = (struct rw_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!is_guest_process(my_pid) || !data->is_wormhole) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            rpc->is_write = true;
            rpc->remote_fd = data->fd;
            rpc->buff = data->buf;
            rpc->len = data->count;

            mattx_dbg("[HOOK] Intercepted write(fd=%d). Freezing Surrogate %d...\n", data->fd, my_pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- FILE I/O KPROBES: LSEEK, FSYNC, STATX ---
struct fileio_kretprobe_data { 
    int fd; 
    bool is_wormhole; 
    loff_t offset; 
    int whence; 
    loff_t start; 
    loff_t end; 
    int datasync; 
    u32 mask; 
    int flags; 
    void __user *buf; 
};

static struct kretprobe lseek_kprobe;
static struct kretprobe fsync_kprobe;
static struct kretprobe statx_kprobe;

static int entry_handler_fileio(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct fileio_kretprobe_data *data = (struct fileio_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    data->is_wormhole = false;

    if (is_guest_process(current->tgid)) {
        data->fd = (int)sys_regs->di;
        if (data->fd >= 0) {
            struct file *f = fget(data->fd);
            if (f) {
                if (f->f_op == &mattx_fops && !config_mattxfs_enabled) data->is_wormhole = true;
                else if (f->f_op != &mattx_fops) {
                    struct inode *inode = file_inode(f);
                    if (S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) data->is_wormhole = true;
                }
                fput(f);
            } else { data->is_wormhole = true; }
        }

        if (data->is_wormhole) {
            if (get_kretprobe(ri) == &lseek_kprobe) { data->offset = (loff_t)sys_regs->si; data->whence = (int)sys_regs->dx; }
            else if (get_kretprobe(ri) == &fsync_kprobe) { data->datasync = (int)sys_regs->si; }
            else if (get_kretprobe(ri) == &statx_kprobe) { data->flags = (int)sys_regs->dx; data->mask = (u32)sys_regs->r10; data->buf = (void __user *)sys_regs->r8; }
            sys_regs->di = -1; // Sabotage!
        }
    }
    return 0;
}

static int ret_handler_fileio(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct fileio_kretprobe_data *data = (struct fileio_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;

    if (!data->is_wormhole) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0; // THE ATOMIC SHIELD!

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node; rpc->remote_fd = data->fd;
            
            if (get_kretprobe(ri) == &lseek_kprobe) { rpc->is_lseek = true; rpc->offset = data->offset; rpc->whence = data->whence; }
            else if (get_kretprobe(ri) == &fsync_kprobe) { rpc->is_fsync = true; rpc->datasync = data->datasync; }
            else if (get_kretprobe(ri) == &statx_kprobe) { rpc->is_statx = true; rpc->mask = data->mask; rpc->flags = data->flags; rpc->statx_buffer = data->buf; }

            regs->ax = -EINTR;
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}




// --- THE EPOLL TRANSLATOR: Create Interceptor (Sterile Lab) ---
struct epoll_create_kretprobe_data {
    int flags;
    bool is_guest;
    bool is_epolltest;
};

static struct kretprobe epoll_create_kprobe;
static struct kretprobe epoll_create1_kprobe;

static int entry_handler_epoll_create(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct epoll_create_kretprobe_data *data = (struct epoll_create_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    
    data->is_guest = is_guest_process(current->tgid);
    data->is_epolltest = (strcmp(current->comm, "epolltest") == 0);

    // STERILE LABORATORY: Only intercept if it's a guest AND it's exactly "epolltest"
    if (data->is_guest && data->is_epolltest) {
        // For epoll_create1, flags are in DI. For epoll_create, size is in DI.
        // We just grab DI so we can pass it to the Home Node.
        data->flags = (int)sys_regs->di;
        sys_regs->di = -1; // Sabotage! Force the local syscall to fail.
    }
    return 0;
}

static int ret_handler_epoll_create(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct epoll_create_kretprobe_data *data = (struct epoll_create_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!data->is_guest || !data->is_epolltest) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {

        // --- THE ATOMIC SHIELD ---
        // If the process is dying (SIGKILL) or exiting, DO NOT freeze it!
        // Freezing a dying process inside a Kretprobe causes a "scheduling while atomic" BUG!
        if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
            mattx_dbg("[HOOK] Surrogate %s is dying. Aborting RPC.\n", current->comm);
            return 0;
        }

        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_epoll_create = true;
            rpc->epoll_flags = data->flags;

            mattx_dbg("[HOOK] Intercepted epoll_create() for %s. Freezing Surrogate %d...\n", current->comm, current->pid);
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}


static struct kretprobe epoll_ctl_kprobe;
static struct kretprobe epoll_wait_kprobe;


// --- THE EPOLL HIJACKER: Ctl ---
struct epoll_ctl_kretprobe_data {
    int epfd;
    int op;
    int fd;
    struct epoll_event __user *event_ptr;
    bool is_ghost;
};

static int entry_handler_epoll_ctl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct epoll_ctl_kretprobe_data *data = (struct epoll_ctl_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    data->is_ghost = false;

    if (is_guest_process(current->tgid)) {
        int epfd = (int)sys_regs->di;
        if (is_wormhole_fd(epfd, NULL)) {
            data->is_ghost = true;
            data->epfd = epfd;
            data->op = (int)sys_regs->si;
            data->fd = (int)sys_regs->dx;
            data->event_ptr = (struct epoll_event __user *)sys_regs->r10; // 4th arg is R10
            sys_regs->di = -1; // Sabotage!
        }
    }
    return 0;
}

static int ret_handler_epoll_ctl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct epoll_ctl_kretprobe_data *data = (struct epoll_ctl_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!data->is_ghost) return 0;

    // --- THE ATOMIC SHIELD ---
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
        return 0;
    }

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_epoll_ctl = true;
            rpc->remote_fd = data->epfd;
            rpc->epoll_op = data->op;
            rpc->new_local_fd = data->fd; 
            rpc->epoll_events_ptr = data->event_ptr; 

            // --- THE RIP DRIFT FIX ---
            // Use standard EINTR so the kernel doesn't rewind the Instruction Pointer!
            regs->ax = -EINTR;

            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- THE EPOLL HIJACKER: Wait ---
struct epoll_wait_kretprobe_data {
    int epfd;
    struct epoll_event __user *event_ptr;
    int maxevents;
    int timeout;
    bool is_ghost;
};

static int entry_handler_epoll_wait(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct epoll_wait_kretprobe_data *data = (struct epoll_wait_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    data->is_ghost = false;

    if (is_guest_process(current->tgid)) {
        int epfd = (int)sys_regs->di;
        if (is_wormhole_fd(epfd, NULL)) {
            data->is_ghost = true;
            data->epfd = epfd;
            data->event_ptr = (struct epoll_event __user *)sys_regs->si;
            data->maxevents = (int)sys_regs->dx;
            data->timeout = (int)sys_regs->r10; // 4th arg is R10
            sys_regs->di = -1; // Sabotage!
        }
    }
    return 0;
}


static int ret_handler_epoll_wait(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct epoll_wait_kretprobe_data *data = (struct epoll_wait_kretprobe_data *)ri->data;
    int home_node = -1;
    u32 orig_pid = 0;
    int i;

    if (!data->is_ghost) return 0;

    // --- THE ATOMIC SHIELD ---
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) {
        return 0;
    }

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node;
            orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; 
            guest_registry[i].rpc_read_buf = NULL;
            break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid;
            rpc->orig_pid = orig_pid;
            rpc->home_node = home_node;
            
            rpc->is_epoll_wait = true;
            rpc->remote_fd = data->epfd;
            rpc->epoll_events_ptr = data->event_ptr;
            rpc->epoll_maxevents = data->maxevents;
            rpc->timeout_ms = data->timeout;

            // --- THE RIP DRIFT FIX ---
            regs->ax = -EINTR;

            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- THE NETWORK HIJACKERS: Sockopts & Names ---
static struct kretprobe getsockname_kprobe;
static struct kretprobe getpeername_kprobe;
static struct kretprobe setsockopt_kprobe;
static struct kretprobe getsockopt_kprobe;

struct sockname_kretprobe_data { int fd; void __user *addr; void __user *len; bool is_ghost; int remote_fd; };
struct sockopt_kretprobe_data { int fd; int level; int optname; void __user *optval; void __user *optlen; bool is_ghost; int remote_fd; };

static int entry_handler_sockname(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct sockname_kretprobe_data *data = (struct sockname_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    data->is_ghost = false;
    if (is_guest_process(current->tgid)) {
        data->fd = (int)sys_regs->di;
        if (is_wormhole_fd(data->fd, &data->remote_fd)) {
            data->is_ghost = true;
            data->addr = (void __user *)sys_regs->si;
            data->len = (void __user *)sys_regs->dx;
            sys_regs->di = -1; // Sabotage!
        }
    }
    return 0;
}

static int ret_handler_sockname(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct sockname_kretprobe_data *data = (struct sockname_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;
    if (!data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;

            // --- Use strstr to safely identify the syscall! ---
            if (strstr(get_kretprobe(ri)->kp.symbol_name, "getsockname")) rpc->is_getsockname = true; 
            else rpc->is_getpeername = true;            

            rpc->remote_fd = data->remote_fd;
            rpc->buff = data->addr;
            rpc->size = (size_t)data->len; // Reuse size for the length pointer

            regs->ax = -EINTR;
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}

static int entry_handler_sockopt(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct sockopt_kretprobe_data *data = (struct sockopt_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    data->is_ghost = false;
    if (is_guest_process(current->tgid)) {
        data->fd = (int)sys_regs->di;
        if (is_wormhole_fd(data->fd, &data->remote_fd)) {
            data->is_ghost = true;
            data->level = (int)sys_regs->si;
            data->optname = (int)sys_regs->dx;
            data->optval = (void __user *)sys_regs->r10;
            data->optlen = (void __user *)sys_regs->r8;
            sys_regs->di = -1; // Sabotage!
        }
    }
    return 0;
}

static int ret_handler_sockopt(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct sockopt_kretprobe_data *data = (struct sockopt_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;
    if (!data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;

            // --- Use strstr to safely identify the syscall! ---
            if (strstr(get_kretprobe(ri)->kp.symbol_name, "setsockopt")) rpc->is_setsockopt = true;
            else rpc->is_getsockopt = true;            
            
            rpc->remote_fd = data->remote_fd;
            rpc->sock_level = data->level;
            rpc->sock_optname = data->optname;
            rpc->buff = data->optval;
            
            // For setsockopt, optlen is an integer. For getsockopt, it's a pointer!
            if (rpc->is_setsockopt) rpc->sock_optlen = (int)(size_t)data->optlen;
            else rpc->size = (size_t)data->optlen; // Reuse size for the pointer

            regs->ax = -EINTR;
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- THE NETWORK HIJACKERS: Sendmsg & Recvmsg ---
static struct kretprobe sendmsg_kprobe;
static struct kretprobe recvmsg_kprobe;

struct msg_kretprobe_data { int fd; struct msghdr __user *msg; int flags; bool is_ghost; int remote_fd; };

static int entry_handler_msg(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct msg_kretprobe_data *data = (struct msg_kretprobe_data *)ri->data;
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    data->is_ghost = false;
    if (is_guest_process(current->tgid)) {
        data->fd = (int)sys_regs->di;
        if (is_wormhole_fd(data->fd, &data->remote_fd)) {
            data->is_ghost = true;
            data->msg = (struct msghdr __user *)sys_regs->si;
            data->flags = (int)sys_regs->dx;
            sys_regs->di = -1; // Sabotage!
        }
    }
    return 0;
}

static int ret_handler_msg(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct msg_kretprobe_data *data = (struct msg_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;
    if (!data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            
            // --- FIXED: Use strstr to safely identify the syscall! ---
            if (strstr(get_kretprobe(ri)->kp.symbol_name, "sendmsg")) rpc->is_sendmsg = true;
            else rpc->is_recvmsg = true;

            rpc->remote_fd = data->remote_fd;
            rpc->msg_ptr = data->msg;
            rpc->flags = data->flags;

            // --- FIXED: The Silent Ninja speaks! ---
            mattx_dbg("[HOOK] Intercepted %s(fd=%d). Freezing Surrogate %d...\n", 
                   rpc->is_sendmsg ? "sendmsg" : "recvmsg", data->fd, current->pid);

            regs->ax = -EINTR;
            send_sig(SIGSTOP, current, 0);
            schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- BATCH 1: LOCAL SPOOFERS (Zero Latency) ---
static struct kretprobe getpid_kprobe;
static struct kretprobe gettid_kprobe;

static int entry_handler_getpid(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }
static int ret_handler_getpid(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                regs->ax = guest_registry[i].orig_pid; // Spoof the PID instantly!
                break;
            }
        }
        spin_unlock(&guest_lock);
    }
    return 0;
}

// --- BATCH 1: WORMHOLE TUNNELERS ---
struct uname_kretprobe_data { void __user *buf; };
static struct kretprobe uname_kprobe;
static int entry_handler_uname(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        ((struct uname_kretprobe_data *)ri->data)->buf = (void __user *)SYSCALL_REGS(regs)->di;
        SYSCALL_REGS(regs)->di = 0; // Sabotage!
    }
    return 0;
}

// --- UNAME RET HANDLER ---
static int ret_handler_uname(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct uname_kretprobe_data *data = (struct uname_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0; int i;

    if (!is_guest_process(my_pid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_uname = true; rpc->buff = data->buf;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}



struct prlimit_kretprobe_data { pid_t pid; int resource; void __user *new_rlim; void __user *old_rlim; };
static struct kretprobe prlimit64_kprobe;
static int entry_handler_prlimit64(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct prlimit_kretprobe_data *data = (struct prlimit_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->pid = (pid_t)sys_regs->di; data->resource = (int)sys_regs->si;
        data->new_rlim = (void __user *)sys_regs->dx; data->old_rlim = (void __user *)sys_regs->r10;
        sys_regs->di = -1; // Sabotage!
    }
    return 0;
}


// --- PRLIMIT64 RET HANDLER ---
static int ret_handler_prlimit64(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct prlimit_kretprobe_data *data = (struct prlimit_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0; int i;

    if (!is_guest_process(my_pid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_prlimit64 = true; rpc->prlimit_pid = data->pid; rpc->prlimit_resource = data->resource;
            rpc->prlimit_has_new = (data->new_rlim != NULL); rpc->prlimit_has_old = (data->old_rlim != NULL);
            rpc->prlimit_new_rlim_ptr = data->new_rlim; rpc->prlimit_old_rlim_ptr = data->old_rlim;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}


struct prctl_kretprobe_data { int option; unsigned long arg2, arg3, arg4, arg5; };
static struct kretprobe prctl_kprobe;
static int entry_handler_prctl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct prctl_kretprobe_data *data = (struct prctl_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->option = (int)sys_regs->di; data->arg2 = sys_regs->si; data->arg3 = sys_regs->dx;
        data->arg4 = sys_regs->r10; data->arg5 = sys_regs->r8;
        sys_regs->di = -1; // Sabotage!
    }
    return 0;
}


// --- PRCTL RET HANDLER ---
static int ret_handler_prctl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    pid_t my_pid = current->pid;
    struct prctl_kretprobe_data *data = (struct prctl_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0; int i;

    if (!is_guest_process(my_pid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = my_pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_prctl = true; rpc->prctl_option = data->option; rpc->prctl_arg2 = data->arg2;
            rpc->prctl_arg3 = data->arg3; rpc->prctl_arg4 = data->arg4; rpc->prctl_arg5 = data->arg5;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}



// --- BATCH 2.1: FCNTL & IOCTL ---
struct fcntl_kretprobe_data { int fd; int cmd; unsigned long arg; bool is_ghost; int remote_fd; };
static struct kretprobe fcntl_kprobe;
static struct kretprobe ioctl_kprobe;

static int entry_handler_fcntl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct fcntl_kretprobe_data *data = (struct fcntl_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->fd = (int)sys_regs->di; data->cmd = (int)sys_regs->si; data->arg = sys_regs->dx;
        data->is_ghost = is_wormhole_fd(data->fd, &data->remote_fd);
        if (data->is_ghost) sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_fcntl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct fcntl_kretprobe_data *data = (struct fcntl_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid) || !data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node; rpc->remote_fd = data->remote_fd;
            
            bool is_ioctl = strstr(get_kretprobe(ri)->kp.symbol_name, "ioctl") != NULL;
            if (is_ioctl) { rpc->is_ioctl = true; rpc->ioctl_cmd = data->cmd; } 
            else { rpc->is_fcntl = true; rpc->fcntl_cmd = data->cmd; }
            
            rpc->ioctl_arg = data->arg;
            rpc->ioctl_has_ptr = (data->arg > 4096); // Heuristic: If arg > 4096, it's a pointer!
            
            if (rpc->ioctl_has_ptr) {
                struct task_struct *s = NULL; rcu_read_lock(); s = pid_task(find_vpid(current->pid), PIDTYPE_PID); if (s) get_task_struct(s); rcu_read_unlock();
                if (s) { access_process_vm(s, data->arg, rpc->ioctl_data, 256, FOLL_FORCE); put_task_struct(s); }
            }
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}

// --- BATCH 2.1: PREAD64 ---
struct pread64_kretprobe_data { int fd; void __user *buf; size_t count; loff_t pos; bool is_ghost; int remote_fd; };
static struct kretprobe pread64_kprobe;

static int entry_handler_pread64(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pread64_kretprobe_data *data = (struct pread64_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->fd = (int)sys_regs->di; data->buf = (void __user *)sys_regs->si;
        data->count = (size_t)sys_regs->dx; data->pos = (loff_t)sys_regs->r10;
        data->is_ghost = is_wormhole_fd(data->fd, &data->remote_fd);
        if (data->is_ghost) sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_pread64(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct pread64_kretprobe_data *data = (struct pread64_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid) || !data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node; rpc->remote_fd = data->remote_fd;
            rpc->is_pread64 = true; rpc->buff = data->buf; rpc->len = data->count; rpc->pread64_pos = data->pos;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}




// --- BATCH 2.2: DEDICATED METADATA FETCHERS ---
static struct kretprobe statfs_kprobe, fstatfs_kprobe, newfstatat_kprobe, faccessat2_kprobe, readlink_kprobe, readlinkat_kprobe;


struct statfs_kretprobe_data { const char __user *path; void __user *buf; bool is_hpc_fastpath; };

static int entry_handler_statfs(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct statfs_kretprobe_data *data = (struct statfs_kretprobe_data *)ri->data;
        data->path = (const char __user *)SYSCALL_REGS(regs)->di; 
        data->buf = (void __user *)SYSCALL_REGS(regs)->si;
        
        data->is_hpc_fastpath = false;
        if (config_hpc_local_libs && data->path) {
            char tmp_path[256] = {0};
            if (strncpy_from_user(tmp_path, data->path, sizeof(tmp_path) - 1) > 0) {
                if (is_hpc_local_lib(tmp_path)) data->is_hpc_fastpath = true;
            }
        }
        if (!data->is_hpc_fastpath) SYSCALL_REGS(regs)->di = 0; // Sabotage
    }
    return 0;
}

static int ret_handler_statfs(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct statfs_kretprobe_data *data = (struct statfs_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid)) return 0;
    if (data->is_hpc_fastpath) return 0; // BYPASS
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;
    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);
    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_statfs = true; rpc->meta_buf_ptr = data->buf;
            if (data->path) {
                if (strncpy_from_user(rpc->meta_path, data->path, sizeof(rpc->meta_path) - 1) < 0) {
                    mattx_dbg("[HOOK] Warning: Failed to read path for statfs!\n");
                } else if (config_hpc_local_libs && is_hpc_local_lib(rpc->meta_path)) {
                    mattx_dbg("[HOOK] HPC Fast-Path: Executing statfs('%s') locally.\n", rpc->meta_path);
                    kfree(rpc);
                    return 0;
                }
            }
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}




struct fstatfs_kretprobe_data { int fd; void __user *buf; bool is_ghost; };
static int entry_handler_fstatfs(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct fstatfs_kretprobe_data *data = (struct fstatfs_kretprobe_data *)ri->data;
        int local_fd = (int)SYSCALL_REGS(regs)->di;
        data->fd = local_fd;
        data->is_ghost = false;

        if (local_fd >= 0) data->is_ghost = is_wormhole_fd(local_fd, &data->fd);

        if (data->is_ghost) {
            mattx_dbg("[HOOK] fstatfs: Intercepted Ghost FD %d (Remote: %d). Tunneling to VM1...\n", local_fd, data->fd);
            SYSCALL_REGS(regs)->di = -1; // Sabotage
        } else {
            mattx_dbg("[HOOK] fstatfs: Local FD %d detected. Executing locally on VM2.\n", local_fd);
        }
        data->buf = (void __user *)SYSCALL_REGS(regs)->si;
    }
    return 0;
}

static int ret_handler_fstatfs(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct fstatfs_kretprobe_data *data = (struct fstatfs_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid) || !data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;
    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);
    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_fstatfs = true; rpc->meta_dfd = data->fd; rpc->meta_buf_ptr = data->buf;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}



struct newfstatat_kretprobe_data { int dfd; const char __user *path; void __user *buf; int flags; bool is_ghost; bool is_hpc_fastpath; };

static int entry_handler_newfstatat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct newfstatat_kretprobe_data *data = (struct newfstatat_kretprobe_data *)ri->data;
        int local_dfd = (int)SYSCALL_REGS(regs)->di;
        data->dfd = local_dfd;
        data->is_ghost = false;
        data->is_hpc_fastpath = false;
        data->path = (const char __user *)SYSCALL_REGS(regs)->si;
        data->buf = (void __user *)SYSCALL_REGS(regs)->dx; 
        data->flags = (int)SYSCALL_REGS(regs)->r10;

        if (config_hpc_local_libs && data->path) {
            char tmp_path[256] = {0};
            if (strncpy_from_user(tmp_path, data->path, sizeof(tmp_path) - 1) > 0) {
                if (is_hpc_local_lib(tmp_path)) data->is_hpc_fastpath = true;
            }
        }

        if (!data->is_hpc_fastpath) {
            if (local_dfd == AT_FDCWD) data->is_ghost = true;
            else if (local_dfd >= 0) data->is_ghost = is_wormhole_fd(local_dfd, &data->dfd);

            if (data->is_ghost) {
                SYSCALL_REGS(regs)->di = -1; // Sabotage
            }
        }
    }
    return 0;
}

static int ret_handler_newfstatat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct newfstatat_kretprobe_data *data = (struct newfstatat_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid)) return 0;
    if (data->is_hpc_fastpath) return 0; // BYPASS
    if (!data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;
    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);
    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_newfstatat = true; rpc->meta_dfd = data->dfd; rpc->meta_flags = data->flags; rpc->meta_buf_ptr = data->buf;
            if (data->path) {
                if (strncpy_from_user(rpc->meta_path, data->path, sizeof(rpc->meta_path) - 1) < 0) {
                    mattx_dbg("[HOOK] Warning: Failed to read path for newfstatat!\n");
                } else if (config_hpc_local_libs && is_hpc_local_lib(rpc->meta_path)) {
                    mattx_dbg("[HOOK] HPC Fast-Path: Executing newfstatat('%s') locally.\n", rpc->meta_path);
                    kfree(rpc);
                    return 0;
                }
            }
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}



struct faccessat2_kretprobe_data { int dfd; const char __user *path; int mode; int flags; bool is_ghost; bool is_hpc_fastpath; };

static int entry_handler_faccessat2(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct faccessat2_kretprobe_data *data = (struct faccessat2_kretprobe_data *)ri->data;
        int local_dfd = (int)SYSCALL_REGS(regs)->di;
        data->dfd = local_dfd;
        data->is_ghost = false;
        data->is_hpc_fastpath = false;
        data->path = (const char __user *)SYSCALL_REGS(regs)->si;
        data->mode = (int)SYSCALL_REGS(regs)->dx; 
        data->flags = (int)SYSCALL_REGS(regs)->r10;

        if (config_hpc_local_libs && data->path) {
            char tmp_path[256] = {0};
            if (strncpy_from_user(tmp_path, data->path, sizeof(tmp_path) - 1) > 0) {
                if (is_hpc_local_lib(tmp_path)) data->is_hpc_fastpath = true;
            }
        }

        if (!data->is_hpc_fastpath) {
            if (local_dfd == AT_FDCWD) data->is_ghost = true;
            else if (local_dfd >= 0) data->is_ghost = is_wormhole_fd(local_dfd, &data->dfd);

            if (data->is_ghost) {
                SYSCALL_REGS(regs)->di = -1; // Sabotage
            }
        }
    }
    return 0;
}

static int ret_handler_faccessat2(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct faccessat2_kretprobe_data *data = (struct faccessat2_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid)) return 0;
    if (data->is_hpc_fastpath) return 0; // BYPASS
    if (!data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;
    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);
    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_faccessat2 = true; rpc->meta_dfd = data->dfd; rpc->meta_mode = data->mode; rpc->meta_flags = data->flags;
            if (data->path) {
                if (strncpy_from_user(rpc->meta_path, data->path, sizeof(rpc->meta_path) - 1) < 0) {
                    mattx_dbg("[HOOK] Warning: Failed to read path for faccessat2!\n");
                } else if (config_hpc_local_libs && is_hpc_local_lib(rpc->meta_path)) {
                    mattx_dbg("[HOOK] HPC Fast-Path: Executing faccessat2('%s') locally.\n", rpc->meta_path);
                    kfree(rpc);
                    return 0;
                }
            }
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}



struct readlink_kretprobe_data { const char __user *path; void __user *buf; size_t bufsiz; bool is_hpc_fastpath; };

static int entry_handler_readlink(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct readlink_kretprobe_data *data = (struct readlink_kretprobe_data *)ri->data;
        data->path = (const char __user *)SYSCALL_REGS(regs)->di; 
        data->buf = (void __user *)SYSCALL_REGS(regs)->si;
        data->bufsiz = (size_t)SYSCALL_REGS(regs)->dx;
        data->is_hpc_fastpath = false;

        if (config_hpc_local_libs && data->path) {
            char tmp_path[256] = {0};
            if (strncpy_from_user(tmp_path, data->path, sizeof(tmp_path) - 1) > 0) {
                if (is_hpc_local_lib(tmp_path)) data->is_hpc_fastpath = true;
            }
        }

        if (!data->is_hpc_fastpath) SYSCALL_REGS(regs)->di = 0; // Sabotage
    }
    return 0;
}

static int ret_handler_readlink(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct readlink_kretprobe_data *data = (struct readlink_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid)) return 0;
    if (data->is_hpc_fastpath) return 0; // BYPASS
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;
    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);
    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_readlink = true; rpc->meta_bufsiz = data->bufsiz; rpc->meta_buf_ptr = data->buf;
            if (data->path) {
                if (strncpy_from_user(rpc->meta_path, data->path, sizeof(rpc->meta_path) - 1) < 0) {
                    mattx_dbg("[HOOK] Warning: Failed to read path for readlink!\n");
                } else if (config_hpc_local_libs && is_hpc_local_lib(rpc->meta_path)) {
                    mattx_dbg("[HOOK] HPC Fast-Path: Executing readlink('%s') locally.\n", rpc->meta_path);
                    kfree(rpc);
                    return 0;
                }
            }
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}



struct readlinkat_kretprobe_data { int dfd; const char __user *path; void __user *buf; size_t bufsiz; bool is_ghost; bool is_hpc_fastpath; };

static int entry_handler_readlinkat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct readlinkat_kretprobe_data *data = (struct readlinkat_kretprobe_data *)ri->data;
        int local_dfd = (int)SYSCALL_REGS(regs)->di;
        data->dfd = local_dfd;
        data->is_ghost = false;
        data->is_hpc_fastpath = false;
        data->path = (const char __user *)SYSCALL_REGS(regs)->si;
        data->buf = (void __user *)SYSCALL_REGS(regs)->dx; 
        data->bufsiz = (size_t)SYSCALL_REGS(regs)->r10;

        if (config_hpc_local_libs && data->path) {
            char tmp_path[256] = {0};
            if (strncpy_from_user(tmp_path, data->path, sizeof(tmp_path) - 1) > 0) {
                if (is_hpc_local_lib(tmp_path)) data->is_hpc_fastpath = true;
            }
        }

        if (!data->is_hpc_fastpath) {
            if (local_dfd == AT_FDCWD) data->is_ghost = true;
            else if (local_dfd >= 0) data->is_ghost = is_wormhole_fd(local_dfd, &data->dfd);

            if (data->is_ghost) SYSCALL_REGS(regs)->di = -1; // Sabotage
        }
    }
    return 0;
}

static int ret_handler_readlinkat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct readlinkat_kretprobe_data *data = (struct readlinkat_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid)) return 0;
    if (data->is_hpc_fastpath) return 0; // BYPASS
    if (!data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;
    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);
    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_readlinkat = true; rpc->meta_dfd = data->dfd; rpc->meta_bufsiz = data->bufsiz; rpc->meta_buf_ptr = data->buf;
            if (data->path) {
                if (strncpy_from_user(rpc->meta_path, data->path, sizeof(rpc->meta_path) - 1) < 0) {
                    mattx_dbg("[HOOK] Warning: Failed to read path for readlinkat!\n");
                } else if (config_hpc_local_libs && is_hpc_local_lib(rpc->meta_path)) {
                    mattx_dbg("[HOOK] HPC Fast-Path: Executing readlinkat('%s') locally.\n", rpc->meta_path);
                    kfree(rpc);
                    return 0;
                }
            }
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- BATCH 2.3: GETDENTS64 ---
struct getdents64_kretprobe_data { int fd; void __user *dirp; u32 count; bool is_ghost; int remote_fd; };
static struct kretprobe getdents64_kprobe;

static int entry_handler_getdents64(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct getdents64_kretprobe_data *data = (struct getdents64_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->fd = (int)sys_regs->di; data->dirp = (void __user *)sys_regs->si; data->count = (u32)sys_regs->dx;
        data->is_ghost = false;
        if (data->fd >= 0) data->is_ghost = is_wormhole_fd(data->fd, &data->remote_fd);
        if (data->is_ghost) sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_getdents64(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct getdents64_kretprobe_data *data = (struct getdents64_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid) || !data->is_ghost) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_getdents64 = true; rpc->getdents64_fd = data->remote_fd; 
            rpc->getdents64_dirp = data->dirp; rpc->getdents64_count = data->count;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}

// --- BATCH 2.3: PIPE2 ---
struct pipe2_kretprobe_data { void __user *pipefd; int flags; };
static struct kretprobe pipe2_kprobe;

static int entry_handler_pipe2(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pipe2_kretprobe_data *data = (struct pipe2_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->pipefd = (void __user *)sys_regs->di; data->flags = (int)sys_regs->si;
        sys_regs->di = 0; // Sabotage! (Pass NULL so native pipe2 fails safely)
    }
    return 0;
}

static int ret_handler_pipe2(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct pipe2_kretprobe_data *data = (struct pipe2_kretprobe_data *)ri->data;
    if (!is_guest_process(current->tgid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    int home_node = -1; u32 orig_pid = 0;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) { home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid; guest_registry[i].rpc_done = false; break; }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_pipe2 = true; rpc->pipe2_pipefd = data->pipefd; rpc->pipe2_flags = data->flags;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}


// ============================================================================
// BATCH 3.1: LOCAL MEMORY ALLOCATORS (Executes natively on VM2)
// ============================================================================

// --- 1. BRK ---
static struct kretprobe brk_kprobe;
static int entry_handler_brk(struct kretprobe_instance *ri, struct pt_regs *regs) {
    // We let brk execute natively on VM2 to allocate local heap memory.
    // (Logging this would spam dmesg and crash the node, as malloc calls it constantly!)
    return 0;
}
static int ret_handler_brk(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 2. MUNMAP ---
static struct kretprobe munmap_kprobe;
static int entry_handler_munmap(struct kretprobe_instance *ri, struct pt_regs *regs) {
    // Let the kernel free the local memory natively.
    return 0;
}
static int ret_handler_munmap(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 3. MREMAP ---
static struct kretprobe mremap_kprobe;
static int entry_handler_mremap(struct kretprobe_instance *ri, struct pt_regs *regs) {
    // Let the kernel resize the local memory natively.
    return 0;
}
static int ret_handler_mremap(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 4. MMAP (The Guardrail) ---
struct mmap_kretprobe_data { bool is_ghost; int fd; };
static struct kretprobe mmap_kprobe;

static int entry_handler_mmap(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct mmap_kretprobe_data *data = (struct mmap_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        
        // In x86_64, the mmap arguments are:
        // rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=offset
        int local_fd = (int)sys_regs->r8;
        data->fd = local_fd;
        data->is_ghost = false;

        if (local_fd >= 0) {
            data->is_ghost = is_wormhole_fd(local_fd, NULL);
        }

        if (data->is_ghost) {
            mattx_dbg("[HOOK] mmap: Ghost FD %d detected! Blocking local mmap (DSM not supported yet).\n", local_fd);
            // Sabotage the syscall so the kernel fails it safely
            sys_regs->di = -1; // Invalid address
            sys_regs->si = 0;  // Zero length
        }
    }
    return 0;
}

static int ret_handler_mmap(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct mmap_kretprobe_data *data = (struct mmap_kretprobe_data *)ri->data;
        if (data->is_ghost) {
            // Force Permission Denied! The app must handle the error.
            regs->ax = -EACCES; 
        }
    }
    return 0;
}



// ============================================================================
// BATCH 3.2: LOCAL MEMORY TUNERS (Executes natively on VM2)
// ============================================================================

// --- 1. MPROTECT ---
static struct kretprobe mprotect_kprobe;
static int entry_handler_mprotect(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        // x86_64 args: rdi=addr, rsi=len, rdx=prot
        mattx_dbg("[HOOK] mprotect: Local execution on VM2 (addr: 0x%lx, len: %lu, prot: %d)\n",
                  sys_regs->di, sys_regs->si, (int)sys_regs->dx);
    }
    return 0; // Let it execute natively!
}
static int ret_handler_mprotect(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 2. MADVISE ---
static struct kretprobe madvise_kprobe;
static int entry_handler_madvise(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        // x86_64 args: rdi=addr, rsi=length, rdx=advice
        mattx_dbg("[HOOK] madvise: Local execution on VM2 (addr: 0x%lx, len: %lu, advice: %d)\n",
                  sys_regs->di, sys_regs->si, (int)sys_regs->dx);
    }
    return 0; // Let it execute natively!
}
static int ret_handler_madvise(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 3. MBIND ---
static struct kretprobe mbind_kprobe;
static int entry_handler_mbind(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        // x86_64 args: rdi=addr, rsi=len, rdx=mode
        mattx_dbg("[HOOK] mbind: Local execution on VM2 (addr: 0x%lx, len: %lu, mode: %d)\n",
                  sys_regs->di, sys_regs->si, (int)sys_regs->dx);
    }
    return 0; // Let it execute natively!
}
static int ret_handler_mbind(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }



// ============================================================================
// BATCH 3.3: LOCAL SCHEDULER CONTROLS (Executes natively on VM2)
// ============================================================================

// --- 1. SCHED_GETAFFINITY ---
static struct kretprobe sched_getaffinity_kprobe;
static int entry_handler_sched_getaffinity(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        // x86_64 args: rdi=pid, rsi=len, rdx=user_mask_ptr
        mattx_dbg("[HOOK] sched_getaffinity: Local execution on VM2 (pid: %d, len: %u)\n",
                  (int)sys_regs->di, (unsigned int)sys_regs->si);
    }
    return 0; // Let it execute natively!
}
static int ret_handler_sched_getaffinity(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 2. SCHED_SETAFFINITY ---
static struct kretprobe sched_setaffinity_kprobe;
static int entry_handler_sched_setaffinity(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        // x86_64 args: rdi=pid, rsi=len, rdx=user_mask_ptr
        mattx_dbg("[HOOK] sched_setaffinity: Local execution on VM2 (pid: %d, len: %u)\n",
                  (int)sys_regs->di, (unsigned int)sys_regs->si);
    }
    return 0; // Let it execute natively!
}
static int ret_handler_sched_setaffinity(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }

// --- 3. SCHED_YIELD ---
static struct kretprobe sched_yield_kprobe;
static int entry_handler_sched_yield(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        mattx_dbg("[HOOK] sched_yield: Local execution on VM2. Yielding CPU.\n");
    }
    return 0; // Let it execute natively!
}
static int ret_handler_sched_yield(struct kretprobe_instance *ri, struct pt_regs *regs) { return 0; }


// ==============================================================================
// 👶 THE CRADLE INTERCEPTOR (Newborn Thread Freezer)
// ==============================================================================
static struct kprobe wake_up_new_task_kprobe;

struct mattx_newborn_ctx {
    struct callback_head cb;
};

static void mattx_newborn_cb(struct callback_head *cb) {
    struct mattx_newborn_ctx *ctx = container_of(cb, struct mattx_newborn_ctx, cb);
    kfree(ctx);
    mattx_dbg("[CRADLE] Newborn thread %d freezing at user-space boundary!\n", current->pid);
    set_current_state(TASK_STOPPED);
    schedule();
}

static int entry_handler_wake_up_new_task(struct kprobe *p, struct pt_regs *regs) {
    // In wake_up_new_task(struct task_struct *p), the child task is in regs->di
    struct task_struct *child = (struct task_struct *)regs->di;
    bool is_growing = false;

    // Check if the current task (the Mother) is actively growing a gang!
    spin_lock(&export_lock);
    for (int i = 0; i < export_count; i++) {
        if (export_registry[i].orig_pid == current->tgid) {
            is_growing = export_registry[i].is_growing_gang;
            break;
        }
    }
    spin_unlock(&export_lock);

    if (is_growing && child) {
        struct mattx_newborn_ctx *ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
        if (ctx) {
            init_task_work(&ctx->cb, mattx_newborn_cb);
            if (real_task_work_add) {
                // Inject the freeze callback directly into the child's brain!
                real_task_work_add(child, &ctx->cb, TWA_SIGNAL);
                mattx_dbg("[CRADLE] Injected freeze callback into newborn thread %d\n", child->pid);
            } else {
                kfree(ctx);
            }
        }
    }
    return 0;
}
// ==============================================================================


// ==============================================================================
// 🎯 THE TGKILL TRANSLATOR (Signal Router)
// ==============================================================================
static struct kretprobe tgkill_kprobe;

static int entry_handler_tgkill(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct pt_regs *sys_regs = SYSCALL_REGS(regs);
    
    if (is_guest_process(current->tgid)) {
        u32 target_tid = (u32)sys_regs->si; // The spoofed VM1 TID
        
        spin_lock(&guest_lock);
        for (int i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == current->tgid) {
                // Found our gang! Let's translate the TID.
                for (int j = 0; j < guest_registry[i].thread_count; j++) {
                    if (guest_registry[i].orig_tids[j] == target_tid) {
                        sys_regs->si = guest_registry[i].local_tids[j]; // Inject real VM2 TID!
                        sys_regs->di = current->tgid; // Translate TGID too, just in case!
                        mattx_dbg("[HOOK] tgkill translated VM1 TID %u -> VM2 TID %u\n", 
                                  target_tid, (u32)sys_regs->si);
                        break;
                    }
                }
                break;
            }
        }
        spin_unlock(&guest_lock);
    }
    return 0;
}

static int ret_handler_tgkill(struct kretprobe_instance *ri, struct pt_regs *regs) {
    return 0; // We only need to modify the entry arguments!
}
// ==============================================================================




// ============================================================================
// DSM CONTROL PLANE KPROBES
// ============================================================================

// --- SHMGET ---
struct shmget_kretprobe_data { int key; size_t size; int shmflg; };
static struct kretprobe shmget_kprobe;

static int entry_handler_shmget(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct shmget_kretprobe_data *data = (struct shmget_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->key = (int)sys_regs->di;
        data->size = (size_t)sys_regs->si;
        data->shmflg = (int)sys_regs->dx;
        sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_shmget(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (!is_guest_process(current->tgid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    struct shmget_kretprobe_data *data = (struct shmget_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_shmget = true; rpc->shm_key = data->key; rpc->shm_size = data->size; rpc->shm_flg = data->shmflg;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}

// --- SHMCTL ---
struct shmctl_kretprobe_data { int shmid; int cmd; void __user *buf; };
static struct kretprobe shmctl_kprobe;

static int entry_handler_shmctl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct shmctl_kretprobe_data *data = (struct shmctl_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->shmid = (int)sys_regs->di;
        data->cmd = (int)sys_regs->si;
        data->buf = (void __user *)sys_regs->dx;
        sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_shmctl(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (!is_guest_process(current->tgid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    struct shmctl_kretprobe_data *data = (struct shmctl_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_shmctl = true; rpc->shm_id = data->shmid; rpc->shm_cmd = data->cmd; rpc->buff = data->buf;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}

// --- SHMDT ---
struct shmdt_kretprobe_data { unsigned long shmaddr; };
static struct kretprobe shmdt_kprobe;

static int entry_handler_shmdt(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct shmdt_kretprobe_data *data = (struct shmdt_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->shmaddr = (unsigned long)sys_regs->di;
        sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_shmdt(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (!is_guest_process(current->tgid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    struct shmdt_kretprobe_data *data = (struct shmdt_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_shmdt = true; rpc->shm_addr = data->shmaddr;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}


// --- SHMAT ---
struct shmat_kretprobe_data { int shmid; unsigned long shmaddr; int shmflg; };
static struct kretprobe shmat_kprobe;

static int entry_handler_shmat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (is_guest_process(current->tgid)) {
        struct shmat_kretprobe_data *data = (struct shmat_kretprobe_data *)ri->data;
        struct pt_regs *sys_regs = SYSCALL_REGS(regs);
        data->shmid = (int)sys_regs->di;
        data->shmaddr = (unsigned long)sys_regs->si;
        data->shmflg = (int)sys_regs->dx;
        sys_regs->di = -1; // Sabotage!
    }
    return 0;
}

static int ret_handler_shmat(struct kretprobe_instance *ri, struct pt_regs *regs) {
    if (!is_guest_process(current->tgid)) return 0;
    if (fatal_signal_pending(current) || (current->flags & PF_EXITING)) return 0;

    struct shmat_kretprobe_data *data = (struct shmat_kretprobe_data *)ri->data;
    int home_node = -1; u32 orig_pid = 0;

    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == current->tgid) {
            home_node = guest_registry[i].home_node; orig_pid = guest_registry[i].orig_pid;
            guest_registry[i].rpc_done = false; break;
        }
    }
    spin_unlock(&guest_lock);

    if (home_node != -1) {
        struct mattx_rpc_work *rpc = kmalloc(sizeof(*rpc), GFP_ATOMIC); 
        if (rpc) {
            INIT_WORK(&rpc->work, mattx_rpc_worker);
            rpc->local_pid = current->pid; rpc->orig_pid = orig_pid; rpc->home_node = home_node;
            rpc->is_shmat = true; rpc->shm_id = data->shmid; rpc->shm_addr = data->shmaddr; rpc->shm_flg = data->shmflg;
            send_sig(SIGSTOP, current, 0); schedule_work(&rpc->work);
        }
    }
    return 0;
}






// --- KPROBE REGISTRATION ---

int mattx_hooks_init(void) {
    int ret;
    memset(&openat_kprobe, 0, sizeof(openat_kprobe));
    openat_kprobe.kp.symbol_name = "do_sys_openat2";
    openat_kprobe.entry_handler = entry_handler_openat;
    openat_kprobe.handler = ret_handler_openat;
    openat_kprobe.data_size = sizeof(struct kretprobe_data); 
    openat_kprobe.maxactive = 64; 

    ret = register_kretprobe(&openat_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed, returned %d\n", ret);
        return ret;
    }

    memset(&dup_kprobe, 0, sizeof(dup_kprobe));
    dup_kprobe.kp.symbol_name = "__x64_sys_dup";
    dup_kprobe.entry_handler = entry_handler_dup;
    dup_kprobe.handler = ret_handler_dup;
    dup_kprobe.data_size = sizeof(struct dup_kretprobe_data);
    dup_kprobe.maxactive = 64;

    ret = register_kretprobe(&dup_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for dup, returned %d\n", ret);
    }

    memset(&dup2_kprobe, 0, sizeof(dup2_kprobe));
    dup2_kprobe.kp.symbol_name = "__x64_sys_dup2";
    dup2_kprobe.entry_handler = entry_handler_dup;
    dup2_kprobe.handler = ret_handler_dup;
    dup2_kprobe.data_size = sizeof(struct dup_kretprobe_data);
    dup2_kprobe.maxactive = 64;

    ret = register_kretprobe(&dup2_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for dup2, returned %d\n", ret);
    }

    memset(&unlinkat_kprobe, 0, sizeof(unlinkat_kprobe));
    unlinkat_kprobe.kp.symbol_name = "__x64_sys_unlinkat";
    unlinkat_kprobe.entry_handler = entry_handler_unlinkat;
    unlinkat_kprobe.handler = ret_handler_unlinkat;
    unlinkat_kprobe.data_size = sizeof(struct unlinkat_kretprobe_data);
    unlinkat_kprobe.maxactive = 64;

    ret = register_kretprobe(&unlinkat_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for unlinkat, returned %d\n", ret);
    }

    memset(&socket_kprobe, 0, sizeof(socket_kprobe));
    socket_kprobe.kp.symbol_name = "__sys_socket";
    socket_kprobe.entry_handler = entry_handler_socket;
    socket_kprobe.handler = ret_handler_socket;
    socket_kprobe.data_size = sizeof(struct socket_kretprobe_data);
    socket_kprobe.maxactive = 64;

    ret = register_kretprobe(&socket_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for socket, returned %d\n", ret);
    }

    memset(&connect_kprobe, 0, sizeof(connect_kprobe));
    connect_kprobe.kp.symbol_name = "__sys_connect";
    connect_kprobe.entry_handler = entry_handler_connect;
    connect_kprobe.handler = ret_handler_connect;
    connect_kprobe.data_size = sizeof(struct connect_kretprobe_data);
    connect_kprobe.maxactive = 64;

    ret = register_kretprobe(&connect_kprobe);
    if (ret < 0) {
        printk(KERN_ERR "MattX: register_kretprobe failed for connect, returned %d\n", ret);
    }

    memset(&bind_kprobe, 0, sizeof(bind_kprobe));
    bind_kprobe.kp.symbol_name = "__sys_bind";
    bind_kprobe.entry_handler = entry_handler_bind;
    bind_kprobe.handler = ret_handler_bind;
    bind_kprobe.data_size = sizeof(struct connect_kretprobe_data); // we reuse connect data structure
    bind_kprobe.maxactive = 64;

    ret = register_kretprobe(&bind_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for bind, returned %d\n", ret);

    memset(&listen_kprobe, 0, sizeof(listen_kprobe));
    listen_kprobe.kp.symbol_name = "__sys_listen";
    listen_kprobe.entry_handler = entry_handler_listen;
    listen_kprobe.handler = ret_handler_listen;
    listen_kprobe.data_size = sizeof(struct listen_kretprobe_data);
    listen_kprobe.maxactive = 64;

    ret = register_kretprobe(&listen_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for listen, returned %d\n", ret);

    // SENDTO / RECVFROM ---
    memset(&sendto_kprobe, 0, sizeof(sendto_kprobe));
    sendto_kprobe.kp.symbol_name = "__sys_sendto";
    sendto_kprobe.entry_handler = entry_handler_sendto;
    sendto_kprobe.handler = ret_handler_sendto;
    sendto_kprobe.data_size = sizeof(struct sendto_kretprobe_data);
    sendto_kprobe.maxactive = 64;

    ret = register_kretprobe(&sendto_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for sendto, returned %d\n", ret);

    memset(&recvfrom_kprobe, 0, sizeof(recvfrom_kprobe));
    recvfrom_kprobe.kp.symbol_name = "__sys_recvfrom";
    recvfrom_kprobe.entry_handler = entry_handler_recvfrom;
    recvfrom_kprobe.handler = ret_handler_recvfrom;
    recvfrom_kprobe.data_size = sizeof(struct recvfrom_kretprobe_data);
    recvfrom_kprobe.maxactive = 64;

    ret = register_kretprobe(&recvfrom_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for recvfrom, returned %d\n", ret);

    memset(&accept_kprobe, 0, sizeof(accept_kprobe));
    accept_kprobe.kp.symbol_name = "__sys_accept4";
    accept_kprobe.entry_handler = entry_handler_accept;
    accept_kprobe.handler = ret_handler_accept;
    accept_kprobe.data_size = sizeof(struct accept_kretprobe_data);
    accept_kprobe.maxactive = 64;
    
    ret = register_kretprobe(&accept_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for accept, returned %d\n", ret);

    memset(&poll_kprobe, 0, sizeof(poll_kprobe));
    poll_kprobe.kp.symbol_name = "__x64_sys_poll";
    poll_kprobe.entry_handler = entry_handler_poll;
    poll_kprobe.handler = ret_handler_poll;
    poll_kprobe.data_size = sizeof(struct poll_kretprobe_data);
    poll_kprobe.maxactive = 64;

    ret = register_kretprobe(&poll_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for poll, returned %d\n", ret);

    memset(&select_kprobe, 0, sizeof(select_kprobe));
    select_kprobe.kp.symbol_name = "__x64_sys_select";
    select_kprobe.entry_handler = entry_handler_select;
    select_kprobe.handler = ret_handler_select;
    select_kprobe.data_size = sizeof(struct select_kretprobe_data);
    select_kprobe.maxactive = 64;
    
    ret = register_kretprobe(&select_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for select, returned %d\n", ret);

    memset(&pselect6_kprobe, 0, sizeof(pselect6_kprobe));
    pselect6_kprobe.kp.symbol_name = "__x64_sys_pselect6";
    pselect6_kprobe.entry_handler = entry_handler_pselect6;
    pselect6_kprobe.handler = ret_handler_pselect6;
    pselect6_kprobe.data_size = sizeof(struct pselect6_kretprobe_data);
    pselect6_kprobe.maxactive = 64;
    
    ret = register_kretprobe(&pselect6_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for pselect6, returned %d\n", ret);
    
    memset(&read_kprobe, 0, sizeof(read_kprobe));
    read_kprobe.kp.symbol_name = "__x64_sys_read";
    read_kprobe.entry_handler = entry_handler_read;
    read_kprobe.handler = ret_handler_read;
    read_kprobe.data_size = sizeof(struct rw_kretprobe_data);
    read_kprobe.maxactive = 64;
    ret = register_kretprobe(&read_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for read, returned %d\n", ret);

    memset(&write_kprobe, 0, sizeof(write_kprobe));
    write_kprobe.kp.symbol_name = "__x64_sys_write";
    write_kprobe.entry_handler = entry_handler_write;
    write_kprobe.handler = ret_handler_write;
    write_kprobe.data_size = sizeof(struct rw_kretprobe_data);
    write_kprobe.maxactive = 64;
    ret = register_kretprobe(&write_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for write, returned %d\n", ret);

    memset(&lseek_kprobe, 0, sizeof(lseek_kprobe));
    lseek_kprobe.kp.symbol_name = "__x64_sys_lseek";
    lseek_kprobe.entry_handler = entry_handler_fileio; // Use the unified handler!
    lseek_kprobe.handler = ret_handler_fileio;         // Use the unified handler!
    lseek_kprobe.data_size = sizeof(struct fileio_kretprobe_data);
    lseek_kprobe.maxactive = 64;
    ret = register_kretprobe(&lseek_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for lseek, returned %d\n", ret);

    memset(&fsync_kprobe, 0, sizeof(fsync_kprobe));
    fsync_kprobe.kp.symbol_name = "__x64_sys_fsync";
    fsync_kprobe.entry_handler = entry_handler_fileio; // Use the unified handler!
    fsync_kprobe.handler = ret_handler_fileio;         // Use the unified handler!
    fsync_kprobe.data_size = sizeof(struct fileio_kretprobe_data);
    fsync_kprobe.maxactive = 64;
    ret = register_kretprobe(&fsync_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for fsync, returned %d\n", ret);

    memset(&statx_kprobe, 0, sizeof(statx_kprobe));
    statx_kprobe.kp.symbol_name = "__x64_sys_statx";
    statx_kprobe.entry_handler = entry_handler_fileio; // Use the unified handler!
    statx_kprobe.handler = ret_handler_fileio;         // Use the unified handler!
    statx_kprobe.data_size = sizeof(struct fileio_kretprobe_data);
    statx_kprobe.maxactive = 64;
    ret = register_kretprobe(&statx_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for statx, returned %d\n", ret);

    memset(&epoll_ctl_kprobe, 0, sizeof(epoll_ctl_kprobe));
    epoll_ctl_kprobe.kp.symbol_name = "__x64_sys_epoll_ctl";
    epoll_ctl_kprobe.entry_handler = entry_handler_epoll_ctl;
    epoll_ctl_kprobe.handler = ret_handler_epoll_ctl;
    epoll_ctl_kprobe.data_size = sizeof(struct epoll_ctl_kretprobe_data);
    epoll_ctl_kprobe.maxactive = 64;
    ret = register_kretprobe(&epoll_ctl_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for epoll_ctl, returned %d\n", ret); 

    memset(&epoll_wait_kprobe, 0, sizeof(epoll_wait_kprobe));
    epoll_wait_kprobe.kp.symbol_name = "__x64_sys_epoll_wait";
    epoll_wait_kprobe.entry_handler = entry_handler_epoll_wait;
    epoll_wait_kprobe.handler = ret_handler_epoll_wait;
    epoll_wait_kprobe.data_size = sizeof(struct epoll_wait_kretprobe_data); // <-- THIS ONE!
    epoll_wait_kprobe.maxactive = 64;
    ret = register_kretprobe(&epoll_wait_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for epoll_wait, returned %d\n", ret);

    memset(&epoll_create_kprobe, 0, sizeof(epoll_create_kprobe));
    epoll_create_kprobe.kp.symbol_name = "__x64_sys_epoll_create";
    epoll_create_kprobe.entry_handler = entry_handler_epoll_create;
    epoll_create_kprobe.handler = ret_handler_epoll_create;
    epoll_create_kprobe.data_size = sizeof(struct epoll_create_kretprobe_data);
    epoll_create_kprobe.maxactive = 64;
    ret = register_kretprobe(&epoll_create_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for epoll_create, returned %d\n", ret);

    memset(&epoll_create1_kprobe, 0, sizeof(epoll_create1_kprobe));
    epoll_create1_kprobe.kp.symbol_name = "__x64_sys_epoll_create1";
    epoll_create1_kprobe.entry_handler = entry_handler_epoll_create;
    epoll_create1_kprobe.handler = ret_handler_epoll_create;
    epoll_create1_kprobe.data_size = sizeof(struct epoll_create_kretprobe_data);
    epoll_create1_kprobe.maxactive = 64;
    ret = register_kretprobe(&epoll_create1_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for epoll_create1, returned %d\n", ret   );

    // --- SOCKNAME & PEERNAME ---
    memset(&getsockname_kprobe, 0, sizeof(getsockname_kprobe));
    getsockname_kprobe.kp.symbol_name = "__x64_sys_getsockname";
    getsockname_kprobe.entry_handler = entry_handler_sockname;
    getsockname_kprobe.handler = ret_handler_sockname;
    getsockname_kprobe.data_size = sizeof(struct sockname_kretprobe_data);
    getsockname_kprobe.maxactive = 64;
    ret = register_kretprobe(&getsockname_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for getsockname, returned %d\n", ret);

    memset(&getpeername_kprobe, 0, sizeof(getpeername_kprobe));
    getpeername_kprobe.kp.symbol_name = "__x64_sys_getpeername";
    getpeername_kprobe.entry_handler = entry_handler_sockname; // Shared!
    getpeername_kprobe.handler = ret_handler_sockname;         // Shared!
    getpeername_kprobe.data_size = sizeof(struct sockname_kretprobe_data);
    getpeername_kprobe.maxactive = 64;
    ret = register_kretprobe(&getpeername_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for getpeername, returned %d\n", ret );

    // --- SOCKOPTS ---
    memset(&setsockopt_kprobe, 0, sizeof(setsockopt_kprobe));
    setsockopt_kprobe.kp.symbol_name = "__x64_sys_setsockopt";
    setsockopt_kprobe.entry_handler = entry_handler_sockopt;
    setsockopt_kprobe.handler = ret_handler_sockopt;
    setsockopt_kprobe.data_size = sizeof(struct sockopt_kretprobe_data);
    setsockopt_kprobe.maxactive = 64;
    ret = register_kretprobe(&setsockopt_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for setsockopt, returned %d\n", ret);

    memset(&getsockopt_kprobe, 0, sizeof(getsockopt_kprobe));
    getsockopt_kprobe.kp.symbol_name = "__x64_sys_getsockopt";
    getsockopt_kprobe.entry_handler = entry_handler_sockopt; // Shared!
    getsockopt_kprobe.handler = ret_handler_sockopt;         // Shared!
    getsockopt_kprobe.data_size = sizeof(struct sockopt_kretprobe_data);
    getsockopt_kprobe.maxactive = 64;
    ret = register_kretprobe(&getsockopt_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for getsockopt, returned %d\n", ret);

    // --- SENDMSG & RECVMSG ---
    memset(&sendmsg_kprobe, 0, sizeof(sendmsg_kprobe));
    sendmsg_kprobe.kp.symbol_name = "__x64_sys_sendmsg";
    sendmsg_kprobe.entry_handler = entry_handler_msg;
    sendmsg_kprobe.handler = ret_handler_msg;
    sendmsg_kprobe.data_size = sizeof(struct msg_kretprobe_data);
    sendmsg_kprobe.maxactive = 64;
    ret = register_kretprobe(&sendmsg_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for sendmsg, returned %d\n", ret);

    memset(&recvmsg_kprobe, 0, sizeof(recvmsg_kprobe));
    recvmsg_kprobe.kp.symbol_name = "__x64_sys_recvmsg";
    recvmsg_kprobe.entry_handler = entry_handler_msg; // Shared!
    recvmsg_kprobe.handler = ret_handler_msg;         // Shared!
    recvmsg_kprobe.data_size = sizeof(struct msg_kretprobe_data);
    recvmsg_kprobe.maxactive = 64;
    ret = register_kretprobe(&recvmsg_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for recvmsg, returned %d\n", ret);

    memset(&getpid_kprobe, 0, sizeof(getpid_kprobe));
    getpid_kprobe.kp.symbol_name = "__x64_sys_getpid";
    getpid_kprobe.entry_handler = entry_handler_getpid;
    getpid_kprobe.handler = ret_handler_getpid;
    getpid_kprobe.maxactive = 64;
    ret = register_kretprobe(&getpid_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for getpid, returned %d\n", ret);

    memset(&gettid_kprobe, 0, sizeof(gettid_kprobe));
    gettid_kprobe.kp.symbol_name = "__x64_sys_gettid";
    gettid_kprobe.entry_handler = entry_handler_getpid; // Reusing getpid handler to spoof main thread!
    gettid_kprobe.handler = ret_handler_getpid;
    gettid_kprobe.maxactive = 64;
    ret = register_kretprobe(&gettid_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for gettid, returned %d\n", ret);

    memset(&uname_kprobe, 0, sizeof(uname_kprobe));
    uname_kprobe.kp.symbol_name = "__x64_sys_newuname";
    uname_kprobe.entry_handler = entry_handler_uname;
    uname_kprobe.handler = ret_handler_uname;
    uname_kprobe.data_size = sizeof(struct uname_kretprobe_data);
    uname_kprobe.maxactive = 64;
    ret = register_kretprobe(&uname_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for uname, returned %d\n", ret   );

    memset(&prlimit64_kprobe, 0, sizeof(prlimit64_kprobe));
    prlimit64_kprobe.kp.symbol_name = "__x64_sys_prlimit64";
    prlimit64_kprobe.entry_handler = entry_handler_prlimit64;
    prlimit64_kprobe.handler = ret_handler_prlimit64;
    prlimit64_kprobe.data_size = sizeof(struct prlimit_kretprobe_data);
    prlimit64_kprobe.maxactive = 64;
    ret = register_kretprobe(&prlimit64_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for prlimit64, returned %d\n", ret   );

    memset(&prctl_kprobe, 0, sizeof(prctl_kprobe));
    prctl_kprobe.kp.symbol_name = "__x64_sys_prctl";
    prctl_kprobe.entry_handler = entry_handler_prctl;
    prctl_kprobe.handler = ret_handler_prctl;
    prctl_kprobe.data_size = sizeof(struct prctl_kretprobe_data);
    prctl_kprobe.maxactive = 64;
    ret = register_kretprobe(&prctl_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for prctl, returned %d\n", ret);

    memset(&fcntl_kprobe, 0, sizeof(fcntl_kprobe));
    fcntl_kprobe.kp.symbol_name = "__x64_sys_fcntl";
    fcntl_kprobe.entry_handler = entry_handler_fcntl;
    fcntl_kprobe.handler = ret_handler_fcntl;
    fcntl_kprobe.data_size = sizeof(struct fcntl_kretprobe_data);
    fcntl_kprobe.maxactive = 64;
    ret = register_kretprobe(&fcntl_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for fcntl, returned %d\n", ret);

    memset(&ioctl_kprobe, 0, sizeof(ioctl_kprobe));
    ioctl_kprobe.kp.symbol_name = "__x64_sys_ioctl";
    ioctl_kprobe.entry_handler = entry_handler_fcntl; // Reused!
    ioctl_kprobe.handler = ret_handler_fcntl;         // Reused!
    ioctl_kprobe.data_size = sizeof(struct fcntl_kretprobe_data);
    ioctl_kprobe.maxactive = 64;
    ret = register_kretprobe(&ioctl_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for ioctl, returned %d\n", ret);

    memset(&pread64_kprobe, 0, sizeof(pread64_kprobe));
    pread64_kprobe.kp.symbol_name = "__x64_sys_pread64";
    pread64_kprobe.entry_handler = entry_handler_pread64;
    pread64_kprobe.handler = ret_handler_pread64;
    pread64_kprobe.data_size = sizeof(struct pread64_kretprobe_data);
    pread64_kprobe.maxactive = 64;
    ret = register_kretprobe(&pread64_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for pread64, returned %d\n", ret);

    memset(&statfs_kprobe, 0, sizeof(statfs_kprobe)); 
    statfs_kprobe.kp.symbol_name = "__x64_sys_statfs"; 
    statfs_kprobe.entry_handler = entry_handler_statfs; 
    statfs_kprobe.handler = ret_handler_statfs; 
    statfs_kprobe.data_size = sizeof(struct statfs_kretprobe_data); 
    statfs_kprobe.maxactive = 64; 
    ret = register_kretprobe(&statfs_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for statfs, returned %d\n", ret);

    memset(&fstatfs_kprobe, 0, sizeof(fstatfs_kprobe)); 
    fstatfs_kprobe.kp.symbol_name = "__x64_sys_fstatfs"; 
    fstatfs_kprobe.entry_handler = entry_handler_fstatfs; 
    fstatfs_kprobe.handler = ret_handler_fstatfs; 
    fstatfs_kprobe.data_size = sizeof(struct fstatfs_kretprobe_data); 
    fstatfs_kprobe.maxactive = 64; 
    ret = register_kretprobe(&fstatfs_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for fstatfs, returned %d\n", ret);

    memset(&newfstatat_kprobe, 0, sizeof(newfstatat_kprobe)); 
    newfstatat_kprobe.kp.symbol_name = "__x64_sys_newfstatat"; 
    newfstatat_kprobe.entry_handler = entry_handler_newfstatat; 
    newfstatat_kprobe.handler = ret_handler_newfstatat; 
    newfstatat_kprobe.data_size = sizeof(struct newfstatat_kretprobe_data); 
    newfstatat_kprobe.maxactive = 64; 
    ret = register_kretprobe(&newfstatat_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for newfstatat, returned %d\n", ret);

    memset(&faccessat2_kprobe, 0, sizeof(faccessat2_kprobe)); 
    faccessat2_kprobe.kp.symbol_name = "__x64_sys_faccessat2"; 
    faccessat2_kprobe.entry_handler = entry_handler_faccessat2; 
    faccessat2_kprobe.handler = ret_handler_faccessat2; 
    faccessat2_kprobe.data_size = sizeof(struct faccessat2_kretprobe_data); 
    faccessat2_kprobe.maxactive = 64; 
    ret = register_kretprobe(&faccessat2_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for faccessat2, returned %d\n", ret);

    memset(&readlink_kprobe, 0, sizeof(readlink_kprobe)); 
    readlink_kprobe.kp.symbol_name = "__x64_sys_readlink"; 
    readlink_kprobe.entry_handler = entry_handler_readlink; 
    readlink_kprobe.handler = ret_handler_readlink; 
    readlink_kprobe.data_size = sizeof(struct readlink_kretprobe_data); 
    readlink_kprobe.maxactive = 64; 
    ret = register_kretprobe(&readlink_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for readlink, returned %d\n", ret);

    memset(&readlinkat_kprobe, 0, sizeof(readlinkat_kprobe)); 
    readlinkat_kprobe.kp.symbol_name = "__x64_sys_readlinkat"; 
    readlinkat_kprobe.entry_handler = entry_handler_readlinkat; 
    readlinkat_kprobe.handler = ret_handler_readlinkat; 
    readlinkat_kprobe.data_size = sizeof(struct readlinkat_kretprobe_data); 
    readlinkat_kprobe.maxactive = 64; 
    ret = register_kretprobe(&readlinkat_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for readlinkat, returned %d\n", ret);

    memset(&getdents64_kprobe, 0, sizeof(getdents64_kprobe)); 
    getdents64_kprobe.kp.symbol_name = "__x64_sys_getdents64"; 
    getdents64_kprobe.entry_handler = entry_handler_getdents64; 
    getdents64_kprobe.handler = ret_handler_getdents64; 
    getdents64_kprobe.data_size = sizeof(struct getdents64_kretprobe_data); 
    getdents64_kprobe.maxactive = 64; 
    ret = register_kretprobe(&getdents64_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for getdents64, returned %d\n", ret);

    memset(&pipe2_kprobe, 0, sizeof(pipe2_kprobe)); 
    pipe2_kprobe.kp.symbol_name = "__x64_sys_pipe2"; 
    pipe2_kprobe.entry_handler = entry_handler_pipe2; 
    pipe2_kprobe.handler = ret_handler_pipe2; 
    pipe2_kprobe.data_size = sizeof(struct pipe2_kretprobe_data); 
    pipe2_kprobe.maxactive = 64; 
    ret = register_kretprobe(&pipe2_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for pipe2, returned %d\n", ret);

    memset(&brk_kprobe, 0, sizeof(brk_kprobe));
    brk_kprobe.kp.symbol_name = "__x64_sys_brk";
    brk_kprobe.entry_handler = entry_handler_brk;
    brk_kprobe.handler = ret_handler_brk;
    brk_kprobe.maxactive = 64;
    ret = register_kretprobe(&brk_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for brk, returned %d\n", ret);

    memset(&munmap_kprobe, 0, sizeof(munmap_kprobe));
    munmap_kprobe.kp.symbol_name = "__x64_sys_munmap";
    munmap_kprobe.entry_handler = entry_handler_munmap;
    munmap_kprobe.handler = ret_handler_munmap;
    munmap_kprobe.maxactive = 64;
    ret = register_kretprobe(&munmap_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for munmap, returned %d\n", ret);

    memset(&mremap_kprobe, 0, sizeof(mremap_kprobe));
    mremap_kprobe.kp.symbol_name = "__x64_sys_mremap";
    mremap_kprobe.entry_handler = entry_handler_mremap;
    mremap_kprobe.handler = ret_handler_mremap;
    mremap_kprobe.maxactive = 64;
    ret = register_kretprobe(&mremap_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for mremap, returned %d\n", ret);

    memset(&mmap_kprobe, 0, sizeof(mmap_kprobe));
    mmap_kprobe.kp.symbol_name = "__x64_sys_mmap";
    mmap_kprobe.entry_handler = entry_handler_mmap;
    mmap_kprobe.handler = ret_handler_mmap;
    mmap_kprobe.data_size = sizeof(struct mmap_kretprobe_data);
    mmap_kprobe.maxactive = 64;
    ret = register_kretprobe(&mmap_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for mmap, returned %d\n", ret);

    // --- BATCH 3.2 KPROBES ---
    memset(&mprotect_kprobe, 0, sizeof(mprotect_kprobe));
    mprotect_kprobe.kp.symbol_name = "__x64_sys_mprotect";
    mprotect_kprobe.entry_handler = entry_handler_mprotect;
    mprotect_kprobe.handler = ret_handler_mprotect;
    mprotect_kprobe.maxactive = 64;
    ret = register_kretprobe(&mprotect_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for mprotect, returned %d\n", ret);

    memset(&madvise_kprobe, 0, sizeof(madvise_kprobe));
    madvise_kprobe.kp.symbol_name = "__x64_sys_madvise";
    madvise_kprobe.entry_handler = entry_handler_madvise;
    madvise_kprobe.handler = ret_handler_madvise;
    madvise_kprobe.maxactive = 64;
    ret = register_kretprobe(&madvise_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for madvise, returned %d\n", ret);

    memset(&mbind_kprobe, 0, sizeof(mbind_kprobe));
    mbind_kprobe.kp.symbol_name = "__x64_sys_mbind";
    mbind_kprobe.entry_handler = entry_handler_mbind;
    mbind_kprobe.handler = ret_handler_mbind;
    mbind_kprobe.maxactive = 64;
    ret = register_kretprobe(&mbind_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for mbind, returned %d\n", ret);

    memset(&sched_getaffinity_kprobe, 0, sizeof(sched_getaffinity_kprobe));
    sched_getaffinity_kprobe.kp.symbol_name = "__x64_sys_sched_getaffinity";
    sched_getaffinity_kprobe.entry_handler = entry_handler_sched_getaffinity;
    sched_getaffinity_kprobe.handler = ret_handler_sched_getaffinity;
    sched_getaffinity_kprobe.maxactive = 64;
    ret = register_kretprobe(&sched_getaffinity_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for sched_getaffinity, returned %d\n", ret);

    memset(&sched_setaffinity_kprobe, 0, sizeof(sched_setaffinity_kprobe));
    sched_setaffinity_kprobe.kp.symbol_name = "__x64_sys_sched_setaffinity";
    sched_setaffinity_kprobe.entry_handler = entry_handler_sched_setaffinity;
    sched_setaffinity_kprobe.handler = ret_handler_sched_setaffinity;
    sched_setaffinity_kprobe.maxactive = 64;
    ret = register_kretprobe(&sched_setaffinity_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for sched_setaffinity, returned %d\n", ret);

    memset(&sched_yield_kprobe, 0, sizeof(sched_yield_kprobe));
    sched_yield_kprobe.kp.symbol_name = "__x64_sys_sched_yield";
    sched_yield_kprobe.entry_handler = entry_handler_sched_yield;
    sched_yield_kprobe.handler = ret_handler_sched_yield;
    sched_yield_kprobe.maxactive = 64;
    ret = register_kretprobe(&sched_yield_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for sched_yield, returned %d\n", ret);

    // --- THE CRADLE INTERCEPTOR ---
    memset(&wake_up_new_task_kprobe, 0, sizeof(wake_up_new_task_kprobe));
    wake_up_new_task_kprobe.symbol_name = "wake_up_new_task";
    wake_up_new_task_kprobe.pre_handler = entry_handler_wake_up_new_task;
    ret = register_kprobe(&wake_up_new_task_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kprobe failed for wake_up_new_task, returned %d\n", ret);

    // --- THE TGKILL TRANSLATOR ---
    memset(&tgkill_kprobe, 0, sizeof(tgkill_kprobe));
    tgkill_kprobe.kp.symbol_name = "__x64_sys_tgkill";
    tgkill_kprobe.entry_handler = entry_handler_tgkill;
    tgkill_kprobe.handler = ret_handler_tgkill;
    tgkill_kprobe.maxactive = 64;
    ret = register_kretprobe(&tgkill_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for tgkill, returned %d\n", ret);

    // DSM - shared memory syscalls
    memset(&shmget_kprobe, 0, sizeof(shmget_kprobe)); shmget_kprobe.kp.symbol_name = "__x64_sys_shmget";
    shmget_kprobe.entry_handler = entry_handler_shmget; shmget_kprobe.handler = ret_handler_shmget;
    shmget_kprobe.data_size = sizeof(struct shmget_kretprobe_data); shmget_kprobe.maxactive = 64;
    ret = register_kretprobe(&shmget_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for shmget, returned %d\n", ret);

    memset(&shmctl_kprobe, 0, sizeof(shmctl_kprobe)); shmctl_kprobe.kp.symbol_name = "__x64_sys_shmctl";
    shmctl_kprobe.entry_handler = entry_handler_shmctl; shmctl_kprobe.handler = ret_handler_shmctl;
    shmctl_kprobe.data_size = sizeof(struct shmctl_kretprobe_data); shmctl_kprobe.maxactive = 64;
    ret = register_kretprobe(&shmctl_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for shmctl, returned %d\n", ret);

    memset(&shmdt_kprobe, 0, sizeof(shmdt_kprobe)); shmdt_kprobe.kp.symbol_name = "__x64_sys_shmdt";
    shmdt_kprobe.entry_handler = entry_handler_shmdt; shmdt_kprobe.handler = ret_handler_shmdt;
    shmdt_kprobe.data_size = sizeof(struct shmdt_kretprobe_data); shmdt_kprobe.maxactive = 64;
    ret = register_kretprobe(&shmdt_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for shmdt, returned %d\n", ret);

    memset(&shmat_kprobe, 0, sizeof(shmat_kprobe)); shmat_kprobe.kp.symbol_name = "__x64_sys_shmat";
    shmat_kprobe.entry_handler = entry_handler_shmat; shmat_kprobe.handler = ret_handler_shmat;
    shmat_kprobe.data_size = sizeof(struct shmat_kretprobe_data); shmat_kprobe.maxactive = 64;
    ret = register_kretprobe(&shmat_kprobe);
    if (ret < 0) printk(KERN_ERR "MattX: register_kretprobe failed for shmat, returned %d\n", ret);

    
    mattx_dbg(" Syscall Hooks (Kprobes) registered successfully.\n");
    return 0;
}

void mattx_hooks_exit(void) {
    unregister_kretprobe(&shmdt_kprobe);
    unregister_kretprobe(&shmctl_kprobe);
    unregister_kretprobe(&shmget_kprobe);
    unregister_kretprobe(&tgkill_kprobe);
    unregister_kprobe(&wake_up_new_task_kprobe);
    unregister_kretprobe(&sched_getaffinity_kprobe);
    unregister_kretprobe(&sched_setaffinity_kprobe);
    unregister_kretprobe(&sched_yield_kprobe);
    unregister_kretprobe(&mprotect_kprobe);
    unregister_kretprobe(&madvise_kprobe);
    unregister_kretprobe(&mbind_kprobe);
    unregister_kretprobe(&brk_kprobe);
    unregister_kretprobe(&munmap_kprobe);
    unregister_kretprobe(&mremap_kprobe);
    unregister_kretprobe(&mmap_kprobe);
    unregister_kretprobe(&getdents64_kprobe);
    unregister_kretprobe(&pipe2_kprobe);
    unregister_kretprobe(&statfs_kprobe);
    unregister_kretprobe(&fstatfs_kprobe);
    unregister_kretprobe(&newfstatat_kprobe);
    unregister_kretprobe(&faccessat2_kprobe);
    unregister_kretprobe(&readlink_kprobe);
    unregister_kretprobe(&readlinkat_kprobe);
    unregister_kretprobe(&fcntl_kprobe);
    unregister_kretprobe(&ioctl_kprobe);
    unregister_kretprobe(&pread64_kprobe);
    unregister_kretprobe(&getpid_kprobe);
    unregister_kretprobe(&gettid_kprobe);
    unregister_kretprobe(&uname_kprobe);
    unregister_kretprobe(&prlimit64_kprobe);
    unregister_kretprobe(&prctl_kprobe);
    unregister_kretprobe(&sendmsg_kprobe);
    unregister_kretprobe(&recvmsg_kprobe);
    unregister_kretprobe(&getsockname_kprobe);
    unregister_kretprobe(&getpeername_kprobe);
    unregister_kretprobe(&getsockopt_kprobe);
    unregister_kretprobe(&setsockopt_kprobe);
    unregister_kretprobe(&epoll_create_kprobe);
    unregister_kretprobe(&epoll_create1_kprobe);
    unregister_kretprobe(&epoll_wait_kprobe);
    unregister_kretprobe(&epoll_ctl_kprobe);
    unregister_kretprobe(&pselect6_kprobe);    
    unregister_kretprobe(&write_kprobe);
    unregister_kretprobe(&read_kprobe);    
    unregister_kretprobe(&lseek_kprobe);    
    unregister_kretprobe(&fsync_kprobe);    
    unregister_kretprobe(&statx_kprobe);    
    unregister_kretprobe(&select_kprobe);
    unregister_kretprobe(&poll_kprobe);
    unregister_kretprobe(&accept_kprobe);
    unregister_kretprobe(&recvfrom_kprobe);
    unregister_kretprobe(&sendto_kprobe);
    unregister_kretprobe(&listen_kprobe);
    unregister_kretprobe(&bind_kprobe);
    unregister_kretprobe(&connect_kprobe);
    unregister_kretprobe(&socket_kprobe);
    unregister_kretprobe(&unlinkat_kprobe);
    unregister_kretprobe(&dup2_kprobe);
    unregister_kretprobe(&dup_kprobe);
    unregister_kretprobe(&openat_kprobe);
    mattx_dbg(" Syscall Hooks (Kprobes) unregistered.\n");
}

