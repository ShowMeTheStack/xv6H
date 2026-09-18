// cp: copy a file (used to clone guest disk images).
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  static char buf[8192];
  if (argc != 3) {
    fprintf(2, "usage: cp src dst\n");
    exit(1);
  }
  int in = open(argv[1], O_RDONLY);
  if (in < 0) {
    fprintf(2, "cp: cannot open %s\n", argv[1]);
    exit(1);
  }
  int out = open(argv[2], O_WRONLY | O_CREATE | O_TRUNC);
  if (out < 0) {
    fprintf(2, "cp: cannot create %s\n", argv[2]);
    exit(1);
  }
  int n;
  long total = 0;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    if (write(out, buf, n) != n) {
      fprintf(2, "cp: write error after %ld bytes\n", total);
      exit(1);
    }
    total += n;
  }
  if (n < 0) {
    fprintf(2, "cp: read error\n");
    exit(1);
  }
  close(in);
  close(out);
  exit(0);
}
