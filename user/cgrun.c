//
// cgrun: run a command in a scheduler group, so that everything it
// starts shares one claim on the processors rather than each process
// competing on its own.
//
//   cgrun [-w WEIGHT] [-m PCT] [-n NAME] command [args...]
//   cgrun -l                        what groups there are
//   cgrun -j ID command [args...]   join a group that already exists
//   cgrun -f ID on|off              freeze a group and let it go again
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

int sched_group(int op, int id, unsigned long a, unsigned long b);

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
  const char *name = 0;
  long weight = 0, pct = -1;
  int join = -1, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-l") == 0) {
      list();
      return 0;
    } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
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
