// yes: the same line, until something stops it.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char line[512];
  int n = 0;

  if (argc > 1) {
    for (int i = 1; i < argc; i++)
      n += snprintf(line + n, sizeof(line) - n, "%s%s", i > 1 ? " " : "", argv[i]);
  } else {
    n = snprintf(line, sizeof(line), "y");
  }
  snprintf(line + n, sizeof(line) - n, "\n");
  for (;;)
    if (fputs(line, stdout) < 0)
      exit(1);
}
