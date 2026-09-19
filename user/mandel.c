// mandel: draws the Mandelbrot set with double arithmetic and checks a
// checksum of the picture, so it doubles as a floating-point test.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define W 60
#define H 22

int
main(int argc, char *argv[])
{
  int quiet = argc > 1;
  long sum = 0;
  char line[W + 2];
  for (int r = 0; r < H; r++) {
    for (int c = 0; c < W; c++) {
      double cr = -2.2 + 3.0 * c / W, ci = -1.2 + 2.4 * r / H;
      double zr = 0, zi = 0;
      int n = 0;
      while (n < 50 && zr * zr + zi * zi < 4.0) {
        double t = zr * zr - zi * zi + cr;
        zi = 2 * zr * zi + ci;
        zr = t;
        n++;
      }
      line[c] = " .:-=+*#%@"[n >= 50 ? 9 : n / 6];
      sum = sum * 31 + n;
    }
    line[W] = '\n';
    line[W + 1] = 0;
    if (!quiet)
      write(1, line, W + 1);
  }
  // the checksum of this picture, computed with a native compiler with
  // floating-point contraction disabled (as this file is built).
  if (sum != -5929259368012162851L) {
    printf("mandel: checksum %ld differs\n", sum);
    exit(1);
  }
  printf("mandel: ok\n");
  exit(0);
}
