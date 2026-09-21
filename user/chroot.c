// chroot: run a command with "/" somewhere else.
//
//   chroot DIR [COMMAND [ARG...]]
//
// with no command it runs a shell, which is what every chroot does.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: chroot DIR [COMMAND [ARG...]]\n");
    exit(1);
  }
  if (chroot(argv[1]) < 0) {
    fprintf(stderr, "chroot: %s: %s\n", argv[1], strerror(errno));
    exit(1);
  }
  if (chdir("/") < 0) {
    fprintf(stderr, "chroot: /: %s\n", strerror(errno));
    exit(1);
  }
  if (argc > 2) {
    execvp(argv[2], &argv[2]);
    fprintf(stderr, "chroot: %s: %s\n", argv[2], strerror(errno));
    exit(127);
  }
  char *sh[] = {"/bin/sh", 0};
  execv(sh[0], sh);
  fprintf(stderr, "chroot: /bin/sh: %s\n", strerror(errno));
  exit(127);
}
