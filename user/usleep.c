// usleep: wait a number of microseconds.
//
// the unit is the whole of the difference from sleep: this one takes
// an integer and it is microseconds, which is what every usleep has
// taken since it was a BSD library call. the clock underneath ticks a
// tenth of a second at a time, so a wait shorter than that is a wait
// until the next tick.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "usage: usleep MICROSECONDS\n");
    exit(1);
  }

  char *end;
  long long v = strtoll(argv[1], &end, 10);
  if (end == argv[1] || *end || v < 0) {
    fprintf(stderr, "usleep: %s: not a number of microseconds\n", argv[1]);
    exit(1);
  }

  struct timespec ts;
  ts.tv_sec = (time_t)(v / 1000000);
  ts.tv_nsec = (long)(v % 1000000) * 1000;
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
    ;
  exit(0);
}
