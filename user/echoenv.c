// echoenv NAME: print the value of an environment variable, and exit 1
// if it is not set.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "usage: echoenv NAME\n");
    exit(1);
  }
  char *v = getenv(argv[1]);
  if (v == 0)
    exit(1);
  printf("%s\n", v);
  exit(0);
}
