// date [-s]: the current UTC date and time from the RTC (-s: seconds since the epoch).
#include "kernel/types.h"
#include "kernel/time.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
    fprintf(2, "date: no real-time clock\n");
    exit(1);
  }
  if (argc > 1 && strcmp(argv[1], "-s") == 0) {
    printf("%ld\n", ts.sec);
  } else if (argc > 1) {
    fprintf(2, "usage: date [-s]\n");
    exit(1);
  } else {
    char buf[32];
    printf("%s\n", ctime_r(ts.sec, buf));
  }
  exit(0);
}
