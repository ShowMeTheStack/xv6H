// touch: make a file exist.
//
//   touch [-c] FILE...    -c: do not create it if it is not there
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int nocreate = 0, i = 1, status = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-c") == 0)
      nocreate = 1;
    else {
      fprintf(stderr, "usage: touch [-c] FILE...\n");
      exit(1);
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: touch [-c] FILE...\n");
    exit(1);
  }
  for (; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) == 0)
      continue; // it is there; the times are the file system's
    if (nocreate)
      continue;
    int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
    if (fd < 0) {
      fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
      status = 1;
      continue;
    }
    close(fd);
  }
  exit(status);
}
