// mkdir [-p] DIRECTORY...: make directories, with -p making the path
// and saying nothing when it is already there.
#include "user/user.h"

static int
make_path(char *path)
{
  struct stat st;

  // make each directory on the way, ignoring the ones that exist
  for (char *p = path + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (stat(path, &st) < 0 && mkdir(path, 0777) < 0 && errno != EEXIST) {
      fprintf(stderr, "mkdir: %s: %s\n", path, strerror(errno));
      *p = '/';
      return -1;
    }
    *p = '/';
  }
  if (stat(path, &st) == 0)
    return 0;
  if (mkdir(path, 0777) < 0 && errno != EEXIST) {
    fprintf(stderr, "mkdir: %s: %s\n", path, strerror(errno));
    return -1;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  int i = 1, parents = 0, status = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      parents = 1;
    } else {
      fprintf(stderr, "usage: mkdir [-p] DIRECTORY...\n");
      exit(1);
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: mkdir [-p] DIRECTORY...\n");
    exit(1);
  }
  for (; i < argc; i++) {
    if (parents) {
      if (make_path(argv[i]) < 0)
        status = 1;
    } else if (mkdir(argv[i], 0777) < 0) {
      fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
      status = 1;
    }
  }
  exit(status);
}
