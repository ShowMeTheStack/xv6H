// paste and fold: two ways of rearranging lines.
//
//   paste [-s] [-d LIST] [FILE...]   lines side by side
//   fold [-s] [-w WIDTH] [FILE...]   lines broken to a width
//
// paste reads one line from each file in turn and joins them with a
// separator; -s takes each file whole instead, one file per line.
// fold breaks a line that is too long, at a space with -s.
#include "user/user.h"

#define MAXFILES 64

static char delims[64] = "\t";
static int ndelims = 1;

static void
set_delims(const char *list)
{
  int n = 0;

  for (const char *p = list; *p && n < (int)sizeof(delims) - 1; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
      case 'n': delims[n++] = '\n'; break;
      case 't': delims[n++] = '\t'; break;
      case '0': delims[n++] = 0; break;
      case '\\': delims[n++] = '\\'; break;
      default: delims[n++] = *p;
      }
    } else {
      delims[n++] = *p;
    }
  }
  ndelims = n ? n : 1;
  if (n == 0)
    delims[0] = '\t';
}

// one line without its newline; 0 at end of file
static int
getline_nl(FILE *f, char *buf, int size)
{
  if (fgets(buf, size, f) == 0)
    return 0;
  int n = strlen(buf);
  if (n > 0 && buf[n - 1] == '\n')
    buf[n - 1] = 0;
  return 1;
}

static int
do_paste(int argc, char *argv[], int i, int serial)
{
  FILE *f[MAXFILES];
  int nf = 0, status = 0;
  static char line[8192];

  if (i >= argc) {
    f[nf++] = stdin;
  } else {
    for (; i < argc && nf < MAXFILES; i++) {
      f[nf] = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
      if (f[nf] == 0) {
        fprintf(stderr, "paste: %s: %s\n", argv[i], strerror(errno));
        status = 1;
        continue;
      }
      nf++;
    }
  }

  if (serial) {
    for (int k = 0; k < nf; k++) {
      int first = 1, d = 0;
      while (getline_nl(f[k], line, sizeof(line))) {
        if (!first)
          putchar(delims[d++ % ndelims]);
        printf("%s", line);
        first = 0;
      }
      if (!first)
        putchar('\n');
    }
  } else {
    int alive = nf;
    int open_file[MAXFILES];
    for (int k = 0; k < nf; k++)
      open_file[k] = 1;
    while (alive > 0) {
      int printed = 0;
      for (int k = 0; k < nf; k++) {
        if (k > 0)
          putchar(delims[(k - 1) % ndelims]);
        if (open_file[k] && getline_nl(f[k], line, sizeof(line))) {
          printf("%s", line);
          printed = 1;
        } else if (open_file[k]) {
          open_file[k] = 0;
          alive--;
        }
      }
      putchar('\n');
      if (!printed && alive == 0)
        break;
      if (alive == 0)
        break;
    }
  }
  for (int k = 0; k < nf; k++)
    if (f[k] != stdin)
      fclose(f[k]);
  return status;
}

static void
fold_stream(FILE *f, int width, int at_space)
{
  static char line[65536];

  while (fgets(line, sizeof(line), f)) {
    int n = strlen(line);
    int nl = n > 0 && line[n - 1] == '\n';
    if (nl)
      line[--n] = 0;
    int start = 0;
    while (n - start > width) {
      int cut = start + width;
      if (at_space) {
        int k = cut;
        while (k > start && line[k - 1] != ' ' && line[k - 1] != '\t')
          k--;
        if (k > start)
          cut = k;
      }
      printf("%.*s\n", cut - start, line + start);
      start = cut;
    }
    printf("%s", line + start);
    if (nl || start < n)
      putchar('\n');
  }
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int i = 1, serial = 0, width = 80;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  int is_fold = strcmp(me, "fold") == 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (!is_fold && argv[i][1] == 'd') {
      set_delims(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : ""));
      continue;
    }
    if (is_fold && argv[i][1] == 'w') {
      width = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "80"));
      if (width < 1)
        width = 1;
      continue;
    }
    if (is_fold && argv[i][1] >= '1' && argv[i][1] <= '9') {
      width = atoi(argv[i] + 1);
      continue;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      if (*p == 's')
        serial = 1;
      else if (is_fold && *p == 'b')
        ; // fold counts bytes, which is what it does anyway here
      else {
        fprintf(stderr, is_fold ? "usage: fold [-s] [-w WIDTH] [FILE...]\n"
                                : "usage: paste [-s] [-d LIST] [FILE...]\n");
        exit(1);
      }
    }
  }

  if (!is_fold)
    exit(do_paste(argc, argv, i, serial));

  int status = 0;
  if (i >= argc) {
    fold_stream(stdin, width, serial);
  } else {
    for (; i < argc; i++) {
      FILE *f = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
      if (f == 0) {
        fprintf(stderr, "fold: %s: %s\n", argv[i], strerror(errno));
        status = 1;
        continue;
      }
      fold_stream(f, width, serial);
      if (f != stdin)
        fclose(f);
    }
  }
  exit(status);
}
