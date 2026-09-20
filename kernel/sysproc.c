#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "blk.h"
#include "platform.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "time.h"
#include "errno.h"
#include "thread.h"
#include "posix.h"
#define XV6H_RELEASE "1.0"
#define XV6H_VERSION "xv6H riscv64"
extern char hostname[64];
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "mm.h"
#include "mman.h"
#include "vm.h"
#include "page.h"
#include "sysconf.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  struct proc *p = myproc();
  return p->tg ? p->tg->tgid : p->pid; // every thread reports the process
}

// the identity of this thread, which is what p->pid really holds.
uint64
sys_gettid(void)
{
  return myproc()->pid;
}

// clone(flags, stack, ptid, tls, ctid): fork's general form. with
// CLONE_VM|CLONE_THREAD it makes a thread of the calling process.
uint64
sys_clone(void)
{
  uint64 flags, stack, ptid, tls, ctid;

  argaddr(0, &flags);
  argaddr(1, &stack);
  argaddr(2, &ptid);
  argaddr(3, &tls);
  argaddr(4, &ctid);
  return kclone(flags, stack, ptid, tls, ctid);
}

// futex(uaddr, op, val, timeout, uaddr2, val3)
uint64
sys_futex(void)
{
  uint64 uaddr, ts, uaddr2;
  int op, val, val3;

  argaddr(0, &uaddr);
  argint(1, &op);
  argint(2, &val);
  argaddr(3, &ts);
  argaddr(4, &uaddr2);
  argint(5, &val3);

  switch (op & FUTEX_CMD_MASK) {
  case FUTEX_WAIT:
    return futex_wait(uaddr, (uint32)val, ts);
  case FUTEX_WAKE:
    return futex_wake(uaddr, val);
  case FUTEX_REQUEUE:
    return futex_requeue(uaddr, val, uaddr2, (int)ts);
  default:
    return -ENOSYS;
  }
}

// signal one thread, rather than the process it belongs to.
uint64
sys_tkill(void)
{
  int tid, sig;

  argint(0, &tid);
  argint(1, &sig);
  return signal_tkill(0, tid, sig);
}

uint64
sys_tgkill(void)
{
  int tgid, tid, sig;

  argint(0, &tgid);
  argint(1, &tid);
  argint(2, &sig);
  return signal_tkill(tgid, tid, sig);
}

// end every thread of this process, not just this one.
uint64
sys_exit_group(void)
{
  int status;

  argint(0, &status);
  struct proc *p = myproc();
  acquire(&p->tg->lock);
  p->tg->exiting = 1;
  p->tg->exit_status = status;
  release(&p->tg->lock);
  tgroup_kill_others(p);
  kexit(status);
  return 0; // not reached
}

// where to clear this thread's id when it exits, for pthread_join()
uint64
sys_set_tid_address(void)
{
  uint64 addr;

  argaddr(0, &addr);
  struct proc *p = myproc();
  p->clear_tid = addr;
  return p->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  int n, t;
  argint(0, &n);
  argint(1, &t);
  struct mm *mm = myproc()->mm;
  uint64 addr = mm->brk;
  if (growproc(n, t == SBRK_EAGER || n < 0) < 0)
    return -1;
  return addr;
}

// mmap(addr, len, prot, flags, fd, off)
uint64
sys_mmap(void)
{
  uint64 addr, len, off;
  int prot, flags, fd;
  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &off);
  struct proc *p = myproc();
  struct inode *ip = 0;
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) || len == 0)
    return -1;
  if (!(flags & MAP_ANONYMOUS)) {
    struct file *f = fdget(p, fd);
    if (f == 0 || f->type != FD_INODE || (off & (PGSIZE - 1)))
      return -1;
    if (!f->readable || ((prot & PROT_WRITE) && (flags & MAP_SHARED) && !f->writable))
      return -1;
    if (flags & MAP_SHARED)
      return -1; // shared file mappings arrive with the page cache
    ip = f->ip;
  }
  int vflags = (flags & MAP_SHARED) ? VMA_SHARED : 0;
  if (flags & MAP_FIXED)
    vflags |= VMA_FIXED;
  uint64 r = mm_mmap(p->mm, addr, len, prot, vflags, ip, off);
  return r ? r : (uint64)-1;
}

uint64
sys_munmap(void)
{
  uint64 addr, len;
  argaddr(0, &addr);
  argaddr(1, &len);
  if ((addr & (PGSIZE - 1)) || len == 0)
    return -1;
  return vma_remove(myproc()->mm, addr, PGROUNDUP(addr + len));
}

uint64
sys_mprotect(void)
{
  uint64 addr, len;
  int prot;
  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return -1;
  return mm_mprotect(myproc()->mm, addr, len, prot);
}



// clock_gettime(clock, &timespec): CLOCK_REALTIME from the RTC (fails
// without one), CLOCK_MONOTONIC from the time counter.
uint64
sys_clock_gettime(void)
{
  int clk;
  uint64 addr, ns;
  struct timespec ts;

  argint(0, &clk);
  argaddr(1, &addr);
  if (clk == CLOCK_REALTIME) {
    if (!rtc_present())
      return -1;
    ns = clock_realtime_ns();
  } else if (clk == CLOCK_MONOTONIC) {
    ns = clock_monotonic_ns();
  } else {
    return -1;
  }
  ts.tv_sec = ns / 1000000000UL;
  ts.tv_nsec = ns % 1000000000UL;
  if (either_copyout(1, addr, &ts, sizeof(ts)) < 0)
    return -1;
  return 0;
}

// wait4(pid, &status, options, &rusage)
uint64
sys_wait4(void)
{
  int pid, options;
  uint64 st, ru;
  argint(0, &pid);
  argaddr(1, &st);
  argint(2, &options);
  argaddr(3, &ru);
  return kwait4(pid, st, options, ru);
}

uint64
sys_getppid(void)
{
  return proc_ppid(myproc());
}

// setpgid(pid, pgid): 0 means "this process" and "its own pid".
uint64
sys_setpgid(void)
{
  int pid, pgid;
  argint(0, &pid);
  argint(1, &pgid);
  struct proc *me = myproc();
  struct proc *p = me;
  if (pid != 0 && pid != me->pid) {
    p = proc_get(pid);
    if (p == 0)
      return -ESRCH;
    if (p->parent != me || p->sid != me->sid) {
      proc_release(p);
      return -EPERM;
    }
  }
  if (pgid == 0)
    pgid = p->pid;
  if (pgid < 0) {
    if (p != me)
      proc_release(p);
    return -EINVAL;
  }
  if (p->sid == p->pid) { // a session leader cannot change group
    if (p != me)
      proc_release(p);
    return -EPERM;
  }
  p->pgid = pgid;
  if (p != me)
    proc_release(p);
  return 0;
}

uint64
sys_getpgid(void)
{
  int pid;
  argint(0, &pid);
  if (pid == 0)
    return myproc()->pgid;
  struct proc *p = proc_get(pid);
  if (p == 0)
    return -ESRCH;
  int pgid = p->pgid;
  proc_release(p);
  return pgid;
}

// setsid(): a new session and group, with no controlling terminal.
uint64
sys_setsid(void)
{
  struct proc *p = myproc();
  if (p->pgid == p->pid)
    return -EPERM; // already a group leader
  p->sid = p->pid;
  p->pgid = p->pid;
  p->tty = 0;
  return p->sid;
}

uint64
sys_getsid(void)
{
  int pid;
  argint(0, &pid);
  if (pid == 0)
    return myproc()->sid;
  struct proc *p = proc_get(pid);
  if (p == 0)
    return -ESRCH;
  int sid = p->sid;
  proc_release(p);
  return sid;
}

// sysconf(name): see sysconf.h. -1 for an unknown name.
uint64
sys_sysconf(void)
{
  int name;
  argint(0, &name);
  switch (name) {
  case _SC_PAGESIZE: return PGSIZE;
  case _SC_PHYS_PAGES: {
    struct meminfo mi;
    kmem_info(&mi);
    return mi.total_pages;
  }
  case _SC_AVPHYS_PAGES:
    kstack_drain(); // released stacks still in quarantine count as free
    // and so do the pages the block cache is holding only because they
    // might be useful again: a clean cached block is a copy of what is
    // on the disk, and kalloc drops it the moment a page is wanted.
    // this is what Linux calls available rather than free.
    return kmem_free_pages() + blk_reclaimable();
  case _SC_NPROCESSORS_ONLN: return plat.ncpu;
  case _SC_CLK_TCK: return 10;
  case _SC_XV6_MAXVA: return plat.maxva;
  case _SC_XV6_TRAPFRAME: return TRAPFRAME;
  case _SC_XV6_BRK_MAX: return mm_brk_max(myproc()->mm); // the heap may grow to here
  case _SC_XV6_STACK_MAX: return STACK_MAX;
  case _SC_XV6_STACK_GUARD: return myproc()->mm->stack_bottom - PGSIZE;
  case _SC_XV6_CACHEPAGES: return blk_cache_pages();
  case _SC_XV6_MMU_LEVELS: return plat.mmu_levels;
  case _SC_XV6_NPROCS: return proc_count();
  case _SC_XV6_PID_MAX: {
    extern uint64 pid_max;
    return pid_max;
  }
  }
  return -1;
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// ---- the remaining POSIX process calls ----

// nanosleep: sleep until the monotonic clock says the time has passed.
// counting scheduler ticks instead would let the call return up to a
// whole tick early, because the tick the caller arrived in had already
// begun, and a program that asks for fifty milliseconds is entitled to
// fifty of them.
uint64
sys_nanosleep(void)
{
  uint64 req, rem;
  argaddr(0, &req);
  argaddr(1, &rem);
  struct proc *p = myproc();
  struct timespec ts;
  if (copyin(p->mm, (char *)&ts, req, sizeof(ts)) < 0)
    return -EFAULT;
  if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L || ts.tv_sec < 0)
    return -EINVAL;

  uint64 want = (uint64)ts.tv_sec * 1000000000UL + (uint64)ts.tv_nsec;
  uint64 start = clock_monotonic_ns();
  uint64 until = start + want;
  p->norestart = 1;

  for (;;) {
    uint64 now = clock_monotonic_ns();
    if (now >= until)
      break;
    if (killed(p) || signal_pending(p)) {
      if (rem) {
        struct timespec r;
        uint64 left = until - now;
        r.tv_sec = left / 1000000000UL;
        r.tv_nsec = left % 1000000000UL;
        copyout(p->mm, rem, (char *)&r, sizeof(r));
      }
      return -EINTR;
    }
    // wait for the next tick, which is the finest the timer offers
    acquire(&tickslock);
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep_intr();
  }
  if (rem) {
    struct timespec r = {0, 0};
    copyout(p->mm, rem, (char *)&r, sizeof(r));
  }
  return 0;
}

// times(&tms): clock ticks used, and the tick count as the return value
uint64
sys_times(void)
{
  uint64 addr;
  argaddr(0, &addr);
  struct proc *p = myproc();
  struct tms t;
  memset(&t, 0, sizeof(t));
  t.tms_utime = p->utime;
  t.tms_stime = p->stime;
  t.tms_cutime = p->cutime;
  t.tms_cstime = p->cstime;
  if (addr && copyout(p->mm, addr, (char *)&t, sizeof(t)) < 0)
    return -EFAULT;
  acquire(&tickslock);
  uint64 t0 = ticks;
  release(&tickslock);
  return t0;
}

uint64
sys_getrusage(void)
{
  int who;
  uint64 addr;
  argint(0, &who);
  argaddr(1, &addr);
  struct proc *p = myproc();
  struct rusage ru;
  memset(&ru, 0, sizeof(ru));
  uint64 u = who == RUSAGE_CHILDREN ? p->cutime : p->utime;
  uint64 st = who == RUSAGE_CHILDREN ? p->cstime : p->stime;
  ru.ru_utime.tv_sec = u / TICK_HZ;
  ru.ru_utime.tv_usec = (u % TICK_HZ) * (1000000 / TICK_HZ);
  ru.ru_stime.tv_sec = st / TICK_HZ;
  ru.ru_stime.tv_usec = (st % TICK_HZ) * (1000000 / TICK_HZ);
  if (who != RUSAGE_CHILDREN && p->mm)
    ru.ru_maxrss = p->mm->pages * (PGSIZE / 1024);
  ru.ru_minflt = p->minflt;
  ru.ru_majflt = p->majflt;
  if (copyout(p->mm, addr, (char *)&ru, sizeof(ru)) < 0)
    return -EFAULT;
  return 0;
}

uint64
sys_uname(void)
{
  uint64 addr;
  argaddr(0, &addr);
  struct utsname u;
  memset(&u, 0, sizeof(u));
  safestrcpy(u.sysname, "xv6H", sizeof(u.sysname));
  safestrcpy(u.nodename, hostname, sizeof(u.nodename));
  safestrcpy(u.release, XV6H_RELEASE, sizeof(u.release));
  safestrcpy(u.version, XV6H_VERSION, sizeof(u.version));
  safestrcpy(u.machine, "riscv64", sizeof(u.machine));
  if (copyout(myproc()->mm, addr, (char *)&u, sizeof(u)) < 0)
    return -EFAULT;
  return 0;
}

uint64
sys_sethostname(void)
{
  uint64 addr;
  int n;
  argaddr(0, &addr);
  argint(1, &n);
  if (n < 0 || n >= (int)sizeof(hostname))
    return -EINVAL;
  char buf[64];
  if (copyin(myproc()->mm, buf, addr, n) < 0)
    return -EFAULT;
  buf[n] = 0;
  safestrcpy(hostname, buf, sizeof(hostname));
  return 0;
}

uint64
sys_gethostname(void)
{
  uint64 addr;
  int n;
  argaddr(0, &addr);
  argint(1, &n);
  int len = strlen(hostname) + 1;
  if (n < len)
    return -ENAMETOOLONG;
  if (copyout(myproc()->mm, addr, hostname, len) < 0)
    return -EFAULT;
  return 0;
}

uint64
sys_umask(void)
{
  int mask;
  argint(0, &mask);
  struct proc *p = myproc();
  int old = p->umask;
  p->umask = mask & 0777;
  return old;
}

// getrandom(buf, len, flags)
uint64
sys_getrandom(void)
{
  uint64 addr, len;
  int flags;
  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &flags);
  struct proc *p = myproc();
  char buf[64];
  uint64 done = 0;
  while (done < len) {
    uint64 n = len - done;
    if (n > sizeof(buf))
      n = sizeof(buf);
    random_bytes(buf, n);
    if (copyout(p->mm, addr + done, buf, n) < 0)
      return done > 0 ? (int64)done : -EFAULT;
    done += n;
  }
  return done;
}
