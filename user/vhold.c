// vhold: hold vector state for SECS seconds. Every 10 ms it touches the
// vector registers, so that at any instant a host asking "does a task hold
// state in the unit" gets a deterministic yes (vecbench's rounds could be
// over, or not yet begun, at the instant of the ask: snap-migrate-negotiated
// 18/20). Exits 0 when the time is up; prints the sum it computed so the
// registers were really used.
#include "kernel/types.h"
#include "user/user.h"

static int buf[256], out[256];

static long
touch(void)
{
  long vl = 0;
  for (int i = 0; i < 256; i++) buf[i] = i;
  asm volatile(".option push\n.option arch, +v\n"
               "vsetvli %0, %1, e32, m4, ta, ma\n"
               "vle32.v v0, (%2)\n"
               "vadd.vv v8, v0, v0\n"
               "vse32.v v8, (%3)\n"
               ".option pop\n"
               : "=r"(vl) : "r"(256L), "r"(buf), "r"(out) : "memory");
  return out[7];
}

int
main(int argc, char *argv[])
{
  int secs = argc > 1 ? atoi(argv[1]) : 10;
  int t0 = uptime();
  long sum = 0;
  while (uptime() - t0 < secs * 100) {   // TICK_HZ is 100
    sum += touch();
    sleep(1);
  }
  printf("vhold: held the vector unit for %d s (sum %ld)\n", secs, sum);
  exit(0);
}
