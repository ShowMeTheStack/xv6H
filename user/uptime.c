// uptime: how long this machine has been running.
#include "user/user.h"

int
main(void)
{
  FILE *f = fopen("/proc/uptime", "r");
  double up = 0, idle = 0;

  if (f == 0 || fscanf(f, "%lf %lf", &up, &idle) < 1) {
    fprintf(stderr, "uptime: /proc/uptime: %s\n", strerror(errno));
    exit(1);
  }
  fclose(f);

  time_t now = time(0);
  struct tm tm;
  localtime_r(&now, &tm);
  char when[32];
  strftime(when, sizeof(when), "%H:%M:%S", &tm);

  long total = (long)up;
  long days = total / 86400, hours = (total % 86400) / 3600, mins = (total % 3600) / 60;
  printf(" %s up ", when);
  if (days)
    printf("%ld day%s, ", days, days == 1 ? "" : "s");
  if (hours)
    printf("%ld:%02ld", hours, mins);
  else
    printf("%ld min", mins);
  printf("\n");
  exit(0);
}
