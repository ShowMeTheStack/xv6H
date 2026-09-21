//
// mkpasswd: the hash of a password, in the form /etc/shadow holds.
//
// this runs on the build machine, so that an image can be made with
// the root password the build was given rather than with none. it
// includes the same pbkdf2 the C library uses, so what it writes is
// exactly what the kernel's login will compute and compare.
//
//   mkpasswd [-r ROUNDS] [-s SALT] PASSWORD
//
// without -s the salt comes from the host's /dev/urandom; with it the
// image is reproducible, which is what a build that must produce the
// same bytes twice needs.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/libc/pbkdf2.h"

int
main(int argc, char *argv[])
{
  int rounds = 100000;
  const char *fixed = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
      rounds = atoi(argv[++i]);
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
      fixed = argv[++i];
    else {
      fprintf(stderr, "usage: mkpasswd [-r ROUNDS] [-s SALT] PASSWORD\n");
      return 1;
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: mkpasswd [-r ROUNDS] [-s SALT] PASSWORD\n");
    return 1;
  }

  char saltb64[48];
  if (fixed) {
    snprintf(saltb64, sizeof(saltb64), "%s", fixed);
  } else {
    unsigned char salt[12];
    FILE *r = fopen("/dev/urandom", "rb");
    if (r == 0 || fread(salt, 1, sizeof(salt), r) != sizeof(salt)) {
      fprintf(stderr, "mkpasswd: cannot read /dev/urandom\n");
      return 1;
    }
    fclose(r);
    b64_encode(salt, sizeof(salt), saltb64);
  }

  char setting[128];
  snprintf(setting, sizeof(setting), PBKDF2_PREFIX "%d$%s$", rounds, saltb64);
  char *h = crypt(argv[i], setting);
  if (h == 0) {
    fprintf(stderr, "mkpasswd: bad setting\n");
    return 1;
  }
  printf("%s\n", h);
  return 0;
}
