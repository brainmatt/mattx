/*
 * preadtest.c - MattX issue #74 / P3 0034: pread64() on inherited fds across migration
 *
 * colibri (st.h) keeps its shard files open and loops pread() over them. On a migrated
 * process those fds are proxy fds; this tool checks that pread() on them is correct
 * (content, short reads, ESPIPE on pipes) and fast (MB/s, longest single stall).
 *
 * Build:   gcc -O2 -Wall -pthread -o preadtest preadtest.c
 *
 * 1) generate a pattern file (every 8-byte word is a function of its file offset):
 *      ./preadtest -g /mnt/shared/preadtest.bin -s 256        # 256 MB
 * 2) run it; it opens the file + a pipe BEFORE the move, prints its PID and loops:
 *      ./preadtest -f /mnt/shared/preadtest.bin
 *    migrate the PID with your usual MattX tooling and watch the per-round lines.
 *
 * Each round, for every size in 4 KB, 64 KB, 1 MB, 4 MB: pread() at a random aligned
 * offset, verify every word, report MB/s and the longest single pread() (stall).
 * Also: pread() at EOF returns 0, pread() across EOF is short, pread() on the pipe must
 * fail with ESPIPE. Exit status 1 if any check failed.
 *
 * Options: -f FILE  file to read      -g FILE  generate and exit     -s MB  size for -g (128)
 *          -r N     rounds (0 = until SIGINT, default 0)             -d SEC pause between rounds (1)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static uint64_t word(uint64_t off) { return (off * 0x9E3779B97F4A7C15ULL) ^ (off >> 3) ^ 0xA5A5A5A5DEADBEEFULL; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int gen(const char *path, long mb)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) { perror("open"); return 1; }
	uint64_t *b = malloc(1 << 20);
	if (!b) return 1;
	for (uint64_t off = 0; off < (uint64_t)mb << 20; off += 1 << 20) {
		for (int i = 0; i < (1 << 20) / 8; i++)
			b[i] = word(off + i * 8);
		if (write(fd, b, 1 << 20) != (1 << 20)) { perror("write"); return 1; }
	}
	close(fd);
	printf("generated %s (%ld MB)\n", path, mb);
	return 0;
}

/* read exactly len bytes with pread, looping on short reads; returns bytes or -1 */
static ssize_t pread_full(int fd, char *buf, size_t len, off_t off, int *calls)
{
	size_t done = 0;
	while (done < len) {
		ssize_t r = pread(fd, buf + done, len - done, off + done);
		(*calls)++;
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		if (r == 0) break;
		done += r;
	}
	return done;
}

static int verify(const char *buf, size_t len, off_t off)
{
	for (size_t i = 0; i + 8 <= len; i += 8) {
		uint64_t v;
		memcpy(&v, buf + i, 8);
		if (v != word(off + i)) {
			printf("  MISMATCH at file offset %lld (got %016llx want %016llx)\n",
			       (long long)(off + i), (unsigned long long)v, (unsigned long long)word(off + i));
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *file = NULL, *genf = NULL;
	long gmb = 128, rounds = 0, pause_s = 1;
	int c;

	while ((c = getopt(argc, argv, "f:g:s:r:d:")) != -1) {
		switch (c) {
		case 'f': file = optarg; break;
		case 'g': genf = optarg; break;
		case 's': gmb = atol(optarg); break;
		case 'r': rounds = atol(optarg); break;
		case 'd': pause_s = atol(optarg); break;
		default: fprintf(stderr, "usage: %s -g FILE [-s MB] | -f FILE [-r N] [-d SEC]\n", argv[0]); return 2;
		}
	}
	if (genf) return gen(genf, gmb);
	if (!file) { fprintf(stderr, "need -f FILE (or -g FILE)\n"); return 2; }

	/* open everything BEFORE the migration: these become proxy fds */
	int fd = open(file, O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	struct stat st;
	if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
	int pp[2];
	if (pipe(pp) < 0) { perror("pipe"); return 1; }
	if (write(pp[1], "x", 1) != 1) { perror("pipe write"); return 1; }

	off_t fsz = st.st_size;
	static const size_t sizes[] = { 4096, 65536, 1 << 20, 4 << 20 };
	char *buf = malloc(4 << 20);
	if (!buf) return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	srand(getpid());
	printf("preadtest pid %d  file %s (%lld bytes)  fd %d, pipe fd %d  -- migrate me\n",
	       getpid(), file, (long long)fsz, fd, pp[0]);
	if (fsz < (8 << 20)) { fprintf(stderr, "file too small (need >= 8 MB)\n"); return 1; }
	fflush(stdout);

	int fails = 0;
	for (long round = 1; !stop && (rounds == 0 || round <= rounds); round++) {
		printf("round %ld\n", round);
		for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			size_t len = sizes[s];
			int n = len >= (1 << 20) ? 8 : 64, calls = 0, bad = 0;
			double t0 = now(), worst = 0;
			size_t total = 0;
			for (int i = 0; i < n && !stop; i++) {
				off_t off = ((off_t)rand() % ((fsz - len) / 4096)) * 4096;
				double a = now();
				ssize_t r = pread_full(fd, buf, len, off, &calls);
				double d = now() - a;
				if (d > worst) worst = d;
				if (r != (ssize_t)len) {
					printf("  SHORT/ERR pread %zu @%lld -> %zd (errno %d)\n", len, (long long)off, r, errno);
					bad = 1;
					break;
				}
				bad |= verify(buf, len, off);
				total += r;
			}
			double el = now() - t0;
			printf("  %-8zu %s  %7.1f MB/s  %d pread calls  longest %.3fs\n", len, bad ? "FAIL" : "ok  ",
			       total / 1048576.0 / (el > 0 ? el : 1e-9), calls, worst);
			fails += bad;
		}
		/* EOF semantics */
		ssize_t r = pread(fd, buf, 4096, fsz);
		if (r != 0) { printf("  FAIL pread at EOF -> %zd (want 0)\n", r); fails++; }
		else printf("  EOF       ok   pread at EOF = 0\n");
		r = pread(fd, buf, 8192, fsz - 4096);
		if (r != 4096) { printf("  FAIL pread across EOF -> %zd (want 4096)\n", r); fails++; }
		else if (verify(buf, 4096, fsz - 4096)) fails++;
		else printf("  EOF       ok   short read across EOF = 4096\n");
		/* pipe: positional I/O must be refused, and must not hang */
		errno = 0;
		r = pread(pp[0], buf, 1, 0);
		if (r != -1 || errno != ESPIPE) { printf("  FAIL pread on pipe -> %zd errno %d (want -1/ESPIPE)\n", r, errno); fails++; }
		else printf("  pipe      ok   pread -> ESPIPE\n");
		fflush(stdout);
		if (!stop) sleep(pause_s);
	}
	printf("preadtest done: %s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
