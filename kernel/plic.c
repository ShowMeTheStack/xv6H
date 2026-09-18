#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
// base address comes from the device tree.
//

#define PRIORITY(irq)     (plat.plic_base + (irq) * 4)
#define SENABLE(hart)     (plat.plic_base + 0x2080 + (hart) * 0x100)
#define SPRIORITY(hart)   (plat.plic_base + 0x201000 + (hart) * 0x2000)
#define SCLAIM(hart)      (plat.plic_base + 0x201004 + (hart) * 0x2000)

static uint32 irqmask; // irqs we enable on every hart

void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  irqmask = 0;
  *(uint32 *)PRIORITY(plat.uart_irq) = 1;
  irqmask |= 1 << plat.uart_irq;
  for (int i = 0; i < plat.nvirtio; i++) {
    *(uint32 *)PRIORITY(plat.virtio_irq[i]) = 1;
    irqmask |= 1 << plat.virtio_irq[i];
  }
}

void
plicinithart(void)
{
  int hart = cpuid();

  // set enable bits for this hart's S-mode for the uart and virtio disks.
  *(uint32 *)SENABLE(hart) = irqmask;

  // set this hart's S-mode priority threshold to 0.
  *(uint32 *)SPRIORITY(hart) = 0;
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32 *)SCLAIM(hart);
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32 *)SCLAIM(hart) = irq;
}
