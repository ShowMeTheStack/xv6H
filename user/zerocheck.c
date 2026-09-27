// zerocheck: fresh memory from the kernel must be zero. Takes N pages with
// sbrk, reads every byte, writes a pattern (so that the pages are really
// ours) and says so. Under xv6H the kernel zeroes pages with cbo.zero at
// the cache-block size the machine is described with (F21): a guest that
// believes a longer block than the hardware has leaves every other block
// unzeroed, and this is what shows it.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char *argv[])
{
  int pages = argc > 1 ? atoi(argv[1]) : 512;
  unsigned char *p = sbrk(pages * 4096);
  if (p == (void *)-1) {
    printf("zerocheck: sbrk failed\n");
    return 1;
  }
  long dirty = 0, first = -1;
  for (long i = 0; i < (long)pages * 4096; i++)
    if (p[i]) {
      if (first < 0)
        first = i;
      dirty++;
    }
  for (long i = 0; i < (long)pages * 4096; i += 4096)
    p[i] = 0xa5;
  if (dirty) {
    printf("zerocheck: DIRTY: %ld bytes of %d pages not zero, first at page %ld offset %ld\n", dirty, pages,
           first / 4096, first % 4096);
    return 1;
  }
  printf("zerocheck: %d pages clean\n", pages);
  return 0;
}
