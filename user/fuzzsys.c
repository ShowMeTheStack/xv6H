//
// fuzzsys: what a container may ask the kernel for, and what happens
// when it asks for everything.
//
// Two jobs, and they are the same job from two sides.
//
//   fuzzsys check          every call the container profile denies is
//                          denied, and ordinary work still works
//   fuzzsys badpid         the calls that take a process number, given
//                          numbers no process can have
//   fuzzsys fuzz N [SEED]  N random system calls with random arguments,
//                          each in a child so that a call which ends a
//                          process ends only that one
//
// `check' expects to be run inside a container -- that is where the
// filter is on -- and reports what it found. `fuzz' does not care where
// it is run: it installs the container profile on itself first, so the
// calls that could take the machine down are already gone before it
// starts guessing at arguments. That is the same trade fuzzguest makes
// against the hypervisor: the thing under test is the kernel's argument
// handling, not the caller's good manners.
//
// The seed is printed, so a run that finds something can be run again.
//
#include "kernel/types.h"
#include "kernel/syscall.h"
#include "kernel/sysfilter.h"
#include "user/user.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

// the raw door. The library's stubs turn a negative return into -1 and
// errno, which is one translation too many when the question is exactly
// what number the kernel returned; and there is no stub at all for a
// call this program is not supposed to be able to make.
static long
raw(long num, long a0, long a1, long a2, long a3)
{
  register long x10 asm("a0") = a0;
  register long x11 asm("a1") = a1;
  register long x12 asm("a2") = a2;
  register long x13 asm("a3") = a3;
  register long x17 asm("a7") = num;
  asm volatile("ecall"
               : "+r"(x10)
               : "r"(x11), "r"(x12), "r"(x13), "r"(x17)
               : "memory");
  return x10;
}

// the calls the container profile gives up, and what to call them in a
// report. This list is the kernel's (kernel/sysfilter.c); keeping a copy
// here is the point -- if the two ever disagree, this program says so.
struct call {
  const char *name;
  int num;
};

static const struct call denied[] = {
    {"vm_create", SYS_vm_create},     {"vm_ctl", SYS_vm_ctl},
    {"vm_run", SYS_vm_run},           {"vm_destroy", SYS_vm_destroy},
    {"vm_cons", SYS_vm_cons},         {"vm_info", SYS_vm_info},
    {"vm_stats", SYS_vm_stats},       {"vm_open", SYS_vm_open},
    {"netroute", SYS_netroute},       {"netgw", SYS_netgw},
    {"pivot_root", SYS_pivot_root},   {"chroot", SYS_chroot},
    {"setns", SYS_setns},             {"reboot", SYS_reboot},
    {"sethostname", SYS_sethostname}, {"ptrace", SYS_ptrace},
    {"consraw", SYS_consraw},         {"sched_group", SYS_sched_group},
};

// ---- check: the denials, and then that the floor is still there ----

static int
check(void)
{
  int bad = 0;

  // the gate. Outside a container these calls do what they say: this
  // program asked the machine to power off the first time it was run on
  // one. So one harmless member of the list is tried first -- vm_info
  // with a machine that cannot exist and nowhere to put the answer --
  // and if that comes back anything but a refusal, the filter is not on
  // here and nothing else is tried.
  long gate = raw(SYS_vm_info, -1, 0, 0, 0);
  if (gate != -EPERM) {
    printf("fuzzsys: the filter is not on here (vm_info %ld)\n", gate);
    printf("fuzzsys: check FAILED\n");
    return 1;
  }

  for (int i = 0; i < (int)(sizeof(denied) / sizeof(denied[0])); i++) {
    // every argument is zero: a call that is refused is refused before
    // it looks at them, and a call that is not refused must not be
    // handed anything it could act on.
    long r = raw(denied[i].num, 0, 0, 0, 0);
    if (r != -EPERM) {
      printf("fuzzsys: %s returned %ld, wanted -EPERM\n", denied[i].name, r);
      bad++;
    }
  }
  if (bad == 0)
    printf("fuzzsys: %d calls refused\n",
           (int)(sizeof(denied) / sizeof(denied[0])));

  // and what a program actually does is untouched. A filter that stopped
  // this would be a filter nothing could run under.
  int fd = open("/tmp/fuzzsys.tmp", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0) {
    printf("fuzzsys: open: %s\n", strerror(errno));
    bad++;
  } else {
    char buf[8] = "ok";
    if (write(fd, buf, 2) != 2)
      printf("fuzzsys: write: %s\n", strerror(errno)), bad++;
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 2) != 2 || strcmp(buf, "ok") != 0)
      printf("fuzzsys: read back: '%s'\n", buf), bad++;
    close(fd);
    unlink("/tmp/fuzzsys.tmp");
  }
  int pid = fork();
  if (pid == 0)
    exit(7);
  if (pid < 0) {
    printf("fuzzsys: fork: %s\n", strerror(errno));
    bad++;
  } else {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
      ;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 7) {
      printf("fuzzsys: child %d status 0x%x\n", pid, st);
      bad++;
    }
  }
  if (getpid() <= 0 || uptime() < 0) {
    printf("fuzzsys: getpid/uptime\n");
    bad++;
  }
  // a child of a filtered process is filtered: what a parent gave up is
  // not something a fork can get back.
  pid = fork();
  if (pid == 0)
    exit(raw(SYS_reboot, 0, 0, 0, 0) == -EPERM ? 3 : 1);
  if (pid > 0) {
    // 3, not 0: a wait that fails leaves the status word as it was, and
    // a status of zero must not be able to read as a pass
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
      ;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 3) {
      printf("fuzzsys: a child was not filtered\n");
      bad++;
    } else {
      printf("fuzzsys: a child is filtered too\n");
    }
  }
  printf(bad ? "fuzzsys: check FAILED\n" : "fuzzsys: check ok\n");
  return bad ? 1 : 0;
}

// ---- the numbers nobody can have ----
//
// a process number is an int, and the calls that take one are handed
// whatever the caller says. The table they look in is indexed by the
// number: C's remainder keeps the sign of its left operand, so a
// negative one indexed in front of the table and the pointer found there
// was followed. getsid(-1) was a kernel panic from any process at all.
static int
badpid(void)
{
  static const int nums[] = {-1, -2, -4096, -4097, -123456, -2147483647 - 1,
                             2147483647, 999999};
  static const struct call takers[] = {
      {"getsid", SYS_getsid},       {"getpgid", SYS_getpgid},
      {"setpgid", SYS_setpgid},     {"kill", SYS_kill},
      {"ptrace", SYS_ptrace},       {"setpriority", SYS_setpriority},
      {"getpriority", SYS_getpriority},
      {"sched_getaffinity", SYS_sched_getaffinity},
      {"sched_setaffinity", SYS_sched_setaffinity},
      {"tkill", SYS_tkill},         {"tgkill", SYS_tgkill},
      {"wait4", SYS_wait4},
  };

  for (int i = 0; i < (int)(sizeof(takers) / sizeof(takers[0])); i++)
    for (int j = 0; j < (int)(sizeof(nums) / sizeof(nums[0])); j++) {
      long r = raw(takers[i].num, nums[j], 0, 0, 0);
      if (r >= 0 && takers[i].num != SYS_kill && takers[i].num != SYS_tkill &&
          takers[i].num != SYS_tgkill && takers[i].num != SYS_wait4)
        printf("fuzzsys: %s(%d) = %ld?\n", takers[i].name, nums[j], r);
    }
  // and this process is still here to say so, which is the whole test:
  // the answers do not matter, the machine being alive does
  printf("fuzzsys: badpid ok (%d)\n", getpid());
  return 0;
}

// ---- fuzz: every call, with arguments nobody meant ----

static uint64 seed;

static uint64
rnd(void)
{
  // xorshift64: small, and the same sequence for the same seed on any
  // machine, which is what makes a failure reproducible
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}

// the calls a fuzzer must not make, not because they are dangerous to
// the kernel but because they end or stop the caller before it can
// report anything. The child below makes exactly one call, so a call
// that blocks forever is the one that has to be kept out; a call that
// kills the child is fine and is counted.
static int
skip(int num)
{
  switch (num) {
  case SYS_read:        // the console, with nobody typing
  case SYS_pause:
  case SYS_sigsuspend:
  case SYS_wait:
  case SYS_wait4:
  case SYS_poll:
  case SYS_select:
  case SYS_futex:
  case SYS_nanosleep:   // a random number of seconds is a hung test
  case SYS_sigreturn:   // only meaningful on a signal stack
  case SYS_recvfrom:
  case SYS_recvmsg:
  case SYS_accept:
  // and the ones that would end somebody else rather than the caller:
  // a random signal to a random process is the machine's init, one time
  // in a few hundred, and an exit with a random status is a count this
  // program would then have to disbelieve.
  case SYS_kill:
  case SYS_tkill:
  case SYS_tgkill:
  case SYS_sigqueue:
  case SYS_exit:
  case SYS_exit_group:
    return 1;
  default:
    return 0;
  }
}

// arguments worth handing over: the edges of the address space, the
// small integers every switch statement in the kernel is made of, and a
// pointer to memory this process really does own.
static long
arg(char *scratch)
{
  switch (rnd() % 10) {
  case 0:
    return 0;
  case 1:
    return 1;
  case 2:
    return (long)(rnd() % 8);
  case 3:
    return -1;
  case 4:
    return 0x7fffffff;
  case 5:
    return (long)scratch;                 // a page this process owns
  case 6:
    return (long)scratch + 4096 - 3;      // and the end of it
  case 7:
    return (long)(rnd() & 0xffffffffffffULL); // somewhere, probably nowhere
  case 8:
    return (long)(1UL << 62);             // the kernel's half
  default:
    return (long)(rnd() % 0x10000);
  }
}

static int
fuzz(int rounds)
{
  char *scratch = malloc(4096 + 16);
  int denials = 0, signalled = 0, made = 0;

  if (scratch == 0) {
    printf("fuzzsys: no memory\n");
    return 1;
  }
  memset(scratch, 0, 4096 + 16);
  // a path, because several calls want one -- and one that is not there,
  // so that a lucky guess at chmod or unlink finds nothing to ruin
  strcpy(scratch, "/tmp/fuzzsys.no");

  // before anything: the profile. Whatever this program then guesses
  // at, it is guessing from inside the box a container lives in.
  if (sysfilter(SYSFILTER_CONTAINER, 0) < 0) {
    printf("fuzzsys: sysfilter: %s\n", strerror(errno));
    return 1;
  }
  printf("fuzzsys: seed %ld, %d rounds\n", (long)seed, rounds);

  // somewhere for a child's descriptors to point. A random write(1, ...)
  // is a console full of nonsense in the middle of somebody's test, and
  // a random read(0, ...) is a wait for a key nobody is going to press.
  int null = open("/dev/null", O_RDWR);
  if (null < 0)
    null = open("/tmp/fuzzsys.null", O_CREAT | O_RDWR | O_TRUNC, 0600);

  for (int i = 0; i < rounds; i++) {
    // mostly numbers this kernel really has, since those are the ones
    // with argument handling to get wrong; now and then one it does not
    int num = (rnd() % 10) ? (int)(1 + rnd() % SYS_sysfilter)
                           : (int)(rnd() % (NSYSCALL + 8));
    if (skip(num))
      continue;
    long a0 = arg(scratch), a1 = arg(scratch);
    long a2 = arg(scratch), a3 = arg(scratch);
    int pid = fork();
    if (pid < 0) {
      // the container's pids.max, or the machine's: wait for room
      int st = 0;
      wait(&st);
      continue;
    }
    if (pid == 0) {
      if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
      }
      long r = raw(num, a0, a1, a2, a3);
      // 1 says refused, 2 says it returned something else. Nothing else
      // about the answer matters: what matters is that the kernel gave
      // one and is still standing.
      _exit(r == -EPERM ? 1 : 2);
    }
    made++;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
      ;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 1)
      denials++;
    else if (!WIFEXITED(st))
      signalled++;
  }

  // and this process, which has been making children do dangerous
  // things for the last few thousand calls, is still able to work
  if (null >= 0)
    close(null);
  unlink("/tmp/fuzzsys.null");
  int fd = open("/tmp/fuzzsys.f", O_CREAT | O_RDWR | O_TRUNC, 0600);
  int ok = fd >= 0 && write(fd, "alive", 5) == 5;
  if (fd >= 0)
    close(fd);
  unlink("/tmp/fuzzsys.f");
  free(scratch);
  printf("fuzzsys: %d calls, %d refused, %d signalled\n", made, denials,
         signalled);
  printf(ok ? "fuzzsys: alive\n" : "fuzzsys: broken\n");
  return ok ? 0 : 1;
}

int
main(int argc, char *argv[])
{
  if (argc >= 2 && strcmp(argv[1], "check") == 0)
    return check();
  if (argc >= 2 && strcmp(argv[1], "badpid") == 0)
    return badpid();
  if (argc >= 2 && strcmp(argv[1], "fuzz") == 0) {
    int rounds = argc >= 3 ? atoi(argv[2]) : 500;
    seed = argc >= 4 ? (uint64)atoi(argv[3]) : (uint64)(uptime() * 2654435761u + 1);
    if (seed == 0)
      seed = 1;
    return fuzz(rounds);
  }
  fprintf(stderr, "usage: fuzzsys check|badpid|fuzz ROUNDS [SEED]\n");
  return 1;
}
