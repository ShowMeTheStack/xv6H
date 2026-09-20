//
// <signal.h>: the set operations and the calls that are built out of
// sigaction rather than being system calls of their own.
//
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "libc.h"

int
sigemptyset(sigset_t *s)
{
  *s = 0;
  return 0;
}

int
sigfillset(sigset_t *s)
{
  *s = ~0UL;
  return 0;
}

int
sigaddset(sigset_t *s, int sig)
{
  if (sig <= 0 || sig >= NSIG)
    return -1;
  *s |= 1UL << (sig - 1);
  return 0;
}

int
sigdelset(sigset_t *s, int sig)
{
  if (sig <= 0 || sig >= NSIG)
    return -1;
  *s &= ~(1UL << (sig - 1));
  return 0;
}

int
sigismember(const sigset_t *s, int sig)
{
  if (sig <= 0 || sig >= NSIG)
    return -1;
  return (*s >> (sig - 1)) & 1;
}

void (*signal(int sig, void (*handler)(int)))(int)
{
  struct sigaction act, old;
  memset(&act, 0, sizeof(act));
  act.sa_handler = (void *)handler;
  act.sa_flags = SA_RESTART;
  if (sigaction(sig, &act, &old) < 0)
    return SIG_ERR;
  return (void (*)(int))old.sa_handler;
}

int
waitpid(int pid, int *status, int options)
{
  return wait4(pid, status, options, 0);
}


int
raise(int sig)
{
  return kill(getpid(), sig);
}
