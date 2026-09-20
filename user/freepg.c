// freepg: the number of free physical pages (and the total).
#include "kernel/types.h"
#include "kernel/sysconf.h"
#include "user/user.h"

int
main(void)
{
  printf("free %ld total %ld cache %ld\n", sysconf(_SC_AVPHYS_PAGES), sysconf(_SC_PHYS_PAGES),
         sysconf(_SC_XV6_CACHEPAGES));
  exit(0);
}
