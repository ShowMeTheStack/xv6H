// sort: lines in order.
//
//   sort [-n] [-r] [-u] [-f] [-k N] [-t SEP] [-o FILE] [FILE...]
#include "user/user.h"

#define MAXLINES 65536

static int numeric, reverse, unique, foldcase, keyfield;
static char sep;

// the part of a line the comparison looks at: the whole line, or one
// field of it when -k says so
static const char *
key_of(const char *line, char *buf, int max)
{
  if (keyfield <= 0)
    return line;
  int field = 1;
  const char *p = line;
  while (field < keyfield && *p) {
    if (sep) {
      while (*p && *p != sep)
        p++;
      if (*p == sep)
        p++;
    } else {
      while (*p == ' ' || *p == '\t')
        p++;
      while (*p && *p != ' ' && *p != '\t')
        p++;
    }
    field++;
  }
  if (sep == 0)
    while (*p == ' ' || *p == '\t')
      p++;
  int n = 0;
  while (*p && n < max - 1 && (sep ? *p != sep : (*p != ' ' && *p != '\t')))
    buf[n++] = *p++;
  buf[n] = 0;
  return buf;
}

static int
compare(const void *a, const void *b)
{
  const char *x = *(const char *const *)a;
  const char *y = *(const char *const *)b;
  char bx[256], by[256];
  const char *kx = key_of(x, bx, sizeof(bx));
  const char *ky = key_of(y, by, sizeof(by));
  int r;

  if (numeric) {
    double dx = strtod(kx, 0), dy = strtod(ky, 0);
    r = dx < dy ? -1 : dx > dy ? 1 : 0;
  } else if (foldcase) {
    r = strcasecmp(kx, ky);
  } else {
    r = strcmp(kx, ky);
  }
  if (r == 0 && kx != x)
    r = strcmp(x, y); // a stable-looking order when the keys are equal
  return reverse ? -r : r;
}

int
main(int argc, char *argv[])
{
  static char *lines[MAXLINES];
  int n = 0, i = 1;
  const char *outfile = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-n") == 0)
      numeric = 1;
    else if (strcmp(argv[i], "-r") == 0)
      reverse = 1;
    else if (strcmp(argv[i], "-u") == 0)
      unique = 1;
    else if (strcmp(argv[i], "-f") == 0)
      foldcase = 1;
    else if (strncmp(argv[i], "-k", 2) == 0 && (argv[i][2] || i + 1 < argc))
      keyfield = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
    else if (strncmp(argv[i], "-t", 2) == 0 && (argv[i][2] || i + 1 < argc))
      sep = argv[i][2] ? argv[i][2] : argv[++i][0];
    else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      outfile = argv[++i];
    else {
      fprintf(stderr, "usage: sort [-nrufl] [-k N] [-t SEP] [-o FILE] [FILE...]\n");
      exit(1);
    }
  }

  int nf = argc - i;
  for (int k = 0; k < (nf ? nf : 1); k++) {
    FILE *f = nf ? fopen(argv[i + k], "r") : stdin;
    if (f == 0) {
      fprintf(stderr, "sort: %s: %s\n", argv[i + k], strerror(errno));
      exit(1);
    }
    char line[4096];
    while (n < MAXLINES && fgets(line, sizeof(line), f)) {
      int len = strlen(line);
      if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = 0;
      lines[n++] = strdup(line);
    }
    if (nf)
      fclose(f);
  }
  qsort(lines, n, sizeof(lines[0]), compare);

  FILE *out = outfile ? fopen(outfile, "w") : stdout;
  if (out == 0) {
    fprintf(stderr, "sort: %s: %s\n", outfile, strerror(errno));
    exit(1);
  }
  for (int k = 0; k < n; k++) {
    if (unique && k > 0 && compare(&lines[k - 1], &lines[k]) == 0)
      continue;
    fprintf(out, "%s\n", lines[k]);
  }
  if (outfile)
    fclose(out);
  exit(0);
}
