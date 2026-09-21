//
// ptrace: watching a process from its parent.
//
// what is here is the subset section 14 of the plan asks for --
// TRACEME, SYSCALL, GETREGS, SETREGS, PEEKDATA, POKEDATA, CONT, KILL
// and DETACH -- which together are enough for strace to run a command
// and print every call it makes, and for a debugger to read and change
// what a stopped process is doing.
//
// what is deliberately not here is ATTACH. Attaching means taking a
// process away from its parent and putting it under the tracer, so
// that the tracer's wait() sees its stops; that is a change to who a
// process belongs to, and it is the part of ptrace that lets one
// program reach into another it did not start. TRACEME keeps the rule
// simple and true: a tracer is the parent, and a process is only ever
// watched by something that made it.
//
// the mechanics reuse the job-control stop the kernel already has. a
// traced process on its way into or out of a system call marks itself
// STOPPED with SIGTRAP, tells its parent, and switches away; the
// parent's wait() with WUNTRACED sees it, looks at or changes the
// registers, and lets it go again. so a syscall stop and a ^Z are the
// same kind of thing to everything below this file.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "defs.h"
#include "errno.h"
#include "signal.h"
#include "ptrace.h"
#include "mm.h"

// the registers, in the order Linux's riscv64 port hands them over
struct user_regs_struct {
  uint64 pc;
  uint64 ra, sp, gp, tp;
  uint64 t0, t1, t2;
  uint64 s0, s1;
  uint64 a0, a1, a2, a3, a4, a5, a6, a7;
  uint64 s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
  uint64 t3, t4, t5, t6;
};

static void
regs_out(struct trapframe *tf, struct user_regs_struct *r)
{
  r->pc = tf->epc;
  r->ra = tf->ra;   r->sp = tf->sp;   r->gp = tf->gp;   r->tp = tf->tp;
  r->t0 = tf->t0;   r->t1 = tf->t1;   r->t2 = tf->t2;
  r->s0 = tf->s0;   r->s1 = tf->s1;
  r->a0 = tf->a0;   r->a1 = tf->a1;   r->a2 = tf->a2;   r->a3 = tf->a3;
  r->a4 = tf->a4;   r->a5 = tf->a5;   r->a6 = tf->a6;   r->a7 = tf->a7;
  r->s2 = tf->s2;   r->s3 = tf->s3;   r->s4 = tf->s4;   r->s5 = tf->s5;
  r->s6 = tf->s6;   r->s7 = tf->s7;   r->s8 = tf->s8;   r->s9 = tf->s9;
  r->s10 = tf->s10; r->s11 = tf->s11;
  r->t3 = tf->t3;   r->t4 = tf->t4;   r->t5 = tf->t5;   r->t6 = tf->t6;
}

static void
regs_in(struct trapframe *tf, const struct user_regs_struct *r)
{
  tf->epc = r->pc;
  tf->ra = r->ra;   tf->sp = r->sp;   tf->gp = r->gp;   tf->tp = r->tp;
  tf->t0 = r->t0;   tf->t1 = r->t1;   tf->t2 = r->t2;
  tf->s0 = r->s0;   tf->s1 = r->s1;
  tf->a0 = r->a0;   tf->a1 = r->a1;   tf->a2 = r->a2;   tf->a3 = r->a3;
  tf->a4 = r->a4;   tf->a5 = r->a5;   tf->a6 = r->a6;   tf->a7 = r->a7;
  tf->s2 = r->s2;   tf->s3 = r->s3;   tf->s4 = r->s4;   tf->s5 = r->s5;
  tf->s6 = r->s6;   tf->s7 = r->s7;   tf->s8 = r->s8;   tf->s9 = r->s9;
  tf->s10 = r->s10; tf->s11 = r->s11;
  tf->t3 = r->t3;   tf->t4 = r->t4;   tf->t5 = r->t5;   tf->t6 = r->t6;
}

// ---- stopping ----

// the tracee puts itself down and tells its parent. called from the
// tracee, holding no locks.
static void
trace_stop(struct proc *p, int sig)
{
  acquire(&p->lock);
  p->state = STOPPED;
  p->stopped_sig = sig;
  p->notify = CLD_STOPPED;
  release(&p->lock);

  proc_notify_parent(p);

  acquire(&p->lock);
  // the notify above may have been seen and answered already, in which
  // case this process is RUNNABLE and there is nothing to wait for.
  // `parked` says the switch is really about to happen: see
  // proc_make_runnable for what it is for.
  p->stop_parked = 1;
  while (p->state == STOPPED)
    sched_stopped();
  p->stop_parked = 0;
  release(&p->lock);
}

// on the way into a system call, and on the way out of one. syscall()
// calls these; they do nothing at all for a process nobody is
// watching, which is the common case and has to stay cheap.
void
ptrace_syscall_entry(struct proc *p)
{
  if (!p->traced || !p->trace_wants_stop)
    return;
  p->trace_at_exit = 1;
  trace_stop(p, SIGTRAP);
}

void
ptrace_syscall_exit(struct proc *p)
{
  if (!p->traced || !p->trace_at_exit)
    return;
  p->trace_at_exit = 0;
  if (!p->trace_wants_stop)
    return;
  trace_stop(p, SIGTRAP);
}

// a traced process that has just replaced itself stops once before the
// new program's first instruction, so that a tracer can see what it
// became before it runs
void
ptrace_exec_stop(struct proc *p)
{
  if (!p->traced)
    return;
  p->trace_at_exit = 0;
  trace_stop(p, SIGTRAP);
}

// a process that is traced and goes away stops being traced
void
ptrace_exit(struct proc *p)
{
  p->traced = 0;
  p->trace_wants_stop = 0;
  p->trace_at_exit = 0;
}

// ---- the system call ----

// may this process look into that one? only its own parent, and only
// when the two are the same user -- or when the caller has the
// capability that lifts that rule.
static int
may_trace(struct proc *t)
{
  struct proc *p = myproc();

  if (t == p)
    return 0;
  if (proc_ppid(t) != p->pid)
    return 0;
  if (cap_ok(CAP_SYS_PTRACE))
    return 1;
  return t->cred.uid == p->cred.euid && t->cred.euid == p->cred.euid;
}

// the tracee must be stopped before its registers or its memory can be
// looked at: otherwise they are whatever they happened to be a moment
// ago, which is not an answer.
static int
must_be_stopped(struct proc *t)
{
  int ok;

  acquire(&t->lock);
  ok = t->state == STOPPED;
  release(&t->lock);
  return ok ? 0 : -ESRCH;
}

uint64
sys_ptrace(void)
{
  int request, pid;
  uint64 addr, data;
  struct proc *p = myproc();
  struct proc *t;
  int r = 0;

  argint(0, &request);
  argint(1, &pid);
  argaddr(2, &addr);
  argaddr(3, &data);

  if (request == PTRACE_TRACEME) {
    p->traced = 1;
    p->trace_wants_stop = 0;
    p->trace_at_exit = 0;
    return 0;
  }
  if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
    // see the note at the top: a tracer here is a parent, and
    // attaching would mean taking a process away from its own
    return -EPERM;
  }

  t = proc_get(pid);
  if (t == 0)
    return -ESRCH;
  if (!may_trace(t) || !t->traced) {
    proc_release(t);
    return -EPERM;
  }

  switch (request) {
  case PTRACE_GETREGS: {
    struct user_regs_struct regs;
    if ((r = must_be_stopped(t)) == 0) {
      regs_out(t->trapframe, &regs);
      if (copyout(p->mm, data, (char *)&regs, sizeof(regs)) < 0)
        r = -EFAULT;
    }
    break;
  }
  case PTRACE_SETREGS: {
    struct user_regs_struct regs;
    if ((r = must_be_stopped(t)) == 0) {
      if (copyin(p->mm, (char *)&regs, data, sizeof(regs)) < 0)
        r = -EFAULT;
      else
        regs_in(t->trapframe, &regs);
    }
    break;
  }
  case PTRACE_PEEKDATA:
  case PTRACE_PEEKTEXT: {
    uint64 word = 0;
    // the word goes through `data`, as the system call has always
    // taken it; the library is what turns that into a return value
    if ((r = must_be_stopped(t)) == 0) {
      if (data == 0)
        r = -EFAULT;
      else if (copyin(t->mm, (char *)&word, addr, sizeof(word)) < 0)
        r = -EIO;
      else if (copyout(p->mm, data, (char *)&word, sizeof(word)) < 0)
        r = -EFAULT;
    }
    break;
  }
  case PTRACE_POKEDATA:
  case PTRACE_POKETEXT: {
    if ((r = must_be_stopped(t)) == 0) {
      uint64 word = data;
      if (copyout(t->mm, addr, (char *)&word, sizeof(word)) < 0)
        r = -EIO;
    }
    break;
  }
  case PTRACE_SYSCALL:
  case PTRACE_CONT: {
    if ((r = must_be_stopped(t)) == 0) {
      t->trace_wants_stop = (request == PTRACE_SYSCALL);
      // a signal number in `data` is delivered as the tracee resumes,
      // which is how a tracer passes on what it intercepted
      if (data > 0 && data < NSIG)
        signal_send(t, (int)data, 0);
      acquire(&t->lock);
      t->notify = 0;
      proc_make_runnable(t);
      release(&t->lock);
    }
    break;
  }
  case PTRACE_DETACH: {
    if ((r = must_be_stopped(t)) == 0) {
      t->traced = 0;
      t->trace_wants_stop = 0;
      t->trace_at_exit = 0;
      acquire(&t->lock);
      t->notify = 0;
      proc_make_runnable(t);
      release(&t->lock);
    }
    break;
  }
  case PTRACE_KILL: {
    t->traced = 0;
    t->trace_wants_stop = 0;
    signal_send(t, SIGKILL, 0);
    acquire(&t->lock);
    proc_make_runnable(t);
    release(&t->lock);
    break;
  }
  default:
    r = -EINVAL;
    break;
  }

  proc_release(t);
  return r;
}
