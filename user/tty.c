// tty: the terminal this is attached to.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int quiet = 0;

  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "-s") == 0)
      quiet = 1;
  if (!isatty(0)) {
    if (!quiet)
      printf("not a tty\n");
    exit(1);
  }
  char *name = ttyname(0);
  if (!quiet)
    printf("%s\n", name ? name : "/dev/console");
  exit(0);
}
