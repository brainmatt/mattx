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

// --- Guest Registry Implementation ---
struct mattx_guest_info guest_registry[MAX_GUESTS];
int guest_count = 0;

// ============================================================================
// DSM MAPPING TABLES (VM2): sized by the segment, not by a limit
// ============================================================================
// The three tables of a mapping (present bitmap, physical page pool, local MESI state) are ONE kvzalloc
// block with one entry per page of the segment. The registry holds one reference on it, and a user that
// works on the tables without holding guest_lock (the sweeper) takes another. The last put hands the block
// to the garbage collector below: remove_guest_process() runs under guest_lock, and the pages and the
// (vmalloc'ed) block must be freed where sleeping is allowed.
struct mattx_dsm_tables {
    refcount_t ref;
    struct list_head gc;        // on mattx_dsm_gc_list while it waits to be freed
    unsigned long npages;
    // present_pages[BITS_TO_LONGS(npages)], pages[npages] and page_states[npages] follow
};

static inline unsigned long *mattx_tbl_present(struct mattx_dsm_tables *t) { return (unsigned long *)(t + 1); }
static inline void **mattx_tbl_pages(struct mattx_dsm_tables *t) { return (void **)(mattx_tbl_present(t) + BITS_TO_LONGS(t->npages)); }
static inline u8 *mattx_tbl_states(struct mattx_dsm_tables *t) { return (u8 *)(mattx_tbl_pages(t) + t->npages); }

static LIST_HEAD(mattx_dsm_gc_list);
static DEFINE_SPINLOCK(mattx_dsm_gc_lock);

static void mattx_dsm_gc_fn(struct work_struct *work)
{
    struct mattx_dsm_tables *t, *tmp;
    LIST_HEAD(dead);

    spin_lock(&mattx_dsm_gc_lock);
    list_splice_init(&mattx_dsm_gc_list, &dead);
    spin_unlock(&mattx_dsm_gc_lock);

    list_for_each_entry_safe(t, tmp, &dead, gc) {
        void **pages = mattx_tbl_pages(t);
        unsigned long p;

        for (p = 0; p < t->npages; p++) {
            if (pages[p])
                free_page((unsigned long)pages[p]);
            if (!(p & 0xfff))
                cond_resched();
        }
        kvfree(t);
    }
}
static DECLARE_WORK(mattx_dsm_gc_work, mattx_dsm_gc_fn);

// Allocates the tables of a mapping. May sleep: never call it under a spinlock.
int mattx_dsm_map_init(struct mattx_dsm_mapping *m, unsigned long base, u32 shmid, unsigned long size)
{
    unsigned long npages = DIV_ROUND_UP(size, PAGE_SIZE);
    struct mattx_dsm_tables *t;
    size_t bytes;

    memset(m, 0, sizeof(*m));
    m->base_addr = base;
    m->shmid = shmid;
    m->size = size;
    if (!npages)
        return 0;               // an empty segment: nothing to track, every fault is refused

    bytes = size_add(sizeof(*t), size_add(size_mul(BITS_TO_LONGS(npages), sizeof(unsigned long)),
                                          size_add(size_mul(npages, sizeof(void *)), npages)));
    if (bytes == SIZE_MAX)
        return -E2BIG;
    t = kvzalloc(bytes, GFP_KERNEL);
    if (!t)
        return -ENOMEM;

    refcount_set(&t->ref, 1);
    INIT_LIST_HEAD(&t->gc);
    t->npages = npages;
    m->npages = npages;
    m->tbl = t;
    m->present_pages = mattx_tbl_present(t);
    m->pages = mattx_tbl_pages(t);
    m->page_states = mattx_tbl_states(t);
    return 0;
}

// A reference for a copy of the mapping that is used without guest_lock
void mattx_dsm_map_get(struct mattx_dsm_mapping *m)
{
    if (m->tbl)
        refcount_inc(&m->tbl->ref);
}

// Drops a reference (and forgets the tables in THIS copy of the mapping). The last one hands the block to
// the garbage collector: safe in any context, also under guest_lock.
void mattx_dsm_map_put(struct mattx_dsm_mapping *m)
{
    struct mattx_dsm_tables *t = m->tbl;

    m->tbl = NULL;
    m->present_pages = NULL;
    m->pages = NULL;
    m->page_states = NULL;
    if (t && refcount_dec_and_test(&t->ref)) {
        spin_lock(&mattx_dsm_gc_lock);
        list_add_tail(&t->gc, &mattx_dsm_gc_list);
        spin_unlock(&mattx_dsm_gc_lock);
        schedule_work(&mattx_dsm_gc_work);
    }
}

// Puts a mapping into the registry of a guest. Takes over *m: it is in the registry on success, its tables
// are released on failure. The array of a guest grows on demand, so there is no limit on the segments.
// May sleep: never call it under a spinlock.
int mattx_dsm_map_add(pid_t local_pid, struct mattx_dsm_mapping *m)
{
    struct mattx_dsm_mapping *grown = NULL, *old = NULL;
    int grown_cap = 0;
    int ret;

    for (;;) {
        struct mattx_guest_info *g = NULL;
        int need = 0, i;

        ret = -ESRCH;
        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].local_pid == local_pid) {
                g = &guest_registry[i];
                break;
            }
        }
        if (g) {
            if (g->dsm_count < g->dsm_cap) {
                g->dsm_map[g->dsm_count++] = *m;
                ret = 0;
            } else if (grown && grown_cap > g->dsm_count) {
                if (g->dsm_count)
                    memcpy(grown, g->dsm_map, g->dsm_count * sizeof(*grown));
                old = g->dsm_map;
                g->dsm_map = grown;
                g->dsm_cap = grown_cap;
                grown = NULL;
                g->dsm_map[g->dsm_count++] = *m;
                ret = 0;
            } else {
                need = g->dsm_cap ? g->dsm_cap * 2 : 4;
                ret = -ENOSPC;
            }
        }
        spin_unlock(&guest_lock);

        if (ret != -ENOSPC)
            break;
        kfree(grown);
        grown = kcalloc(need, sizeof(*grown), GFP_KERNEL);
        grown_cap = need;
        if (!grown) {
            ret = -ENOMEM;
            break;
        }
    }
    kfree(old);
    kfree(grown);

    if (ret)
        mattx_dsm_map_put(m);
    else
        memset(m, 0, sizeof(*m));   // the registry owns the tables now
    return ret;
}

// The Funeral Flush (MODE 2): sends every dirty (EXCLUSIVE) page of a mapping to the home node as a one-way
// Data Bomb. Needs no big array: it works on the tables themselves, one page at a time. May sleep.
void mattx_dsm_map_flush(struct mattx_dsm_mapping *m, u32 orig_pid, int home_node, const char *why)
{
    struct mattx_dsm_page_update_req *req;
    unsigned long p;

    if (!m->tbl || !m->npages)
        return;
    req = kmalloc(sizeof(*req), GFP_KERNEL);
    if (!req) {
        printk(KERN_ERR "MattX:[MESI_VM2] Funeral Flush (%s): out of memory, the dirty pages of SHMID %u are lost\n", why, m->shmid);
        return;
    }
    for (p = 0; p < m->npages; p++) {
        if (m->page_states[p] != MATTX_PAGE_EXCLUSIVE || !m->pages[p])
            continue;
        req->orig_pid = orig_pid;
        req->shmid = m->shmid;
        req->offset = p * PAGE_SIZE;
        memcpy(req->data, m->pages[p], PAGE_SIZE);
        if (home_node >= 0 && home_node < MAX_NODES && cluster_map[home_node]) {
            mattx_comm_send(cluster_map[home_node], MATTX_MSG_DSM_PAGE_UPDATE, req, sizeof(*req));
            mattx_dbg("[MESI_VM2] Funeral Flush (%s): Sent dirty page (SHMID %u, Offset %lu) to VM1\n", why, req->shmid, req->offset);
        }
    }
    kfree(req);
}

// Module unload: gives the tables of all guests back and waits for the garbage collector
void mattx_guest_exit(void)
{
    int i, d;

    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        for (d = 0; d < guest_registry[i].dsm_count; d++)
            mattx_dsm_map_put(&guest_registry[i].dsm_map[d]);
        kfree(guest_registry[i].dsm_map);
        guest_registry[i].dsm_map = NULL;
        guest_registry[i].dsm_count = 0;
        guest_registry[i].dsm_cap = 0;
    }
    spin_unlock(&guest_lock);
    flush_work(&mattx_dsm_gc_work);
}

DEFINE_SPINLOCK(guest_lock);

// --- Clock-Offset Fixup Table -------------------------------------------
// CLOCK_MONOTONIC is relative to each kernel's own boot instant, not
// wall-clock time -- two nodes with perfectly synced wall clocks can have
// arbitrarily different CLOCK_MONOTONIC readings (e.g. one rebooted more
// recently). A process that was frozen mid clock_nanosleep(TIMER_ABSTIME)
// (or whose libc/runtime retry loop re-issues the syscall against a
// stack-resident, pre-migration deadline -- see mattx_hooks.c's
// clock_nanosleep kretprobe for the full story) carries an absolute
// deadline that's only meaningful in the SOURCE node's clock frame. This
// table records, per just-Awakened process, the one-time ns offset needed
// to reinterpret that stale deadline correctly on the node it just landed
// on, and lets the kretprobe consume it (at most once per thread).
//
// Deliberately NOT folded into guest_registry: a returned Deputy is no
// longer a guest (is_guest_process() must say false for it so other
// Wormhole hooks stop routing its syscalls home), yet it still needs this
// exact same one-shot correction after RETURN_DONE. Keeping this table
// separate means it doesn't care whether the pid it's tracking is
// currently a guest, a Deputy, or anything else -- it only cares that an
// Awakening just happened for it.
#define MAX_CLOCK_FIXUPS 64

struct mattx_clock_fixup {
    bool in_use;
    pid_t local_pid;                 // tgid this entry applies to
    s64 offset_ns;                   // add to a stale deadline to fix it
    int thread_count;
    pid_t tids[MAX_GANG_THREADS];    // this gang's thread ids
    bool pending[MAX_GANG_THREADS];  // not yet consumed for that tid
};

static struct mattx_clock_fixup clock_fixups[MAX_CLOCK_FIXUPS];
static DEFINE_SPINLOCK(clock_fixup_lock);

void mattx_clock_fixup_register(pid_t local_pid, s64 offset_ns, int thread_count, const pid_t *tids) {
    int i, slot = -1;

    if (thread_count > MAX_GANG_THREADS) thread_count = MAX_GANG_THREADS;
    if (thread_count < 0) thread_count = 0;

    spin_lock(&clock_fixup_lock);

    // Reuse an existing entry for this pid (e.g. it got migrated again
    // before the previous fixup was fully consumed), else take a free
    // slot, else evict slot 0 as a last resort -- this table is best-effort
    // bookkeeping, not a hard correctness requirement (worst case a very
    // unlucky eviction just leaves one stale deadline uncorrected).
    for (i = 0; i < MAX_CLOCK_FIXUPS; i++) {
        if (clock_fixups[i].in_use && clock_fixups[i].local_pid == local_pid) { slot = i; break; }
    }
    if (slot < 0) {
        for (i = 0; i < MAX_CLOCK_FIXUPS; i++) {
            if (!clock_fixups[i].in_use) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0;

    clock_fixups[slot].in_use = true;
    clock_fixups[slot].local_pid = local_pid;
    clock_fixups[slot].offset_ns = offset_ns;
    clock_fixups[slot].thread_count = thread_count;
    for (i = 0; i < thread_count; i++) {
        clock_fixups[slot].tids[i] = tids[i];
        clock_fixups[slot].pending[i] = true;
    }

    spin_unlock(&clock_fixup_lock);
}

bool mattx_clock_fixup_consume(pid_t local_pid, pid_t tid, s64 *out_offset_ns) {
    int i, j;
    bool found = false;

    spin_lock(&clock_fixup_lock);
    for (i = 0; i < MAX_CLOCK_FIXUPS; i++) {
        if (!clock_fixups[i].in_use || clock_fixups[i].local_pid != local_pid) continue;
        for (j = 0; j < clock_fixups[i].thread_count; j++) {
            if (clock_fixups[i].tids[j] == tid && clock_fixups[i].pending[j]) {
                clock_fixups[i].pending[j] = false;
                *out_offset_ns = clock_fixups[i].offset_ns;
                found = true;
                break;
            }
        }
        break;
    }
    spin_unlock(&clock_fixup_lock);
    return found;
}

bool is_guest_process(pid_t pid) {
    int i;
    bool found = false;
    spin_lock(&guest_lock);
    for (i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            found = true;
            break;
        }
    }
    spin_unlock(&guest_lock);
    return found;
}

void add_guest_process(pid_t local_pid, u32 orig_pid, int home_node) {
    spin_lock(&guest_lock);
    if (guest_count < MAX_GUESTS) {
        guest_registry[guest_count].local_pid = local_pid;
        guest_registry[guest_count].orig_pid = orig_pid;
        guest_registry[guest_count].home_node = home_node;

        //Exorcise the ghosts! Clean up dirty memory from previous guests ---
        guest_registry[guest_count].is_migrating = false;
        guest_registry[guest_count].rpc_wq = NULL;
        guest_registry[guest_count].rpc_done = false;
        guest_registry[guest_count].rpc_read_buf = NULL;
        guest_registry[guest_count].rpc_read_bytes = 0;
        guest_registry[guest_count].rpc_lseek_res = 0;
        guest_registry[guest_count].rpc_statx_buf = NULL;
        guest_registry[guest_count].rpc_fsync_res = 0;

        // Init DSM Map ---
        guest_registry[guest_count].dsm_count = 0;
        guest_registry[guest_count].dsm_cap = 0;
        guest_registry[guest_count].dsm_map = NULL; // allocated by mattx_dsm_map_add() on the first segment

        guest_count++;
    } else {
        printk(KERN_WARNING "MattX: [REGISTRY] Guest registry is full!\n");
    }
    spin_unlock(&guest_lock);
}

void remove_guest_process(int index) {
    if (index < 0 || index >= guest_count) return;

    // Free DSM Physical Pages! ---
    // We hold guest_lock: the garbage collector frees the pages and the tables where sleeping is allowed
    for (int d = 0; d < guest_registry[index].dsm_count; d++)
        mattx_dsm_map_put(&guest_registry[index].dsm_map[d]);
    kfree(guest_registry[index].dsm_map);

    guest_registry[index] = guest_registry[guest_count - 1];
    guest_count--;
}

// --- Export Registry Implementation ---
struct mattx_export_info export_registry[MAX_GUESTS];
int export_count = 0;
DEFINE_SPINLOCK(export_lock);

void add_export_process(pid_t orig_pid, int target_node) {
    spin_lock(&export_lock);
    if (export_count < MAX_GUESTS) {
        export_registry[export_count].orig_pid = orig_pid;
        export_registry[export_count].target_node = target_node;

        // Reset the Kill-Switch for ping-pong migrations! ---
        export_registry[export_count].abort_rpc = false; 
        // NOTICE: The following line was missing before - re-added
        export_registry[export_count].is_growing_gang = false;

        export_count++;
    } else {
        printk(KERN_WARNING "MattX: [REGISTRY] Export registry is full!\n");
    }
    spin_unlock(&export_lock);
}

void remove_export_process(int index) {
    if (index < 0 || index >= export_count) return;
    export_registry[index] = export_registry[export_count - 1];
    export_count--;
}

int get_export_target(pid_t orig_pid) {
    int target = -1;
    int i;
    spin_lock(&export_lock);
    for (i = 0; i < export_count; i++) {
        if (export_registry[i].orig_pid == orig_pid) {
            target = export_registry[i].target_node;
            break;
        }
    }
    spin_unlock(&export_lock);
    return target;
}

// --- Network Handlers for Lifecycle Management ---
static void handle_process_exit(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_process_exit *exit_msg = (struct mattx_process_exit *)payload;
        struct task_struct *deputy = NULL;
        int i;
        struct file **files_to_close = NULL;
        bool found = false;

        mattx_dbg(" [FUNERAL] Received exit notice for Deputy PID %u from Node %u\n", 
               exit_msg->orig_pid, hdr->sender_id);

        rcu_read_lock();
        deputy = pid_task(find_vpid(exit_msg->orig_pid), PIDTYPE_PID);
        if (deputy) get_task_struct(deputy);
        rcu_read_unlock();

        if (deputy) {
            mattx_dbg(" [FUNERAL] Laying Deputy PID %u to rest (Sending SIGKILL)...\n", deputy->pid);
            send_sig(SIGKILL, deputy, 0);
            put_task_struct(deputy);
        }
        
        // --- THE STACK DIET FIX ---
        // Allocate the 2KB array on the heap instead of the tiny kernel stack!
        files_to_close = kzalloc(MAX_FDS * sizeof(struct file *), GFP_KERNEL);
        
        spin_lock(&export_lock);
        for (i = 0; i < export_count; i++) {
            if (export_registry[i].orig_pid == exit_msg->orig_pid) {
                // Extract the files safely before removing the entry!
                if (files_to_close) {
                    memcpy(files_to_close, export_registry[i].remote_files, MAX_FDS * sizeof(struct file *));
                }
                remove_export_process(i);
                found = true;
                break;
            }
        }
        spin_unlock(&export_lock); // DROP THE LOCK!

        // Safely close the files outside the spinlock to prevent deadlocks!
        if (found && files_to_close) {
            for (int j = 0; j < MAX_FDS; j++) {
                if (files_to_close[j]) fput(files_to_close[j]);
            }
        }
        
        if (files_to_close) kfree(files_to_close);
    }
}

static void handle_kill_surrogate(struct mattx_link *link, struct mattx_header *hdr, void *payload) {
    if (payload) {
        struct mattx_process_exit *kill_msg = (struct mattx_process_exit *)payload;
        pid_t local_stub_pid = -1;
        int i;
        void *buf1 = NULL;
        void *buf2 = NULL;

        spin_lock(&guest_lock);
        for (i = 0; i < guest_count; i++) {
            if (guest_registry[i].orig_pid == kill_msg->orig_pid && guest_registry[i].home_node == hdr->sender_id) {
                local_stub_pid = guest_registry[i].local_pid;
                // Extract the buffers to prevent memory leaks!
                buf1 = guest_registry[i].rpc_read_buf;
                buf2 = guest_registry[i].rpc_statx_buf;
                remove_guest_process(i);
                break;
            }
        }
        spin_unlock(&guest_lock); // DROP THE LOCK!

        // Safely free the memory outside the spinlock!
        if (buf1) kfree(buf1);
        if (buf2) kfree(buf2);

        if (local_stub_pid != -1) {
            struct task_struct *surrogate = NULL;
            rcu_read_lock();
            surrogate = pid_task(find_vpid(local_stub_pid), PIDTYPE_PID);
            if (surrogate) get_task_struct(surrogate);
            rcu_read_unlock();

            if (surrogate) {
                mattx_dbg("[ASSASSIN] Executing Surrogate PID %d (Sending SIGKILL)...\n", surrogate->pid);
                send_sig(SIGKILL, surrogate, 0);
                put_task_struct(surrogate);
            }
        }
    }
}

bool is_rpc_pending(pid_t pid) {
    bool pending = false;
    spin_lock(&guest_lock);
    for (int i = 0; i < guest_count; i++) {
        if (guest_registry[i].local_pid == pid) {
            // If rpc_wq is not NULL, the process is currently waiting for Node 1!
            pending = (guest_registry[i].rpc_wq != NULL);
            break;
        }
    }
    spin_unlock(&guest_lock);
    return pending;
}

void mattx_guest_init_handlers(void) {
    mattx_register_handler(MATTX_MSG_PROCESS_EXIT, handle_process_exit);
    mattx_register_handler(MATTX_MSG_KILL_SURROGATE, handle_kill_surrogate);
    mattx_dbg(" [GUEST] Network handlers registered.\n");
}

