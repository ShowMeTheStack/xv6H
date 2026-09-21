// comm: what two sorted files have in common, and what they do not.
//
//   comm [-1] [-2] [-3] FILE1 FILE2
//
// three columns: lines only in the first, lines only in the second,
// and lines in both. -1, -2 and -3 each drop one of them.
#include "user/user.h"

static int drop1, drop2, drop3;

static void
show(int column, const char *line)
{
  if (column == 1 && drop1)
    return;
  if (column == 2 && drop2)
    return;
  if (column == 3 && drop3)
    return;
  // a column is indented past the ones before it that are still shown
  if (column >= 2 && !drop1)
    putchar('\t');
  if (column == 3 && !drop2)
    putchar('\t');
  printf("%s\n", line);
}

// read one line without its newline; 0 at end of file
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

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (const char *p = argv[i] + 1; *p; p++) {
      if (*p == '1') drop1 = 1;
      else if (*p == '2') drop2 = 1;
      else if (*p == '3') drop3 = 1;
      else {
        fprintf(stderr, "usage: comm [-123] FILE1 FILE2\n");
        exit(1);
      }
    }
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: comm [-123] FILE1 FILE2\n");
    exit(1);
  }

  FILE *a = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
  FILE *b = strcmp(argv[i + 1], "-") == 0 ? stdin : fopen(argv[i + 1], "r");
  if (a == 0 || b == 0) {
    fprintf(stderr, "comm: %s: %s\n", a == 0 ? argv[i] : argv[i + 1],
            strerror(errno));
    exit(1);
  }

  static char la[4096], lb[4096];
  int have_a = getline_nl(a, la, sizeof(la));
  int have_b = getline_nl(b, lb, sizeof(lb));

  while (have_a || have_b) {
    if (!have_b) {
      show(1, la);
      have_a = getline_nl(a, la, sizeof(la));
    } else if (!have_a) {
      show(2, lb);
      have_b = getline_nl(b, lb, sizeof(lb));
    } else {
      int c = strcmp(la, lb);
      if (c < 0) {
        show(1, la);
        have_a = getline_nl(a, la, sizeof(la));
      } else if (c > 0) {
        show(2, lb);
        have_b = getline_nl(b, lb, sizeof(lb));
      } else {
        show(3, la);
        have_a = getline_nl(a, la, sizeof(la));
        have_b = getline_nl(b, lb, sizeof(lb));
      }
    }
  }
  exit(0);
}
