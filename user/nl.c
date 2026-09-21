// nl, rev and expand: three small things done to each line.
//
//   nl [-b a|t] [FILE...]     number the lines
//   rev [FILE...]             each line backwards
//   expand [-t N] [FILE...]   tabs as spaces
#include "user/user.h"

static int number_all, tabstop = 8;

static void
do_nl(FILE *f)
{
  static long n = 0;
  char line[4096];

  while (fgets(line, sizeof(line), f)) {
    int len = strlen(line);
    int blank = len == 0 || (len == 1 && line[0] == '\n');
    if (blank && !number_all)
      printf("%s", line);
    else
      printf("%6ld\t%s", ++n, line);
  }
}

static void
do_rev(FILE *f)
{
  char line[4096];

  while (fgets(line, sizeof(line), f)) {
    int len = strlen(line);
    int nl = len > 0 && line[len - 1] == '\n';
    if (nl)
      line[--len] = 0;
    for (int i = len - 1; i >= 0; i--)
      putchar(line[i]);
    if (nl)
      putchar('\n');
  }
}

static void
do_expand(FILE *f)
{
  int c, col = 0;

  while ((c = fgetc(f)) != EOF) {
    if (c == '\t') {
      int to = ((col / tabstop) + 1) * tabstop;
      while (col < to) {
        putchar(' ');
        col++;
      }
    } else {
      putchar(c);
      col = c == '\n' ? 0 : col + 1;
    }
  }
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
      number_all = argv[++i][0] == 'a';
    else if (strncmp(argv[i], "-t", 2) == 0 && (argv[i][2] || i + 1 < argc))
      tabstop = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
    else if (argv[i][1] >= '0' && argv[i][1] <= '9')
      tabstop = atoi(argv[i] + 1);
    else {
      fprintf(stderr, "usage: %s [options] [FILE...]\n", me);
      exit(1);
    }
  }
  if (tabstop < 1)
    tabstop = 8;

  void (*fn)(FILE *) = strcmp(me, "rev") == 0      ? do_rev
                       : strcmp(me, "expand") == 0 ? do_expand
                                                   : do_nl;
  if (i >= argc) {
    fn(stdin);
    exit(0);
  }
  int status = 0;
  for (; i < argc; i++) {
    FILE *f = fopen(argv[i], "r");
    if (f == 0) {
      fprintf(stderr, "%s: %s: %s\n", me, argv[i], strerror(errno));
      status = 1;
      continue;
    }
    fn(f);
    fclose(f);
  }
  exit(status);
}
