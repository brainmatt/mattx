/*
 * MattX - The Modern Single System Image (SSI) Cluster
 *
 * Copyright (c) 2026 by Matthias Rechenburg
 * All rights reserved.
 *
 * dsmreadwritetest - DSM "read-then-write" coherence reproducer (issue #66)
 *
 * WHAT IT PROVES
 * --------------
 * The Manager stays on the Home Node (VM1) and is the WRITER. The Workers are
 * migrated to a remote node (VM2) and are the POLLING READERS. For every round
 * the Manager changes one 4K page natively, then waits until every Worker has
 * ACKed the new value (through a separate page that the Worker writes = the
 * direction that already works). If a Worker never sees the new value, VM1's
 * native write was SILENT: no Page Fault, no INVALIDATE -> STALE (the bug).
 *
 * The VM1 write patterns differ in HOW the native PTE got (re)mapped before
 * the write, because that is exactly what hides the write from the DSM:
 *
 *   ctl       control group: VM2 writes, VM1 polls (must always PASS)
 *   atomic    lock xadd on the page, no prior load      (issue #66 scenario)
 *   store     plain store, no prior load                (write-first)
 *   rw        load the word, then store it              (read fault maps the
 *                                                        PTE writable, then a
 *                                                        silent write)
 *   rwfield   load ANOTHER field of the same 4K page,   (page granularity:
 *             then store                                 the header-page case)
 *
 * Every Worker also validates the page snapshot it sees (data words must be
 * consistent and never older than seq) -> TORN / ORDER counters.
 *
 * The ack latency is measured on VM1's clock only (no clock skew involved).
 * A latency that clusters around the 50ms Sweeper period is reported as WARN:
 * the page was not stale forever, but consistency is only "eventual".
 *
 * USAGE
 * -----
 *   dsmreadwritetest <num_workers> <rounds> <action_delay> [scenario|all] [migrate_wait_secs]
 *
 *   num_workers        readers that get migrated to the remote node(s) (1..16)
 *   rounds             write rounds per scenario
 *   action_delay       seconds between rounds (float, 0 = hammer)
 *   scenario           ctl | atomic | store | rw | rwfield | all   (default: all)
 *   migrate_wait_secs  how long a Worker waits to be migrated (default: 60)
 *
 *   The Manager (this process) MUST stay on the Home Node. Migrate the workers
 *   (as with dsmstresstest / dsmimagetest) while they wait.
 *
 * ENVIRONMENT (all optional)
 *   DSMRW_STALE_MS      ack timeout before a round counts as STALE (default 2000)
 *   DSMRW_SLOW_MS       ack latency that counts as WARN             (default 40)
 *   DSMRW_POLL_US       poll interval in microseconds               (default 200)
 *   DSMRW_VERBOSE=1     print every round
 *   DSMRW_ALLOW_LOCAL=1 accept a run without migration (harness self-test only)
 *   DSMRW_FAULT_INJECT=N  self-test: Workers pretend to see no update after
 *                       round N (proves the FAIL path and the no-hang timeouts)
 *
 * EXIT CODE: 0 = all PASS (WARN counts as pass), 1 = FAIL, 2 = INCONCLUSIVE
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>

#define PAGE_SZ      4096
#define SHM_KEY      0x4D415457 /* Hex for "MATW" */
#define MAX_WORKERS  16
#define NDATA        8
#define NHIST        5

enum { SC_CTL = 0, SC_ATOMIC, SC_STORE, SC_RW, SC_RWFIELD, SC_COUNT };
enum { V_PASS = 0, V_WARN, V_FAIL, V_INCONC };

static const char *sc_name[SC_COUNT] = { "ctl", "atomic", "store", "rw", "rwfield" };
static const char *sc_desc[SC_COUNT] = {
    "control: VM2 writes, VM1 polls",
    "VM1 atomic add, no prior load (issue #66)",
    "VM1 plain store, no prior load",
    "VM1 load word, then store it",
    "VM1 load other field of same page, then store"
};
static const char *sc_hint[SC_COUNT] = {
    "baseline of the harness itself",
    "stale PTE after downgrade / first share",
    "stale PTE after downgrade / first share",
    "no write-notify on VM1: read fault maps PTE writable",
    "no write-notify on VM1: page granularity"
};
static const char *v_name[] = { "PASS", "WARN", "FAIL", "INCONCLUSIVE" };

/* One 4K payload page: written by one node, polled by the other */
struct payload {
    volatile uint64_t seq;           /* written LAST in every round */
    volatile uint64_t data[NDATA];   /* all words == round number   */
    volatile uint64_t other;         /* never written after init    */
    char pad[PAGE_SZ - (NDATA + 2) * sizeof(uint64_t)];
};

/* One 4K result page per (scenario, worker): written by the Worker */
struct wres {
    volatile uint64_t ready;
    volatile uint64_t ack;           /* last seq the Worker has seen */
    volatile uint64_t done;
    volatile uint64_t last_seen;
    volatile uint64_t polls;
    volatile uint64_t torn;          /* data words of one snapshot disagree  */
    volatile uint64_t order;         /* data older than seq                  */
    volatile uint64_t timed_out;
    volatile int32_t  node;
    char pad[PAGE_SZ - 8 * sizeof(uint64_t) - sizeof(int32_t)];
};

_Static_assert(sizeof(struct payload) == PAGE_SZ, "payload must be exactly one page");
_Static_assert(sizeof(struct wres) == PAGE_SZ, "wres must be exactly one page");

struct sc_result {
    int ran;
    int ready_timeout, done_timeout;
    int stale_round;                 /* 0 = none */
    int rounds_ok;
    unsigned long long lat_sum_us, lat_max_us;
    int hist[NHIST];
    unsigned long long torn, order, regress;
    int not_migrated;
    int verdict;
    char note[200];
};

/* ---- globals (inherited by the forked workers) ---- */
static pid_t *child_pids = NULL;
static int num_workers = 0, rounds = 0, sel_sc = -1, migrate_wait = 60;
static double action_delay = 0.0;
static int shmid = -1;
static uint8_t *shm_base = (uint8_t *)-1;
static int home_node = -1;
static long stale_ms = 2000, slow_ms = 40, poll_us = 200, verbose = 0;
static long allow_local = 0, inject_round = 0;

/* ---- layout helpers ---- */
static struct payload *payload_page(int sc)
{
    return (struct payload *)(shm_base + (size_t)(sc - 1) * PAGE_SZ);
}

static struct payload *ctl_page(int w)
{
    return (struct payload *)(shm_base + (size_t)(SC_COUNT - 1 + w) * PAGE_SZ);
}

static struct wres *wres_page(int sc, int w)
{
    return (struct wres *)(shm_base +
        (size_t)(SC_COUNT - 1 + num_workers + sc * num_workers + w) * PAGE_SZ);
}

static size_t total_pages(void)
{
    return (size_t)(SC_COUNT - 1) + num_workers + (size_t)SC_COUNT * num_workers;
}

/* ---- small helpers ---- */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static long env_long(const char *name, long def)
{
    const char *v = getenv(name);
    return (v && *v) ? atol(v) : def;
}

/* Dynamically read the current MattX Node ID (same helper as the other tools) */
static int get_local_node_id(void)
{
    FILE *f = fopen("/proc/mattx/nodes", "r");
    if (!f) return -1;
    char line[256];
    int node_id = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "(Local)")) {
            sscanf(line, "%d", &node_id);
            break;
        }
    }
    fclose(f);
    return node_id;
}

static int scenario_selected(int sc)
{
    return sel_sc < 0 || sel_sc == sc;
}

/* The Manager catches Ctrl-C / the watchdog and kills all children cleanly */
static void cleanup_and_exit(int sig)
{
    if (sig == SIGALRM)
        printf("\n[Manager] WATCHDOG expired! Something is stuck. Killing workers...\n");
    else
        printf("\n[Manager] Caught Ctrl-C! Sending kill signals to workers...\n");
    for (int i = 0; i < num_workers; i++)
        if (child_pids && child_pids[i] > 0) kill(child_pids[i], SIGKILL);
    if (shm_base != (uint8_t *)-1) shmdt((void *)shm_base);
    if (shmid >= 0) shmctl(shmid, IPC_RMID, NULL);
    printf("[Manager] Shared memory destroyed. Exiting.\n");
    exit(sig == SIGALRM ? 1 : 0);
}

/* ======================================================================== */
/* THE WRITER PATTERNS (VM1, native)                                        */
/* ======================================================================== */
static void vm1_write(int sc, struct payload *p, uint64_t k)
{
    uint64_t sink = 0;

    /* The "read" half: this is what silently maps the PTE writable again */
    if (sc == SC_RW)      sink = p->seq;    /* the very word we store next  */
    if (sc == SC_RWFIELD) sink = p->other;  /* another field, same 4K page  */
    (void)sink;

    if (sc == SC_ATOMIC) {
        /* write-first RMW, like chunks_written in dsmimagetest */
        for (int i = 0; i < NDATA; i++) __sync_fetch_and_add(&p->data[i], 1);
        __sync_fetch_and_add(&p->seq, 1);
    } else {
        for (int i = 0; i < NDATA; i++) p->data[i] = k;
        __atomic_store_n(&p->seq, k, __ATOMIC_RELEASE);
    }
}

/* ======================================================================== */
/* WORKER SIDE (VM2)                                                        */
/* ======================================================================== */
static int wait_for_migration(void)
{
    int node = get_local_node_id();

    if (home_node < 0) return node;   /* no MattX /proc: nothing to wait for */
    for (int t = 0; t < migrate_wait && node == home_node; t++) {
        sleep(1);
        node = get_local_node_id();
    }
    return node;
}

/* Reader scenarios: poll VM1's page, ACK every new value */
static void worker_poll(int w, int sc)
{
    struct payload *p = payload_page(sc);
    struct wres *r = wres_page(sc, w);
    uint64_t last = 0, polls = 0, torn = 0, order = 0, timed_out = 0;
    uint64_t idle_us = (uint64_t)(stale_ms * 3000L) + (uint64_t)(action_delay * 1000000.0);
    /* until the first round the Manager may still wait for other workers to migrate */
    uint64_t first_us = idle_us + (uint64_t)(migrate_wait + 30) * 1000000ULL;
    uint64_t t_idle;

    r->node = get_local_node_id();
    __sync_synchronize();
    r->ready = 1;
    t_idle = now_us();

    while (last < (uint64_t)rounds) {
        uint64_t s = p->seq;
        polls++;

        /* self-test: pretend that the DSM never delivers an update again */
        if (inject_round > 0 && last >= (uint64_t)inject_round) s = last;

        if (s > last) {
            uint64_t d[NDATA];
            for (int i = 0; i < NDATA; i++) d[i] = p->data[i];
            if (d[0] < s) order++;
            for (int i = 1; i < NDATA; i++)
                if (d[i] != d[0]) { torn++; break; }
            last = s;
            r->ack = s;                       /* the working direction: VM2 write */
            t_idle = now_us();
        } else if (now_us() - t_idle > (last == 0 ? first_us : idle_us)) {
            timed_out = 1;
            break;
        }
        usleep((useconds_t)poll_us);
    }

    r->last_seen = last; r->polls = polls; r->torn = torn;
    r->order = order; r->timed_out = timed_out;
    __sync_synchronize();
    r->done = 1;
}

/* Control scenario: the Worker is the WRITER (VM2 -> VM1) */
static void worker_ctl(int w, int sc)
{
    struct payload *p = ctl_page(w);
    struct wres *r = wres_page(sc, w);

    r->node = get_local_node_id();
    __sync_synchronize();
    r->ready = 1;

    for (int k = 1; k <= rounds; k++) {
        for (int i = 0; i < NDATA; i++) p->data[i] = (uint64_t)k;
        __atomic_store_n(&p->seq, (uint64_t)k, __ATOMIC_RELEASE);
        if (action_delay > 0.0) usleep((useconds_t)(action_delay * 1000000.0));
    }
    r->last_seen = (uint64_t)rounds;
    __sync_synchronize();
    r->done = 1;
}

static void worker_main(int w)
{
    int node;

    srand(time(NULL) ^ getpid());
    printf("[Worker %d | PID %d] Started on node %d. Waiting up to %ds for migration...\n",
           w, getpid(), get_local_node_id(), migrate_wait);
    node = wait_for_migration();
    printf("[Worker %d | PID %d] Running on node %d (home node is %d). Starting scenarios.\n",
           w, getpid(), node, home_node);

    for (int sc = 0; sc < SC_COUNT; sc++) {
        if (!scenario_selected(sc)) continue;
        if (sc == SC_CTL) worker_ctl(w, sc);
        else              worker_poll(w, sc);
        if (verbose) printf("  -> [Worker %d] scenario %s finished.\n", w, sc_name[sc]);
    }
    printf("[Worker %d | PID %d] All scenarios finished. Exiting cleanly (Funeral Flush).\n",
           w, getpid());
    exit(0);
}

/* ======================================================================== */
/* MANAGER SIDE (VM1)                                                       */
/* ======================================================================== */
static int wait_all(int sc, int field_done, long timeout_s)
{
    uint64_t deadline = now_us() + (uint64_t)timeout_s * 1000000ULL;

    for (;;) {
        int all = 1;
        for (int w = 0; w < num_workers; w++) {
            struct wres *r = wres_page(sc, w);
            if (!(field_done ? r->done : r->ready)) all = 0;
        }
        if (all) return 0;
        if (now_us() > deadline) return -1;
        usleep(10000);
    }
}

static void collect_worker_stats(int sc, struct sc_result *res)
{
    for (int w = 0; w < num_workers; w++) {
        struct wres *r = wres_page(sc, w);

        if (r->node == home_node || r->node < 0) res->not_migrated = 1;
        if (sc != SC_CTL) {
            res->torn  += r->torn;
            res->order += r->order;
        }
        if (sc == SC_CTL)
            printf("      worker %d: node %d, wrote %llu rounds\n",
                   w, (int)r->node, (unsigned long long)r->last_seen);
        else
            printf("      worker %d: node %d, last_seen=%llu, polls=%llu, torn=%llu, order=%llu%s\n",
                   w, (int)r->node, (unsigned long long)r->last_seen,
                   (unsigned long long)r->polls, (unsigned long long)r->torn,
                   (unsigned long long)r->order, r->timed_out ? "  <- gave up (STALE)" : "");
    }
}

/* VM1 writes, VM2 polls and ACKs */
static void run_poll_scenario(int sc, struct sc_result *res)
{
    struct payload *p = payload_page(sc);
    long idle_s = (stale_ms * 3) / 1000 + 1;

    if (wait_all(sc, 0, migrate_wait + 30 + idle_s + 10) != 0) {
        res->ready_timeout = 1;
        snprintf(res->note, sizeof(res->note), "workers never became ready");
        return;
    }

    for (int k = 1; k <= rounds; k++) {
        uint64_t t0, lat, deadline;

        vm1_write(sc, p, (uint64_t)k);
        t0 = now_us();
        deadline = t0 + (uint64_t)stale_ms * 1000ULL;

        for (;;) {
            int all = 1;
            for (int w = 0; w < num_workers; w++)
                if (wres_page(sc, w)->ack < (uint64_t)k) all = 0;
            if (all) break;
            if (now_us() > deadline) {
                size_t off;

                res->stale_round = k;
                off = snprintf(res->note, sizeof(res->note),
                               "STALE: VM1 wrote seq=%d, no ack after %ldms from:", k, stale_ms);
                for (int w = 0; w < num_workers && off < sizeof(res->note); w++) {
                    uint64_t a = wres_page(sc, w)->ack;
                    if (a < (uint64_t)k)
                        off += snprintf(res->note + off, sizeof(res->note) - off,
                                        " worker %d (stuck at seq=%llu)", w, (unsigned long long)a);
                }
                goto collect;
            }
            usleep((useconds_t)poll_us);
        }

        lat = now_us() - t0;
        res->rounds_ok++;
        res->lat_sum_us += lat;
        if (lat > res->lat_max_us) res->lat_max_us = lat;
        if      (lat <   5000) res->hist[0]++;
        else if (lat <  20000) res->hist[1]++;
        else if (lat <  50000) res->hist[2]++;
        else if (lat < 100000) res->hist[3]++;
        else                   res->hist[4]++;
        if (verbose)
            printf("  -> [Manager] round %d acked by all workers after %.2f ms\n", k, lat / 1000.0);
        if (action_delay > 0.0) usleep((useconds_t)(action_delay * 1000000.0));
    }

collect:
    if (wait_all(sc, 1, idle_s + 10) != 0) res->done_timeout = 1;
    collect_worker_stats(sc, res);
}

/* VM2 writes, VM1 polls (the control group) */
static void run_ctl_scenario(int sc, struct sc_result *res)
{
    uint64_t last[MAX_WORKERS];
    long total_s = (long)(rounds * (action_delay + 0.05)) + migrate_wait + 60;
    uint64_t deadline = now_us() + (uint64_t)total_s * 1000000ULL;
    int all_done = 0;

    memset(last, 0, sizeof(last));

    while (!all_done) {
        all_done = 1;
        for (int w = 0; w < num_workers; w++) {
            struct payload *p = ctl_page(w);
            struct wres *r = wres_page(sc, w);
            int finished = r->done ? 1 : 0;      /* sample BEFORE the final read */
            uint64_t s = p->seq;

            if (s < last[w]) res->regress++;     /* VM1 must never see time go backwards */
            if (s > last[w]) {
                uint64_t d[NDATA];
                for (int i = 0; i < NDATA; i++) d[i] = p->data[i];
                if (d[0] < s) res->order++;
                for (int i = 1; i < NDATA; i++)
                    if (d[i] != d[0]) { res->torn++; break; }
                last[w] = s;
            }
            if (!finished) all_done = 0;
        }
        if (now_us() > deadline) {
            res->done_timeout = 1;
            snprintf(res->note, sizeof(res->note), "workers never finished writing");
            break;
        }
        usleep((useconds_t)poll_us);
    }

    /* After the Worker is done, VM1 must see the final value (within the stale timeout) */
    for (int w = 0; w < num_workers && !res->done_timeout; w++) {
        struct payload *p = ctl_page(w);
        uint64_t end = now_us() + (uint64_t)stale_ms * 1000ULL;

        while (p->seq != (uint64_t)rounds && now_us() < end) usleep((useconds_t)poll_us);
        if (p->seq != (uint64_t)rounds) {
            res->stale_round = rounds;
            snprintf(res->note, sizeof(res->note),
                     "STALE: worker %d wrote seq=%d but VM1 still reads seq=%llu after %ldms",
                     w, rounds, (unsigned long long)p->seq, stale_ms);
        } else {
            res->rounds_ok = rounds;
        }
    }
    collect_worker_stats(sc, res);
}

static void judge(struct sc_result *res)
{
    if (res->ready_timeout || res->done_timeout)            res->verdict = V_FAIL;
    else if (res->stale_round || res->torn || res->order || res->regress) res->verdict = V_FAIL;
    else if (res->not_migrated && !allow_local)             res->verdict = V_INCONC;
    else if (res->lat_max_us >= (unsigned long long)slow_ms * 1000ULL) res->verdict = V_WARN;
    else                                                    res->verdict = V_PASS;
}

static void print_result(int sc, struct sc_result *res)
{
    printf("   result : %s\n", v_name[res->verdict]);
    if (res->note[0]) printf("   note   : %s\n", res->note);
    if (res->not_migrated && !allow_local)
        printf("   note   : a worker never left the home node (or node unknown) -> the DSM was not exercised!\n");
    printf("   rounds : %d/%d acked", res->rounds_ok, rounds);
    if (sc != SC_CTL && res->rounds_ok)
        printf(" | latency avg %.2f ms, max %.2f ms",
               res->lat_sum_us / (double)res->rounds_ok / 1000.0, res->lat_max_us / 1000.0);
    printf("\n");
    if (sc != SC_CTL && res->rounds_ok)
        printf("   latency: <5ms:%d  <20ms:%d  <50ms:%d  <100ms:%d  >=100ms:%d\n",
               res->hist[0], res->hist[1], res->hist[2], res->hist[3], res->hist[4]);
    if (res->torn || res->order || res->regress)
        printf("   broken : torn=%llu order=%llu regress=%llu\n",
               res->torn, res->order, res->regress);
    if (res->verdict == V_FAIL && sc != SC_CTL)
        printf("   likely : %s\n", sc_hint[sc]);
}

int main(int argc, char *argv[])
{
    struct sc_result results[SC_COUNT];
    size_t shm_size;
    struct payload *op;
    int any_fail = 0, any_inconc = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 4 || argc > 6) {
        printf("Usage: %s <num_workers> <rounds> <action_delay> [scenario|all] [migrate_wait_secs]\n", argv[0]);
        printf("  scenario: ctl | atomic | store | rw | rwfield | all (default)\n");
        exit(1);
    }

    num_workers = atoi(argv[1]);
    rounds = atoi(argv[2]);
    action_delay = atof(argv[3]);
    if (argc >= 5 && strcasecmp(argv[4], "all") != 0) {
        sel_sc = -1;
        for (int sc = 0; sc < SC_COUNT; sc++)
            if (strcasecmp(argv[4], sc_name[sc]) == 0) sel_sc = sc;
        if (sel_sc < 0) { printf("Unknown scenario '%s'\n", argv[4]); exit(1); }
    }
    if (argc == 6) migrate_wait = atoi(argv[5]);

    if (num_workers <= 0 || num_workers > MAX_WORKERS || rounds <= 0 ||
        action_delay < 0.0 || migrate_wait < 0) {
        printf("Invalid arguments. (Max workers: %d, rounds > 0, action_delay >= 0)\n", MAX_WORKERS);
        exit(1);
    }

    stale_ms     = env_long("DSMRW_STALE_MS", 2000);
    slow_ms      = env_long("DSMRW_SLOW_MS", 40);
    poll_us      = env_long("DSMRW_POLL_US", 200);
    verbose      = env_long("DSMRW_VERBOSE", 0);
    allow_local  = env_long("DSMRW_ALLOW_LOCAL", 0);
    inject_round = env_long("DSMRW_FAULT_INJECT", 0);

    home_node = get_local_node_id();
    child_pids = calloc(num_workers, sizeof(pid_t));

    printf("==================================================\n");
    printf("[Manager] MattX DSM Read-Then-Write Test Initializing...\n");
    printf("[Manager] Workers: %d | Rounds: %d | Action Delay: %.3fs | Home node: %d\n",
           num_workers, rounds, action_delay, home_node);
    printf("[Manager] Stale timeout: %ldms | Slow (WARN) threshold: %ldms\n", stale_ms, slow_ms);
    if (home_node < 0)
        printf("[Manager] WARNING: /proc/mattx/nodes not readable (module not loaded?) -> run is INCONCLUSIVE\n");
    if (inject_round > 0)
        printf("[Manager] SELF-TEST: fault injection after round %ld (expect FAIL)\n", inject_round);

    /* 1. MASTER CREATES THE SHARED MEMORY */
    shm_size = total_pages() * PAGE_SZ;
    shmid = shmget(SHM_KEY, shm_size, IPC_CREAT | 0666);
    if (shmid < 0) { perror("shmget failed"); exit(1); }
    shm_base = (uint8_t *)shmat(shmid, NULL, 0);
    if (shm_base == (uint8_t *)-1) { perror("shmat failed"); shmctl(shmid, IPC_RMID, NULL); exit(1); }

    memset(shm_base, 0, shm_size);
    for (int sc = 1; sc < SC_COUNT; sc++) {
        op = payload_page(sc);
        op->other = 0xC0FFEE;
    }
    printf("[Manager] SHM created and attached at %p (SHMID: %d, %zu pages, one page per role)\n",
           (void *)shm_base, shmid, total_pages());
    printf("==================================================\n");

    signal(SIGINT, cleanup_and_exit);
    signal(SIGALRM, cleanup_and_exit);
    {   /* the watchdog: this tool must never hang, whatever the DSM does */
        int nsc = sel_sc < 0 ? SC_COUNT : 1;
        long wd = migrate_wait + 120 +
                  (long)nsc * ((long)(rounds * (action_delay + 0.5)) + (stale_ms * 8) / 1000 + 60);
        alarm((unsigned)wd);
    }

    /* 2. FORK THE WORKERS */
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("Fork failed"); cleanup_and_exit(SIGINT); }
        if (pid == 0) worker_main(i);
        child_pids[i] = pid;
        printf("[Manager] Spawned worker %d/%d (PID %d). Migrate it to the remote node now!\n",
               i + 1, num_workers, pid);
        usleep(500000);
    }

    /* 3. RUN THE SCENARIOS (this process must stay on the Home Node!) */
    memset(results, 0, sizeof(results));
    for (int sc = 0; sc < SC_COUNT; sc++) {
        if (!scenario_selected(sc)) continue;

        printf("\n[Manager] === Scenario '%s': %s ===\n", sc_name[sc], sc_desc[sc]);
        results[sc].ran = 1;
        if (sc == SC_CTL) run_ctl_scenario(sc, &results[sc]);
        else              run_poll_scenario(sc, &results[sc]);
        judge(&results[sc]);
        print_result(sc, &results[sc]);
    }

    for (int i = 0; i < num_workers; i++) waitpid(child_pids[i], NULL, 0);
    alarm(0);

    /* 4. THE VERDICT */
    printf("\n==================================================\n");
    printf("[Manager] SUMMARY (workers: %d, rounds: %d)\n", num_workers, rounds);
    for (int sc = 0; sc < SC_COUNT; sc++) {
        if (!results[sc].ran) continue;
        printf("  %-8s %-13s %s\n", sc_name[sc], v_name[results[sc].verdict], sc_desc[sc]);
        if (results[sc].verdict == V_FAIL) any_fail = 1;
        if (results[sc].verdict == V_INCONC) any_inconc = 1;
    }
    if (results[SC_CTL].ran && results[SC_CTL].verdict == V_FAIL)
        printf("  NOTE: the control group failed -> the remaining results are suspect.\n");
    printf("==================================================\n");

    shmdt((void *)shm_base);
    shmctl(shmid, IPC_RMID, NULL);
    free(child_pids);
    printf("[Manager] Clean exit. Goodbye!\n");

    return any_fail ? 1 : (any_inconc ? 2 : 0);
}
