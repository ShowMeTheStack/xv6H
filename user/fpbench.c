// fpbench: a floating-point workload (double matrix product, float
// accumulation, sqrt) checked against exact integer arithmetic.
// exits 0 on success. used by usertests and by hand.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N 48

static double a[N][N], b[N][N], c[N][N];
static long ia[N][N], ib[N][N];

int
main(int argc, char *argv[])
{
  int rounds = argc > 1 ? atoi(argv[1]) : 1;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < N; i++)
      for (int j = 0; j < N; j++) {
        ia[i][j] = (i * 7 + j * 3 + r) % 11;
        ib[i][j] = (i * 5 + j * 2 + r) % 13;
        a[i][j] = ia[i][j];
        b[i][j] = ib[i][j];
      }
    for (int i = 0; i < N; i++)
      for (int j = 0; j < N; j++) {
        double s = 0;
        for (int k = 0; k < N; k++)
          s += a[i][k] * b[k][j];
        c[i][j] = s;
      }
    for (int i = 0; i < N; i++)
      for (int j = 0; j < N; j++) {
        long s = 0;
        for (int k = 0; k < N; k++)
          s += ia[i][k] * ib[k][j];
        if ((long)c[i][j] != s) {
          printf("fpbench: mismatch at %d,%d: %ld vs %ld\n", i, j, (long)c[i][j], s);
          exit(1);
        }
      }
    float f = 0;
    for (int i = 0; i < 1024; i++)
      f += 0.5f;
    if ((int)f != 512) {
      printf("fpbench: float accumulation wrong\n");
      exit(1);
    }
    double q = __builtin_sqrt(1e10);
    if ((long)q != 100000) {
      printf("fpbench: sqrt wrong\n");
      exit(1);
    }
  }
  printf("fpbench: ok (%d round%s, %dx%d double matrix products)\n", rounds, rounds == 1 ? "" : "s", N, N);
  exit(0);
}
