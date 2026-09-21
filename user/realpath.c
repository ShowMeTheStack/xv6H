// realpath: a path with no ".", ".." or symbolic links left in it.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int status = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: realpath PATH...\n");
    exit(1);
  }
  for (int i = 1; i < argc; i++) {
    char out[PATH_MAX];
    if (realpath(argv[i], out) == 0) {
      fprintf(stderr, "realpath: %s: %s\n", argv[i], strerror(errno));
      status = 1;
      continue;
    }
    printf("%s\n", out);
  }
  exit(status);
}
