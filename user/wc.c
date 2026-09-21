// wc: how many lines, words and characters.
//
//   wc [-l] [-w] [-c] [-m] [FILE...]
//
// with no option it prints all three, as it always has; with options
// it prints only what was asked, in the order POSIX gives.
#include "user/user.h"

static int want_lines, want_words, want_chars, any;
static long total_l, total_w, total_c;
static int nfiles;

static void
report(long l, long w, long c, const char *name)
{
  int first = 1;

  if (want_lines || !any) {
    printf("%s%ld", first ? "" : " ", l);
    first = 0;
  }
  if (want_words || !any) {
    printf("%s%ld", first ? "" : " ", w);
    first = 0;
  }
  if (want_chars || !any) {
    printf("%s%ld", first ? "" : " ", c);
    first = 0;
  }
  if (name)
    printf(" %s", name);
  printf("\n");
}

static void
wc(int fd, const char *name)
{
  char buf[4096];
  long l = 0, w = 0, c = 0;
  int inword = 0, n;

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      c++;
      if (buf[i] == '\n')
        l++;
      if (strchr(" \r\t\n\v\f", buf[i]))
        inword = 0;
      else if (!inword) {
        w++;
        inword = 1;
      }
    }
  }
  if (n < 0) {
    fprintf(stderr, "wc: %s: %s\n", name ? name : "standard input", strerror(errno));
    exit(1);
  }
  total_l += l;
  total_w += w;
  total_c += c;
  report(l, w, c, name);
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *p = argv[i] + 1; *p; p++)
      switch (*p) {
      case 'l': want_lines = any = 1; break;
      case 'w': want_words = any = 1; break;
      case 'c': case 'm': want_chars = any = 1; break;
      default:
        fprintf(stderr, "usage: wc [-lwcm] [FILE...]\n");
        exit(1);
      }

  if (i >= argc) {
    wc(0, 0);
    exit(0);
  }
  int status = 0;
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
      status = 1;
      continue;
    }
    wc(fd, argv[i]);
    close(fd);
    nfiles++;
  }
  if (nfiles > 1)
    report(total_l, total_w, total_c, "total");
  exit(status);
}
