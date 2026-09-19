#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
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
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if ((which_dev = devintr()) != 0) {
    // ok
  } else if ((r_scause() == 15 || r_scause() == 13) &&
             vmfault(p->pagetable, p->sz, r_stval(),
                     (r_scause() == 13) ? 1 : 0) != 0) {
    // page fault on lazily-allocated page
  } else {
    printk("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printk("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if (killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
    yield();

  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);

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
  w_sscratch(TRAPFRAME); // the trampoline finds the trapframe here
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
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

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if (cpuid() == 0) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // let the hypervisor wake guests whose virtual timers expired.
  hyp_tick();
  if (cpuid() == 0)
    net_timer();

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second; an idle guest's timer may need to fire sooner.
  uint64 now = r_time();
  uint64 next = hyp_next_deadline(now + 1000000);
  if (next < now + 100)
    next = now + 100;
  w_stimecmp(next);
}

// dispatch one external interrupt identity from the interrupt controller.
static void
extintr(int irq)
{
  int idx;
  if (plat.intc_kind == INTC_IMSIC && irq == IMSIC_IPI_ID) {
    ipi_handle();
  } else if (irq == plat.uart_irq) {
    uartintr();
  } else if ((idx = platform_virtio_by_irq(irq)) >= 0) {
    virtio_disk_intr(idx);
    virtio_net_intr(idx);
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
    ipi_handle();
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
