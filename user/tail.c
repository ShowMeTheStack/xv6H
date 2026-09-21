// tail: the last lines of each file, and with -f whatever is added.
//
//   tail [-n LINES] [-c BYTES] [-f] [-q] [FILE...]
#include "user/user.h"

#define MAXKEEP 1024

static int quiet, many, follow;

// -n +N and -c +N count from the start of the file rather than from
// its end: "everything from line N on", which is how a header is
// skipped and how a diff's body is taken without its two name lines.
static int from_start;

static void
tail(FILE *f, const char *name, long lines, long bytes)
{
  if (many && !quiet)
    printf("==> %s <==\n", name);

  if (from_start) {
    if (bytes > 0) {
      // skip bytes - 1 bytes, then print the rest
      for (long i = 1; i < bytes; i++)
        if (fgetc(f) == EOF)
          return;
      int c;
      while ((c = fgetc(f)) != EOF)
        putchar(c);
      return;
    }
    char line[4096];
    long at = 0;
    while (fgets(line, sizeof(line), f)) {
      at++;
      if (at >= lines)
        fputs(line, stdout);
    }
    return;
  }

  if (bytes > 0) {
    // the last `bytes` bytes: seek if we can, read through if not
    if (fseek(f, -bytes, SEEK_END) == 0) {
      int c;
      while ((c = fgetc(f)) != EOF)
        putchar(c);
      return;
    }
  }
  // a ring of the last `lines` lines, so a file of any size costs the
  // same memory
  static char *keep[MAXKEEP];
  long n = lines > MAXKEEP ? MAXKEEP : lines;
  long count = 0;
  char line[4096];

  for (long i = 0; i < n; i++) {
    free(keep[i]);
    keep[i] = 0;
  }
  while (fgets(line, sizeof(line), f)) {
    long slot = count % n;
    free(keep[slot]);
    keep[slot] = strdup(line);
    count++;
  }
  long first = count > n ? count - n : 0;
  for (long i = first; i < count; i++)
    if (keep[i % n])
      fputs(keep[i % n], stdout);
}

int
main(int argc, char *argv[])
{
  long lines = 10, bytes = 0;
  int i = 1;

  for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
    if (strncmp(argv[i], "-n", 2) == 0 && (argv[i][2] || i + 1 < argc)) {
      const char *v = argv[i][2] ? argv[i] + 2 : argv[++i];
      from_start = (*v == '+');
      lines = atol(from_start ? v + 1 : v);
      if (from_start && lines < 1)
        lines = 1;
    } else if (strncmp(argv[i], "-c", 2) == 0 && (argv[i][2] || i + 1 < argc)) {
      const char *v = argv[i][2] ? argv[i] + 2 : argv[++i];
      from_start = (*v == '+');
      bytes = atol(from_start ? v + 1 : v);
      if (from_start && bytes < 1)
        bytes = 1;
    }
    else if (strcmp(argv[i], "-f") == 0)
      follow = 1;
    else if (strcmp(argv[i], "-q") == 0)
      quiet = 1;
    else if (argv[i][1] >= '0' && argv[i][1] <= '9')
      lines = atol(argv[i] + 1);
    else if (argv[i][0] == '+' && argv[i][1] >= '0' && argv[i][1] <= '9') {
      from_start = 1;
      lines = atol(argv[i] + 1);
    }
    else {
      fprintf(stderr, "usage: tail [-n LINES] [-c BYTES] [-f] [FILE...]\n");
      exit(1);
    }
  }
  many = argc - i > 1;
  if (i >= argc) {
    tail(stdin, "standard input", lines, bytes);
    exit(0);
  }
  int status = 0;
  for (int k = i; k < argc; k++) {
    FILE *f = fopen(argv[k], "r");
    if (f == 0) {
      fprintf(stderr, "tail: %s: %s\n", argv[k], strerror(errno));
      status = 1;
      continue;
    }
    if (k > i && !quiet)
      printf("\n");
    tail(f, argv[k], lines, bytes);
    // -f on the last file: whatever is appended, as it arrives
    if (follow && k == argc - 1) {
      fflush(stdout);
      for (;;) {
        char buf[1024];
        clearerr(f);
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
          fwrite(buf, 1, n, stdout);
          fflush(stdout);
        } else {
          struct timespec ts = {0, 200 * 1000 * 1000};
          nanosleep(&ts, 0);
        }
      }
    }
    fclose(f);
  }
  exit(status);
}
