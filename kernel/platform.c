//
// fill in struct platform from the device tree.
// called once, by hart 0, in machine mode, before anything else runs.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "fdt.h"
#include "platform.h"

struct platform plat;
__attribute__((aligned(16))) char fdt_copy[FDT_COPY_MAX];

// used by mvec.S (machine-mode IPI relay) to clear msip.
uint64 clint_msip_base;

// accumulated properties of the node currently being parsed.
struct nodeacc {
  int pending;
  int depth;
  const char *name;
  const void *compat;
  uint32 compat_len;
  uint64 reg[8];
  int nreg;
  uint32 irqs[4];
  int nirq;
  uint32 iext[16]; // interrupts-extended (phandle, cause) pairs
  int niext;
  uint32 gib;      // riscv,guest-index-bits
  uint32 numids;   // riscv,num-ids
  uint32 nsrc;     // riscv,num-sources
  int has_children;
};

static int
streq(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void
add_virtio(uint64 base, int irq)
{
  if (plat.nvirtio >= PLAT_MAXVIRTIO)
    return;
  // keep sorted by base address (qemu lists slots highest first).
  int i = plat.nvirtio;
  while (i > 0 && plat.virtio_base[i - 1] > base) {
    plat.virtio_base[i] = plat.virtio_base[i - 1];
    plat.virtio_irq[i] = plat.virtio_irq[i - 1];
    i--;
  }
  plat.virtio_base[i] = base;
  plat.virtio_irq[i] = irq;
  plat.nvirtio++;
}

static void
finalize(struct nodeacc *n)
{
  n->pending = 0;
  if (n->depth == 0)
    return;
  if (fdt_name_is(n->name, "memory") && n->nreg >= 2) {
    plat.ram_base = n->reg[0];
    plat.ram_size = n->reg[1];
    return;
  }
  if (n->compat == 0)
    return;
  if (fdt_strlist_has(n->compat, n->compat_len, "riscv,plic0") ||
      fdt_strlist_has(n->compat, n->compat_len, "sifive,plic-1.0.0")) {
    plat.plic_base = n->reg[0];
    plat.plic_size = n->reg[1];
    if (plat.intc_kind < INTC_PLIC)
      plat.intc_kind = INTC_PLIC;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "riscv,aplic")) {
    if (n->has_children)
      plat.aplic_m_base = n->reg[0];
    else
      plat.aplic_s_base = n->reg[0];
    plat.aplic_size = n->reg[1];
    plat.aplic_nsrc = n->nsrc;
    if (plat.intc_kind < INTC_APLIC)
      plat.intc_kind = INTC_APLIC;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "riscv,imsics")) {
    // interrupts-extended cause 9 = S-mode file, 11 = M-mode file.
    int is_s = (n->niext >= 2 && n->iext[1] == 9);
    if (is_s) {
      plat.imsic_s_base = n->reg[0];
      plat.imsic_s_size = n->reg[1];
      plat.imsic_guest_bits = n->gib;
      plat.imsic_nids = n->numids;
    } else {
      plat.imsic_m_base = n->reg[0];
    }
    plat.intc_kind = INTC_IMSIC;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "ns16550a")) {
    plat.uart_base = n->reg[0];
    plat.uart_irq = n->nirq ? n->irqs[0] : 0;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "virtio,mmio")) {
    add_virtio(n->reg[0], n->nirq ? n->irqs[0] : 0);
  } else if (fdt_strlist_has(n->compat, n->compat_len, "riscv,clint0") ||
             fdt_strlist_has(n->compat, n->compat_len, "sifive,clint0")) {
    plat.clint_base = n->reg[0];
  }
}

static void
parse(const void *fdt)
{
  struct fdt_iter it;
  struct fdt_item item;
  struct nodeacc acc;

  acc.pending = 0;
  fdt_iter_init(&it, fdt);
  while (fdt_iter_next(&it, &item)) {
    if (item.tok == FDT_BEGIN_NODE) {
      if (acc.pending) {
        acc.has_children = 1;
        finalize(&acc);
      }
      acc.pending = 1;
      acc.depth = item.depth;
      acc.name = item.name;
      acc.compat = 0;
      acc.compat_len = 0;
      acc.nreg = 0;
      acc.nirq = 0;
      acc.niext = 0;
      acc.gib = 0;
      acc.numids = 0;
      acc.nsrc = 0;
      acc.has_children = 0;
      if (item.depth == 2 && fdt_name_is(item.name, "cpu@"))
        plat.ncpu++;
    } else if (item.tok == FDT_END_NODE) {
      if (acc.pending)
        finalize(&acc);
    } else if (item.tok == FDT_PROP) {
      if (item.depth == 0) {
        // root-node properties: the hypervisor marks guests here.
        if (streq(item.prop, "xv6h,guest"))
          plat.is_guest = 1;
        else if (streq(item.prop, "xv6h,vmid") && item.len >= 4)
          plat.guest_id = fdt32(item.data);
        continue;
      }
      if (!acc.pending)
        continue;
      if (streq(item.prop, "compatible")) {
        acc.compat = item.data;
        acc.compat_len = item.len;
      } else if (streq(item.prop, "reg")) {
        // assume #address-cells = #size-cells = 2 (qemu virt, and our guest DTB).
        uint32 n = item.len / 8;
        if (n > 8)
          n = 8;
        for (uint32 i = 0; i < n; i++)
          acc.reg[i] = fdt64((const char *)item.data + 8 * i);
        acc.nreg = n;
      } else if (streq(item.prop, "interrupts")) {
        uint32 n = item.len / 4;
        if (n > 4)
          n = 4;
        for (uint32 i = 0; i < n; i++)
          acc.irqs[i] = fdt32((const char *)item.data + 4 * i);
        acc.nirq = n;
      } else if (streq(item.prop, "interrupts-extended")) {
        uint32 n = item.len / 4;
        if (n > 16)
          n = 16;
        for (uint32 i = 0; i < n; i++)
          acc.iext[i] = fdt32((const char *)item.data + 4 * i);
        acc.niext = n;
      } else if (streq(item.prop, "riscv,guest-index-bits")) {
        acc.gib = fdt32(item.data);
      } else if (streq(item.prop, "riscv,num-ids")) {
        acc.numids = fdt32(item.data);
      } else if (streq(item.prop, "riscv,num-sources")) {
        acc.nsrc = fdt32(item.data);
      } else if (streq(item.prop, "riscv,children")) {
        acc.has_children = 1;
      }
    }
  }
}

static void
defaults(void)
{
  // used only if there is no usable device tree (should not happen on qemu).
  plat.ram_base = KERNBASE;
  plat.ram_size = 128L * 1024 * 1024;
  plat.ncpu = NCPU;
  plat.intc_kind = INTC_PLIC;
  plat.plic_base = PLIC;
  plat.plic_size = 0x4000000;
  plat.clint_base = CLINT_BASE;
  plat.uart_base = UART0;
  plat.uart_irq = UART0_IRQ;
  plat.nvirtio = 1;
  plat.virtio_base[0] = VIRTIO0;
  plat.virtio_irq[0] = VIRTIO0_IRQ;
}

void
platform_init(uint64 fdt_pa)
{
  const void *fdt = (const void *)fdt_pa;

  plat.intc_kind = -1;
  if (fdt_valid(fdt) && fdt_totalsize(fdt) <= FDT_COPY_MAX) {
    // the blob lives near the top of RAM where the allocator will
    // scribble; keep a private copy.
    uint32 sz = fdt_totalsize(fdt);
    for (uint32 i = 0; i < sz; i++)
      fdt_copy[i] = ((const char *)fdt)[i];
    plat.fdt_addr = fdt_pa;
    plat.fdt_size = sz;
    parse(fdt_copy);
  }
  if (plat.ram_size == 0 || plat.intc_kind < 0 || plat.uart_base == 0)
    defaults();

  plat.ram_end = plat.ram_base + plat.ram_size;
  plat.host_top = plat.ram_base + HOSTMEM;
  if (plat.host_top > plat.ram_end)
    plat.host_top = plat.ram_end;
  if (plat.ncpu <= 0 || plat.ncpu > NCPU)
    plat.ncpu = NCPU;
  if (plat.clint_base == 0)
    plat.clint_base = CLINT_BASE;
  clint_msip_base = plat.clint_base;

  plat.misa = r_misa();
  plat.has_h = (plat.misa >> 7) & 1;

  __sync_synchronize();
  plat.ready = 1;
}

int
platform_virtio_by_irq(int irq)
{
  for (int i = 0; i < plat.nvirtio; i++)
    if (plat.virtio_irq[i] == irq)
      return i;
  return -1;
}
