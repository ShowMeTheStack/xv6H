// rmdir DIR...: remove a directory, which must be empty.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int status = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: rmdir DIR...\n");
    exit(1);
  }
  for (int i = 1; i < argc; i++)
    if (rmdir(argv[i]) < 0) {
      fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno));
      status = 1;
    }
  exit(status);
}
