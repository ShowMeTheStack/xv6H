//
// schedbench: what this scheduler costs, and how fairly it shares.
//
//   schedbench [-t secs] [-c cpu] [-v] [test ...]
//
// tests:
//   msg       a byte back and forth through two pipes: round trips a second
//   wake      how long a write takes to reach a process blocked on a read
//   fair      three tasks of different nice spinning together: who gets what
//   null      getpid in a loop: what a trap into the kernel costs
//   spin      one task using everything it is given, and how much that was
//   fork      fork, exit, wait: how many a second
//   forkexec  and with an exec of -e PROGRAM (/bin/true) in between
//   ctx       two tasks handing the processor to each other: cost of a switch
//
// everything is pinned to one hart by default, because these numbers
// are about the scheduler and not about how many harts the machine
// has; `-c -1' leaves the placement to the scheduler, which is what
// to do when the placement is the thing being measured. The
// same program builds and runs on Linux, which is the point: the
// numbers only mean something next to somebody else's.
//
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/resource.h>

static int  bench_cpu = 0;
static int  verbose = 0;
static double secs = 3.0;
static const char *exec_path = "/bin/true";

static uint64_t
now_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// every task in a test runs on the same hart: a benchmark that lets
// the scheduler spread out measures the machine, not the scheduler.
static void
pin(void)
{
  cpu_set_t set;

  if (bench_cpu < 0)
    return;   // -c -1: leave it to the scheduler, which is the point
              // when what is being measured is where it puts things
  CPU_ZERO(&set);
  CPU_SET(bench_cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set) < 0 && verbose)
    fprintf(stderr, "schedbench: affinity: %s\n", strerror(errno));
}

static void
die(const char *what)
{
  fprintf(stderr, "schedbench: %s: %s\n", what, strerror(errno));
  exit(1);
}

static int
readn(int fd, void *buf, size_t n)
{
  char *p = buf;
  size_t got = 0;

  while (got < n) {
    int r = read(fd, p + got, n - got);
    if (r <= 0)
      return -1;
    got += r;
  }
  return 0;
}

// ---- message passing ----------------------------------------------
//
// a byte down one pipe and back up another, as often as it will go.
// Each round trip is two context switches and four system calls.

static void
bench_msg(void)
{
  int up[2], down[2];
  char c = 'x';

  if (pipe(up) < 0 || pipe(down) < 0)
    die("pipe");

  pid_t pid = fork();
  if (pid < 0)
    die("fork");
  if (pid == 0) {
    pin();
    close(down[1]);
    close(up[0]);
    for (;;) {
      if (readn(down[0], &c, 1) < 0)
        _exit(0);
      if (write(up[1], &c, 1) != 1)
        _exit(0);
    }
  }
  pin();
  close(down[0]);
  close(up[1]);

  // a warm-up, so the first round trip's page faults are not counted
  for (int i = 0; i < 100; i++) {
    if (write(down[1], &c, 1) != 1 || readn(up[0], &c, 1) < 0)
      die("warmup");
  }

  uint64_t t0 = now_ns(), end = t0 + (uint64_t)(secs * 1e9), n = 0;
  while (now_ns() < end) {
    for (int i = 0; i < 100; i++) {
      if (write(down[1], &c, 1) != 1 || readn(up[0], &c, 1) < 0)
        die("roundtrip");
    }
    n += 100;
  }
  uint64_t dt = now_ns() - t0;

  close(down[1]);
  close(up[0]);
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);

  printf("msg roundtrips %llu in %llu ns\n", (unsigned long long)n,
         (unsigned long long)dt);
  printf("msg roundtrip_ns %llu\n", (unsigned long long)(dt / (n ? n : 1)));
  printf("msg roundtrips_per_sec %llu\n",
         (unsigned long long)(n * 1000000000ULL / (dt ? dt : 1)));
}

// ---- wakeup latency ------------------------------------------------
//
// the child blocks on a read; the parent writes the time it wrote at.
// The child answers with how late the byte was. This is the number
// that decides whether an interactive program feels alive.

static int
cmp_u64(const void *a, const void *b)
{
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

  return x < y ? -1 : x > y ? 1 : 0;
}

static void
bench_wake(void)
{
  int down[2], up[2];
  enum { N = 2000 };
  uint64_t *lat = malloc(N * sizeof(uint64_t));

  if (lat == 0)
    die("malloc");
  if (pipe(down) < 0 || pipe(up) < 0)
    die("pipe");

  pid_t pid = fork();
  if (pid < 0)
    die("fork");
  if (pid == 0) {
    uint64_t t0;
    pin();
    close(down[1]);
    close(up[0]);
    for (;;) {
      if (readn(down[0], &t0, sizeof(t0)) < 0)
        _exit(0);
      uint64_t d = now_ns() - t0;
      if (write(up[1], &d, sizeof(d)) != (int)sizeof(d))
        _exit(0);
    }
  }
  pin();
  close(down[0]);
  close(up[1]);

  for (int i = 0; i < 200; i++) {   // warm up
    uint64_t t = now_ns(), d;
    if (write(down[1], &t, sizeof(t)) != (int)sizeof(t) ||
        readn(up[0], &d, sizeof(d)) < 0)
      die("warmup");
  }
  for (int i = 0; i < N; i++) {
    uint64_t t = now_ns(), d;
    if (write(down[1], &t, sizeof(t)) != (int)sizeof(t) ||
        readn(up[0], &d, sizeof(d)) < 0)
      die("wake");
    lat[i] = d;
  }
  close(down[1]);
  close(up[0]);
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);

  qsort(lat, N, sizeof(uint64_t), cmp_u64);
  uint64_t sum = 0;
  for (int i = 0; i < N; i++)
    sum += lat[i];
  printf("wake latency_ns_mean %llu\n", (unsigned long long)(sum / N));
  printf("wake latency_ns_p50 %llu\n", (unsigned long long)lat[N / 2]);
  printf("wake latency_ns_p90 %llu\n", (unsigned long long)lat[N * 9 / 10]);
  printf("wake latency_ns_p99 %llu\n", (unsigned long long)lat[N * 99 / 100]);
  printf("wake latency_ns_max %llu\n", (unsigned long long)lat[N - 1]);
  free(lat);
}

// ---- fairness ------------------------------------------------------
//
// three tasks that want the whole processor, at nice 6, 3 and 0. The
// weights behind those are 272, 526 and 1024 -- near enough 1:2:4 --
// and the share each one gets should follow them. This is the test
// that says whether the scheduler is fair at all.

static const int fair_nice[3] = {6, 3, 0};
static const double fair_weight[3] = {272.0, 526.0, 1024.0};

static uint64_t
spin_until(uint64_t end)
{
  volatile uint64_t x = 0;
  uint64_t n = 0;

  while (now_ns() < end) {
    for (int i = 0; i < 20000; i++)
      x = x + 1;
    n++;
  }
  return n;
}

static void
bench_fair(void)
{
  int fd[3][2], go[3][2];
  pid_t pid[3];
  uint64_t got[3], total = 0, deadline;

  // they start together and they stop together. Forked one after
  // another they would not start together, and the first one made
  // would be credited with the time the other two took to appear; and
  // if each timed its own three seconds, one that started late would
  // have the machine to itself at the end. So: every child waits for
  // the same absolute deadline, sent to it once they all exist.
  for (int i = 0; i < 3; i++) {
    if (pipe(fd[i]) < 0 || pipe(go[i]) < 0)
      die("pipe");
    if ((pid[i] = fork()) < 0)
      die("fork");
    if (pid[i] == 0) {
      uint64_t end;
      pin();
      close(fd[i][0]);
      close(go[i][1]);
      if (setpriority(PRIO_PROCESS, 0, fair_nice[i]) < 0 && verbose)
        fprintf(stderr, "schedbench: setpriority: %s\n", strerror(errno));
      if (readn(go[i][0], &end, sizeof(end)) < 0)
        _exit(1);
      uint64_t n = spin_until(end);
      if (write(fd[i][1], &n, sizeof(n)) != (int)sizeof(n))
        _exit(1);
      _exit(0);
    }
    close(fd[i][1]);
    close(go[i][0]);
  }
  deadline = now_ns() + (uint64_t)(secs * 1e9);
  for (int i = 0; i < 3; i++) {
    if (write(go[i][1], &deadline, sizeof(deadline)) != (int)sizeof(deadline))
      die("start");
    close(go[i][1]);
  }

  for (int i = 0; i < 3; i++) {
    if (readn(fd[i][0], &got[i], sizeof(got[i])) < 0)
      got[i] = 0;
    close(fd[i][0]);
    waitpid(pid[i], 0, 0);
    total += got[i];
  }

  double wsum = fair_weight[0] + fair_weight[1] + fair_weight[2];
  double worst = 0;
  for (int i = 0; i < 3; i++) {
    double want = fair_weight[i] / wsum;
    double have = total ? (double)got[i] / (double)total : 0;
    double err = want > 0 ? (have - want) / want * 100.0 : 0;
    if (err < 0)
      err = -err;
    if (err > worst)
      worst = err;
    printf("fair nice=%d weight=%d work=%llu share=%.4f expected=%.4f err=%.2f%%\n",
           fair_nice[i], (int)fair_weight[i], (unsigned long long)got[i],
           have, want, err);
  }
  printf("fair worst_err_pct %.2f\n", worst);
}

// ---- one task, all the processor it can get ------------------------
//
// nothing to do with the scheduler on its own; it is what a group or
// an affinity mask is pointed at when the question is how much of the
// machine something was given.

static void
bench_spin(void)
{
  uint64_t t0 = now_ns(), end = t0 + (uint64_t)(secs * 1e9);

  pin();
  uint64_t n = spin_until(end);
  uint64_t dt = now_ns() - t0;
  printf("spin work %llu in %llu ns\n", (unsigned long long)n,
         (unsigned long long)dt);
}

// ---- the cost of a trap ---------------------------------------------
//
// getpid, which does nothing. What it measures is the way in and the
// way out: the trap, the change of address space, and the return. Two
// of these are in a wakeup and about twenty are in an exec, so it is
// the number everything else here is built on.

static void
bench_null(void)
{
  uint64_t t0, dt, n = 0;

  pin();
  for (int i = 0; i < 1000; i++)
    getpid();
  t0 = now_ns();
  uint64_t end = t0 + (uint64_t)(secs * 1e9);
  while (now_ns() < end) {
    for (int i = 0; i < 1000; i++)
      getpid();
    n += 1000;
  }
  dt = now_ns() - t0;
  printf("null count %llu in %llu ns\n", (unsigned long long)n,
         (unsigned long long)dt);
  printf("null ns_each %llu\n", (unsigned long long)(dt / (n ? n : 1)));
  printf("null per_sec %llu\n",
         (unsigned long long)(n * 1000000000ULL / (dt ? dt : 1)));
}

// ---- fork, exec, exit ----------------------------------------------
//
// two numbers, because they answer different questions: `fork' is the
// address space being copied and torn down again, and `forkexec' adds
// what it costs to load a program -- which, for a program that is
// linked dynamically, is most of it.

static void
bench_fork(void)
{
  uint64_t t0 = now_ns(), end = t0 + (uint64_t)(secs * 1e9), n = 0;

  pin();
  while (now_ns() < end) {
    pid_t pid = fork();
    if (pid < 0)
      die("fork");
    if (pid == 0)
      _exit(0);
    waitpid(pid, 0, 0);
    n++;
  }
  uint64_t dt = now_ns() - t0;
  printf("fork count %llu in %llu ns\n", (unsigned long long)n,
         (unsigned long long)dt);
  printf("fork ns_each %llu\n", (unsigned long long)(dt / (n ? n : 1)));
  printf("fork per_sec %llu\n",
         (unsigned long long)(n * 1000000000ULL / (dt ? dt : 1)));
}

static void
bench_forkexec(void)
{
  uint64_t t0 = now_ns(), end = t0 + (uint64_t)(secs * 1e9), n = 0, bad = 0;

  pin();
  while (now_ns() < end) {
    int st = 0;
    pid_t pid = fork();
    if (pid < 0)
      die("fork");
    if (pid == 0) {
      char *av[2];
      av[0] = (char *)exec_path;
      av[1] = 0;
      execv(exec_path, av);
      _exit(127);
    }
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127)
      bad++;
    n++;
  }
  uint64_t dt = now_ns() - t0;
  if (bad)
    printf("forkexec failed_execs %llu (%s)\n", (unsigned long long)bad, exec_path);
  printf("forkexec program %s\n", exec_path);
  printf("forkexec count %llu in %llu ns\n", (unsigned long long)n,
         (unsigned long long)dt);
  printf("forkexec ns_each %llu\n", (unsigned long long)(dt / (n ? n : 1)));
  printf("forkexec per_sec %llu\n",
         (unsigned long long)(n * 1000000000ULL / (dt ? dt : 1)));
}

// ---- context switches ----------------------------------------------
//
// two tasks on one hart, each handing the processor straight back.

static void
bench_ctx(void)
{
  int fd[2];
  uint64_t child = 0;

  if (pipe(fd) < 0)
    die("pipe");
  uint64_t end = now_ns() + (uint64_t)(secs * 1e9);

  pid_t pid = fork();
  if (pid < 0)
    die("fork");
  if (pid == 0) {
    uint64_t n = 0;
    pin();
    close(fd[0]);
    while (now_ns() < end) {
      for (int i = 0; i < 100; i++)
        sched_yield();
      n += 100;
    }
    if (write(fd[1], &n, sizeof(n)) != (int)sizeof(n))
      _exit(1);
    _exit(0);
  }
  pin();
  close(fd[1]);

  uint64_t t0 = now_ns(), n = 0;
  while (now_ns() < end) {
    for (int i = 0; i < 100; i++)
      sched_yield();
    n += 100;
  }
  uint64_t dt = now_ns() - t0;
  if (readn(fd[0], &child, sizeof(child)) < 0)
    child = 0;
  close(fd[0]);
  waitpid(pid, 0, 0);

  uint64_t all = n + child;
  printf("ctx yields %llu in %llu ns\n", (unsigned long long)all,
         (unsigned long long)dt);
  printf("ctx ns_each %llu\n", (unsigned long long)(dt / (all ? all : 1)));
  printf("ctx per_sec %llu\n",
         (unsigned long long)(all * 1000000000ULL / (dt ? dt : 1)));
}

// --------------------------------------------------------------------

static void
usage(void)
{
  fprintf(stderr, "usage: schedbench [-t secs] [-c cpu|-1] [-e program] [-v]\n"
                  "                  [msg|wake|fair|null|spin|fork|forkexec|ctx|all]\n");
  exit(1);
}

int
main(int argc, char **argv)
{
  int i = 1, ran = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
      secs = atof(argv[++i]);
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
      bench_cpu = atoi(argv[++i]);
    else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc)
      exec_path = argv[++i];
    else if (strcmp(argv[i], "-v") == 0)
      verbose = 1;
    else
      usage();
  }
  if (secs <= 0)
    usage();

  for (; i < argc; i++) {
    ran = 1;
    if (strcmp(argv[i], "msg") == 0)
      bench_msg();
    else if (strcmp(argv[i], "wake") == 0)
      bench_wake();
    else if (strcmp(argv[i], "fair") == 0)
      bench_fair();
    else if (strcmp(argv[i], "spin") == 0)
      bench_spin();
    else if (strcmp(argv[i], "null") == 0)
      bench_null();
    else if (strcmp(argv[i], "fork") == 0)
      bench_fork();
    else if (strcmp(argv[i], "forkexec") == 0)
      bench_forkexec();
    else if (strcmp(argv[i], "ctx") == 0)
      bench_ctx();
    else if (strcmp(argv[i], "all") == 0) {
      bench_msg();
      bench_wake();
      bench_fair();
      bench_null();
      bench_fork();
      bench_forkexec();
      bench_ctx();
    } else
      usage();
  }
  if (!ran) {
    bench_msg();
    bench_wake();
    bench_fair();
    bench_null();
    bench_fork();
    bench_forkexec();
    bench_ctx();
  }
  return 0;
}
