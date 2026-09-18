//
// RISC-V Incoming MSI Controller (IMSIC) driver: the supervisor-level
// interrupt file of each hart, accessed through the indirect CSRs
// siselect/sireg, and claimed through stopei.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"
#include "imsic.h"

static inline void
w_siselect(uint64 x)
{
  asm volatile("csrw 0x150, %0" : : "r"(x));
}

static inline void
w_sireg(uint64 x)
{
  asm volatile("csrw 0x151, %0" : : "r"(x));
}

static inline void
s_sireg(uint64 x)
{
  asm volatile("csrs 0x151, %0" : : "r"(x));
}

// read-and-claim the top pending interrupt: csrrw rd, stopei, x0.
static inline uint64
claim_stopei(void)
{
  uint64 x;
  asm volatile("csrrw %0, 0x15c, x0" : "=r"(x));
  return x;
}

// address of the supervisor-level interrupt file of a hart
// (guest == 0), or of one of its guest interrupt files (guest >= 1).
uint64
imsic_file_addr(int hart, int guest)
{
  uint64 stride = 0x1000L << plat.imsic_guest_bits;
  return plat.imsic_s_base + hart * stride + guest * 0x1000L;
}

void
imsic_init(void)
{
  if (plat.imsic_s_base == 0)
    panic("imsic: no supervisor files");
}

static void
enable_id(int id)
{
  if (id <= 0 || id >= plat.imsic_nids)
    return;
  w_siselect(IMSIC_EIE0 + 2 * (id / 64));
  s_sireg(1L << (id % 64));
}

void
imsic_inithart(void)
{
  w_siselect(IMSIC_EIDELIVERY);
  w_sireg(0);
  w_siselect(IMSIC_EITHRESHOLD);
  w_sireg(0);

  enable_id(plat.uart_irq);
  for (int i = 0; i < plat.nvirtio; i++)
    enable_id(plat.virtio_irq[i]);
  enable_id(IMSIC_IPI_ID);

  w_siselect(IMSIC_EIDELIVERY);
  w_sireg(1);
}

// returns the claimed interrupt identity, or 0 if none pending.
int
imsic_claim(void)
{
  return (int)(claim_stopei() >> 16);
}

// send an IPI: write our IPI identity into the target hart's S file.
void
imsic_ipi(int hart)
{
  *(volatile uint32 *)imsic_file_addr(hart, 0) = IMSIC_IPI_ID;
}
