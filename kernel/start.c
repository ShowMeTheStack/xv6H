#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"

void main();
void timerinit();
void mvec();

// entry.S needs one stack per CPU.
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// mvec.S needs a small machine-mode stack per CPU.
#define MSTACK 512
__attribute__((aligned(16))) char mstack0[MSTACK * NCPU];

// entry.S jumps here in machine mode on stack0,
// with a0 = hartid and a1 = physical address of the device tree.
//
// no canary of its own: it is where the canary is chosen, and a
// function cannot check on the way out a value that changed while it
// was inside (kernel/canary.c).
__attribute__((no_stack_protector)) void
start(uint64 hartid, uint64 fdt)
{
  canary_init(hartid);

  // hart 0 learns about the machine from the device tree and configures
  // the machine-level parts of the interrupt controller; the other harts
  // wait for it (they may well get here first).
  if (hartid == 0) {
    platform_init(fdt);
    aplic_minit();
  } else {
    while (plat.ready == 0)
      ;
  }

  // set M Previous Privilege mode to Supervisor, for mret.
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  w_mepc((uint64)main);

  // disable paging for now.
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  // with the hypervisor extension, also delegate the guest-related
  // exceptions (20..23) and the guest external interrupt (12).
  uint64 medeleg = 0xffff;
  uint64 mideleg = 0xffff;
  if (plat.has_h) {
    medeleg |= (1L << 20) | (1L << 21) | (1L << 22) | (1L << 23);
    mideleg |= (1L << 12);
  }
  w_medeleg(medeleg);
  w_mideleg(mideleg);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

  // machine-mode traps that are not delegated (only MSIP in practice)
  // go to mvec.S, on a small per-hart machine stack.
  w_mscratch((uint64)&mstack0[MSTACK * (hartid + 1)]);
  w_mtvec((uint64)mvec);
  w_mie(r_mie() | MIE_MSIE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // enable hardware updates of page table A and D bits
  w_menvcfg(r_menvcfg() | MENVCFG_ADUE);

  // ask for clock interrupts.
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  w_tp(hartid);

  // switch to supervisor mode and jump to main().
  asm volatile("mret");
}

// ask each hart to generate timer interrupts.
void
timerinit()
{
  // enable the sstc extension (i.e. stimecmp).
  w_menvcfg(r_menvcfg() | MENVCFG_STCE);

  // allow supervisor (and guests) to use stimecmp, time, cycle, instret.
  w_mcounteren(r_mcounteren() | 7);

  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
