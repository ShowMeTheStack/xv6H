// maskbench: what a description costs a Linux program (the masking baseline).
//
//   maskbench vec MB ROUNDS    an add over arrays of MB megabytes, scalar and (when the kernel
//                              says there is a vector unit) vector: MB/s each
//   maskbench clock N          N clock_gettime(CLOCK_MONOTONIC): ns each (the vDSO reads `time';
//                              under a counter at another rate that read traps)
//   maskbench page MB          MB of fresh anonymous memory touched a page at a time: ns per page
//                              (each page cleared by the kernel: cbo.zero where it has it)
//   maskbench read DEV MB      MB read from DEV with O_DIRECT, 64 KB at a time: MB/s
//
// One line per result: "maskbench <what> <value> <unit>". Built like vecuser (static, musl,
// rv64gc), with the three vector instructions hand-encoded, so the only vector code is the
// kernel's and this.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NR_RISCV_HWPROBE 258
#define HWPROBE_KEY_IMA_EXT_0 4
#define HWPROBE_IMA_V (1ULL << 2)

struct hwprobe { long long key; unsigned long long value; };

static int has_v(void)
{
	struct hwprobe p = { HWPROBE_KEY_IMA_EXT_0, 0 };

	if (syscall(NR_RISCV_HWPROBE, &p, 1, 0, NULL, 0) != 0)
		return 0;
	return (p.value & HWPROBE_IMA_V) != 0;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void add_scalar(int *c, const int *a, const int *b, long n)
{
	for (long i = 0; i < n; i++)
		c[i] = a[i] + b[i];
}

static void add_vector(int *c, const int *a, const int *b, long n)
{
	asm volatile(
		"mv a0, %0\n\t"
		"mv a1, %1\n\t"
		"mv a2, %2\n\t"
		"mv a3, %3\n\t"
		"1:\n\t"
		".word 0x0d3572d7\n\t"	/* vsetvli t0, a0, e32, m8, ta, ma */
		".word 0x0205e407\n\t"	/* vle32.v v8, (a1) */
		".word 0x02066807\n\t"	/* vle32.v v16, (a2) */
		".word 0x02880457\n\t"	/* vadd.vv v8, v8, v16 */
		".word 0x0206e427\n\t"	/* vse32.v v8, (a3) */
		"slli t1, t0, 2\n\t"
		"add a1, a1, t1\n\t"
		"add a2, a2, t1\n\t"
		"add a3, a3, t1\n\t"
		"sub a0, a0, t0\n\t"
		"bnez a0, 1b\n\t"
		: : "r"(n), "r"(a), "r"(b), "r"(c) : "a0", "a1", "a2", "a3", "t0", "t1", "memory");
}

static int bench_vec(long mb, int rounds)
{
	long n = mb * (1 << 20) / sizeof(int) / 3;
	int *a = malloc(n * sizeof(int)), *b = malloc(n * sizeof(int)), *c = malloc(n * sizeof(int));
	double t0, bytes = 3.0 * n * sizeof(int) * rounds;

	if (!a || !b || !c)
		return 1;
	for (long i = 0; i < n; i++) { a[i] = i; b[i] = 2 * i; c[i] = 0; }
	add_scalar(c, a, b, n);
	t0 = now();
	for (int r = 0; r < rounds; r++)
		add_scalar(c, a, b, n);
	printf("maskbench vec-scalar %.1f MB/s\n", bytes / (now() - t0) / 1e6);
	if (!has_v()) {
		printf("maskbench vec-vector absent -\n");
		return 0;
	}
	add_vector(c, a, b, n);
	for (long i = 0; i < n; i++)
		if (c[i] != 3 * i) {
			printf("maskbench vec-vector wrong -\n");
			return 1;
		}
	t0 = now();
	for (int r = 0; r < rounds; r++)
		add_vector(c, a, b, n);
	printf("maskbench vec-vector %.1f MB/s\n", bytes / (now() - t0) / 1e6);
	return 0;
}

static int bench_clock(long n)
{
	struct timespec t;
	double t0 = now();

	for (long i = 0; i < n; i++)
		clock_gettime(CLOCK_MONOTONIC, &t);
	printf("maskbench clock %.1f ns\n", (now() - t0) / n * 1e9);
	return 0;
}

static int bench_page(long mb)
{
	long ps = sysconf(_SC_PAGESIZE), np = mb * (1 << 20) / ps;
	char *m = mmap(NULL, np * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	double t0;

	if (m == MAP_FAILED)
		return 1;
	t0 = now();
	for (long i = 0; i < np; i++)
		m[i * ps] = 1;
	printf("maskbench page %.1f ns\n", (now() - t0) / np * 1e9);
	munmap(m, np * ps);
	return 0;
}

static int bench_read(const char *dev, long mb)
{
	int fd = open(dev, O_RDONLY | O_DIRECT);
	size_t bs = 64 << 10;
	void *buf;
	long got = 0;
	double t0;

	if (fd < 0 || posix_memalign(&buf, 4096, bs))
		return 1;
	t0 = now();
	while (got < mb * (1 << 20)) {
		ssize_t r = pread(fd, buf, bs, got);

		if (r <= 0)
			break;
		got += r;
	}
	printf("maskbench read %.1f MB/s\n", got / (now() - t0) / 1e6);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "vec"))
		return bench_vec(atol(argv[2]), atoi(argv[3]));
	if (argc >= 3 && !strcmp(argv[1], "clock"))
		return bench_clock(atol(argv[2]));
	if (argc >= 3 && !strcmp(argv[1], "page"))
		return bench_page(atol(argv[2]));
	if (argc >= 4 && !strcmp(argv[1], "read"))
		return bench_read(argv[2], atol(argv[3]));
	fprintf(stderr, "usage: maskbench vec MB ROUNDS | clock N | page MB | read DEV MB\n");
	return 2;
}
