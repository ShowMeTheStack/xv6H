//
// taskset: run a command on named harts, or read and set the harts an
// existing process may use.
//
//   taskset MASK command [args...]     run it there
//   taskset -p PID                     what it may use now
//   taskset -p MASK PID                change it
//
// MASK is hexadecimal, one bit per hart, the way util-linux writes it.
//
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int
parse_mask(const char *s, unsigned long *out)
{
  char *end;
  unsigned long v;

  errno = 0;
  v = strtoul(s, &end, 16);
  if (end == s || *end != 0 || errno != 0 || v == 0)
    return -1;
  *out = v;
  return 0;
}

static void
set_from_mask(cpu_set_t *set, unsigned long mask)
{
  CPU_ZERO(set);
  for (int i = 0; i < CPU_SETSIZE; i++)
    if ((mask >> i) & 1)
      CPU_SET(i, set);
}

static unsigned long
mask_from_set(const cpu_set_t *set)
{
  unsigned long m = 0;

  for (int i = 0; i < CPU_SETSIZE && i < 64; i++)
    if (CPU_ISSET(i, set))
      m |= 1UL << i;
  return m;
}

static void
usage(void)
{
  fprintf(stderr, "usage: taskset MASK command [args...]\n");
  fprintf(stderr, "       taskset -p [MASK] PID\n");
  exit(1);
}

int
main(int argc, char **argv)
{
  cpu_set_t set;
  unsigned long mask;

  if (argc < 2)
    usage();

  if (strcmp(argv[1], "-p") == 0) {
    if (argc == 3) {
      pid_t pid = atoi(argv[2]);
      if (sched_getaffinity(pid, sizeof(set), &set) < 0) {
        fprintf(stderr, "taskset: pid %d: %s\n", pid, strerror(errno));
        return 1;
      }
      printf("pid %d's current affinity mask: %lx\n", pid, mask_from_set(&set));
      return 0;
    }
    if (argc == 4) {
      pid_t pid = atoi(argv[3]);
      if (parse_mask(argv[2], &mask) < 0)
        usage();
      if (sched_getaffinity(pid, sizeof(set), &set) == 0)
        printf("pid %d's current affinity mask: %lx\n", pid, mask_from_set(&set));
      set_from_mask(&set, mask);
      if (sched_setaffinity(pid, sizeof(set), &set) < 0) {
        fprintf(stderr, "taskset: pid %d: %s\n", pid, strerror(errno));
        return 1;
      }
      printf("pid %d's new affinity mask: %lx\n", pid, mask);
      return 0;
    }
    usage();
  }

  if (argc < 3 || parse_mask(argv[1], &mask) < 0)
    usage();
  set_from_mask(&set, mask);
  if (sched_setaffinity(0, sizeof(set), &set) < 0) {
    fprintf(stderr, "taskset: %s\n", strerror(errno));
    return 1;
  }
  execvp(argv[2], &argv[2]);
  fprintf(stderr, "taskset: %s: %s\n", argv[2], strerror(errno));
  return 1;
}
