// free: how much memory there is and how much is in use.
#include "user/user.h"

static long
field(const char *name)
{
  FILE *f = fopen("/proc/meminfo", "r");
  char line[256];
  long v = -1;

  if (f == 0)
    return -1;
  int n = strlen(name);
  while (fgets(line, sizeof(line), f))
    if (strncmp(line, name, n) == 0 && line[n] == ':') {
      v = atol(line + n + 1);
      break;
    }
  fclose(f);
  return v;
}

int
main(int argc, char *argv[])
{
  int human = 0, mega = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0)
      human = 1;
    else if (strcmp(argv[i], "-m") == 0)
      mega = 1;
    else if (strcmp(argv[i], "-k") == 0)
      mega = 0;
    else {
      fprintf(stderr, "usage: free [-h] [-m] [-k]\n");
      exit(1);
    }
  }
  long total = field("MemTotal");
  long freek = field("MemFree");
  long avail = field("MemAvailable");
  long cached = field("Cached");
  if (total < 0) {
    fprintf(stderr, "free: /proc/meminfo: %s\n", strerror(errno));
    exit(1);
  }
  long used = total - freek - (cached > 0 ? cached : 0);

  const char *unit = "kB";
  double d = 1;
  if (mega) {
    d = 1024;
    unit = "MB";
  } else if (human) {
    d = total >= 1024 * 1024 ? 1024 * 1024 : 1024;
    unit = total >= 1024 * 1024 ? "GB" : "MB";
  }
  printf("%-10s %10s %10s %10s %10s\n", "", "total", "used", "free", "available");
  printf("%-10s %9.0f%s %9.0f%s %9.0f%s %9.0f%s\n", "Mem:", total / d, unit, used / d, unit,
         (double)freek / d, unit, (avail < 0 ? 0 : avail) / d, unit);
  exit(0);
}
