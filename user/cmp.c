// cmp: whether two files are the same, and where they first differ.
//
//   cmp [-s] [-l] FILE1 [FILE2]
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int silent = 0, list = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-s") == 0)
      silent = 1;
    else if (strcmp(argv[i], "-l") == 0)
      list = 1;
    else {
      fprintf(stderr, "usage: cmp [-s] [-l] FILE1 [FILE2]\n");
      exit(2);
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: cmp [-s] [-l] FILE1 [FILE2]\n");
    exit(2);
  }
  const char *n1 = argv[i];
  const char *n2 = i + 1 < argc ? argv[i + 1] : "-";
  FILE *f1 = strcmp(n1, "-") == 0 ? stdin : fopen(n1, "r");
  FILE *f2 = strcmp(n2, "-") == 0 ? stdin : fopen(n2, "r");
  if (f1 == 0 || f2 == 0) {
    fprintf(stderr, "cmp: %s: %s\n", f1 == 0 ? n1 : n2, strerror(errno));
    exit(2);
  }

  long byte = 1, line = 1;
  int differed = 0;
  for (;;) {
    int c1 = fgetc(f1), c2 = fgetc(f2);
    if (c1 == EOF && c2 == EOF)
      break;
    if (c1 == EOF || c2 == EOF) {
      if (!silent)
        fprintf(stderr, "cmp: EOF on %s\n", c1 == EOF ? n1 : n2);
      exit(1);
    }
    if (c1 != c2) {
      differed = 1;
      if (silent)
        exit(1);
      if (list) {
        printf("%ld %o %o\n", byte, c1, c2);
      } else {
        printf("%s %s differ: byte %ld, line %ld\n", n1, n2, byte, line);
        exit(1);
      }
    }
    if (c1 == '\n')
      line++;
    byte++;
  }
  exit(differed ? 1 : 0);
}
