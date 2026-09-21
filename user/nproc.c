// nproc: how many processors there are.
#include "user/user.h"

int
main(void)
{
  printf("%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
  exit(0);
}
