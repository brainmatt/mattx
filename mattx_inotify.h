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

/*
 * THE INOTIFY WORMHOLE
 *
 * A Surrogate that holds an inotify instance owns only a mattx_vfs_proxy fake
 * file; the real inotify instance lives in the Deputy's fd table on the Home
 * Node. read() already tunnels home, but inotify_add_watch()/inotify_rm_watch()
 * used to run natively on the Surrogate's node against the proxy fd, where they
 * fail with -EINVAL. No IN_IGNORED event is ever generated on the Home Node, so
 * the thread that blocks in read() on the real instance never wakes up
 * (UCX: "vfs_fuse.c inotify_rm_watch(fd=10, wd=1) failed: Invalid argument",
 * followed by a hang in MPI_Finalize).
 *
 * Every inotify syscall therefore gets its own flat Wormhole route
 * (_req / _reply / VM2 hook + task_work / VM1 Identity-Theft kworker), in the
 * same ticket style as read(), connect() and accept(): only the calling
 * thread sleeps on its own ticket, the reply is matched by req_id.
 */

#ifndef MATTX_INOTIFY_H
#define MATTX_INOTIFY_H

#include "mattx.h"
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/atomic.h>

/*
 * Message ids live in their own number space far above enum mattx_msg_type
 * (handler table has 256 slots). mattx_hooks_init() BUILD_BUG_ON()s any overlap.
 */
enum mattx_inotify_msg_type {
    MATTX_MSG_SYS_INOTIFY_INIT_REQ = 240,
    MATTX_MSG_SYS_INOTIFY_INIT_REPLY,
    MATTX_MSG_SYS_INOTIFY_ADD_WATCH_REQ,
    MATTX_MSG_SYS_INOTIFY_ADD_WATCH_REPLY,
    MATTX_MSG_SYS_INOTIFY_RM_WATCH_REQ,
    MATTX_MSG_SYS_INOTIFY_RM_WATCH_REPLY,
};

struct mattx_sys_inotify_init_req {
    u32 req_id;         // ticket id (mattx_ticket_*): matches the reply to the ONE thread that waits
    u32 orig_pid;
    int flags;          // IN_NONBLOCK | IN_CLOEXEC (0 for plain inotify_init)
};

struct mattx_sys_inotify_init_reply {
    u32 req_id;         // ticket id (mattx_ticket_*): matches the reply to the ONE thread that waits
    u32 orig_pid;
    int remote_fd;      // fd number inside the Deputy's table (or -errno)
    int error;
};

struct mattx_sys_inotify_add_watch_req {
    u32 req_id;         // ticket id (mattx_ticket_*): matches the reply to the ONE thread that waits
    u32 orig_pid;
    int fd;             // remote fd of the inotify instance
    u32 mask;
    char path[256];     // resolved on the Home Node (Deputy's cwd/root)
};

struct mattx_sys_inotify_add_watch_reply {
    u32 req_id;         // ticket id (mattx_ticket_*): matches the reply to the ONE thread that waits
    u32 orig_pid;
    int error;          // watch descriptor (>= 0) or -errno
};

struct mattx_sys_inotify_rm_watch_req {
    u32 req_id;         // ticket id (mattx_ticket_*): matches the reply to the ONE thread that waits
    u32 orig_pid;
    int fd;             // remote fd of the inotify instance
    int wd;
};

struct mattx_sys_inotify_rm_watch_reply {
    u32 req_id;         // ticket id (mattx_ticket_*): matches the reply to the ONE thread that waits
    u32 orig_pid;
    int error;          // 0 or -errno
};

// --- THE INOTIFY GHOST RESOLVERS (resolved in mattx_hooks_init) ---
typedef long (*mattx_sys_inotify_init1_fn)(const struct pt_regs *regs);
extern mattx_sys_inotify_init1_fn real_sys_inotify_init1;

typedef long (*mattx_sys_inotify_add_watch_fn)(const struct pt_regs *regs);
extern mattx_sys_inotify_add_watch_fn real_sys_inotify_add_watch;

typedef long (*mattx_sys_inotify_rm_watch_fn)(const struct pt_regs *regs);
extern mattx_sys_inotify_rm_watch_fn real_sys_inotify_rm_watch;

#endif // MATTX_INOTIFY_H
