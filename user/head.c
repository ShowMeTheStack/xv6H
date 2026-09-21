// head: the first lines, or bytes, of each file.
//
//   head [-n LINES] [-c BYTES] [-q] [FILE...]
#include "user/user.h"

static int quiet, many;

static void
head(FILE *f, const char *name, long lines, long bytes)
{
  if (many && !quiet)
    printf("==> %s <==\n", name);
  if (bytes > 0) {
    int c;
    while (bytes-- > 0 && (c = fgetc(f)) != EOF)
      putchar(c);
    return;
  }
  char line[4096];
  while (lines-- > 0 && fgets(line, sizeof(line), f))
    fputs(line, stdout);
}

int
main(int argc, char *argv[])
{
  long lines = 10, bytes = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strncmp(argv[i], "-n", 2) == 0 && (argv[i][2] || i + 1 < argc))
      lines = atol(argv[i][2] ? argv[i] + 2 : argv[++i]);
    else if (strncmp(argv[i], "-c", 2) == 0 && (argv[i][2] || i + 1 < argc))
      bytes = atol(argv[i][2] ? argv[i] + 2 : argv[++i]);
    else if (strcmp(argv[i], "-q") == 0)
      quiet = 1;
    else if (argv[i][1] >= '0' && argv[i][1] <= '9')
      lines = atol(argv[i] + 1);
    else {
      fprintf(stderr, "usage: head [-n LINES] [-c BYTES] [FILE...]\n");
      exit(1);
    }
  }
  many = argc - i > 1;
  if (i >= argc) {
    head(stdin, "standard input", lines, bytes);
    exit(0);
  }
  int status = 0;
  for (int k = i; k < argc; k++) {
    FILE *f = fopen(argv[k], "r");
    if (f == 0) {
      fprintf(stderr, "head: %s: %s\n", argv[k], strerror(errno));
      status = 1;
      continue;
    }
    if (k > i && !quiet)
      printf("\n");
    head(f, argv[k], lines, bytes);
    fclose(f);
  }
  exit(status);
}
