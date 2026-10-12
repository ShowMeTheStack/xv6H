//
// interrupt controller abstraction: PLIC, APLIC (direct), or APLIC-MSI + IMSIC,
// chosen at boot from the device tree. also provides inter-processor
// interrupts (IPIs) on top of whichever controller is present.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"

// per-hart IPI mailbox: bitmask of IPI_* reasons, plus a counter for tests.
static volatile uint64 ipi_pending[NCPU];
volatile uint64 ipi_count[NCPU];

void
intc_init(void)
{
  switch (plat.intc_kind) {
  case INTC_PLIC:
    plicinit();
    break;
  case INTC_APLIC:
    aplic_init(0);
    break;
  case INTC_IMSIC:
    aplic_init(1);
    imsic_init();
    break;
  }
}

void
intc_inithart(void)
{
  switch (plat.intc_kind) {
  case INTC_PLIC:
    plicinithart();
    break;
  case INTC_APLIC:
    aplic_inithart();
    break;
  case INTC_IMSIC:
    imsic_inithart();
    break;
  }
}

// returns the pending irq number (interrupt identity) to service, or 0.
int
intc_claim(void)
{
  switch (plat.intc_kind) {
  case INTC_PLIC:
    return plic_claim();
  case INTC_APLIC:
    return aplic_claim();
  case INTC_IMSIC:
    return imsic_claim();
  }
  return 0;
}

void
intc_complete(int irq)
{
  switch (plat.intc_kind) {
  case INTC_PLIC:
    plic_complete(irq);
    break;
  case INTC_APLIC:
    aplic_complete(irq);
    break;
  case INTC_IMSIC:
    if (irq != IMSIC_IPI_ID)
      aplic_complete(irq); // re-arm level sources
    break;
  }
}

// send an IPI to hart with the given reason bit(s).
void
intc_ipi(int hart, uint64 reason)
{
  if (hart < 0 || hart >= NCPU)
    return;
  __atomic_fetch_or(&ipi_pending[hart], reason, __ATOMIC_SEQ_CST);
  if (plat.intc_kind == INTC_IMSIC) {
    imsic_ipi(hart);
  } else if (plat.clint_base) {
    // write the target hart's CLINT msip; its machine-mode stub (mvec.S)
    // converts the resulting MSIP into a supervisor software interrupt.
    *(volatile uint32 *)(plat.clint_base + 4 * plat.hartid[hart]) = 1;   // F9: by hart id
  } else {
    machine_sbi_ipi(plat.hartid[hart]);   // F4: a machine without a CLINT asks its firmware
  }
}

// called from devintr() on a supervisor software interrupt (or the IMSIC
// IPI identity). returns the reasons that were pending.
uint64
ipi_handle(void)
{
  int hart = cpuid();
  // SSIP is cleared whatever the controller. With an IMSIC our own IPIs come
  // as external interrupts (IMSIC_IPI_ID) and SSIP is never set by them -- but
  // it *is* how the hypervisor tells us the machine description changed, on
  // any controller (trap.c: "nobody asked for this one"). Left set on an IMSIC
  // guest, that one notification re-trapped forever: a revision of anything
  // but the controller itself (irqmap) spun the guest in cause-1 traps,
  // one MACH_PENDING ecall each (11 661 of them in the probe).
  c_sip(SIP_SSIP);
  uint64 reasons = __atomic_exchange_n(&ipi_pending[hart], 0, __ATOMIC_SEQ_CST);
  ipi_count[hart]++;
  return reasons;
}
