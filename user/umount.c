// umount TARGET: take a file system off its directory.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "usage: umount TARGET\n");
    exit(1);
  }
  if (umount(argv[1]) < 0) {
    fprintf(stderr, "umount: %s: %s\n", argv[1], strerror(errno));
    exit(1);
  }
  exit(0);
}
