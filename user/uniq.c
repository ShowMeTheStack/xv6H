// uniq: collapse runs of equal lines.
//
//   uniq [-c] [-d] [-u] [-i] [INPUT [OUTPUT]]
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int count = 0, only_dup = 0, only_uniq = 0, fold = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *p = argv[i] + 1; *p; p++)
      switch (*p) {
      case 'c': count = 1; break;
      case 'd': only_dup = 1; break;
      case 'u': only_uniq = 1; break;
      case 'i': fold = 1; break;
      default:
        fprintf(stderr, "usage: uniq [-cdui] [INPUT [OUTPUT]]\n");
        exit(1);
      }
  }
  FILE *in = stdin, *out = stdout;
  if (i < argc && (in = fopen(argv[i], "r")) == 0) {
    fprintf(stderr, "uniq: %s: %s\n", argv[i], strerror(errno));
    exit(1);
  }
  if (i + 1 < argc && (out = fopen(argv[i + 1], "w")) == 0) {
    fprintf(stderr, "uniq: %s: %s\n", argv[i + 1], strerror(errno));
    exit(1);
  }

  char cur[4096], prev[4096];
  long n = 0;
  int have = 0;
  while (fgets(cur, sizeof(cur), in)) {
    int len = strlen(cur);
    if (len > 0 && cur[len - 1] == '\n')
      cur[len - 1] = 0;
    int same = have && (fold ? strcasecmp(cur, prev) == 0 : strcmp(cur, prev) == 0);
    if (same) {
      n++;
      continue;
    }
    if (have && !(only_dup && n == 1) && !(only_uniq && n > 1)) {
      if (count)
        fprintf(out, "%7ld %s\n", n, prev);
      else
        fprintf(out, "%s\n", prev);
    }
    snprintf(prev, sizeof(prev), "%s", cur);
    n = 1;
    have = 1;
  }
  if (have && !(only_dup && n == 1) && !(only_uniq && n > 1)) {
    if (count)
      fprintf(out, "%7ld %s\n", n, prev);
    else
      fprintf(out, "%s\n", prev);
  }
  exit(0);
}
