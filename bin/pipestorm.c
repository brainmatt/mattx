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
 * pipestorm.c - MattX issue #74 reproducer ("Kris' colibri engine" in 200 lines)
 *
 * Mimics the qwen36 engine: 2 threads, stdin/stdout are pipes from a gateway,
 * one thread waits for input (read or select/poll with timeout), the other
 * thread either blocks in read(0), polls too, or just computes.
 *
 * Build:   gcc -O2 -pthread -o pipestorm pipestorm.c
 *
 * Modes (-m):
 *   rs  T1: blocking read(0)            T2: select(100ms) on a private pipe (never ready)
 *   rp  T1: blocking read(0)            T2: poll(100ms)   on a private pipe (never ready)
 *   ss  T1: select(stdin,100ms)+read    T2: select(stdin,100ms)+read      (both threads select)
 *   sc  T1: select(stdin,100ms)+read    T2: compute loop (1 ms sleeps + busy work)  <- colibri shape
 *   pc  T1: poll(stdin,100ms)+read      T2: compute loop
 *
 * Feeding it (the "gateway"):
 *   ./pipestorm -f 3 | ./pipestorm -m sc        # feeder writes a line every 3 s
 *   (or type lines by hand:  ./pipestorm -m sc  and press ENTER)
 *
 * What it prints (stderr, so stdout stays a clean reply pipe):
 *   - every received line with age (ms, only meaningful if feeder and reader
 *     share a clock, i.e. same node or synced clocks) and which thread got it
 *   - a status line every 2 s: per-thread loop counters + /proc task states
 *   - WATCHDOG warning if nothing arrives for -w seconds (default 10) while
 *     the feeder is still alive: prints /proc/self/task/<tid>/{stat,syscall,wchan}
 *   - every received line is echoed to stdout as "ECHO <n>" (tests the write path)
 *
 * Test plan:
 *   1. baseline on one node, all modes -> lines must arrive within ~100 ms
 *   2. start it on VM1 (MATTX_EXCLUDE off), migrate to VM2 while the feeder keeps
 *      writing, watch whether lines still arrive and whether the counters advance
 *   3. if a mode hangs: the status lines show T=stopped states; compare modes to
 *      find the culprit syscall (select vs poll vs read vs write)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <dirent.h>

enum { M_RS, M_RP, M_SS, M_SC, M_PC };

static int mode = M_SC;
static int watchdog_s = 10;
static volatile int stop_flag;
static volatile unsigned long cnt[2];          /* loop iterations per thread */
static volatile unsigned long lines_rx;
static volatile long last_rx_ms;
static int priv_pipe[2] = { -1, -1 };
static pthread_mutex_t rd_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile pid_t tid_of[2];

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long wall_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* read one chunk from stdin and report each line */
static void consume(int who, const char *how)
{
	char buf[4096];
	ssize_t n = read(0, buf, sizeof(buf) - 1);

	if (n == 0) {
		fprintf(stderr, "[T%d] EOF on stdin, exiting\n", who);
		stop_flag = 1;
		return;
	}
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN)
			fprintf(stderr, "[T%d] read error: %s\n", who, strerror(errno));
		return;
	}
	buf[n] = 0;
	last_rx_ms = now_ms();
	for (char *p = buf, *e; p && *p; p = e ? e + 1 : NULL) {
		long sent = 0, age = -1;
		e = strchr(p, '\n');
		if (e) *e = 0;
		if (!*p) continue;
		if (sscanf(p, "%*d %ld", &sent) == 1 && sent > 0)
			age = wall_ms() - sent;
		lines_rx++;
		fprintf(stderr, "[T%d] RX via %s: \"%.40s\" age=%ldms (total %lu)\n",
			who, how, p, age, lines_rx);
		dprintf(1, "ECHO %lu\n", lines_rx);
	}
}

static void wait_and_read_select(int who)
{
	fd_set rfds;
	struct timeval tv = { 0, 100 * 1000 };

	FD_ZERO(&rfds);
	FD_SET(0, &rfds);
	int r = select(1, &rfds, NULL, NULL, &tv);
	if (r > 0 && FD_ISSET(0, &rfds)) {
		/* both threads may see readiness: serialize so nobody blocks forever */
		if (pthread_mutex_trylock(&rd_lock) == 0) {
			struct pollfd p = { 0, POLLIN, 0 };
			if (poll(&p, 1, 0) > 0)
				consume(who, "select");
			pthread_mutex_unlock(&rd_lock);
		}
	} else if (r < 0 && errno != EINTR) {
		fprintf(stderr, "[T%d] select: %s\n", who, strerror(errno));
	}
}

static void wait_and_read_poll(int who)
{
	struct pollfd p = { 0, POLLIN, 0 };
	int r = poll(&p, 1, 100);

	if (r > 0 && (p.revents & (POLLIN | POLLHUP))) {
		if (pthread_mutex_trylock(&rd_lock) == 0) {
			struct pollfd q = { 0, POLLIN, 0 };
			if (poll(&q, 1, 0) > 0)
				consume(who, "poll");
			pthread_mutex_unlock(&rd_lock);
		}
	} else if (r < 0 && errno != EINTR) {
		fprintf(stderr, "[T%d] poll: %s\n", who, strerror(errno));
	}
}

static void *t_reader(void *arg)
{
	(void)arg;
	tid_of[0] = syscall(SYS_gettid);
	while (!stop_flag) {
		cnt[0]++;
		switch (mode) {
		case M_RS: case M_RP:
			consume(1, "read");           /* blocking read(0) */
			break;
		case M_PC:
			wait_and_read_poll(1);
			break;
		default:
			wait_and_read_select(1);
			break;
		}
	}
	return NULL;
}

static void *t_second(void *arg)
{
	(void)arg;
	tid_of[1] = syscall(SYS_gettid);
	volatile unsigned long x = 0;

	while (!stop_flag) {
		cnt[1]++;
		switch (mode) {
		case M_RS: {
			fd_set r; struct timeval tv = { 0, 100 * 1000 };
			FD_ZERO(&r); FD_SET(priv_pipe[0], &r);
			select(priv_pipe[0] + 1, &r, NULL, NULL, &tv);
			break;
		}
		case M_RP: {
			struct pollfd p = { priv_pipe[0], POLLIN, 0 };
			poll(&p, 1, 100);
			break;
		}
		case M_SS:
			wait_and_read_select(2);
			break;
		default: /* SC, PC: compute */
			for (int i = 0; i < 2000000; i++) x += i * 3;
			usleep(1000);
			break;
		}
	}
	return NULL;
}

static void dump_file(const char *path, const char *tag)
{
	char buf[512];
	FILE *f = fopen(path, "r");

	if (!f) { fprintf(stderr, "    %s: <%s>\n", tag, strerror(errno)); return; }
	if (fgets(buf, sizeof(buf), f)) {
		buf[strcspn(buf, "\n")] = 0;
		fprintf(stderr, "    %s: %s\n", tag, buf);
	}
	fclose(f);
}

static char task_state(pid_t tid)
{
	char path[64], buf[512];
	FILE *f;
	char *p, st = '?';

	snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
	f = fopen(path, "r");
	if (!f) return '?';
	if (fgets(buf, sizeof(buf), f) && (p = strrchr(buf, ')')) && p[1] == ' ')
		st = p[2];
	fclose(f);
	return st;
}

static void dump_threads(void)
{
	for (int i = 0; i < 2; i++) {
		char path[96];
		pid_t t = tid_of[i];
		if (!t) continue;
		fprintf(stderr, "  T%d tid=%d state=%c\n", i + 1, t, task_state(t));
		snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", t);
		dump_file(path, "syscall");
		snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", t);
		dump_file(path, "wchan");
	}
}

static int feeder(int period_s)
{
	for (int n = 1; ; n++) {
		if (dprintf(1, "%d %ld\n", n, wall_ms()) < 0)
			return 0;                   /* reader gone */
		fprintf(stderr, "[FEED] sent line %d\n", n);
		sleep(period_s);
	}
}

int main(int argc, char **argv)
{
	int opt, feed = 0;
	pthread_t a, b;

	setvbuf(stderr, NULL, _IOLBF, 0);
	while ((opt = getopt(argc, argv, "m:f:w:h")) != -1) {
		switch (opt) {
		case 'm':
			if (!strcmp(optarg, "rs")) mode = M_RS;
			else if (!strcmp(optarg, "rp")) mode = M_RP;
			else if (!strcmp(optarg, "ss")) mode = M_SS;
			else if (!strcmp(optarg, "sc")) mode = M_SC;
			else if (!strcmp(optarg, "pc")) mode = M_PC;
			else { fprintf(stderr, "unknown mode %s\n", optarg); return 2; }
			break;
		case 'f': feed = atoi(optarg); break;
		case 'w': watchdog_s = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-m rs|rp|ss|sc|pc] [-w watchdog_s]\n"
				"       %s -f <period_s>     (feeder: writes a line every N s)\n",
				argv[0], argv[0]);
			return 2;
		}
	}
	if (feed > 0)
		return feeder(feed);

	if ((mode == M_RS || mode == M_RP) && pipe(priv_pipe) < 0) {
		perror("pipe");
		return 1;
	}
	fprintf(stderr, "pipestorm pid=%d mode=%d watchdog=%ds (stdin/stdout should be pipes)\n",
		getpid(), mode, watchdog_s);

	last_rx_ms = now_ms();
	pthread_create(&a, NULL, t_reader, NULL);
	pthread_create(&b, NULL, t_second, NULL);

	long start = now_ms(), next_status = start + 2000, next_wd = start + watchdog_s * 1000L;
	unsigned long prev[2] = { 0, 0 };

	while (!stop_flag) {
		sleep(1);
		long t = now_ms();
		if (t >= next_status) {
			unsigned long c0 = cnt[0], c1 = cnt[1];
			fprintf(stderr, "[STAT] t=%lds T1 loops=%lu (+%lu) T2 loops=%lu (+%lu) lines=%lu idle=%lds\n",
				(t - start) / 1000, c0, c0 - prev[0], c1, c1 - prev[1],
				lines_rx, (t - last_rx_ms) / 1000);
			if (c0 == prev[0] && c1 == prev[1] && mode != M_RS && mode != M_RP)
				fprintf(stderr, "[STAT] !! no thread made progress in the last 2 s\n");
			prev[0] = c0; prev[1] = c1;
			next_status = t + 2000;
		}
		if (t >= next_wd && (t - last_rx_ms) > watchdog_s * 1000L) {
			fprintf(stderr, "[WATCHDOG] no input for %lds - thread dump:\n", (t - last_rx_ms) / 1000);
			dump_threads();
			next_wd = t + watchdog_s * 1000L;
		}
	}
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	return 0;
}
