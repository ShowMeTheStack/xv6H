#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"
#include "mm.h"
#include "signal.h"

struct spinlock tickslock;
uint ticks;

// how often the timer interrupts, and the same figure in the units
// the time counter uses. Everything the kernel counts in ticks --
// alarms, poll and select deadlines, futex waits, the writeback and
// journal timers, the processor time a process is charged -- is in
// units of one of these, so changing the rate changes what an already
// counted tick meant. It is a thing to set at boot and leave alone;
// it is settable because the plan asks for it and because the way to
// find out what a hundred hertz costs is to be able to try ten.
int tick_hz = TICK_HZ;
uint64 tick_interval;

void
tickinit(void)
{
  uint64 hz = plat.timebase ? plat.timebase : 10000000;

  tick_interval = hz / (tick_hz > 0 ? tick_hz : 1);
}

// change the rate. The next interrupt on each hart is the first one
// at the new interval.
int
tick_set_hz(int hz)
{
  if (hz < 1 || hz > 10000)
    return -1;
  tick_hz = hz;
  tickinit();
  return 0;
}

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
  tickinit();
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
  w_scounteren(7); // user programs may read cycle, time and instret
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64
usertrap(void)
{
  int which_dev = 0;

  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec); //DOC: kernelvec

  struct proc *p = myproc();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  // the vector registers are saved here rather than in the trampoline
  // (nothing in the kernel touches them); only if the process used them.
  if (p->vstate && (r_sstatus() & SSTATUS_VS_MASK) == SSTATUS_VS_DIRTY)
    vec_save(p->vstate, p->trapframe->vcsrs);

  if (r_scause() == 8) {
    // system call

    if (killed(p))
      kexit(0);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if ((which_dev = devintr()) != 0) {
    // ok
  } else if ((r_scause() == 15 || r_scause() == 13 || r_scause() == 12) &&
             mm_fault(p->mm, r_stval(), r_scause() == 15) == 0) {
    // a page brought in on first touch, or copied on a first write
  } else {
    // a fault the process must answer for
    uint64 cause = r_scause(), addr = r_stval();
    int sig = SIGSEGV, code = SEGV_MAPERR;
    if (cause == 2) {
      sig = SIGILL;
      code = ILL_ILLOPC;
    } else if (cause == 1 || cause == 5 || cause == 7) {
      sig = SIGBUS;
      code = BUS_ADRERR;
    } else if (cause == 3) {
      sig = SIGTRAP;
      code = 0;
    } else if (cause != 12 && cause != 13 && cause != 15) {
      printk("usertrap(): unexpected scause 0x%lx pid=%d\n", cause, p->pid);
      printk("            sepc=0x%lx stval=0x%lx\n", r_sepc(), addr);
    }
    signal_fault(p, sig, code, addr);
  }

  if (killed(p))
    kexit(0);

  // a tick is not by itself a reason to give up the processor: the
  // scheduler says when this task has had its slice, or when
  // something with less virtual time is waiting (sched.c). Nor is
  // every timer interrupt a tick any more -- the timer also ends
  // slices -- so what is charged is the tick the interrupt crossed.
  if (which_dev == 2 && mycpu()->tick) {
    mycpu()->tick = 0;
    p->utime++; // the tick interrupted user code
  }
  {
    int r = mycpu()->resched;
    if (r || (which_dev == 2 && sched_tick_expired())) {
      sched_count_yield(r);
      yield();
    }
  }

  // deliver a pending signal before going back to user space
  if (signal_pending(p))
    do_signals();

  if (killed(p))
    kexit(0);

  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->mm->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
}

//
// set up trapframe and control registers for a return to user space
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_sscratch(p->tf_va); // the trampoline finds this thread's trapframe here
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + KSTACK_PAGES * PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

  // restore the process's vector state (zeros for a process that never
  // used it) and mark the unit Clean.
  if (p->vstate) {
    w_sstatus((r_sstatus() & ~SSTATUS_VS_MASK) | SSTATUS_VS_INITIAL);
    vec_restore(p->vstate, p->trapframe->vcsrs);
    w_sstatus((r_sstatus() & ~SSTATUS_VS_MASK) | SSTATUS_VS_CLEAN);
  }

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0) {
    // interrupt or trap from an unknown source
    printk("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(),
           r_stval());
    panic("kerneltrap");
  }

  if (which_dev == 2 && mycpu()->tick) {
    mycpu()->tick = 0;
    struct proc *p = myproc();
    if (p)
      p->stime++; // the tick interrupted this task in the kernel
  }

  // the same in the kernel: only when the slice is up or somebody
  // asked for this hart
  if (myproc() != 0) {
    int r = mycpu()->resched;
    if (r || (which_dev == 2 && sched_tick_expired())) {
      sched_count_yield(r);
      yield();
    }
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

// when this hart owes the world its next tick. The timer may be asked
// to fire sooner than that -- for the end of a task's slice, or for a
// guest whose own timer is due -- so the tick is a deadline that is
// counted when it passes, not every time the timer goes off.
static uint64 next_tick[NCPU];
static uint64 programmed[NCPU];   // what stimecmp is set to

// program the timer for the first of: the tick this hart owes, the
// end of the running task's slice, and anything a guest is waiting
// for. Called from the timer interrupt and whenever a task goes on
// the processor with a slice that ends sooner than the timer is set.
void
timer_rearm(void)
{
  int me = cpuid();
  uint64 now = r_time();

  if (next_tick[me] <= now)
    next_tick[me] = now + tick_interval;
  // a hart with nothing to run has no reason to be woken on a
  // schedule: what gives it something to do is an interrupt, and an
  // interrupt ends wfi whether a timer was set or not. Hart 0 keeps
  // ticking, because it is the one that counts `ticks' and runs the
  // timers everything else waits on.
  if (me != 0 && sched_hart_idle())
    next_tick[me] = now + tick_interval * 64;
  uint64 next = next_tick[me];
  uint64 slice = sched_slice_deadline();
  if (slice > now && slice < next)
    next = slice;
  next = hyp_next_deadline(next);
  if (next < now + 100)
    next = now + 100;
  programmed[me] = next;
  w_stimecmp(next);
}

// a task has just gone on the processor. If its slice ends before the
// timer is already set to fire there is nothing to do: an interrupt
// that comes early costs one trip through clockintr, which re-arms
// properly, and writing stimecmp and walking the guests' timers on
// every context switch costs more than that comes to.
void
timer_slice(uint64 deadline)
{
  int me = cpuid();

  if (deadline && deadline < programmed[me])
    timer_rearm();
}

void
clockintr()
{
  int me = cpuid();
  uint64 now = r_time();

  // a tick is owed only when one has actually gone by: the timer is
  // also used for slice ends, which are far more frequent.
  if (now >= next_tick[me]) {
    next_tick[me] = now + tick_interval;
    mycpu()->tick = 1;    // one to charge whoever this interrupted
    sched_balance();      // work waiting here and a hart idle next door
    if (me == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
      net_timer();
      signal_timers();      // SIGALRM where it is due
      sched_bandwidth_tick(); // groups whose period has turned over
    }
  }

  // let the hypervisor wake guests whose virtual timers expired.
  hyp_tick();

  timer_rearm();
}

// dispatch one external interrupt identity from the interrupt controller.
static void
extintr(int irq)
{
  int idx;
  if (plat.intc_kind == INTC_IMSIC && irq == IMSIC_IPI_ID) {
    uint64 reasons = ipi_handle();
    if (reasons & IPI_TLBFLUSH)
      kstack_flush_ack();
    if (reasons & IPI_RESCHED)
      mycpu()->resched = 1;
  } else if (irq == plat.uart_irq) {
    uartintr();
  } else if ((idx = platform_virtio_by_irq(irq)) >= 0) {
    virtio_disk_intr(idx);
    virtio_net_intr(idx);
    virtio_balloon_intr(idx);
  } else {
    printk("unexpected interrupt irq=%d\n", irq);
  }
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if (scause == 0x8000000000000009L) {
    // this is a supervisor external interrupt, via the PLIC/APLIC/IMSIC.
    // irq indicates which device interrupted. the controller may have
    // several pending; drain them.
    int irq;
    while ((irq = intc_claim()) != 0) {
      extintr(irq);
      // the PLIC allows each device to raise at most one
      // interrupt at a time; tell it the device is
      // now allowed to interrupt again.
      intc_complete(irq);
    }
    return 1;
  } else if (scause == 0x8000000000000001L) {
    // supervisor software interrupt: an IPI from another hart.
    uint64 reasons = ipi_handle();
    if (reasons & IPI_TLBFLUSH)
      kstack_flush_ack();
    if (reasons & IPI_RESCHED)
      mycpu()->resched = 1;
    return 1;
  } else if (scause == 0x8000000000000005L) {
    // timer interrupt.
    clockintr();
    return 2;
  } else if (scause == 0x800000000000000cL) {
    // supervisor guest external interrupt (IMSIC guest file).
    hyp_sgei();
    return 1;
  } else {
    return 0;
  }
}
