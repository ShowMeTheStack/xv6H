// date [-s]: the current UTC date and time from the RTC (-s: seconds since the epoch).
#include "kernel/types.h"
#include "kernel/time.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
    fprintf(stderr, "date: no real-time clock\n");
    exit(1);
  }
  if (argc > 1 && strcmp(argv[1], "-s") == 0) {
    printf("%ld\n", ts.tv_sec);
  } else if (argc > 1) {
    fprintf(stderr, "usage: date [-s]\n");
    exit(1);
  } else {
    char buf[64];
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm);
    printf("%s\n", buf);
  }
  exit(0);
}
