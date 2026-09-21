// sleep: wait. the argument may have a fraction and a unit, as every
// modern sleep accepts: 0.5, 100ms, 2s, 1m.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: sleep SECONDS\n");
    exit(1);
  }
  for (int i = 1; i < argc; i++) {
    char *end;
    double v = strtod(argv[i], &end);
    if (end == argv[i]) {
      fprintf(stderr, "sleep: %s: not a number\n", argv[i]);
      exit(1);
    }
    if (strcmp(end, "ms") == 0)
      v /= 1000;
    else if (strcmp(end, "m") == 0)
      v *= 60;
    else if (strcmp(end, "h") == 0)
      v *= 3600;
    else if (*end && strcmp(end, "s") != 0) {
      fprintf(stderr, "sleep: %s: not a time\n", argv[i]);
      exit(1);
    }
    struct timespec ts;
    ts.tv_sec = (time_t)v;
    ts.tv_nsec = (long)((v - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
      ;
  }
  exit(0);
}
