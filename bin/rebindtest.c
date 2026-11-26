/*
 * rebindtest.c - MattX issue #74 / P3 0035: inherited fds of files on the shared DFSA tree
 *
 * 0035 opens read-only regular files below DFSA_DIR again at the target (native speed, mmap works) instead of proxying them
 * through the home node. This tool opens one fd of every kind BEFORE the migration, then keeps checking all of them while you
 * migrate it to another node and back:
 *
 *   shard   read-only file below DFSA_DIR          -> rebound at the target ("native"), seen in /proc/self/fd
 *   dup     dup() of the shard                      -> must keep SHARING the file offset with the shard fd
 *   rw      read/write file below DFSA_DIR          -> must stay a proxy (written, read back)
 *   gone    file below DFSA_DIR, unlinked after open-> must stay a proxy (still readable)
 *   local   file outside DFSA_DIR (/tmp)            -> must stay a proxy (only the home node has it)
 *   pipe    pipe()                                  -> pread must fail with ESPIPE
 *
 * Each round: a sequential read() on the shard and on its dup (one shared offset: content is verified at the expected
 * position, so a stale offset after the RETURN migration shows up at once), 16 x 1 MB pread() of the shard (MB/s and the
 * longest single call), a late mmap() of the shard fd (works on a native fd, not on a proxy), the rw / gone / local / pipe
 * checks, and for every fd what /proc/self/fd/N points to ("anon_inode:mattx_vfs_proxy" = proxy, a path = a real file).
 *
 * Build:   gcc -O2 -Wall -pthread -o rebindtest rebindtest.c
 * 1) generate the shard (under DFSA_DIR, on the shared NAS):   ./rebindtest -g /mnt/shared/rebind-shard.bin -s 256
 * 2) run:   ./rebindtest -D /mnt/shared      (prints its PID; migrate it, let it run, recall it)
 *
 * Options: -g FILE -s MB generate and exit (default 128 MB)   -D DIR  the DFSA directory (shard = DIR/rebind-shard.bin)
 *          -d SEC pause between rounds (1)    -r N rounds (0 = until SIGINT)
 * Exit status 1 if any check failed. Use -v to print every round in full (default: only on change of state or failure).
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
#include <sys/mman.h>
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
	uint64_t *b = malloc(1 << 20);

	if (fd < 0 || !b) { perror("open/malloc"); return 1; }
	for (uint64_t off = 0; off < (uint64_t)mb << 20; off += 1 << 20) {
		for (int i = 0; i < (1 << 20) / 8; i++)
			b[i] = word(off + i * 8);
		if (write(fd, b, 1 << 20) != (1 << 20)) { perror("write"); return 1; }
	}
	close(fd);
	printf("generated %s (%ld MB)\n", path, mb);
	return 0;
}

static int verify(const char *buf, size_t len, off_t off)
{
	for (size_t i = 0; i + 8 <= len; i += 8) {
		uint64_t v;
		memcpy(&v, buf + i, 8);
		if (v != word(off + i)) {
			printf("    MISMATCH at file offset %lld (got %016llx want %016llx)\n", (long long)(off + i),
			       (unsigned long long)v, (unsigned long long)word(off + i));
			return 1;
		}
	}
	return 0;
}

static const char *kind(int fd, char *out, size_t n)
{
	char l[64];
	ssize_t r;

	snprintf(l, sizeof(l), "/proc/self/fd/%d", fd);
	r = readlink(l, out, n - 1);
	if (r < 0) { snprintf(out, n, "?"); return out; }
	out[r] = 0;
	return out;
}

/* write a small file of the pattern (for the gone / local fds) */
static int mkpattern(const char *path, size_t len)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	char *b = malloc(len);

	if (fd < 0 || !b) return -1;
	for (size_t i = 0; i + 8 <= len; i += 8) {
		uint64_t v = word(i);
		memcpy(b + i, &v, 8);
	}
	if (write(fd, b, len) != (ssize_t)len) { close(fd); free(b); return -1; }
	free(b);
	return fd;
}

int main(int argc, char **argv)
{
	const char *dir = NULL, *genf = NULL;
	long gmb = 128, rounds = 0, pause_s = 1;
	int verbose = 0, c;
	char shard[512], rwp[512], gonep[512], localp[] = "/tmp/rebindtest-local.bin";

	while ((c = getopt(argc, argv, "g:s:D:d:r:v")) != -1) {
		switch (c) {
		case 'g': genf = optarg; break;
		case 's': gmb = atol(optarg); break;
		case 'D': dir = optarg; break;
		case 'd': pause_s = atol(optarg); break;
		case 'r': rounds = atol(optarg); break;
		case 'v': verbose = 1; break;
		default: fprintf(stderr, "usage: %s -g FILE [-s MB] | -D DFSA_DIR [-r N] [-d SEC] [-v]\n", argv[0]); return 2;
		}
	}
	if (genf) return gen(genf, gmb);
	if (!dir) { fprintf(stderr, "need -D DFSA_DIR (or -g FILE)\n"); return 2; }
	snprintf(shard, sizeof(shard), "%s/rebind-shard.bin", dir);
	snprintf(rwp, sizeof(rwp), "%s/rebind-rw.bin", dir);
	snprintf(gonep, sizeof(gonep), "%s/rebind-gone.bin", dir);

	/* ---- everything is opened BEFORE the migration ---- */
	int fa = open(shard, O_RDONLY);
	struct stat st;
	if (fa < 0) { fprintf(stderr, "cannot open %s: %s (generate it with -g)\n", shard, strerror(errno)); return 1; }
	if (fstat(fa, &st) < 0 || st.st_size < (16 << 20)) { fprintf(stderr, "shard too small (need >= 16 MB)\n"); return 1; }
	off_t fsz = st.st_size;
	int fb = dup(fa);
	int fw = open(rwp, O_CREAT | O_TRUNC | O_RDWR, 0644);
	int fg = mkpattern(gonep, 64 << 10);
	int fl = mkpattern(localp, 64 << 10);
	int pp[2];
	if (fb < 0 || fw < 0 || fg < 0 || fl < 0 || pipe(pp) < 0) { perror("setup"); return 1; }
	unlink(gonep);          /* the file is gone, the fd stays */
	char *buf = malloc(1 << 20);
	if (!buf) return 1;

	/* the sequential reads start in the middle of the shard (offset != 0: a lost offset is visible) */
	off_t seq = 3 << 20;
	if (lseek(fa, seq, SEEK_SET) != seq) { perror("lseek"); return 1; }
	off_t wpos = 0;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	srand(getpid());
	printf("rebindtest pid %d  shard %s (%lld bytes)  fds: shard %d dup %d rw %d gone %d local %d pipe %d  -- migrate me\n",
	       getpid(), shard, (long long)fsz, fa, fb, fw, fg, fl, pp[0]);
	fflush(stdout);

	int fails = 0, lastsig = -1;
	for (long round = 1; !stop && (rounds == 0 || round <= rounds); round++) {
		int bad = 0;
		char k1[300], k2[300], k3[300], k4[300], k5[300];
		char node[64] = "?";
		gethostname(node, sizeof(node));

		/* 1. sequential reads: shard, then its dup, ONE shared offset (seq = where the next read must be) */
		for (int w = 0; w < 2; w++) {
			int fd = w ? fb : fa;
			ssize_t r = read(fd, buf, 4096);
			if (r != 4096) { printf("  FAIL read(%s) -> %zd (errno %d)\n", w ? "dup" : "shard", r, errno); bad = 1; break; }
			if (verify(buf, 4096, seq)) { printf("  FAIL %s read is not at the expected offset %lld (offset lost or not shared)\n", w ? "dup" : "shard", (long long)seq); bad = 1; break; }
			seq += 4096;
		}

		/* 2. pread speed */
		double t0 = now(), worst = 0;
		size_t total = 0;
		for (int i = 0; i < 16 && !stop; i++) {
			off_t off = ((off_t)rand() % ((fsz - (1 << 20)) / 4096)) * 4096;
			double a = now();
			ssize_t r = pread(fa, buf, 1 << 20, off);
			double d = now() - a;
			if (d > worst) worst = d;
			if (r < 0) { printf("  FAIL pread -> errno %d\n", errno); bad = 1; break; }
			if (r != (1 << 20) && off + r != fsz) { printf("  FAIL short pread %zd\n", r); bad = 1; break; }
			if (verify(buf, r, off)) { bad = 1; break; }
			total += r;
		}
		double el = now() - t0;

		/* 3. late mmap of the shard fd: a native fd can, a proxy cannot */
		const char *mm = "mmap ok";
		{
			off_t off = 5 << 20;
			void *m = mmap(NULL, 1 << 20, PROT_READ, MAP_SHARED, fa, off);
			if (m == MAP_FAILED) mm = "mmap FAILED (proxy?)";
			else {
				if (verify(m, 1 << 20, off)) { bad = 1; mm = "mmap CONTENT BAD"; }
				munmap(m, 1 << 20);
			}
		}

		/* 4. rw file: must stay writable (a proxy) */
		{
			char rec[16];
			ssize_t r;
			snprintf(rec, sizeof(rec), "r%014ld", round % 100000000000000L);
			r = write(fw, rec, 16);
			if (r != 16) { printf("  FAIL write(rw) -> %zd errno %d\n", r, errno); bad = 1; }
			else {
				char back[16];
				if (pread(fw, back, 16, wpos) != 16 || memcmp(back, rec, 16)) { printf("  FAIL rw read-back at %lld\n", (long long)wpos); bad = 1; }
				wpos += 16;
			}
		}

		/* 5. gone / local */
		if (pread(fg, buf, 4096, 0) != 4096 || verify(buf, 4096, 0)) { printf("  FAIL gone (unlinked) file\n"); bad = 1; }
		if (pread(fl, buf, 4096, 0) != 4096 || verify(buf, 4096, 0)) { printf("  FAIL local file\n"); bad = 1; }

		/* 6. pipe */
		errno = 0;
		if (pread(pp[0], buf, 1, 0) != -1 || errno != ESPIPE) { printf("  FAIL pread on the pipe (want ESPIPE, errno %d)\n", errno); bad = 1; }

		kind(fa, k1, sizeof(k1)); kind(fb, k2, sizeof(k2)); kind(fw, k3, sizeof(k3)); kind(fg, k4, sizeof(k4)); kind(fl, k5, sizeof(k5));
		int sig = (k1[0] == '/') | ((k2[0] == '/') << 1) | ((k3[0] == '/') << 2) | ((k4[0] == '/') << 3) | ((k5[0] == '/') << 4) | (mm[5] == 'o' ? 32 : 0);
		if (verbose || bad || sig != lastsig || round == 1) {
			printf("round %ld on %s: seq offset %lld, pread %.1f MB/s (longest %.3fs), %s\n", round, node, (long long)seq,
			       total / 1048576.0 / (el > 0 ? el : 1e-9), worst, mm);
			printf("    shard -> %s\n    dup   -> %s\n    rw    -> %s\n    gone  -> %s\n    local -> %s\n", k1, k2, k3, k4, k5);
			lastsig = sig;
		}
		fails += bad;
		fflush(stdout);
		if (bad) break;
		if (!stop) sleep(pause_s);
	}
	printf("rebindtest done: %s\n", fails ? "FAILED" : "all ok");
	unlink(localp);
	return fails ? 1 : 0;
}
