// hostname: the name of this machine, printed or set.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char buf[64];

  if (argc > 1) {
    if (sethostname(argv[1], strlen(argv[1])) < 0) {
      fprintf(stderr, "hostname: %s\n", strerror(errno));
      exit(1);
    }
    exit(0);
  }
  if (gethostname(buf, sizeof(buf)) < 0) {
    fprintf(stderr, "hostname: %s\n", strerror(errno));
    exit(1);
  }
  printf("%s\n", buf);
  exit(0);
}
