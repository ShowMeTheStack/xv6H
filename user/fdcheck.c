// fdcheck FD: exit 0 if that descriptor is closed, 1 if it is open.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  if (argc != 2)
    exit(2);
  int fd = atoi(argv[1]);
  struct stat st;
  exit(fstat(fd, &st) < 0 ? 0 : 1);
}
