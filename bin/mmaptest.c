/*
 * mmaptest.c - MattX issue #67 / P3 0032+0033: file mappings across migration
 *
 * Builds read-only file mappings the way colibri does (COLI_MMAP / COLI_MAP_EXPERTS:
 * one PROT_READ + MAP_SHARED mapping per shard file), verifies them continuously, and
 * keeps verifying while the process is migrated. The test file is generated and every
 * 8-byte word is a known function of its file offset, so the test never needs a
 * reference copy and cannot be fooled by a zero-filled hole.
 *
 * Build:   gcc -O2 -pthread -o mmaptest mmaptest.c
 *
 * 1) generate a test file (put it under DFSA_DIR, or on a MattXFS-visible path):
 *      ./mmaptest -g /mnt/shared/mmaptest.bin -s 256        # 256 MB
 *
 * 2) run it (it prints its PID, then a status line every second):
 *      ./mmaptest -f /mnt/shared/mmaptest.bin -n 8          # 8 non-adjacent mappings
 *    then migrate the PID with your usual MattX tooling and watch the output.
 *
 *    Mappings made (each one is a separate VMA, windows are NOT adjacent in the file):
 *      M0..Mn-1  PROT_READ, MAP_SHARED     (the colibri shape: must go as "map this file")
 *      P         PROT_READ, MAP_PRIVATE    (clean private: may go as a file)
 *      C         MAP_PRIVATE, one page written then mprotect(PROT_READ)
 *                (COW page present: must NOT go as a file, must keep its modified bytes)
 *
 * Signals while running:
 *      SIGUSR1   LATE-MAP test: open() + mmap() the file NOW (i.e. on the surrogate, after
 *                the migration) and verify it; reports errno on failure. This is the case
 *                of colibri's lazy map_of_fd() / st_shard_mapped() for a shard that was
 *                never touched before the move. Failure is expected until a Ghost-fd mmap
 *                path exists, and the engines then fall back to pread: the test also does
 *                a pread of the same range so you can see both.
 *      SIGUSR2   one FULL verification pass over every mapping (all words), with timing.
 *      SIGTERM / SIGINT   summary and exit.
 *
 * Options:
 *      -f FILE   file to map (required to run)           -n N   number of M mappings (default 4)
 *      -w MB     window size per mapping (default: whole file / n, at most 64 MB, min 16 MB)
 *      -g FILE   generate FILE and exit                  -s MB  size for -g (default 128)
 *      -F        full verification every tick (default: sample 64 words per mapping)
 *      -d SEC    tick interval (default 1)
 *
 * What to look for:
 *   - "ok" lines stay "ok" before, during and after the move. "BAD" = wrong bytes, "SIGBUS" =
 *     fault on a page the file cannot deliver (size/mtime check failed, file missing, ...).
 *   - RssAnon in the status line should NOT jump by the size of the mappings after the move:
 *     the file mappings are re-created empty on the target and fault in from DFSA/MattXFS
 *     (RssFile grows instead). With 0032 off (mmap_file_min_mb huge) RssAnon grows by it.
 *   - C keeps its modified word (tag "cow" in the status line).
 *   - Kernel side: dmesg | grep -E "File mapping|Skipping VMA"  (home), stub log on the target:
 *     /tmp/mattx_stub.log  ("map_file_vma").
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define PAGE 4096UL
#define MAXMAP 512

struct map {
	char     tag[16];
	uint8_t *p;
	size_t   len;
	off_t    off;       /* file offset of the window */
	int      cow;       /* 1: word at index 0 was modified before mprotect */
};

static struct map maps[MAXMAP + 2];
static int nmaps;
static const char *path;
static volatile sig_atomic_t want_late, want_full, want_exit;
static sigjmp_buf jb;
static volatile sig_atomic_t in_probe;
static uint64_t bad_total, bus_total, ticks;

static uint64_t mix(uint64_t x)           /* splitmix64 of the word index */
{
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	return x ^ (x >> 31);
}
#define EXPECT(fileoff) mix((uint64_t)(fileoff) / 8)
#define COW_XOR 0xA5A5A5A5A5A5A5A5ULL

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_sigbus(int sig)
{
	(void)sig;
	if (in_probe) siglongjmp(jb, 1);
	_exit(99);
}
static void on_usr1(int s) { (void)s; want_late = 1; }
static void on_usr2(int s) { (void)s; want_full = 1; }
static void on_term(int s) { (void)s; want_exit = 1; }

static int generate(const char *f, size_t mb)
{
	int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 1; }
	size_t total = mb << 20, done = 0;
	size_t bs = 1 << 20;
	uint64_t *buf = malloc(bs);
	if (!buf) return 1;
	while (done < total) {
		for (size_t i = 0; i < bs / 8; i++) buf[i] = EXPECT(done + i * 8);
		if (write(fd, buf, bs) != (ssize_t)bs) { perror("write"); return 1; }
		done += bs;
	}
	fsync(fd);
	close(fd);
	printf("generated %s: %zu MB (every 8-byte word = mix(offset/8))\n", f, mb);
	return 0;
}

/* check one word; returns 0 ok, 1 bad, 2 sigbus */
static int check_word(const struct map *m, size_t idx)
{
	uint64_t want = EXPECT(m->off + idx * 8), got;

	if (m->cow && idx == 0) want ^= COW_XOR;
	in_probe = 1;
	if (sigsetjmp(jb, 1)) { in_probe = 0; return 2; }
	got = ((volatile uint64_t *)m->p)[idx];
	in_probe = 0;
	return got == want ? 0 : 1;
}

/* verify one mapping: sample (n words spread over the window) or all words */
static void verify(const struct map *m, int full, uint64_t *bad, uint64_t *bus, uint64_t *words)
{
	size_t nw = m->len / 8;

	if (full) {
		for (size_t i = 0; i < nw; i++) {
			int r = check_word(m, i);
			(*words)++;
			if (r == 1) (*bad)++;
			if (r == 2) { (*bus)++; i += PAGE / 8; }   /* skip the rest of the page */
		}
	} else {
		for (int k = 0; k < 64; k++) {
			size_t i = (size_t)(((uint64_t)k * 2654435761ULL + ticks * 40503ULL) % nw);
			int r = check_word(m, i);
			(*words)++;
			if (r == 1) (*bad)++;
			if (r == 2) (*bus)++;
		}
	}
}

static void rss_line(char *out, size_t n)
{
	char buf[4096];
	long anon = -1, file = -1;
	FILE *f = fopen("/proc/self/status", "r");
	if (f) {
		while (fgets(buf, sizeof buf, f)) {
			sscanf(buf, "RssAnon: %ld kB", &anon);
			sscanf(buf, "RssFile: %ld kB", &file);
		}
		fclose(f);
	}
	snprintf(out, n, "RssAnon=%ldMB RssFile=%ldMB", anon / 1024, file / 1024);
}

static void late_map_test(void)
{
	printf("[LATE] open+mmap of %s AFTER startup (surrogate case)...\n", path);
	int fd = open(path, O_RDONLY);
	if (fd < 0) { printf("[LATE] open failed: %s\n", strerror(errno)); return; }
	struct stat st;
	if (fstat(fd, &st) < 0) { printf("[LATE] fstat failed: %s\n", strerror(errno)); close(fd); return; }
	size_t len = (size_t)st.st_size < (16UL << 20) ? (size_t)st.st_size : (16UL << 20);
	len &= ~(PAGE - 1);

	/* pread first: this is what the engines fall back to */
	uint64_t *buf = malloc(len);
	if (buf) {
		ssize_t r = pread(fd, buf, len, 0);
		int bad = 0;
		if (r != (ssize_t)len) printf("[LATE] pread: %zd of %zu bytes (%s)\n", r, len, r < 0 ? strerror(errno) : "short");
		else {
			for (size_t i = 0; i < len / 8; i++) if (buf[i] != EXPECT(i * 8)) bad++;
			printf("[LATE] pread of %zu bytes: %s (%d bad words)\n", len, bad ? "BAD" : "ok", bad);
		}
		free(buf);
	}

	void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		printf("[LATE] mmap FAILED: %s  -> engines fall back to pread (expected until a Ghost-fd mmap path exists)\n", strerror(errno));
	} else {
		struct map m = { "late", p, len, 0, 0 };
		uint64_t bad = 0, bus = 0, w = 0;
		verify(&m, 1, &bad, &bus, &w);
		printf("[LATE] mmap ok, %llu words checked: %s (bad %llu, sigbus %llu)\n", (unsigned long long)w,
		       bad || bus ? "BAD" : "ok", (unsigned long long)bad, (unsigned long long)bus);
		munmap(p, len);
	}
	close(fd);
}

int main(int argc, char **argv)
{
	int opt, n = 4, full_each = 0, tick = 1;
	size_t win = 0, gen_mb = 128;
	const char *gen = NULL;

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((opt = getopt(argc, argv, "f:n:w:g:s:Fd:h")) != -1) {
		switch (opt) {
		case 'f': path = optarg; break;
		case 'n': n = atoi(optarg); break;
		case 'w': win = (size_t)atol(optarg) << 20; break;
		case 'g': gen = optarg; break;
		case 's': gen_mb = (size_t)atol(optarg); break;
		case 'F': full_each = 1; break;
		case 'd': tick = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s -g FILE [-s MB]   |   %s -f FILE [-n N] [-w MB] [-F] [-d SEC]\n", argv[0], argv[0]);
			return 2;
		}
	}
	if (gen) return generate(gen, gen_mb);
	if (!path || n < 1 || n > MAXMAP) { fprintf(stderr, "need -f FILE and 1 <= n <= %d\n", MAXMAP); return 2; }

	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	struct stat st;
	fstat(fd, &st);
	size_t fsz = (size_t)st.st_size;
	if (!win) {
		win = fsz / (size_t)(n + 2) - PAGE;
		if (win > (64UL << 20)) win = 64UL << 20;
		if (win < (16UL << 20)) win = 16UL << 20;
	}
	win &= ~(PAGE - 1);
	/* windows are separated by one page of file offset, so neighbouring VMAs never merge */
	size_t need = (size_t)(n + 2) * (win + PAGE);
	if (need > fsz) {
		fprintf(stderr, "file too small: %zu MB, need %zu MB for %d mappings of %zu MB (use -n/-w/-s)\n",
		        fsz >> 20, need >> 20, n, win >> 20);
		return 1;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_sigbus; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER;
	sigaction(SIGBUS, &sa, NULL);
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	off_t off = 0;
	for (int i = 0; i < n; i++, off += win + PAGE) {
		void *p = mmap(NULL, win, PROT_READ, MAP_SHARED, fd, off);
		if (p == MAP_FAILED) { perror("mmap M"); return 1; }
		snprintf(maps[nmaps].tag, sizeof maps[nmaps].tag, "M%d", i);
		maps[nmaps].p = p; maps[nmaps].len = win; maps[nmaps].off = off; nmaps++;
	}
	{   /* P: clean private read-only mapping */
		void *p = mmap(NULL, win, PROT_READ, MAP_PRIVATE, fd, off);
		if (p == MAP_FAILED) { perror("mmap P"); return 1; }
		strcpy(maps[nmaps].tag, "P");
		maps[nmaps].p = p; maps[nmaps].len = win; maps[nmaps].off = off; nmaps++;
		off += win + PAGE;
	}
	{   /* C: private, COW one page, then read-only again */
		void *p = mmap(NULL, win, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, off);
		if (p == MAP_FAILED) { perror("mmap C"); return 1; }
		((volatile uint64_t *)p)[0] ^= COW_XOR;
		if (mprotect(p, win, PROT_READ)) { perror("mprotect C"); return 1; }
		strcpy(maps[nmaps].tag, "C");
		maps[nmaps].p = p; maps[nmaps].len = win; maps[nmaps].off = off; maps[nmaps].cow = 1; nmaps++;
	}
	close(fd);   /* like colibri: the mapping outlives the fd */

	printf("mmaptest pid=%d file=%s (%zu MB) mappings=%d x %zu MB (+P +C)\n", getpid(), path, fsz >> 20, n, win >> 20);
	printf("  migrate me now; SIGUSR1 = late-map test, SIGUSR2 = full pass, SIGTERM = summary\n");
	for (int i = 0; i < nmaps; i++)
		printf("  %-3s %p-%p file off %lld\n", maps[i].tag, (void *)maps[i].p, (void *)(maps[i].p + maps[i].len), (long long)maps[i].off);

	while (!want_exit) {
		uint64_t bad = 0, bus = 0, words = 0;
		double t0 = now_s();
		int full = full_each || want_full;

		if (want_full) { want_full = 0; printf("[FULL] verifying every word of every mapping...\n"); }
		for (int i = 0; i < nmaps; i++) verify(&maps[i], full, &bad, &bus, &words);
		bad_total += bad; bus_total += bus; ticks++;

		char rss[96];
		rss_line(rss, sizeof rss);
		printf("[tick %llu] %s  %llu words, bad=%llu sigbus=%llu  %.3fs  %s  (cow word %s)\n",
		       (unsigned long long)ticks, bad || bus ? "BAD" : "ok", (unsigned long long)words,
		       (unsigned long long)bad, (unsigned long long)bus, now_s() - t0, rss,
		       check_word(&maps[nmaps - 1], 0) == 0 ? "kept" : "LOST");

		if (want_late) { want_late = 0; late_map_test(); }
		sleep(tick);
	}
	printf("summary: %llu ticks, bad=%llu sigbus=%llu -> %s\n", (unsigned long long)ticks,
	       (unsigned long long)bad_total, (unsigned long long)bus_total, bad_total || bus_total ? "FAIL" : "PASS");
	return bad_total || bus_total ? 1 : 0;
}
