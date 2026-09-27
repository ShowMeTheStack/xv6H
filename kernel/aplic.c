//
// RISC-V Advanced PLIC (APLIC) driver, per the AIA specification.
//
// qemu's virt machine has a machine-level root domain (0x0c000000) and one
// supervisor-level child domain (0x0d000000). since xv6 owns machine mode
// (no firmware), start() delegates every source from the root domain to the
// child, and configures the MSI address registers when an IMSIC is present.
// supervisor code then programs only the child domain.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"
#include "aplic.h"

#define R(base, off) ((volatile uint32 *)((base) + (off)))

static int aplic_msi; // 1 if the domain delivers MSIs to IMSICs

static int
log2ceil(int n)
{
  int b = 0;
  while ((1 << b) < n)
    b++;
  return b;
}

// machine mode, hart 0, before paging: set up the root domain.
void
aplic_minit(void)
{
  if (plat.intc_kind != INTC_APLIC && plat.intc_kind != INTC_IMSIC)
    return;
  uint64 m = plat.aplic_m_base;
  if (m == 0)
    return;

  if (plat.intc_kind == INTC_IMSIC) {
    // where should the APLIC write MSIs? machine files at imsic_m_base with
    // one page per hart (LHXS=0); supervisor files at imsic_s_base with
    // (1 << guest_index_bits) pages per hart (LHXS=guest_index_bits).
    // LHXW = number of hart-index bits; HHXW/HHXS = 0 (single group).
    uint32 lhxw = log2ceil(NCPU);
    uint64 mppn = plat.imsic_m_base >> 12;
    uint64 sppn = plat.imsic_s_base >> 12;
    *R(m, APLIC_MMSIADDRCFG) = (uint32)mppn;
    *R(m, APLIC_MMSIADDRCFGH) = (uint32)((mppn >> 32) & 0xfff) | (lhxw << 12) | (0 << 16) | (0 << 20) | (0 << 24);
    *R(m, APLIC_SMSIADDRCFG) = (uint32)sppn;
    *R(m, APLIC_SMSIADDRCFGH) = (uint32)((sppn >> 32) & 0xfff) | ((uint32)plat.imsic_guest_bits << 20);
  }

  // delegate all sources to child domain 0 (the supervisor domain).
  int nsrc = plat.aplic_nsrc ? plat.aplic_nsrc : 32;
  for (int i = 1; i < nsrc; i++)
    *R(m, APLIC_SOURCECFG(i)) = APLIC_SOURCECFG_D | 0;

  *R(m, APLIC_DOMAINCFG) = APLIC_DOMAINCFG_IE;
}

static void
setup_source(int irq)
{
  uint64 s = plat.aplic_s_base;
  if (irq <= 0)
    return;
  // qemu's uart and virtio-mmio lines are level-triggered, active high.
  *R(s, APLIC_SOURCECFG(irq)) = APLIC_SM_LEVEL1;
  if (aplic_msi) {
    // route to the boot hart (logical cpu 0 -- by its hart *id*, F9),
    // supervisor file (guest index 0), identity = irq.
    *R(s, APLIC_TARGET(irq)) = ((uint32)plat.hartid[0] << 18) | (0 << 12) | irq;
  } else {
    // route to hart 0 with priority 1.
    *R(s, APLIC_TARGET(irq)) = ((uint32)plat.hartid[0] << 18) | 1;   // direct mode: the boot hart, priority 1 (F9: by hart id)
  }
  *R(s, APLIC_SETIENUM) = irq;
}

void
aplic_init(int msi)
{
  aplic_msi = msi;
  uint64 s = plat.aplic_s_base;
  if (s == 0)
    panic("aplic: no supervisor domain");

  // disable while configuring.
  *R(s, APLIC_DOMAINCFG) = 0;

  setup_source(plat.uart_irq);
  for (int i = 0; i < plat.nvirtio; i++)
    setup_source(plat.virtio_irq[i]);

  *R(s, APLIC_DOMAINCFG) = APLIC_DOMAINCFG_IE | (msi ? APLIC_DOMAINCFG_DM : 0);
}

void
aplic_inithart(void)
{
  if (aplic_msi)
    return; // IMSIC does per-hart delivery
  int hart = plat.hartid[cpuid()];    // F9
  uint64 idc = plat.aplic_s_base + APLIC_IDC(hart);
  *R(idc, APLIC_IDC_ITHRESHOLD) = 0;
  *R(idc, APLIC_IDC_IDELIVERY) = 1;
}

// direct mode: read claimi, which returns (source << 16) | priority
// and clears the pending bit.
int
aplic_claim(void)
{
  int hart = plat.hartid[cpuid()];    // F9
  uint32 v = *R(plat.aplic_s_base + APLIC_IDC(hart), APLIC_IDC_CLAIMI);
  return v >> 16;
}

void
aplic_complete(int irq)
{
  // direct mode: nothing to do, level sources follow their input.
  // MSI mode: a level source's pending bit was cleared when the MSI was
  // sent; writing setipnum re-pends it if the input is still asserted.
  if (aplic_msi && irq > 0 && irq < (plat.aplic_nsrc ? plat.aplic_nsrc : 32))
    *R(plat.aplic_s_base, APLIC_SETIPNUM_LE) = irq;
}
