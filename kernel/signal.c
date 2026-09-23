//
// POSIX signals (signal.h): dispositions per process, a mask and a
// pending queue per thread, delivery on the way back to user space
// through a frame on the user stack, and the job-control stops.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "page.h"
#include "slab.h"
#include "mm.h"
#include "signal.h"
#include "errno.h"

extern char sigtramp[]; // sigtramp.S, mapped at SIGTRAMP in every address space

static struct kmem_cache *sighand_cache, *sigq_cache;

// what a signal does when nobody handles it
#define D_TERM 0
#define D_IGN  1
#define D_STOP 2
#define D_CONT 3
#define D_CORE 4 // terminate; we have no core files

static const char defact[NSIG] = {
    [SIGHUP] = D_TERM,  [SIGINT] = D_TERM,   [SIGQUIT] = D_CORE, [SIGILL] = D_CORE,
    [SIGTRAP] = D_CORE, [SIGABRT] = D_CORE,  [SIGBUS] = D_CORE,  [SIGFPE] = D_CORE,
    [SIGKILL] = D_TERM, [SIGUSR1] = D_TERM,  [SIGSEGV] = D_CORE, [SIGUSR2] = D_TERM,
    [SIGPIPE] = D_TERM, [SIGALRM] = D_TERM,  [SIGTERM] = D_TERM, [SIGSTKFLT] = D_TERM,
    [SIGCHLD] = D_IGN,  [SIGCONT] = D_CONT,  [SIGSTOP] = D_STOP, [SIGTSTP] = D_STOP,
    [SIGTTIN] = D_STOP, [SIGTTOU] = D_STOP,  [SIGURG] = D_IGN,   [SIGXCPU] = D_CORE,
    [SIGXFSZ] = D_CORE, [SIGVTALRM] = D_TERM, [SIGPROF] = D_TERM, [SIGWINCH] = D_IGN,
    [SIGIO] = D_TERM,   [SIGPWR] = D_TERM,   [SIGSYS] = D_CORE,
};

static int
default_action(int sig)
{
  if (sig >= SIGRTMIN)
    return D_TERM;
  return defact[sig];
}

void
siginit(void)
{
  sighand_cache = kmem_cache_create("sighand", sizeof(struct sighand), 8, 0);
  sigq_cache = kmem_cache_create("sigq", sizeof(struct sigq), 8, 0);
}

// ---- dispositions ----

struct sighand *
sighand_alloc(void)
{
  struct sighand *sh = kmem_cache_alloc(sighand_cache);
  if (sh == 0)
    return 0;
  memset(sh, 0, sizeof(*sh));
  initlock(&sh->lock, "sighand");
  sh->ref = 1;
  return sh;
}

// a copy for a new process (fork): handlers are inherited.
struct sighand *
sighand_copy(struct sighand *src)
{
  struct sighand *sh = kmem_cache_alloc(sighand_cache);
  if (sh == 0)
    return 0;
  memset(sh, 0, sizeof(*sh));
  initlock(&sh->lock, "sighand");
  sh->ref = 1;
  acquire(&src->lock);
  memmove(sh->act, src->act, sizeof(sh->act));
  release(&src->lock);
  return sh;
}

// another thread shares these handlers.
struct sighand *
sighand_share(struct sighand *sh)
{
  acquire(&sh->lock);
  sh->ref++;
  release(&sh->lock);
  return sh;
}

void
sighand_put(struct sighand *sh)
{
  if (sh == 0)
    return;
  acquire(&sh->lock);
  int n = --sh->ref;
  release(&sh->lock);
  if (n == 0)
    kmem_cache_free(sighand_cache, sh);
}

// exec: handled signals go back to the default, ignored ones stay ignored.
void
sighand_exec(struct proc *p)
{
  struct sighand *sh = p->sighand;
  acquire(&sh->lock);
  for (int i = 1; i < NSIG; i++) {
    if (sh->act[i].sa_handler != SIG_IGN)
      memset(&sh->act[i], 0, sizeof(sh->act[i]));
  }
  release(&sh->lock);
  p->sigaltstk.ss_flags = SS_DISABLE;
  p->sigaltstk.ss_sp = 0;
  p->sigaltstk.ss_size = 0;
}

void
sigq_free(struct sigq *q)
{
  kmem_cache_free(sigq_cache, q);
}

// ---- the pending queue ----

// drop every queued instance of sig (p->lock held)
static void
sigq_drop(struct proc *p, sigset_t mask)
{
  struct sigq **pp = &p->sigqueue;
  while (*pp) {
    struct sigq *q = *pp;
    if (sigismember_(mask, q->info.si_signo)) {
      *pp = q->next;
      kmem_cache_free(sigq_cache, q);
    } else {
      pp = &q->next;
    }
  }
  p->sigpending &= ~mask;
}

// take the information for sig off the queue (p->lock held)
static void
sigq_take(struct proc *p, int sig, siginfo_t *out)
{
  struct sigq **pp = &p->sigqueue;
  while (*pp) {
    struct sigq *q = *pp;
    if (q->info.si_signo == sig) {
      *out = q->info;
      *pp = q->next;
      kmem_cache_free(sigq_cache, q);
      // another instance of the same signal may be queued (realtime)
      for (struct sigq *r = p->sigqueue; r; r = r->next)
        if (r->info.si_signo == sig)
          return;
      p->sigpending &= ~sigmask_(sig);
      return;
    }
    pp = &q->next;
  }
  // no information was queued: synthesize what we know
  memset(out, 0, sizeof(*out));
  out->si_signo = sig;
  out->si_code = SI_KERNEL;
  p->sigpending &= ~sigmask_(sig);
}

// ---- sending ----

// p->lock held. returns 0, or -EAGAIN when a realtime signal cannot be queued.
static int
signal_deliver_locked(struct proc *p, int sig, siginfo_t *info)
{
  if (p->state == UNUSED || p->state == ZOMBIE)
    return 0;

  struct sighand *sh = p->sighand;
  acquire(&sh->lock);
  void *h = sh->act[sig].sa_handler;
  release(&sh->lock);

  // an ignored signal that nobody can be waiting for is dropped here;
  // SIGKILL and SIGSTOP cannot be ignored.
  if (h == SIG_IGN && !(sigmask_(sig) & SIG_UNCATCHABLE))
    return 0;
  if (h == SIG_DFL && default_action(sig) == D_IGN)
    return 0;

  // stop and continue cancel each other
  if (default_action(sig) == D_CONT || sig == SIGCONT) {
    sigq_drop(p, sigmask_(SIGSTOP) | sigmask_(SIGTSTP) | sigmask_(SIGTTIN) | sigmask_(SIGTTOU));
    if (p->state == STOPPED) {
      p->stopped_sig = 0;
      p->notify = CLD_CONTINUED;
      proc_make_runnable(p);
      proc_notify_parent(p);
    }
  } else if (default_action(sig) == D_STOP) {
    sigq_drop(p, sigmask_(SIGCONT));
  }

  // a continue with no handler has already done its work: queueing it
  // would only interrupt a system call for nothing.
  if (h == SIG_DFL && default_action(sig) == D_CONT)
    return 0;

  int already = sigismember_(p->sigpending, sig);
  if (already && sig < SIGRTMIN)
    return 0; // standard signals do not queue

  if (p->nsigq < SIGQUEUE_MAX) {
    struct sigq *q = kmem_cache_alloc(sigq_cache);
    if (q) {
      q->info = *info;
      q->info.si_signo = sig;
      q->next = 0;
      struct sigq **pp = &p->sigqueue;
      while (*pp)
        pp = &(*pp)->next;
      *pp = q;
      p->nsigq++;
    } else if (sig >= SIGRTMIN) {
      return -EAGAIN;
    }
  } else if (sig >= SIGRTMIN) {
    return -EAGAIN;
  }
  p->sigpending |= sigmask_(sig);

  // SIGKILL and SIGSTOP take effect even against the mask; anything else
  // wakes the target only when it is not blocked.
  if (sig == SIGKILL) {
    p->killed = 1;
    p->exitsig = SIGKILL; // the wait status says what killed it
    p->stopped_sig = 0;
    proc_make_runnable(p);
  } else if (!sigismember_(p->sigblocked, sig) || (sigmask_(sig) & SIG_UNCATCHABLE)) {
    proc_make_runnable(p);
  }
  return 0;
}

int
signal_send(struct proc *p, int sig, siginfo_t *info)
{
  if (sig <= 0 || sig >= NSIG)
    return -EINVAL;
  siginfo_t local;
  if (info == 0) {
    memset(&local, 0, sizeof(local));
    local.si_signo = sig;
    local.si_code = SI_USER;
    struct proc *me = myproc();
    local.si_pid = me ? me->pid : 0;
    info = &local;
  }
  acquire(&p->lock);
  int r = signal_deliver_locked(p, sig, info);
  release(&p->lock);
  return r;
}

// a fault in the current process: deliver synchronously.
void
signal_fault(struct proc *p, int sig, int code, uint64 addr)
{
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  info.si_code = code;
  info.si_addr = (void *)addr;
  // a fault signal the process blocks or ignores must still kill it
  struct sighand *sh = p->sighand;
  acquire(&sh->lock);
  void *h = sh->act[sig].sa_handler;
  release(&sh->lock);
  if (h == SIG_IGN || sigismember_(p->sigblocked, sig)) {
    acquire(&sh->lock);
    memset(&sh->act[sig], 0, sizeof(sh->act[sig])); // back to the default
    release(&sh->lock);
    acquire(&p->lock);
    p->sigblocked &= ~sigmask_(sig);
    release(&p->lock);
  }
  signal_send(p, sig, &info);
}

// tkill/tgkill: the target is one thread, named by its own id, not the
// process it belongs to.
int
signal_tkill(int tgid, int tid, int sig)
{
  if (sig < 0 || sig >= NSIG || tid <= 0)
    return -EINVAL;
  struct proc *p = proc_get(tid);
  if (p == 0)
    return -ESRCH;
  if (tgid > 0 && (p->tg == 0 || p->tg->tgid != tgid)) {
    proc_release(p);
    return -ESRCH;
  }
  int r = sig ? signal_send(p, sig, 0) : 0;
  proc_release(p);
  return r;
}

// kill(pid, sig) semantics: pid > 0 one process, pid == 0 our group,
// pid == -1 everyone we may signal, pid < -1 the group -pid.
int
signal_kill(int pid, int sig, siginfo_t *info)
{
  if (sig < 0 || sig >= NSIG)
    return -EINVAL;
  struct proc *me = myproc();
  if (pid > 0) {
    struct proc *p = proc_get(pid);
    if (p == 0)
      return -ESRCH;
    int r = sig ? signal_send(p, sig, info) : 0;
    proc_release(p);
    return r;
  }
  int pgid = (pid == 0) ? me->pgid : -pid;
  int found = 0, r = 0;
  proc_iter_start();
  for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
    if (p == me && pid == -1)
      continue;
    if (p->pid == 1 && pid == -1)
      continue;
    if (pid != -1 && p->pgid != pgid)
      continue;
    if (p->state == UNUSED || p->state == ZOMBIE)
      continue;
    found = 1;
    if (sig) {
      int e = signal_send(p, sig, info);
      if (e < 0)
        r = e;
    }
  }
  proc_iter_end();
  return found ? r : -ESRCH;
}

// ---- delivery ----

int
signal_pending(struct proc *p)
{
  return (p->sigpending & ~p->sigblocked) != 0;
}

// the lowest-numbered deliverable signal, or 0 (p->lock held)
static int
next_signal(struct proc *p)
{
  sigset_t s = p->sigpending & ~p->sigblocked;
  for (int sig = 1; sig < NSIG; sig++)
    if (sigismember_(s, sig))
      return sig;
  return 0;
}

// build the frame on the user stack and point the trapframe at the handler.
static int
setup_frame(struct proc *p, int sig, siginfo_t *info, struct sigaction *act)
{
  struct trapframe *tf = p->trapframe;
  uint64 sp = tf->sp;

  // the alternate stack, if the handler asked for one and we are not on it
  if ((act->sa_flags & SA_ONSTACK) && !(p->sigaltstk.ss_flags & SS_DISABLE) &&
      !(sp >= (uint64)p->sigaltstk.ss_sp && sp < (uint64)p->sigaltstk.ss_sp + p->sigaltstk.ss_size))
    sp = (uint64)p->sigaltstk.ss_sp + p->sigaltstk.ss_size;

  uint64 veclen = 0;
  if (p->vstate && plat.vlenb)
    veclen = 32 * plat.vlenb;

  sp -= veclen;
  sp &= ~15UL;
  uint64 vecaddr = sp;
  sp -= sizeof(struct sigframe);
  sp &= ~15UL;

  struct sigframe fr;
  memset(&fr, 0, sizeof(fr));
  fr.info = *info;
  fr.uc.uc_sigmask = p->sigblocked;
  fr.uc.uc_stack = p->sigaltstk;
  fr.uc.uc_mcontext.pc = tf->epc;
  uint64 *regs = &tf->ra; // ra is x1, and the trapframe holds x1..x31 in order
  for (int i = 0; i < 31; i++)
    fr.uc.uc_mcontext.regs[i] = regs[i];
  memmove(fr.uc.uc_mcontext.fregs, tf->fregs, sizeof(tf->fregs));
  fr.uc.uc_mcontext.fcsr = tf->fcsr;
  memmove(fr.uc.uc_mcontext.vcsrs, tf->vcsrs, sizeof(tf->vcsrs));
  fr.uc.uc_mcontext.vlenb = veclen ? plat.vlenb : 0;

  if (copyout(p->mm, sp, (char *)&fr, sizeof(fr)) < 0)
    return -1;
  if (veclen && copyout(p->mm, vecaddr, p->vstate, veclen) < 0)
    return -1;

  tf->sp = sp;
  tf->epc = (uint64)act->sa_handler;
  tf->ra = SIGTRAMP; // returns through sigreturn
  tf->a0 = sig;
  tf->a1 = sp + __builtin_offsetof(struct sigframe, info);
  tf->a2 = sp + __builtin_offsetof(struct sigframe, uc);
  return 0;
}

// stop this process until SIGCONT (p->lock not held)
static void
do_stop(struct proc *p, int sig)
{
  acquire(&p->lock);
  p->stopped_sig = sig;
  p->notify = CLD_STOPPED;
  p->state = STOPPED;
  release(&p->lock);
  proc_notify_parent(p);
  acquire(&p->lock);
  // `parked` says the switch is really about to happen, so that a
  // wakeup arriving in this window queues the process rather than
  // leaving it both running here and waiting to be run elsewhere
  p->stop_parked = 1;
  while (p->state == STOPPED)
    sched_stopped(); // switches away; the scheduler will not pick a STOPPED process
  p->stop_parked = 0;
  release(&p->lock);
}

// called on the way back to user space, with no locks held.
void
do_signals(void)
{
  struct proc *p = myproc();
  if (p == 0 || p->mm == 0)
    return;

  for (;;) {
    acquire(&p->lock);
    int sig = next_signal(p);
    if (sig == 0) {
      // sigsuspend's mask is restored once a signal has been handled
      release(&p->lock);
      return;
    }
    siginfo_t info;
    sigq_take(p, sig, &info);
    release(&p->lock);

    struct sighand *sh = p->sighand;
    acquire(&sh->lock);
    struct sigaction act = sh->act[sig];
    if ((act.sa_flags & SA_RESETHAND) && act.sa_handler != SIG_DFL && act.sa_handler != SIG_IGN)
      memset(&sh->act[sig], 0, sizeof(sh->act[sig]));
    release(&sh->lock);

    if (act.sa_handler == SIG_IGN)
      continue;
    if (act.sa_handler == SIG_DFL) {
      switch (default_action(sig)) {
      case D_IGN:
        continue;
      case D_CONT:
        continue; // the stop was already undone when the signal was sent
      case D_STOP:
        do_stop(p, sig);
        continue;
      default:
        p->exitsig = sig;
        kexit(0); // does not return; the status says "killed by sig"
      }
    }

    // a system call that returned EINTR is retried if the handler wants it
    if (p->restartable) {
      if (act.sa_flags & SA_RESTART) {
        p->trapframe->epc -= 4;
        p->trapframe->a0 = p->syscall_a0;
      }
      p->restartable = 0;
    }

    if (setup_frame(p, sig, &info, &act) < 0) {
      // the stack is unusable: die on the spot
      p->exitsig = SIGSEGV;
      kexit(0);
    }
    acquire(&p->lock);
    p->sigblocked |= act.sa_mask;
    if (!(act.sa_flags & SA_NODEFER))
      p->sigblocked |= sigmask_(sig);
    p->sigblocked &= ~SIG_UNCATCHABLE;
    p->sigrestore_valid = p->sigsuspended; // sigreturn restores the pre-suspend mask
    p->sigsuspended = 0;
    release(&p->lock);
    return; // one handler at a time
  }
}

// ---- system calls ----

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  struct trapframe *tf = p->trapframe;
  uint64 sp = tf->sp;
  struct sigframe fr;

  if (copyin(p->mm, (char *)&fr, sp, sizeof(fr)) < 0)
    return -EFAULT;
  uint64 *regs = &tf->ra;
  for (int i = 0; i < 31; i++)
    regs[i] = fr.uc.uc_mcontext.regs[i];
  tf->epc = fr.uc.uc_mcontext.pc;
  memmove(tf->fregs, fr.uc.uc_mcontext.fregs, sizeof(tf->fregs));
  tf->fcsr = fr.uc.uc_mcontext.fcsr;
  memmove(tf->vcsrs, fr.uc.uc_mcontext.vcsrs, sizeof(tf->vcsrs));
  if (fr.uc.uc_mcontext.vlenb && p->vstate) {
    uint64 veclen = 32 * fr.uc.uc_mcontext.vlenb;
    if (veclen > PGSIZE)
      veclen = PGSIZE;
    uint64 vecaddr = (sp + sizeof(struct sigframe) + 15) & ~15UL;
    if (copyin(p->mm, p->vstate, vecaddr, veclen) < 0)
      return -EFAULT;
  }
  acquire(&p->lock);
  p->sigblocked = fr.uc.uc_sigmask & ~SIG_UNCATCHABLE;
  if (p->sigrestore_valid) {
    p->sigblocked = p->sigsaved & ~SIG_UNCATCHABLE;
    p->sigrestore_valid = 0;
  }
  p->sigaltstk = fr.uc.uc_stack;
  release(&p->lock);
  return tf->a0; // the interrupted a0, which syscall() stores back
}

uint64
sys_sigaction(void)
{
  int sig;
  uint64 actaddr, oldaddr;
  argint(0, &sig);
  argaddr(1, &actaddr);
  argaddr(2, &oldaddr);
  if (sig <= 0 || sig >= NSIG)
    return -EINVAL;
  struct proc *p = myproc();
  struct sighand *sh = p->sighand;
  struct sigaction act, old;

  acquire(&sh->lock);
  old = sh->act[sig];
  release(&sh->lock);
  if (oldaddr && copyout(p->mm, oldaddr, (char *)&old, sizeof(old)) < 0)
    return -EFAULT;
  if (actaddr == 0)
    return 0;
  if (copyin(p->mm, (char *)&act, actaddr, sizeof(act)) < 0)
    return -EFAULT;
  if (sigmask_(sig) & SIG_UNCATCHABLE)
    return -EINVAL;
  act.sa_mask &= ~SIG_UNCATCHABLE;
  acquire(&sh->lock);
  sh->act[sig] = act;
  release(&sh->lock);
  // a signal that has become ignored is discarded
  if (act.sa_handler == SIG_IGN || (act.sa_handler == SIG_DFL && default_action(sig) == D_IGN)) {
    acquire(&p->lock);
    sigq_drop(p, sigmask_(sig));
    release(&p->lock);
  }
  return 0;
}

uint64
sys_sigprocmask(void)
{
  int how;
  uint64 setaddr, oldaddr;
  argint(0, &how);
  argaddr(1, &setaddr);
  argaddr(2, &oldaddr);
  struct proc *p = myproc();
  sigset_t set, old;

  acquire(&p->lock);
  old = p->sigblocked;
  release(&p->lock);
  if (oldaddr && copyout(p->mm, oldaddr, (char *)&old, sizeof(old)) < 0)
    return -EFAULT;
  if (setaddr == 0)
    return 0;
  if (copyin(p->mm, (char *)&set, setaddr, sizeof(set)) < 0)
    return -EFAULT;
  acquire(&p->lock);
  switch (how) {
  case SIG_BLOCK:
    p->sigblocked |= set;
    break;
  case SIG_UNBLOCK:
    p->sigblocked &= ~set;
    break;
  case SIG_SETMASK:
    p->sigblocked = set;
    break;
  default:
    release(&p->lock);
    return -EINVAL;
  }
  p->sigblocked &= ~SIG_UNCATCHABLE;
  release(&p->lock);
  return 0;
}

uint64
sys_sigpending(void)
{
  uint64 addr;
  argaddr(0, &addr);
  struct proc *p = myproc();
  acquire(&p->lock);
  sigset_t s = p->sigpending;
  release(&p->lock);
  if (copyout(p->mm, addr, (char *)&s, sizeof(s)) < 0)
    return -EFAULT;
  return 0;
}

uint64
sys_sigsuspend(void)
{
  uint64 addr;
  argaddr(0, &addr);
  struct proc *p = myproc();
  sigset_t set;
  if (copyin(p->mm, (char *)&set, addr, sizeof(set)) < 0)
    return -EFAULT;
  p->norestart = 1; // sigsuspend always reports EINTR; it is never retried
  acquire(&p->lock);
  p->sigsaved = p->sigblocked;
  p->sigsuspended = 1;
  p->sigblocked = set & ~SIG_UNCATCHABLE;
  release(&p->lock);
  while (!signal_pending(p) && !killed(p)) {
    sleep_prepare(&p->sigpending);
    sleep_intr();
  }
  return -EINTR;
}

uint64
sys_sigaltstack(void)
{
  uint64 addr, oldaddr;
  argaddr(0, &addr);
  argaddr(1, &oldaddr);
  struct proc *p = myproc();
  stack_t ss, old = p->sigaltstk;
  if (oldaddr && copyout(p->mm, oldaddr, (char *)&old, sizeof(old)) < 0)
    return -EFAULT;
  if (addr == 0)
    return 0;
  if (copyin(p->mm, (char *)&ss, addr, sizeof(ss)) < 0)
    return -EFAULT;
  if (ss.ss_flags & SS_DISABLE) {
    p->sigaltstk.ss_flags = SS_DISABLE;
    p->sigaltstk.ss_sp = 0;
    p->sigaltstk.ss_size = 0;
    return 0;
  }
  if (ss.ss_size < MINSIGSTKSZ)
    return -EINVAL;
  ss.ss_flags = 0;
  p->sigaltstk = ss;
  return 0;
}

uint64
sys_kill(void)
{
  int pid, sig;
  argint(0, &pid);
  argint(1, &sig);
  return signal_kill(pid, sig, 0);
}

uint64
sys_sigqueue(void)
{
  int pid, sig, val;
  argint(0, &pid);
  argint(1, &sig);
  argint(2, &val);
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  info.si_code = SI_QUEUE;
  info.si_pid = myproc()->pid;
  info.si_value.sival_int = val;
  return signal_kill(pid, sig, &info);
}

// alarm(seconds): a SIGALRM after that many seconds, 0 cancels.
uint64
sys_alarm(void)
{
  int secs;
  argint(0, &secs);
  struct proc *p = myproc();
  acquire(&tickslock);
  uint64 now = ticks;
  release(&tickslock);
  uint64 left = 0;
  if (p->alarm_at > now)
    left = (p->alarm_at - now + tick_hz - 1) / tick_hz;
  p->alarm_at = secs ? now + (uint64)secs * tick_hz : 0;
  return left;
}

// called from the timer tick: deliver SIGALRM where it is due.
void
signal_timers(void)
{
  uint64 now;
  acquire(&tickslock);
  now = ticks;
  release(&tickslock);
  proc_iter_start();
  for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
    if (p->alarm_at && now >= p->alarm_at) {
      p->alarm_at = 0;
      signal_send(p, SIGALRM, 0);
    }
    if (p->futex_until && now >= p->futex_until)
      futex_timeout(p);
  }
  proc_iter_end();
}

uint64
sys_pause(void)
{
  int n;
  argint(0, &n);
  struct proc *p = myproc();
  p->norestart = 1; // a sleep that a signal cut short is not restarted
  if (n > 0) {
    // xv6's pause(ticks): sleep that many ticks, or until a signal
    acquire(&tickslock);
    uint64 until = ticks + n;
    while (ticks < until) {
      if (killed(p) || signal_pending(p)) {
        release(&tickslock);
        return -EINTR;
      }
      sleep_prepare(&ticks);
      release(&tickslock);
      sleep_intr();
      acquire(&tickslock);
    }
    release(&tickslock);
    return 0;
  }
  // pause(): until a signal is handled
  while (!signal_pending(p) && !killed(p)) {
    sleep_prepare(&p->sigpending);
    sleep_intr();
  }
  return -EINTR;
}
