//
// cgrun: run a command in a scheduler group, so that everything it
// starts shares one claim on the processors rather than each process
// competing on its own.
//
//   cgrun [-w WEIGHT] [-m PCT] [-n NAME] command [args...]
//   cgrun [--mem PAGES] [--pids N] [-n NAME] command [args...]
//   cgrun -l                        what groups there are
//   cgrun -j ID command [args...]   join a group that already exists
//   cgrun -f ID on|off              freeze a group and let it go again
//   cgrun --cgls                    the control groups and what they hold
//   cgrun --rm NAME                 take a control group away
//
// WEIGHT is on the same scale as a task's: 1024 is what one process of
// nice 0 gets, 2048 twice that. PCT is a ceiling, in hundredths of one
// processor over a tenth of a second, so -m 50 is half a processor
// however idle the machine is.
//
#include "kernel/types.h"
#include "kernel/schedgroup.h"
#include "user/user.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

int sched_group(int op, int id, unsigned long a, unsigned long b);

// ---- the control groups, which live in /sys/fs/cgroup ----

static int
cg_put(const char *path, const char *value)
{
  int fd = open(path, O_WRONLY);

  if (fd < 0)
    return -1;
  int n = write(fd, value, strlen(value));
  close(fd);
  return n > 0 ? 0 : -1;
}

static int
cg_get(const char *path, char *out, int max)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    return -1;
  int n = read(fd, out, max - 1);
  close(fd);
  if (n < 0)
    n = 0;
  out[n] = 0;
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
    out[--n] = 0;
  return 0;
}

// make it if it is not there; either way, this process joins it
static int
cg_use(const char *name, const char *mem, const char *pids, long weight,
       long pct)
{
  char path[128], num[32];

  snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/procs", name);
  int fd = open(path, O_WRONLY);
  if (fd >= 0)
    close(fd);
  else if (cg_put("/sys/fs/cgroup/cgroup.create", name) < 0)
    return -1;
  if (mem) {
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.max", name);
    if (cg_put(path, mem) < 0)
      return -1;
  }
  if (pids) {
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/pids.max", name);
    if (cg_put(path, pids) < 0)
      return -1;
  }
  if (weight > 0) {
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cpu.weight", name);
    snprintf(num, sizeof(num), "%ld", weight);
    if (cg_put(path, num) < 0)
      return -1;
  }
  if (pct >= 0) {
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cpu.max", name);
    snprintf(num, sizeof(num), "%ld", pct);
    if (cg_put(path, num) < 0)
      return -1;
  }
  snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/procs", name);
  snprintf(num, sizeof(num), "%d", getpid());
  return cg_put(path, num);
}

static void
cglist(void)
{
  DIR *d = opendir("/sys/fs/cgroup");
  struct dirent *e;

  if (d == 0) {
    fprintf(stderr, "cgrun: /sys/fs/cgroup: %s\n", strerror(errno));
    exit(1);
  }
  printf("%-16s %8s %8s %8s %8s %6s\n", "group", "mem", "mem.max", "pids",
         "pids.max", "frozen");
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.' || e->d_name[0] == 0)
      continue;
    char path[128], v[5][32];
    static const char *files[5] = {"memory.current", "memory.max",
                                   "pids.current", "pids.max", "freeze"};
    int ok = 1;
    for (int k = 0; k < 5; k++) {
      snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/%s", e->d_name, files[k]);
      if (cg_get(path, v[k], sizeof(v[k])) < 0)
        ok = 0;
    }
    if (!ok)
      continue;   // cgroup.create and cgroup.destroy are not groups
    printf("%-16s %8s %8s %8s %8s %6s\n", e->d_name, v[0], v[1], v[2], v[3],
           v[4]);
  }
  closedir(d);
  exit(0);
}

static void
usage(void)
{
  fprintf(stderr,
          "usage: cgrun [-n name] [-w weight] [-m pct] command [args...]\n"
          "       cgrun -j ID command [args...]\n"
          "       cgrun -f ID on|off\n"
          "       cgrun -l\n");
  exit(1);
}

static void
list(void)
{
  unsigned long info[8];

  printf("%3s %6s %8s %8s %9s %6s %8s %12s\n",
         "id", "tasks", "weight", "quota_us", "period_us", "frozen", "runnable",
         "usage_us");
  for (int id = 0; id < 64; id++) {
    if (sched_group(SGC_INFO, id, (unsigned long)info, 0) < 0)
      continue;
    printf("%3d %6lu %8lu %8lu %9lu %6lu %8lu %12lu\n", id, info[0], info[1],
           info[2], info[3], info[5], info[6], info[7]);
  }
}

int
main(int argc, char **argv)
{
  const char *name = 0, *mem = 0, *pids = 0;
  long weight = 0, pct = -1;
  int join = -1, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-l") == 0) {
      list();
      return 0;
    } else if (strcmp(argv[i], "--cgls") == 0) {
      cglist();
    } else if (strcmp(argv[i], "--rm") == 0 && i + 1 < argc) {
      if (cg_put("/sys/fs/cgroup/cgroup.destroy", argv[++i]) < 0) {
        fprintf(stderr, "cgrun: rm %s: %s\n", argv[i], strerror(errno));
        return 1;
      }
      return 0;
    } else if (strcmp(argv[i], "--mem") == 0 && i + 1 < argc)
      mem = argv[++i];
    else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc)
      pids = argv[++i];
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
      weight = atol(argv[++i]);
    else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
      pct = atol(argv[++i]);
    else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
      join = atoi(argv[++i]);
    else if (strcmp(argv[i], "-f") == 0 && i + 2 < argc) {
      int id = atoi(argv[i + 1]);
      int on = strcmp(argv[i + 2], "on") == 0;
      if (sched_group(SGC_FREEZE, id, on, 0) < 0) {
        fprintf(stderr, "cgrun: group %d: %s\n", id, strerror(errno));
        return 1;
      }
      printf("group %d %s\n", id, on ? "frozen" : "thawed");
      return 0;
    } else
      usage();
  }
  if (i >= argc)
    usage();

  if (mem || pids) {
    // the limits that are not the processor's live in a control group,
    // and so does the processor's share of one that has them
    if (cg_use(name ? name : argv[i], mem, pids, weight, pct) < 0) {
      fprintf(stderr, "cgrun: the group %s: %s\n", name ? name : argv[i],
              strerror(errno));
      return 1;
    }
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "cgrun: %s: %s\n", argv[i], strerror(errno));
    return 1;
  }

  int id = join;
  if (id < 0) {
    id = sched_group(SGC_CREATE, 0, (unsigned long)(name ? name : argv[i]), 0);
    if (id < 0) {
      fprintf(stderr, "cgrun: cannot make a group: %s\n", strerror(errno));
      return 1;
    }
    if (weight > 0 && sched_group(SGC_WEIGHT, id, weight, 0) < 0) {
      fprintf(stderr, "cgrun: weight %ld: %s\n", weight, strerror(errno));
      return 1;
    }
    if (pct >= 0 && sched_group(SGC_MAX, id, pct * 1000, 100000) < 0) {
      fprintf(stderr, "cgrun: ceiling %ld%%: %s\n", pct, strerror(errno));
      return 1;
    }
  }
  if (sched_group(SGC_JOIN, id, 0, 0) < 0) {
    fprintf(stderr, "cgrun: group %d: %s\n", id, strerror(errno));
    return 1;
  }
  execvp(argv[i], &argv[i]);
  fprintf(stderr, "cgrun: %s: %s\n", argv[i], strerror(errno));
  return 1;
}
