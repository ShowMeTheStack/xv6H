// loopsig: print "ready" and wait to be signalled (for the terminal tests).
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  printf("ready\n");
  for (;;)
    ticksleep(10);
}
