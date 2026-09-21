// strings: the runs of printable characters in a file.
//
//   strings [-n MIN] [FILE...]
#include "user/user.h"

static int minlen = 4;

static void
scan(FILE *f)
{
  char run[1024];
  int n = 0;
  int c;

  while ((c = fgetc(f)) != EOF) {
    if ((c >= 0x20 && c < 0x7f) || c == '\t') {
      if (n < (int)sizeof(run) - 1)
        run[n++] = (char)c;
      continue;
    }
    if (n >= minlen) {
      run[n] = 0;
      printf("%s\n", run);
    }
    n = 0;
  }
  if (n >= minlen) {
    run[n] = 0;
    printf("%s\n", run);
  }
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strncmp(argv[i], "-n", 2) == 0 && (argv[i][2] || i + 1 < argc))
      minlen = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
    else if (argv[i][1] >= '0' && argv[i][1] <= '9')
      minlen = atoi(argv[i] + 1);
    else {
      fprintf(stderr, "usage: strings [-n MIN] [FILE...]\n");
      exit(1);
    }
  }
  if (minlen < 1)
    minlen = 1;
  if (i >= argc) {
    scan(stdin);
    exit(0);
  }
  int status = 0;
  for (; i < argc; i++) {
    FILE *f = fopen(argv[i], "r");
    if (f == 0) {
      fprintf(stderr, "strings: %s: %s\n", argv[i], strerror(errno));
      status = 1;
      continue;
    }
    scan(f);
    fclose(f);
  }
  exit(status);
}
