//
// what belongs to this system rather than to any standard: the two
// forms of sbrk, and the wrappers whose system calls take an extra
// argument the standard form does not have.
//
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "kernel/vm.h"
#include <sys/types.h>
#include "libc.h"

// the raw call, which says whether the pages should be mapped now or
// on first touch.
char *sys_sbrk(int n, int flags);

void *
sbrk(long n)
{
  return sys_sbrk((int)n, SBRK_EAGER);
}

// the lazy form: the address space grows, the pages arrive as they are
// touched. programs that reserve a large area and use little of it ask
// for this one.
char *
sbrklazy(int n)
{
  return sys_sbrk(n, SBRK_LAZY);
}

// fork() leaves the child holding a copy of whatever is still in the
// output buffers, which it would write out a second time. flushing
// first is what keeps one line from appearing twice, and it costs
// nothing when there is nothing buffered.
pid_t sys_fork(void);

pid_t
fork(void)
{
  fflush(0);
  return sys_fork();
}

// the pause system call is two calls in one: with a count it sleeps
// that many scheduler ticks, and with none it waits for a signal.
int sys_pause(int ticks);

int
pause(void)
{
  return sys_pause(0);
}

// sleeping by the tick, which the tests and the scheduler care about;
// ordinary programs want sleep() or nanosleep().
int
ticksleep(int ticks)
{
  return sys_pause(ticks);
}

int
brk(void *addr)
{
  char *cur = sbrk(0);

  if (cur == (char *)-1)
    return -1;
  return sbrk((char *)addr - cur) == (void *)-1 ? -1 : 0;
}
