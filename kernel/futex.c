#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "errno.h"
#include "mm.h"
#include "time.h"
#include "thread.h"

//
// futexes: the kernel half of a userspace lock. a thread that cannot
// take a lock in userspace asks the kernel to sleep until the word at
// uaddr changes; whoever releases the lock asks the kernel to wake the
// sleepers. the uncontended path never enters the kernel at all.
//
// a futex is keyed on the physical address of its word, so threads
// sharing an address space and processes sharing a file-backed page
// wait on the same thing. the key doubles as the sleep channel.
//

extern uint ticks;
extern struct spinlock tickslock;

// resolve uaddr to the physical address of its word. the page is
// faulted in for writing, so a copy-on-write page is broken here
// rather than under the wait queue's lock.
static uint64
futex_key(uint64 uaddr, int *err)
{
  struct proc *p = myproc();

  if (uaddr % sizeof(uint32) != 0) {
    *err = -EINVAL;
    return 0;
  }
  uint64 pa = mm_fault_pa(p->mm, PGROUNDDOWN(uaddr), 1);
  if (pa == 0) {
    *err = -EFAULT;
    return 0;
  }
  return pa + (uaddr % PGSIZE);
}

// sleep until someone wakes this address, but only while it still
// holds val. registering on the queue before re-reading the word is
// what closes the race against a waker between the two.
int
futex_wait(uint64 uaddr, uint32 val, uint64 tsaddr)
{
  struct proc *p = myproc();
  int err = 0;
  uint64 key = futex_key(uaddr, &err);

  if (key == 0)
    return err;

  uint64 until = 0;
  if (tsaddr) {
    struct timespec ts;
    if (copyin(p->mm, (char *)&ts, tsaddr, sizeof(ts)) < 0)
      return -EFAULT;
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L || ts.tv_sec < 0)
      return -EINVAL;
    uint64 want = (uint64)ts.tv_sec * tick_hz + ((uint64)ts.tv_nsec * tick_hz + 999999999UL) / 1000000000UL;
    acquire(&tickslock);
    until = ticks + (want ? want : 1);
    release(&tickslock);
  }

  p->norestart = 1; // a signal here is reported, never replayed
  sleep_prepare((void *)key);
  if (__atomic_load_n((uint32 *)key, __ATOMIC_ACQUIRE) != val) {
    sleep_cancel();
    return -EAGAIN;
  }
  p->futex_until = until;
  sleep_intr();
  p->futex_until = 0;

  if (killed(p) || signal_pending(p))
    return -EINTR;
  if (until) {
    acquire(&tickslock);
    uint64 now = ticks;
    release(&tickslock);
    if (now >= until)
      return -ETIMEDOUT;
  }
  return 0;
}

// wake up to n threads waiting on this address; returns how many.
int
futex_wake(uint64 uaddr, int n)
{
  int err = 0;
  uint64 key = futex_key(uaddr, &err);

  if (key == 0)
    return err;
  if (n < 0)
    n = 0;
  return wakeup_n((void *)key, n);
}

// move waiters from one address to another without waking them: wake
// n of them, and let the rest wait on uaddr2 instead. the simple
// implementation wakes them all and lets them retry, which is correct
// (a futex wait may always return early) if less efficient.
int
futex_requeue(uint64 uaddr, int n, uint64 uaddr2, int n2)
{
  int woken = futex_wake(uaddr, n);
  if (woken < 0)
    return woken;
  if (n2 > 0) {
    int more = futex_wake(uaddr, n2);
    if (more > 0)
      woken += more;
  }
  (void)uaddr2;
  return woken;
}

// the timer tick found a wait that has run out of time.
void
futex_timeout(struct proc *p)
{
  p->futex_until = 0;
  wake_proc(p);
}
