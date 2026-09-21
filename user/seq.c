// seq: a run of numbers.
//
//   seq LAST | seq FIRST LAST | seq FIRST STEP LAST
//   -s SEP   -w (pad with zeros to the same width)
#include "user/user.h"

int
main(int argc, char *argv[])
{
  const char *sep = "\n";
  int pad = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] && !(argv[i][1] >= '0' && argv[i][1] <= '9');
       i++) {
    if (strncmp(argv[i], "-s", 2) == 0 && argv[i][2])
      sep = argv[i] + 2; // the attached form, -s,
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
      sep = argv[++i];
    else if (strcmp(argv[i], "-w") == 0)
      pad = 1;
    else {
      fprintf(stderr, "usage: seq [-w] [-s SEP] [FIRST [STEP]] LAST\n");
      exit(1);
    }
  }
  double first = 1, step = 1, last;
  int n = argc - i;
  if (n == 1)
    last = strtod(argv[i], 0);
  else if (n == 2) {
    first = strtod(argv[i], 0);
    last = strtod(argv[i + 1], 0);
  } else if (n == 3) {
    first = strtod(argv[i], 0);
    step = strtod(argv[i + 1], 0);
    last = strtod(argv[i + 2], 0);
  } else {
    fprintf(stderr, "usage: seq [-w] [-s SEP] [FIRST [STEP]] LAST\n");
    exit(1);
  }
  if (step == 0)
    exit(1);
  // the width every number is padded to, which -w asks for
  int width = 0;
  if (pad) {
    char buf[64];
    for (double v = first; step > 0 ? v <= last : v >= last; v += step) {
      int w = snprintf(buf, sizeof(buf), "%g", v);
      if (w > width)
        width = w;
    }
  }
  int first_out = 1;
  for (double v = first; step > 0 ? v <= last : v >= last; v += step) {
    if (!first_out)
      printf("%s", sep);
    if (pad)
      printf("%0*g", width, v);
    else
      printf("%g", v);
    first_out = 0;
  }
  if (!first_out)
    printf("\n");
  exit(0);
}
