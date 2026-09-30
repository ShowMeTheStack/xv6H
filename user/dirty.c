// dirty: dirty memory at a set rate, for the migration curves (L14d).
//   dirty MB RATE SECONDS
// Takes MB of memory, writes it once, then rewrites RATE megabytes of it a
// second (spread over the second, paced by rdtime) for SECONDS, cycling
// through the region so that every page is dirtied again and again. Each
// write is a pattern keyed by the address and the round, so that at the end
// the last round can be verified: a page the host copied stale after a
// crossing shows as a mismatch. Prints one line a second and a verdict.
#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

#define TIMEBASE 10000000UL          // rdtime ticks a second (the described 10 MHz)

static inline uint64
rdtime(void)
{
  uint64 x;
  asm volatile("rdtime %0" : "=r"(x));
  return x;
}

int
main(int argc, char *argv[])
{
  if (argc != 4) {
    fprintf(stderr, "usage: dirty MB RATE SECONDS\n");
    exit(1);
  }
  long mb = atoi(argv[1]), rate = atoi(argv[2]), secs = atoi(argv[3]);
  uint64 n = (uint64)mb << 20;
  uint64 *m = (uint64 *)malloc(n);
  if (m == 0) {
    fprintf(stderr, "dirty: no memory for %ld MB\n", mb);
    exit(1);
  }
  uint64 words = n / sizeof(uint64);
  uint64 round = 0;
  for (uint64 i = 0; i < words; i++)
    m[i] = (uint64)&m[i] ^ round;
  printf("dirty: %ld MB written, now %ld MB/s for %ld s\n", mb, rate, secs);

  // RATE MB a second in slices of a megabyte, each slice paced to its share
  // of the second; a slice of zero rate is a plain sleep
  uint64 t0 = rdtime(), pos = 0, touched = 0;
  uint64 slice = TIMEBASE / (rate > 0 ? rate : 1);     // ticks per megabyte
  uint64 next = t0;
  for (long s = 0; s < secs; s++) {
    uint64 tsec = t0 + (uint64)(s + 1) * TIMEBASE;
    if (rate == 0) {
      while (rdtime() < tsec)
        sleep(1);
    } else {
      while (rdtime() < tsec) {
        uint64 w = (1UL << 20) / sizeof(uint64);
        if (pos + w > words) {
          pos = 0;
          round++;
        }
        for (uint64 i = pos; i < pos + w; i++)
          m[i] = (uint64)&m[i] ^ round;
        pos += w;
        touched++;
        next += slice;
        while (rdtime() < next)
          ;                                           // pace: spin the rest of the slice
      }
    }
    printf("dirty: %ld s, %ld MB touched, round %ld\n", s + 1, (long)touched, (long)round);
  }
  // verify: every word holds the pattern of the round it was last written in
  // (the current round up to pos, the previous one after it)
  uint64 bad = 0;
  for (uint64 i = 0; i < words; i++) {
    uint64 want = (uint64)&m[i] ^ (i < pos ? round : (round ? round - 1 : 0));
    if (rate > 0 && m[i] != want) {
      if (bad == 0)
        printf("dirty: MISMATCH at %lx: %lx, wanted %lx\n", (uint64)&m[i], m[i], want);
      bad++;
    }
  }
  printf("dirty: %s (%ld MB touched, %ld bad words)\n", bad ? "FAILED" : "ok", (long)touched, (long)bad);
  exit(bad ? 1 : 0);
}
