// vecprobe: does a task's vector register file survive a printf? (crossck on Linux saw
// it turn to all-ones between two sums with only a printf between; the same probe on
// xv6, natively on the host and in a guest, says whose fault it is.)
#include "kernel/types.h"
#include "user/user.h"

static uint32 vbuf[256], vout[256];
static void vload(void) { uint32 *p = vbuf; long n = 256;
  asm volatile("mv a0, %0\n\tmv a1, %1\n\t.word 0x0d3572d7\n\t.word 0x0205e407\n\t" : : "r"(n), "r"(p) : "a0", "a1", "t0", "memory"); }
static uint64
vsum(void)
{
  uint32 *p = vout; long n = 256; uint64 s = 0;
  memset(vout, 0, sizeof vout);
  asm volatile("mv a0, %0\n\tmv a3, %1\n\t.word 0x0d3572d7\n\t.word 0x0206e427\n\t" : : "r"(n), "r"(p) : "a0", "a3", "t0", "memory");
  for (int i = 0; i < 32; i++)
    s = s * 1000003 + vout[i];
  return s;
}
static const char *what(uint64 s) { if (s == 0xeb1f7cd4fdb731b0ULL) return "vbuf"; if (s == 0xbf2f6d657366fcc0ULL) return "ALL-ONES"; if (s == 0) return "zeros"; return "other"; }
int main(int argc, char *argv[])
{
  for (int i = 0; i < 256; i++)
    vbuf[i] = 0x1234567u * (uint32)(i + 1);
  vload();
  uint64 a = vsum();
  printf("vecprobe: after load %s\n", what(a));
  printf("vecprobe: summed again %s\n", what(vsum()));
  sleep(10);
  printf("vecprobe: after a sleep %s\n", what(vsum()));
  vload();
  int pid = fork();
  if (pid == 0) { exit(0); }
  wait(0);
  printf("vecprobe: after a fork and wait %s\n", what(vsum()));
  exit(0);
}
