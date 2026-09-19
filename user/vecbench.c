// vecbench: vector (V extension) workloads checked against scalar code:
// vector add, byte copy, and a sum reduction. exits 0 on success, and
// also 0 (with a message) when the hart has no vector unit.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/hwinfo.h"
#include "user/user.h"

#define N 4096

static int x[N], y[N], z[N];
static char s1[N], s2[N];

// z[i] = x[i] + y[i] with 32-bit elements
static void
vadd(int *dst, int *p, int *q, long n)
{
  while (n > 0) {
    long vl;
    asm volatile(".option push\n.option arch, +v\n"
                 "vsetvli %0, %1, e32, m4, ta, ma\n"
                 "vle32.v v0, (%2)\n"
                 "vle32.v v4, (%3)\n"
                 "vadd.vv v8, v0, v4\n"
                 "vse32.v v8, (%4)\n"
                 ".option pop\n"
                 : "=r"(vl) : "r"(n), "r"(p), "r"(q), "r"(dst) : "memory");
    p += vl;
    q += vl;
    dst += vl;
    n -= vl;
  }
}

static void
vcopy(char *dst, char *src, long n)
{
  while (n > 0) {
    long vl;
    asm volatile(".option push\n.option arch, +v\n"
                 "vsetvli %0, %1, e8, m8, ta, ma\n"
                 "vle8.v v0, (%2)\n"
                 "vse8.v v0, (%3)\n"
                 ".option pop\n"
                 : "=r"(vl) : "r"(n), "r"(src), "r"(dst) : "memory");
    src += vl;
    dst += vl;
    n -= vl;
  }
}

static long
vsum(int *p, long n)
{
  long total = 0;
  while (n > 0) {
    long vl, part;
    asm volatile(".option push\n.option arch, +v\n"
                 "vsetvli %0, %2, e32, m4, ta, ma\n"
                 "vle32.v v0, (%3)\n"
                 "vmv.s.x v8, zero\n"
                 "vredsum.vs v8, v0, v8\n"
                 "vmv.x.s %1, v8\n"
                 ".option pop\n"
                 : "=r"(vl), "=r"(part) : "r"(n), "r"(p) : "memory");
    total += (int)part;
    p += vl;
    n -= vl;
  }
  return total;
}

int
main(int argc, char *argv[])
{
  struct hwinfo hi;
  if (hwinfo(&hi) < 0 || !hi.has_v) {
    printf("vecbench: no vector extension on this hart\n");
    exit(0);
  }
  int rounds = argc > 1 ? atoi(argv[1]) : 1;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < N; i++) {
      x[i] = i * 3 + r;
      y[i] = 1000 - i;
      s1[i] = (char)(i * 7 + r);
      s2[i] = 0;
    }
    vadd(z, x, y, N);
    for (int i = 0; i < N; i++)
      if (z[i] != x[i] + y[i]) {
        printf("vecbench: vadd wrong at %d\n", i);
        exit(1);
      }
    vcopy(s2, s1, N - 3);
    for (int i = 0; i < N - 3; i++)
      if (s2[i] != s1[i]) {
        printf("vecbench: vcopy wrong at %d\n", i);
        exit(1);
      }
    long expect = 0;
    for (int i = 0; i < N; i++)
      expect += x[i];
    if (vsum(x, N) != expect) {
      printf("vecbench: reduction wrong\n");
      exit(1);
    }
  }
  printf("vecbench: ok (vlen %ld bits, %d round%s)\n", hi.vlenb * 8, rounds, rounds == 1 ? "" : "s");
  exit(0);
}
