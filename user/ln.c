// ln: another name for a file, or a symbolic link to one.
//
//   ln TARGET NAME      a second name for the same file
//   ln -s TARGET NAME   a file whose contents are TARGET
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int
main(int argc, char *argv[])
{
  int sym = 0, i = 1;

  if (argc > 1 && strcmp(argv[1], "-s") == 0) {
    sym = 1;
    i = 2;
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: ln [-s] target name\n");
    return 1;
  }
  if (sym) {
    if (symlink(argv[i], argv[i + 1]) < 0) {
      fprintf(stderr, "ln: %s -> %s: %s\n", argv[i + 1], argv[i], strerror(errno));
      return 1;
    }
  } else if (link(argv[i], argv[i + 1]) < 0) {
    fprintf(stderr, "ln: %s %s: %s\n", argv[i], argv[i + 1], strerror(errno));
    return 1;
  }
  return 0;
}
