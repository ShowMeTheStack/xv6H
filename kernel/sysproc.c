#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "nsproxy.h"
#include "sysfilter.h"
#include "blk.h"
#include "platform.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "time.h"
#include "errno.h"
#include "thread.h"
#include "vfs.h"
#include "posix.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "mm.h"
#include "mman.h"
#include "vm.h"
#include "page.h"
#include "sysconf.h"
#include "schedgroup.h"

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
  // every thread reports the process, and the process reports the number
  // its own namespace knows it by
  int pid = p->tg ? p->tg->tgid : p->pid;
  struct pidns *ns = pidns_current();
  if (ns != pidns_root()) {
    struct proc *leader = p->tg ? proc_get(pid) : 0;
    int nr = pid_in_ns(leader ? leader : p, ns);
    if (leader)
      proc_release(leader);
    return nr;
  }
  return pid;
}

// the identity of this thread, which is what p->pid really holds.
uint64
sys_gettid(void)
{
  struct proc *p = myproc();

  return pid_in_ns(p, pidns_current());
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

// sysfilter(profile, num): give up system calls.
//
//   sysfilter(SYSFILTER_CONTAINER, 0)  the container profile
//   sysfilter(SYSFILTER_NONE, n)       give up call number n
//
// Nothing here needs privilege: giving something up never does, and
// there is no way to take anything back.
uint64
sys_sysfilter(void)
{
  int profile, num;
  struct proc *p = myproc();

  argint(0, &profile);
  argint(1, &num);
  if (num > 0)
    return sysfilter_deny(&p->filter, num);
  if (profile == SYSFILTER_NO_NEW_PRIVS) {
    p->cred.no_new_privs = 1;
    return 0;
  }
  return sysfilter_profile(&p->filter, profile);
}

// unshare(flags): the calling process leaves the namespaces named and
// gets copies of them. What it does not name it keeps sharing.
uint64
sys_unshare(void)
{
  int flags;

  argint(0, &flags);
  if (flags & ~CLONE_NEW_ANY)
    return -EINVAL;
  if (flags & CLONE_NEW_UNSUPPORTED)
    return -EINVAL;
  return ns_unshare(flags);
}

// setns(fd, nstype): join the namespace a descriptor names. The
// descriptor is on /proc/<pid>/ns/<kind>, so what it names is the
// namespace of that kind that that process is in -- which is what makes
// `nsenter -t PID' work and is all a container needs.
uint64
sys_setns(void)
{
  int fd, nstype, pid, kind;
  struct file *f;

  argint(0, &fd);
  argint(1, &nstype);
  if ((f = fdget(myproc(), fd)) == 0)
    return -EBADF;
  if (f->type != FD_INODE || f->ip == 0)
    return -EINVAL;
  if (procfs_ns_of(f->ip, &pid, &kind) < 0)
    return -EINVAL;
  // nstype 0 means "whatever this is"; anything else has to agree
  if (nstype != 0) {
    int want = -1;
    switch (nstype) {
    case CLONE_NEWUTS:  want = NSK_UTS;  break;
    case CLONE_NEWPID:  want = NSK_PID;  break;
    case CLONE_NEWNS:   want = NSK_MNT;  break;
    case CLONE_NEWUSER: want = NSK_USER; break;
    case CLONE_NEWNET:  want = NSK_NET;  break;
    }
    if (want != kind)
      return -EINVAL;
  }
  return ns_join(pid, kind);
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
  // W^X: nothing may be writable and executable at the same time. A
  // program that wants to generate code writes it and then asks for
  // it to become executable, which mprotect will do, and a process
  // that may hold both at once is one that an overflow can turn into
  // whatever it likes.
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
    return -EACCES;
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
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
    return -EACCES;   // W^X, as in mmap above
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
  struct pidns *ns = pidns_current();
  int ppid = proc_ppid(myproc());

  if (ns == pidns_root() || ppid == 0)
    return ppid;
  // a parent outside the namespace is no parent that can be named in
  // it: the namespace's init reports 0, as Linux's does
  struct proc *pp = proc_get(ppid);
  int nr = pp ? pid_in_ns(pp, ns) : 0;
  if (pp)
    proc_release(pp);
  return nr;
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
    p = proc_get_ns(pidns_current(), pid);
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
  struct proc *p = proc_get_ns(pidns_current(), pid);
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
  struct proc *p = proc_get_ns(pidns_current(), pid);
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
  case _SC_CLK_TCK: return tick_hz;
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
  ru.ru_utime.tv_sec = u / tick_hz;
  ru.ru_utime.tv_usec = (u % tick_hz) * (1000000 / tick_hz);
  ru.ru_stime.tv_sec = st / tick_hz;
  ru.ru_stime.tv_usec = (st % tick_hz) * (1000000 / tick_hz);
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
  safestrcpy(u.nodename, uts_hostname(), sizeof(u.nodename));
  safestrcpy(u.release, XV6H_RELEASE, sizeof(u.release));
  safestrcpy(u.version, XV6H_VERSION, sizeof(u.version));
  safestrcpy(u.machine, "riscv64", sizeof(u.machine));
  if (copyout(myproc()->mm, addr, (char *)&u, sizeof(u)) < 0)
    return -EFAULT;
  return 0;
}

// the host's name, set from a string the kernel already holds. both
// sethostname(2) and a write to /sys/kernel/hostname come here.
int
sethostname_str(const char *s, int n)
{
  return uts_sethostname(s, n);
}

uint64
sys_sethostname(void)
{
  uint64 addr;
  int n;
  argaddr(0, &addr);
  argint(1, &n);
  if (n < 0 || n >= UTS_LEN)
    return -EINVAL;
  char buf[64];
  if (copyin(myproc()->mm, buf, addr, n) < 0)
    return -EFAULT;
  buf[n] = 0;
  if (!cap_ok(CAP_SYS_ADMIN))
    return -EPERM;
  // in a user namespace, the name it may change is its own namespace's:
  // root inside a container may name the container and not the machine
  if (!userns_in_root() && uts_is_root())
    return -EPERM;
  sethostname_str(buf, n);
  return 0;
}

uint64
sys_gethostname(void)
{
  uint64 addr;
  int n;
  argaddr(0, &addr);
  argint(1, &n);
  const char *h = uts_hostname();
  int len = strlen(h) + 1;
  if (n < len)
    return -ENAMETOOLONG;
  if (copyout(myproc()->mm, addr, (char *)h, len) < 0)
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

// ---- who a process is ----
//
// three user ids and three group ids. the effective one is what every
// permission check uses; the real one is who logged in; the saved one
// is what a set-user-id program may go back to. a process may set any
// of them to any of the other three, and only a privileged one may set
// them to something else.

// what root inside a user namespace may be root for. Anything not here
// is the machine's, whatever the process's capability set says.
#define CAP_NS_ALLOWED                                                         \
  (CAP_BIT(CAP_CHOWN) | CAP_BIT(CAP_DAC_OVERRIDE) |                            \
   CAP_BIT(CAP_DAC_READ_SEARCH) | CAP_BIT(CAP_FOWNER) | CAP_BIT(CAP_FSETID) |  \
   CAP_BIT(CAP_KILL) | CAP_BIT(CAP_SETGID) | CAP_BIT(CAP_SETUID) |             \
   CAP_BIT(CAP_SETPCAP) | CAP_BIT(CAP_SYS_CHROOT) | CAP_BIT(CAP_SYS_ADMIN) |   \
   CAP_BIT(CAP_SYS_NICE) | CAP_BIT(CAP_NET_BIND_SERVICE) |                     \
   CAP_BIT(CAP_AUDIT_WRITE))

// does this process have a capability?
int
cap_ok(int cap)
{
  struct proc *p = myproc();

  if ((p->cred.cap_effective & CAP_BIT(cap)) == 0)
    return 0;
  // in a user namespace of its own, a capability reaches its own
  // namespaces and no further: rebooting the machine, starting a guest,
  // changing the machine's clock or its network are not a container's to
  // do however root it is inside.
  if (!userns_in_root() && (CAP_BIT(cap) & CAP_NS_ALLOWED) == 0)
    return 0;
  return 1;
}

// what a program is told about who it is, in its own namespace's terms.
// A user the namespace has no name for is nobody: 65534, as everywhere.
#define NOBODY 65534u

static uint64
uid_out(unsigned int kuid)
{
  int v = uid_to_ns(userns_current(), kuid);

  return v < 0 ? NOBODY : (uint64)v;
}

static uint64
gid_out(unsigned int kgid)
{
  int v = gid_to_ns(userns_current(), kgid);

  return v < 0 ? NOBODY : (uint64)v;
}

uint64
sys_getuid(void)
{
  return uid_out(myproc()->cred.uid);
}

uint64
sys_geteuid(void)
{
  return uid_out(myproc()->cred.euid);
}

uint64
sys_getgid(void)
{
  return gid_out(myproc()->cred.gid);
}

uint64
sys_getegid(void)
{
  return gid_out(myproc()->cred.egid);
}

// every capability goes when the effective user id becomes an ordinary
// one, and comes back from what is permitted when it becomes root
// again -- which is what a set-user-id root program relies on.
static void
cap_follow_euid(struct cred *c, uint old_euid)
{
  // "root" is root in the process's own user namespace. Inside a
  // container, uid 0 is some ordinary user of the machine, and a process
  // that becomes it has become root there: dropping its capabilities
  // because its host number is not zero would leave a container with a
  // root that can do nothing (which is what it did -- unshare and
  // sethostname inside a user namespace both failed with EPERM).
  struct userns *un = userns_current();
  int was_root = uid_to_ns(un, old_euid) == 0;
  int is_root = uid_to_ns(un, c->euid) == 0;

  if (was_root && !is_root)
    c->cap_effective = 0;
  else if (!was_root && is_root)
    c->cap_effective = c->cap_permitted;
}

uint64
sys_setuid(void)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;
  int uid;

  argint(0, &uid);
  if (uid < 0)
    return -EINVAL;
  // the number is the caller's namespace's; the kernel works in the
  // machine's, so one that cannot be translated is not a user at all
  int kuid = uid_from_ns(userns_current(), (unsigned int)uid);
  if (kuid < 0)
    return -EINVAL;
  uid = kuid;
  uint old = c->euid;
  if (cap_ok(CAP_SETUID)) {
    // a privileged process sets all three, and cannot go back
    c->uid = c->euid = c->suid = (uint)uid;
  } else if ((uint)uid == c->uid || (uint)uid == c->suid) {
    c->euid = (uint)uid;
  } else {
    audit("setuid", "euid", old, (uint64)uid, 0);
    return -EPERM;
  }
  cap_follow_euid(c, old);
  audit("setuid", "euid", old, c->euid, 1);
  return 0;
}

uint64
sys_setgid(void)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;
  int gid;

  argint(0, &gid);
  if (gid < 0)
    return -EINVAL;
  int kgid = gid_from_ns(userns_current(), (unsigned int)gid);
  if (kgid < 0)
    return -EINVAL;
  gid = kgid;
  uint oldg = c->egid;
  if (cap_ok(CAP_SETGID))
    c->gid = c->egid = c->sgid = (uint)gid;
  else if ((uint)gid == c->gid || (uint)gid == c->sgid)
    c->egid = (uint)gid;
  else {
    audit("setgid", "egid", oldg, (uint64)gid, 0);
    return -EPERM;
  }
  audit("setgid", "egid", oldg, c->egid, 1);
  return 0;
}

// setresuid(ruid, euid, suid): -1 leaves one alone
uint64
sys_setresuid(void)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;
  int r, e, sv;

  argint(0, &r);
  argint(1, &e);
  argint(2, &sv);
  uint old = c->euid;
  uint nr = r < 0 ? c->uid : (uint)r;
  uint ne = e < 0 ? c->euid : (uint)e;
  uint ns = sv < 0 ? c->suid : (uint)sv;
  if (!cap_ok(CAP_SETUID)) {
    // each new value has to be one of the three it already has
    uint have[3] = {c->uid, c->euid, c->suid};
    for (int i = 0; i < 3; i++) {
      uint want = i == 0 ? nr : i == 1 ? ne : ns;
      if (want != have[0] && want != have[1] && want != have[2])
        return -EPERM;
    }
  }
  c->uid = nr;
  c->euid = ne;
  c->suid = ns;
  cap_follow_euid(c, old);
  audit("setresuid", "euid", old, ne, 1);
  return 0;
}

uint64
sys_setresgid(void)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;
  int r, e, sv;

  argint(0, &r);
  argint(1, &e);
  argint(2, &sv);
  uint nr = r < 0 ? c->gid : (uint)r;
  uint ne = e < 0 ? c->egid : (uint)e;
  uint ns = sv < 0 ? c->sgid : (uint)sv;
  if (!cap_ok(CAP_SETGID)) {
    uint have[3] = {c->gid, c->egid, c->sgid};
    for (int i = 0; i < 3; i++) {
      uint want = i == 0 ? nr : i == 1 ? ne : ns;
      if (want != have[0] && want != have[1] && want != have[2])
        return -EPERM;
    }
  }
  c->gid = nr;
  c->egid = ne;
  c->sgid = ns;
  return 0;
}

uint64
sys_getresuid(void)
{
  struct proc *p = myproc();
  uint64 a, b, cc;

  argaddr(0, &a);
  argaddr(1, &b);
  argaddr(2, &cc);
  if (copyout(p->mm, a, (char *)&p->cred.uid, sizeof(uint)) < 0 ||
      copyout(p->mm, b, (char *)&p->cred.euid, sizeof(uint)) < 0 ||
      copyout(p->mm, cc, (char *)&p->cred.suid, sizeof(uint)) < 0)
    return -EFAULT;
  return 0;
}

uint64
sys_getresgid(void)
{
  struct proc *p = myproc();
  uint64 a, b, cc;

  argaddr(0, &a);
  argaddr(1, &b);
  argaddr(2, &cc);
  if (copyout(p->mm, a, (char *)&p->cred.gid, sizeof(uint)) < 0 ||
      copyout(p->mm, b, (char *)&p->cred.egid, sizeof(uint)) < 0 ||
      copyout(p->mm, cc, (char *)&p->cred.sgid, sizeof(uint)) < 0)
    return -EFAULT;
  return 0;
}

// getgroups(size, list): with size 0, how many there are
uint64
sys_getgroups(void)
{
  struct proc *p = myproc();
  uint64 addr;
  int size;

  argint(0, &size);
  argaddr(1, &addr);
  if (size == 0)
    return p->cred.ngroups;
  if (size < p->cred.ngroups)
    return -EINVAL;
  if (copyout(p->mm, addr, (char *)p->cred.groups, p->cred.ngroups * sizeof(uint)) < 0)
    return -EFAULT;
  return p->cred.ngroups;
}

uint64
sys_setgroups(void)
{
  struct proc *p = myproc();
  uint64 addr;
  int size;
  uint tmp[NGROUPS_MAX];

  argint(0, &size);
  argaddr(1, &addr);
  if (size < 0 || size > NGROUPS_MAX)
    return -EINVAL;
  if (!cap_ok(CAP_SETGID))
    return -EPERM;
  if (size > 0 && copyin(p->mm, (char *)tmp, addr, size * sizeof(uint)) < 0)
    return -EFAULT;
  memmove(p->cred.groups, tmp, size * sizeof(uint));
  p->cred.ngroups = size;
  return 0;
}

// capget/capset in the shape this kernel uses: one word for each set
uint64
sys_capget(void)
{
  struct proc *p = myproc();
  uint64 addr;
  uint64 sets[4];

  argaddr(0, &addr);
  sets[0] = p->cred.cap_effective;
  sets[1] = p->cred.cap_permitted;
  sets[2] = p->cred.cap_inheritable;
  sets[3] = p->cred.cap_bounding;
  if (copyout(p->mm, addr, (char *)sets, sizeof(sets)) < 0)
    return -EFAULT;
  return 0;
}

uint64
sys_capset(void)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;
  uint64 addr, sets[4];

  argaddr(0, &addr);
  if (copyin(p->mm, (char *)sets, addr, sizeof(sets)) < 0)
    return -EFAULT;
  // nothing may be raised that is not permitted, and the bounding set
  // only ever shrinks
  if ((sets[0] & ~c->cap_permitted) || (sets[1] & ~c->cap_permitted) ||
      (sets[2] & ~c->cap_permitted) || (sets[3] & ~c->cap_bounding))
    return -EPERM;
  if (!cap_ok(CAP_SETPCAP) && sets[1] != c->cap_permitted)
    return -EPERM;
  uint64 was = c->cap_effective;
  c->cap_effective = sets[0];
  c->cap_permitted = sets[1];
  c->cap_inheritable = sets[2];
  c->cap_bounding = sets[3];
  audit("capset", "effective", was, c->cap_effective, 1);
  return 0;
}

// ---- turning the machine off ----
//
// qemu's virt board has the "sifive,test" finisher at 0x100000: a
// halfword written there ends the machine. 0x5555 is a clean power
// off, 0x7777 a reset. a real board would use SBI SRST, which is the
// same two operations behind a call; this kernel is the one under
// SBI, so it writes the device.
#define FINISHER_PASS 0x5555
#define FINISHER_RESET 0x7777

// ---- the nice value ----
//
// setpriority(which, who, value) and getpriority(which, who). the
// value runs from -20 to 19; only a process with CAP_SYS_NICE may ask
// for one smaller than the one it already has, which is the rule that
// stops a program promoting itself. getpriority answers with the
// smallest value among the processes named, as POSIX says, and -1 is
// a real value, so a caller tells an error from it by clearing errno.

// may this process change that one's value?
static int
may_renice(struct proc *target, int newval)
{
  struct proc *p = myproc();

  if (cap_ok(CAP_SYS_NICE))
    return 1;
  // otherwise only one's own processes, and only downwards
  if (p->cred.euid != target->cred.uid && p->cred.euid != target->cred.euid)
    return 0;
  return newval >= target->nice;
}

// does this process fall in the set the caller named?
static int
in_set(struct proc *p, int which, int who)
{
  struct proc *me = myproc();

  switch (which) {
  case PRIO_PROCESS:
    return who == 0 ? p->pid == me->pid : p->pid == who;
  case PRIO_PGRP:
    return who == 0 ? p->pgid == me->pgid : p->pgid == who;
  case PRIO_USER:
    return who == 0 ? p->cred.uid == me->cred.uid
                    : p->cred.uid == (uint)who;
  }
  return 0;
}

// sched_setaffinity(pid, size, mask): the harts this task may run on.
// pid 0 means the caller. The mask is a bitmap of harts, as Linux's
// cpu_set_t is; a mask naming no hart this machine has is refused
// rather than silently leaving a task nowhere to run.
uint64
sys_sched_setaffinity(void)
{
  int pid;
  uint64 size, addr, mask = 0;

  argint(0, &pid);
  argaddr(1, &size);
  argaddr(2, &addr);
  if (size < sizeof(uint64))
    return -EINVAL;
  if (either_copyin(&mask, 1, addr, sizeof(mask)) < 0)
    return -EFAULT;
  if ((mask & ((plat.ncpu >= 64 ? ~0UL : (1UL << plat.ncpu) - 1))) == 0)
    return -EINVAL;

  struct proc *me = myproc();
  if (pid == 0 || pid == me->pid) {
    acquire(&me->lock);
    sched_set_affinity(me, mask);
    release(&me->lock);
    return 0;
  }

  struct proc *p = proc_get_ns(pidns_current(), pid);
  if (p == 0)
    return -ESRCH;
  // the same rule as renicing somebody: the owner or a privileged
  // caller, and nobody else.
  if (p->cred.euid != me->cred.euid && p->cred.uid != me->cred.euid &&
      !cap_ok(CAP_SYS_NICE)) {
    proc_release(p);
    return -EPERM;
  }
  acquire(&p->lock);
  sched_set_affinity(p, mask);
  release(&p->lock);
  proc_release(p);
  return 0;
}

// sched_getaffinity(pid, size, mask): and the bytes written, the way
// Linux answers it.
uint64
sys_sched_getaffinity(void)
{
  int pid;
  uint64 size, addr, mask;

  argint(0, &pid);
  argaddr(1, &size);
  argaddr(2, &addr);
  if (size < sizeof(uint64))
    return -EINVAL;

  struct proc *me = myproc();
  if (pid == 0 || pid == me->pid) {
    mask = sched_affinity(me);
  } else {
    struct proc *p = proc_get_ns(pidns_current(), pid);
    if (p == 0)
      return -ESRCH;
    mask = sched_affinity(p);
    proc_release(p);
  }
  if (either_copyout(1, addr, &mask, sizeof(mask)) < 0)
    return -EFAULT;
  return sizeof(mask);
}

// sched_yield(): give the rest of this slice to whoever is next.
uint64
sys_sched_yield(void)
{
  yield();
  return 0;
}

// getcpu(): the hart this task is on. It may be somewhere else by the
// time the answer is read, which is true of Linux's answer too.
uint64
sys_getcpu(void)
{
  return cpuid();
}

// sched_group(op, id, a, b): the scheduler's groups from user space.
//
// this is what `cgrun' uses and what the cgroup file system of
// this kernel sits on: a group is made, a process joins it, and
// from then on the group's share of the processor is divided among
// whatever is in it rather than each of them competing separately.
//
//   SGC_CREATE  a = the name           -> the group's id
//   SGC_JOIN    id (-1: back to the root)
//   SGC_WEIGHT  id, a = weight
//   SGC_MAX     id, a = quota us, b = period us (a = 0: no ceiling)
//   SGC_FREEZE  id, a = 0 or 1
//   SGC_DESTROY id
//   SGC_INFO    id, a = where to put eight numbers
uint64
sys_sched_group(void)
{
  int op, id;
  uint64 a, b;
  char name[SCHED_GROUP_NAMELEN];

  argint(0, &op);
  argint(1, &id);
  argaddr(2, &a);
  argaddr(3, &b);

  // everything but joining the root and asking questions is a
  // privileged operation: a group is a claim on the machine.
  if (op != SGC_JOIN && op != SGC_INFO && !cap_ok(CAP_SYS_NICE))
    return -EPERM;
  // and the groups themselves are the machine's. They are one set named
  // by small integers, so a process in a user namespace of its own could
  // otherwise reach into another container's group -- or leave its own
  // by joining the root, which is its cpu.max gone. Its share is its
  // control group's and is set from outside it.
  if (op != SGC_INFO && !userns_in_root())
    return -EPERM;

  struct proc *p = myproc();
  struct sched_group *g;

  switch (op) {
  case SGC_CREATE:
    name[0] = 0;
    if (a && fetchstr(a, name, sizeof(name)) < 0)
      return -EFAULT;
    g = sched_group_create(name, 0);
    return g ? sched_group_id(g) : -ENOSPC;
  case SGC_JOIN:
    if (id < 0) {
      acquire(&p->lock);
      sched_group_join(p, 0);
      release(&p->lock);
      return 0;
    }
    g = sched_group_by_id(id);
    if (g == 0)
      return -ESRCH;
    if (!cap_ok(CAP_SYS_NICE))
      return -EPERM;
    acquire(&p->lock);
    sched_group_join(p, g);
    release(&p->lock);
    return 0;
  case SGC_WEIGHT:
    g = sched_group_by_id(id);
    if (g == 0)
      return -ESRCH;
    return sched_group_set_weight(g, (uint32)a) < 0 ? -EINVAL : 0;
  case SGC_MAX:
    g = sched_group_by_id(id);
    if (g == 0)
      return -ESRCH;
    return sched_group_set_max(g, a, b) < 0 ? -EINVAL : 0;
  case SGC_FREEZE:
    g = sched_group_by_id(id);
    if (g == 0)
      return -ESRCH;
    return sched_group_freeze(g, (int)a) < 0 ? -EINVAL : 0;
  case SGC_DESTROY:
    g = sched_group_by_id(id);
    if (g == 0)
      return -ESRCH;
    sched_group_destroy(g);
    return 0;
  case SGC_INFO: {
    uint64 out[8];
    g = sched_group_by_id(id);
    if (g == 0)
      return -ESRCH;
    sched_group_stat(g, out, 8);
    if (either_copyout(1, a, out, sizeof(out)) < 0)
      return -EFAULT;
    return 0;
  }
  }
  return -EINVAL;
}

uint64
sys_setpriority(void)
{
  int which, who, value;

  argint(0, &which);
  argint(1, &who);
  argint(2, &value);
  if (which < PRIO_PROCESS || which > PRIO_USER)
    return -EINVAL;
  if (value < -20)
    value = -20;
  if (value > 19)
    value = 19;

  int found = 0, denied = 0;
  proc_iter_start();
  for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
    if (p->state == UNUSED || !in_set(p, which, who))
      continue;
    found = 1;
    if (!may_renice(p, value)) {
      denied = 1;
      continue;
    }
    p->nice = value;
    sched_renice(p);
  }
  proc_iter_end();
  if (!found)
    return -ESRCH;
  return denied ? -EACCES : 0;
}

uint64
sys_getpriority(void)
{
  int which, who;

  argint(0, &which);
  argint(1, &who);
  if (which < PRIO_PROCESS || which > PRIO_USER)
    return -EINVAL;

  int best = 20, found = 0;

  proc_iter_start();
  for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
    if (p->state == UNUSED || !in_set(p, which, who))
      continue;
    found = 1;
    if (p->nice < best)
      best = p->nice;
  }
  proc_iter_end();
  if (!found)
    return -ESRCH;
  // 20 - nice, so that the answer is never negative: a negative
  // return from a system call here means -errno, and a nice value of
  // -5 would otherwise look like an error. the library subtracts it
  // back, which is the arrangement Linux and glibc have.
  return 20 - best;
}

uint64
sys_reboot(void)
{
  int how;

  argint(0, &how);
  if (!cap_ok(CAP_SYS_BOOT)) {
    audit("reboot", "", (uint64)how, 0, 0);
    return -EPERM;
  }
  audit("reboot", "", (uint64)how, 0, 1);

  // everything that is waiting to be written goes to the disk first:
  // a power off that loses the last minute of work is a bug, not a
  // feature of turning a machine off.
  vfs_sync_all();

  printk("\n");
  printk(how == XV6_REBOOT_RESTART ? "reboot: restarting\n" : "reboot: power off\n");
  *(volatile uint32 *)SIFIVE_TEST =
      how == XV6_REBOOT_RESTART ? FINISHER_RESET : FINISHER_PASS;
  // the device stops the machine; if it does not, say so rather than
  // return to a program that thinks the machine is gone
  for (;;)
    ;
  return 0;
}
