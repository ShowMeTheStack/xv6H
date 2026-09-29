// memck: what a crossing must not disturb, checked from inside an xv6 guest (L17).
//   memck [MB] [seconds]
// Fills MB (default 16) of memory with a pattern keyed by the address and, once a
// second, verifies it and checks that the clock has not gone back and advances at
// the rate of the ticks. One line per round; a mismatch is fatal and names its address.
#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

static uint64
rdtime(void)
{
  uint64 x;
  asm volatile("rdtime %0" : "=r"(x));
  return x;
}

int
main(int argc, char *argv[])
{
  int mb = argc > 1 ? atoi(argv[1]) : 16, secs = argc > 2 ? atoi(argv[2]) : 60;
  uint64 n = (uint64)mb << 20;
  uint64 *m = (uint64 *)malloc(n);
  if (m == 0) { printf("memck: no %d MB\n", mb); exit(3); }
  for (uint64 i = 0; i < n / 8; i++) m[i] = i * 0x9E3779B97F4A7C15ULL ^ 0xC0FFEE;
  uint64 t0 = rdtime(), tprev = t0; int u0 = uptime();
  printf("memck: %d MB written, time %ld\n", mb, t0);
  int round = 0, bad = 0;
  while (uptime() - u0 < secs * TICK_HZ) {
    sleep(1);                             // xv6H's sleep() takes seconds
    uint64 t = rdtime();
    if (t < tprev) { printf("memck: round %d: time went back %ld -> %ld\n", round, tprev, t); bad++; }
    tprev = t;
    uint64 errs = 0, first = 0;
    for (uint64 i = 0; i < n / 8; i++)
      if (m[i] != (i * 0x9E3779B97F4A7C15ULL ^ 0xC0FFEE)) { if (!errs) first = i; errs++; }
    if (errs) { printf("memck: round %d: %ld wrong words, first at byte %ld\n", round, errs, first * 8); bad++; }
    printf("memck: round %d ok=%s ticks %d\n", round, bad ? "no" : "yes", uptime() - u0);
    round++;
  }
  printf("memck: %s after %d rounds (%d MB, clock)\n", bad ? "FAILED" : "intact", round, mb);
  exit(bad ? 1 : 0);
}
