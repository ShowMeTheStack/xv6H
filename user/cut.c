// cut: parts of each line.
//
//   cut -f LIST [-d DELIM] [-s] [FILE...]   fields
//   cut -c LIST [FILE...]                   characters
//
// LIST is comma-separated numbers and N-M ranges, as everywhere.
#include "user/user.h"

#define MAXRANGE 64

static struct {
  int from, to; // to == 0 means "to the end"
} ranges[MAXRANGE];
static int nranges;
static int by_field, only_delim;
static char delim = '\t';

static int
selected(int n)
{
  for (int i = 0; i < nranges; i++)
    if (n >= ranges[i].from && (ranges[i].to == 0 || n <= ranges[i].to))
      return 1;
  return 0;
}

static void
parse_list(const char *spec)
{
  const char *p = spec;

  while (*p && nranges < MAXRANGE) {
    int from = 0, to = 0;
    if (*p == '-') {
      from = 1;
    } else {
      while (*p >= '0' && *p <= '9')
        from = from * 10 + (*p++ - '0');
      to = from;
    }
    if (*p == '-') {
      p++;
      to = 0;
      while (*p >= '0' && *p <= '9')
        to = to * 10 + (*p++ - '0');
    }
    ranges[nranges].from = from;
    ranges[nranges].to = to;
    nranges++;
    if (*p == ',')
      p++;
    else
      break;
  }
}

static void
cut(FILE *f)
{
  char line[4096];

  while (fgets(line, sizeof(line), f)) {
    int len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
      line[--len] = 0;
    if (!by_field) {
      for (int i = 0; i < len; i++)
        if (selected(i + 1))
          putchar(line[i]);
      putchar('\n');
      continue;
    }
    if (strchr(line, delim) == 0) {
      // a line with no delimiter is one field: printed whole, unless
      // -s says to leave it out
      if (!only_delim)
        printf("%s\n", line);
      continue;
    }
    int field = 1, first = 1;
    char *p = line;
    for (;;) {
      char *d = strchr(p, delim);
      if (d)
        *d = 0;
      if (selected(field)) {
        printf("%s%s", first ? "" : (char[]){delim, 0}, p);
        first = 0;
      }
      if (d == 0)
        break;
      p = d + 1;
      field++;
    }
    printf("\n");
  }
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    // each of these takes its argument attached or separate, because
    // people write both and the difference is not worth an error
    if (strncmp(argv[i], "-f", 2) == 0 && (argv[i][2] || i + 1 < argc)) {
      by_field = 1;
      parse_list(argv[i][2] ? argv[i] + 2 : argv[++i]);
    } else if (strncmp(argv[i], "-c", 2) == 0 && (argv[i][2] || i + 1 < argc)) {
      by_field = 0;
      parse_list(argv[i][2] ? argv[i] + 2 : argv[++i]);
    } else if (strncmp(argv[i], "-d", 2) == 0 && (argv[i][2] || i + 1 < argc))
      delim = argv[i][2] ? argv[i][2] : argv[++i][0];
    else if (strcmp(argv[i], "-s") == 0)
      only_delim = 1;
    else {
      fprintf(stderr, "usage: cut -f LIST [-d DELIM] [-s] | -c LIST [FILE...]\n");
      exit(1);
    }
  }
  if (nranges == 0) {
    fprintf(stderr, "cut: -f or -c is needed\n");
    exit(1);
  }
  if (i >= argc) {
    cut(stdin);
    exit(0);
  }
  int status = 0;
  for (; i < argc; i++) {
    FILE *f = fopen(argv[i], "r");
    if (f == 0) {
      fprintf(stderr, "cut: %s: %s\n", argv[i], strerror(errno));
      status = 1;
      continue;
    }
    cut(f);
    fclose(f);
  }
  exit(status);
}
