// crossck: what a crossing must not disturb, checked from inside the guest.
//   crossck [MB] [seconds]
// Fills MB (default 64) of memory with a pattern keyed by the address, then once a
// second: verifies the whole region, reads CLOCK_MONOTONIC (must not go back, and
// must advance by the wall time), and checksums the vector register file it holds
// (32 registers written once; a crossing that lost or scrambled the file changes
// the sum). Prints one line per round and a final verdict; a mismatch is printed
// with its address and is fatal. Static, musl, vector ops hand-encoded (see vecuser.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double wall(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static sigjmp_buf jb;
static int have_v = 1;
static void on_sigill(int s) { have_v = 0; siglongjmp(jb, 1); }

/*
 * The vector file is held by a thread that makes no system call once it has
 * loaded the registers: Linux discards a task's vector registers on every
 * system call by design (riscv_v_vstate_discard fills them with ones; the
 * psABI makes them caller-saved across syscalls), so a thread that made one
 * between checks would report "vector file changed" after its first
 * printf. The thread spins on rdtime and re-checks the file once a second,
 * leaving its verdicts in shared memory for the main thread to report.
 */
static uint32_t vbuf[256], vout[256];
static volatile uint64_t vec_rounds, vec_bad, vec_first, vec_last;
static volatile int vec_stop;
static inline uint64_t rdtime(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }
static void vload(void)
{
	uint32_t *p = vbuf; long n = 256;
	asm volatile("mv a0, %0\n\tmv a1, %1\n\t"
		".word 0x0d3572d7\n\t"		/* vsetvli t0, a0, e32, m8, ta, ma */
		".word 0x0205e407\n\t"		/* vle32.v v8, (a1) */
		: : "r"(n), "r"(p) : "a0", "a1", "t0", "memory");
}
static uint64_t vsum(void)
{
	uint32_t *p = vout; long n = 256; uint64_t s = 0;
	asm volatile("mv a0, %0\n\tmv a3, %1\n\t"
		".word 0x0d3572d7\n\t"		/* vsetvli t0, a0, e32, m8, ta, ma */
		".word 0x0206e427\n\t"		/* vse32.v v8, (a3) */
		: : "r"(n), "r"(p) : "a0", "a3", "t0", "memory");
	for (int i = 0; i < 32; i++) s = s * 1000003 + vout[i];
	return s;
}
static void *vec_thread(void *arg)
{
	uint64_t hz = 10000000;		/* the timebase this machine describes; rdtime counts it */
	for (int i = 0; i < 256; i++) vbuf[i] = 0x1234567u * (uint32_t)(i + 1);
	vload();
	vec_first = vsum();
	uint64_t t = rdtime();
	while (!vec_stop) {
		while (rdtime() - t < hz) ;	/* a second, without a system call */
		t = rdtime();
		uint64_t s = vsum();
		if (s != vec_first) { vec_bad++; vec_last = s; }
		vec_rounds++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	long mb = argc > 1 ? atol(argv[1]) : 64, secs = argc > 2 ? atol(argv[2]) : 60;
	size_t n = (size_t)mb << 20;
	uint64_t *m = malloc(n);
	if (!m) { printf("crossck: no %ld MB\n", mb); return 3; }
	for (size_t i = 0; i < n / 8; i++) m[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL ^ 0xC0FFEE;
	signal(SIGILL, on_sigill);
	pthread_t th;
	if (!sigsetjmp(jb, 1)) {
		/* probe once here (SIGILL if the unit is absent), then hand the file to the thread */
		vload();
		if (pthread_create(&th, NULL, vec_thread, NULL) != 0) have_v = 0;
	}
	usleep(200000);
	double m0 = now(), w0 = wall(), mprev = m0;
	printf("crossck: %ld MB written, vector %s, mono %.3f\n", mb, have_v ? "held" : "absent", m0);
	fflush(stdout);
	int round = 0, bad = 0;
	while (now() - m0 < secs) {
		sleep(1);
		double mn = now(), wn = wall();
		if (mn < mprev) { printf("crossck: round %d: CLOCK_MONOTONIC went back: %.6f -> %.6f\n", round, mprev, mn); bad++; }
		mprev = mn;
		size_t first = 0; long errs = 0;
		for (size_t i = 0; i < n / 8; i++)
			if (m[i] != ((uint64_t)i * 0x9E3779B97F4A7C15ULL ^ 0xC0FFEE)) { if (!errs) first = i; errs++; }
		if (errs) { printf("crossck: round %d: %ld wrong words, first at byte %zu\n", round, errs, first * 8); bad++; }
		static uint64_t vec_bad_seen;
		if (have_v && vec_bad != vec_bad_seen) {
			printf("crossck: round %d: vector file changed (%llx -> %llx) in the holding thread, %llu time(s)\n", round,
			       (unsigned long long)vec_first, (unsigned long long)vec_last, (unsigned long long)vec_bad);
			bad++; vec_bad_seen = vec_bad;
		}
		double dm = mn - m0, dw = wn - w0;
		if (dw > 2 && (dm < dw * 0.9 || dm > dw * 1.1)) { printf("crossck: round %d: mono advanced %.2f s in %.2f s of wall time\n", round, dm, dw); bad++; }
		printf("crossck: round %d ok=%s mono +%.2f wall +%.2f vec rounds %llu\n", round, bad ? "no" : "yes", dm, dw, (unsigned long long)vec_rounds);
		fflush(stdout);
		round++;
	}
	vec_stop = 1;
	printf("crossck: %s after %d rounds (%ld MB, clock, %s)\n", bad ? "FAILED" : "intact", round, mb, have_v ? "vector file" : "no vector unit");
	return bad ? 1 : 0;
}
