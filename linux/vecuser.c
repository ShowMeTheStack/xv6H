// vecuser: the vector unit as a Linux program sees it (xv6H F10-F12, L9).
//
//   vecuser probe       what the kernel says: the hwprobe syscall and AT_HWCAP
//   vecuser use [N]     N rounds (default 1) of a checked vector add; an illegal
//                       instruction is reported ("vecuser: SIGILL"), not fatal
//   vecuser hold SECS   touch the vector registers every 10 ms for SECS seconds:
//                       a task holding vector state for as long as the test needs
//
// Built static against musl (a scalar libc) with -march=rv64gc; the five vector
// instructions are hand-encoded (.word), because musl's gcc 11 assembler has no
// V and the GNU toolchain's glibc must not be used: Ubuntu 26.04's riscv64
// baseline is RVA23, whose glibc runs `vsetivli' in __memset_generic -- every
// program linked against it uses the vector unit before main (lxvs2: SIGILL in
// memset, no handler installed yet). Here the only vector instructions are the
// ones written below, so "use" is the first use.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <time.h>
#include <sys/auxv.h>
#include <sys/syscall.h>

#define NR_RISCV_HWPROBE 258
#define HWPROBE_KEY_IMA_EXT_0 4
#define HWPROBE_IMA_V (1ULL << 2)
#define HWCAP_V (1UL << ('v' - 'a'))

struct hwprobe { long long key; unsigned long long value; };

static int hwprobe_v(void)
{
	struct hwprobe p = { HWPROBE_KEY_IMA_EXT_0, 0 };
	if (syscall(NR_RISCV_HWPROBE, &p, 1, 0, NULL, 0) != 0)
		return -1;
	return (p.value & HWPROBE_IMA_V) != 0;
}

static sigjmp_buf jb;
static volatile int illegal;

static void on_sigill(int sig)
{
	illegal++;
	siglongjmp(jb, 1);
}

// c[i] = a[i] + b[i] over 256 ints with the vector unit; 0 if right, -1 if wrong
static int vadd_round(void)
{
	static int a[256], b[256], c[256];
	long n = 256;
	for (int i = 0; i < 256; i++) { a[i] = i; b[i] = 2 * i; c[i] = -1; }
	/* the operands are moved into a0-a3 inside the asm: a local register
	 * variable is not kept across the memset the compiler makes of the loop
	 * above (lxv3: a2 clobbered, SIGSEGV on the first load) */
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
	for (int i = 0; i < 256; i++)
		if (c[i] != 3 * i)
			return -1;
	return 0;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "probe";
	signal(SIGILL, on_sigill);
	if (strcmp(what, "probe") == 0) {
		int hp = hwprobe_v();
		int hc = (getauxval(AT_HWCAP) & HWCAP_V) != 0;
		if (hp == 1)
			printf("vecuser: V present (hwprobe), hwcap-v %s\n", hc ? "yes" : "no");
		else
			printf("vecuser: no vector unit (hwprobe %s), hwcap-v %s\n", hp < 0 ? "failed" : "says none", hc ? "yes" : "no");
		return 0;
	}
	if (strcmp(what, "use") == 0) {
		int rounds = argc > 2 ? atoi(argv[2]) : 1, bad = 0;
		if (sigsetjmp(jb, 1)) {
			printf("vecuser: SIGILL (no vector unit for this task)\n");
			return 2;
		}
		for (int r = 0; r < rounds; r++)
			if (vadd_round() < 0)
				bad++;
		printf(bad ? "vecuser: %d of %d rounds WRONG\n" : "vecuser: ok, %d round(s)\n", bad ? bad : rounds, rounds);
		return bad ? 1 : 0;
	}
	if (strcmp(what, "hold") == 0) {
		double secs = argc > 2 ? atof(argv[2]) : 10, t0 = now();
		long touches = 0;
		if (sigsetjmp(jb, 1)) {
			printf("vecuser: SIGILL while holding vector state after %ld touches\n", touches);
			return 2;
		}
		printf("vecuser: holding vector state for %.0f s\n", secs);
		fflush(stdout);
		while (now() - t0 < secs) {
			if (vadd_round() < 0) {
				printf("vecuser: hold: WRONG result after %ld touches\n", touches);
				return 1;
			}
			touches++;
			usleep(10000);
		}
		printf("vecuser: held vector state for %.0f s (%ld touches), released\n", secs, touches);
		return 0;
	}
	fprintf(stderr, "usage: vecuser probe | use [N] | hold SECS\n");
	return 64;
}
