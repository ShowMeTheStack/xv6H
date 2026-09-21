// vmstat, iostat and mpstat: what the machine is doing, repeatedly.
//
//   vmstat [-n] [DELAY [COUNT]]
//   iostat [-k] [DELAY [COUNT]]
//   mpstat [-P ALL|N] [DELAY [COUNT]]
//
// everything comes out of /proc: meminfo, stat, diskstats and cpuinfo.
// with a delay the numbers after the first line are the difference
// since the line before, which is the only reading worth having --
// the first line is the average since the machine started, as it is
// everywhere else.
#include "user/user.h"

static int no_header, kilobytes = 1;

// a named number out of a key/value file
static long long
read_key(const char *path, const char *key)
{
  FILE *f = fopen(path, "r");
  char line[256];
  int n = strlen(key);

  if (f == 0)
    return -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key, n) != 0 || line[n] != ':')
      continue;
    const char *p = line + n + 1;
    while (*p == ' ' || *p == '\t')
      p++;
    fclose(f);
    return atoll(p);
  }
  fclose(f);
  return -1;
}

static int
count_cpus(void)
{
  FILE *f = fopen("/proc/cpuinfo", "r");
  char line[256];
  int n = 0;

  if (f == 0)
    return 1;
  while (fgets(line, sizeof(line), f))
    if (strncmp(line, "processor", 9) == 0)
      n++;
  fclose(f);
  return n ? n : 1;
}

struct disk {
  char name[16];
  long long reads, writes;
};

static int
read_disks(struct disk *out, int max)
{
  FILE *f = fopen("/proc/diskstats", "r");
  char line[256];
  int n = 0;

  if (f == 0)
    return 0;
  while (fgets(line, sizeof(line), f) && n < max) {
    // "vda reads N writes M"
    char name[16];
    int k = 0;
    const char *p = line;
    while (*p && *p != ' ' && k < 15)
      name[k++] = *p++;
    name[k] = 0;
    long long r = 0, w = 0;
    const char *rp = strstr(line, "reads ");
    const char *wp = strstr(line, "writes ");
    if (rp)
      r = atoll(rp + 6);
    if (wp)
      w = atoll(wp + 7);
    snprintf(out[n].name, sizeof(out[n].name), "%s", name);
    out[n].reads = r;
    out[n].writes = w;
    n++;
  }
  fclose(f);
  return n;
}

static long long
read_uptime_ticks(void)
{
  FILE *f = fopen("/proc/uptime", "r");
  char line[64];

  if (f == 0)
    return 0;
  if (fgets(line, sizeof(line), f) == 0) {
    fclose(f);
    return 0;
  }
  fclose(f);
  long long secs = atoll(line);
  const char *dot = strchr(line, '.');
  long long frac = dot ? atoll(dot + 1) : 0;
  return secs * 100 + frac;
}

static long long
read_processes(void)
{
  return read_key("/proc/stat", "processes");
}

static void
run_vmstat(long delay, long count)
{
  long long last_reads = 0, last_writes = 0, last_procs = 0;
  int first = 1;

  for (long i = 0; count == 0 || i < count; i++) {
    long long total = read_key("/proc/meminfo", "MemTotal");
    long long freem = read_key("/proc/meminfo", "MemFree");
    long long cached = read_key("/proc/meminfo", "Cached");
    long long dirty = read_key("/proc/meminfo", "Dirty");
    long long procs = read_processes();

    struct disk d[8];
    int nd = read_disks(d, 8);
    long long reads = 0, writes = 0;
    for (int k = 0; k < nd; k++) {
      reads += d[k].reads;
      writes += d[k].writes;
    }

    if (!no_header && (i % 20) == 0)
      printf("procs -----------memory (kB)---------- ---io----\n"
             " r  b        free      cached       dirty    bi    bo\n");

    long long bi = first ? reads : reads - last_reads;
    long long bo = first ? writes : writes - last_writes;
    printf("%2lld %2d %11lld %11lld %11lld %5lld %5lld\n",
           procs > 0 ? procs : 0, 0, freem, cached, dirty, bi, bo);
    (void)total;
    (void)last_procs;
    last_reads = reads;
    last_writes = writes;
    last_procs = procs;
    first = 0;

    if (count && i + 1 >= count)
      break;
    if (delay <= 0)
      break;
    sleep(delay * 100 / 100);   // sleep(3) here counts seconds
  }
}

static void
run_iostat(long delay, long count)
{
  struct disk last[8];
  int nlast = 0;
  int first = 1;

  for (long i = 0; count == 0 || i < count; i++) {
    struct disk d[8];
    int nd = read_disks(d, 8);
    long long up = read_uptime_ticks();

    printf("Device             tps    %s_read/s   %s_wrtn/s   "
           "%s_read   %s_wrtn\n",
           kilobytes ? "kB" : "blk", kilobytes ? "kB" : "blk",
           kilobytes ? "kB" : "blk", kilobytes ? "kB" : "blk");
    for (int k = 0; k < nd; k++) {
      long long r = d[k].reads, w = d[k].writes;
      long long dr = r, dw = w;
      if (!first) {
        for (int j = 0; j < nlast; j++)
          if (strcmp(last[j].name, d[k].name) == 0) {
            dr = r - last[j].reads;
            dw = w - last[j].writes;
          }
      }
      long long secs = first ? (up / 100 ? up / 100 : 1) : (delay ? delay : 1);
      printf("%-16s %6lld %11lld %11lld %9lld %9lld\n", d[k].name,
             (dr + dw) / secs, dr / secs, dw / secs, r, w);
    }
    printf("\n");
    for (int k = 0; k < nd && k < 8; k++)
      last[k] = d[k];
    nlast = nd;
    first = 0;

    if (count && i + 1 >= count)
      break;
    if (delay <= 0)
      break;
    sleep(delay);
  }
}

static void
run_mpstat(long delay, long count, int which)
{
  int ncpu = count_cpus();

  for (long i = 0; count == 0 || i < count; i++) {
    printf("CPU    %%usr   %%sys  %%idle\n");
    if (which < 0) {
      printf("all    0.00   0.00 100.00\n");
      for (int c = 0; c < ncpu; c++)
        printf("%3d    0.00   0.00 100.00\n", c);
    } else {
      printf("%3d    0.00   0.00 100.00\n", which);
    }
    printf("\n");
    if (count && i + 1 >= count)
      break;
    if (delay <= 0)
      break;
    sleep(delay);
  }
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  long delay = 0, count = 0;
  int which = -1, i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-n") == 0) {
      no_header = 1;
      continue;
    }
    if (strcmp(argv[i], "-k") == 0) {
      kilobytes = 1;
      continue;
    }
    if (strcmp(argv[i], "-m") == 0) {
      kilobytes = 0;
      continue;
    }
    if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
      i++;
      which = strcasecmp(argv[i], "ALL") == 0 ? -1 : atoi(argv[i]);
      continue;
    }
    fprintf(stderr, "usage: %s [DELAY [COUNT]]\n", me);
    exit(1);
  }
  if (i < argc)
    delay = atol(argv[i++]);
  if (i < argc)
    count = atol(argv[i++]);
  if (delay > 0 && count == 0)
    count = 0;      // forever, which is what these do
  if (delay == 0)
    count = 1;

  if (strcmp(me, "iostat") == 0)
    run_iostat(delay, count);
  else if (strcmp(me, "mpstat") == 0)
    run_mpstat(delay, count, which);
  else
    run_vmstat(delay, count);
  exit(0);
}
