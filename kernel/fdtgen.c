//
// generate the device tree handed to a guest: what our virtual machine
// looks like, in the same dialect qemu's virt machine uses, so that the
// guest's platform.c parses it with the same code as on real qemu.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "fdt.h"
#include "platform.h"
#include "hyp.h"

struct fdtw {
  char *st;   // structure block
  int stpos, stmax;
  char *ss;   // strings block
  int sspos, ssmax;
  int err;
};

static void
put32(struct fdtw *w, uint32 v)
{
  if (w->stpos + 4 > w->stmax) {
    w->err = 1;
    return;
  }
  w->st[w->stpos++] = v >> 24;
  w->st[w->stpos++] = v >> 16;
  w->st[w->stpos++] = v >> 8;
  w->st[w->stpos++] = v;
}

static void
putbytes(struct fdtw *w, const void *p, int n)
{
  int padded = (n + 3) & ~3;
  if (w->stpos + padded > w->stmax) {
    w->err = 1;
    return;
  }
  memmove(w->st + w->stpos, p, n);
  memset(w->st + w->stpos + n, 0, padded - n);
  w->stpos += padded;
}

static int
strofs(struct fdtw *w, const char *s)
{
  int n = strlen(s) + 1;
  // reuse an existing copy if present.
  for (int i = 0; i + n <= w->sspos; i++) {
    if (memcmp(w->ss + i, s, n) == 0)
      return i;
  }
  if (w->sspos + n > w->ssmax) {
    w->err = 1;
    return 0;
  }
  memmove(w->ss + w->sspos, s, n);
  int off = w->sspos;
  w->sspos += n;
  return off;
}

static void
node_begin(struct fdtw *w, const char *name)
{
  put32(w, FDT_BEGIN_NODE);
  putbytes(w, name, strlen(name) + 1);
}

static void
node_end(struct fdtw *w)
{
  put32(w, FDT_END_NODE);
}

static void
prop(struct fdtw *w, const char *name, const void *data, int len)
{
  put32(w, FDT_PROP);
  put32(w, len);
  put32(w, strofs(w, name));
  putbytes(w, data, len);
}

static void
prop_str(struct fdtw *w, const char *name, const char *s)
{
  prop(w, name, s, strlen(s) + 1);
}

static void
prop_empty(struct fdtw *w, const char *name)
{
  prop(w, name, "", 0);
}

static void
be32(char *p, uint32 v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static void
prop_u32(struct fdtw *w, const char *name, uint32 v)
{
  char b[4];
  be32(b, v);
  prop(w, name, b, 4);
}

// cells: an array of big-endian u32 values
static void
prop_cells(struct fdtw *w, const char *name, uint32 *vals, int n)
{
  char b[64];
  if (n > 16) {
    w->err = 1;
    return;
  }
  for (int i = 0; i < n; i++)
    be32(b + 4 * i, vals[i]);
  prop(w, name, b, 4 * n);
}

static void
prop_reg(struct fdtw *w, uint64 base, uint64 size)
{
  uint32 c[4] = {(uint32)(base >> 32), (uint32)base, (uint32)(size >> 32), (uint32)size};
  prop_cells(w, "reg", c, 4);
}

#define CPU_PHANDLE(i) (10 + (i))

int
hyp_make_dtb(struct vm *vm, char *buf, int max)
{
  struct fdtw w;
  char *scratch = kalloc();
  if (scratch == 0)
    return -1;
  memset(&w, 0, sizeof(w));
  w.st = scratch;
  w.stmax = 3072;
  w.ss = scratch + 3072;
  w.ssmax = 1024;

  uint32 cells[32];
  char name[32];

  // phandles: cpu intcs 10.., plic 3, aplic S 4, aplic M 5, imsic S 6, imsic M 7
  int ic_phandle = (vm->vic_kind == VIC_PLIC) ? 3 : 4;
  int hart_bits = 0;
  while ((1 << hart_bits) < vm->nvcpu)
    hart_bits++;

  node_begin(&w, "");
  prop_u32(&w, "#address-cells", 2);
  prop_u32(&w, "#size-cells", 2);
  prop_str(&w, "compatible", "riscv-virtio");
  prop_str(&w, "model", "xv6h,vm");
  prop_empty(&w, "xv6h,guest");
  prop_u32(&w, "xv6h,vmid", vm->id);

  node_begin(&w, "chosen");
  if (vm->cfg.bootargs[0])
    prop_str(&w, "bootargs", vm->cfg.bootargs);
  prop_str(&w, "stdout-path", "/soc/serial@10000000");
  node_end(&w);

  // cpus
  node_begin(&w, "cpus");
  prop_u32(&w, "#address-cells", 1);
  prop_u32(&w, "#size-cells", 0);
  prop_u32(&w, "timebase-frequency", 10000000);
  for (int i = 0; i < vm->nvcpu; i++) {
    memmove(name, "cpu@", 4);
    if (i >= 10) {
      name[4] = '0' + i / 10;
      name[5] = '0' + i % 10;
      name[6] = 0;
    } else {
      name[4] = '0' + i;
      name[5] = 0;
    }
    node_begin(&w, name);
    prop_str(&w, "device_type", "cpu");
    prop_u32(&w, "reg", i);
    prop_str(&w, "status", "okay");
    prop_str(&w, "compatible", "riscv");
    prop_str(&w, "riscv,isa", "rv64imafdc_zicsr_zifencei_sstc");
    prop_str(&w, "riscv,isa-base", "rv64i");
    prop(&w, "riscv,isa-extensions", "i\0m\0a\0f\0d\0c\0zicsr\0zifencei\0sstc\0", 32);
    prop_str(&w, "mmu-type", "riscv,sv39");
    node_begin(&w, "interrupt-controller");
    prop_u32(&w, "#interrupt-cells", 1);
    prop_empty(&w, "interrupt-controller");
    prop_str(&w, "compatible", "riscv,cpu-intc");
    prop_u32(&w, "phandle", CPU_PHANDLE(i));
    node_end(&w);
    node_end(&w);
  }
  node_end(&w);

  // memory
  node_begin(&w, "memory@80000000");
  prop_str(&w, "device_type", "memory");
  prop_reg(&w, G_RAM_BASE, vm->ram_size);
  node_end(&w);

  // soc
  node_begin(&w, "soc");
  prop_u32(&w, "#address-cells", 2);
  prop_u32(&w, "#size-cells", 2);
  prop_str(&w, "compatible", "simple-bus");
  prop_empty(&w, "ranges");

  node_begin(&w, "clint@2000000");
  prop_str(&w, "compatible", "riscv,clint0");
  prop_reg(&w, G_CLINT_BASE, 0x10000);
  for (int i = 0; i < vm->nvcpu; i++) {
    cells[4 * i] = CPU_PHANDLE(i);
    cells[4 * i + 1] = 3;
    cells[4 * i + 2] = CPU_PHANDLE(i);
    cells[4 * i + 3] = 7;
  }
  prop_cells(&w, "interrupts-extended", cells, 4 * vm->nvcpu);
  node_end(&w);

  if (vm->vic_kind == VIC_PLIC) {
    node_begin(&w, "plic@c000000");
    prop_str(&w, "compatible", "riscv,plic0");
    prop_reg(&w, G_PLIC_BASE, G_PLIC_SIZE);
    prop_u32(&w, "riscv,ndev", 31);
    prop_empty(&w, "interrupt-controller");
    prop_u32(&w, "#interrupt-cells", 1);
    prop_u32(&w, "#address-cells", 0);
    for (int i = 0; i < vm->nvcpu; i++) {
      cells[4 * i] = CPU_PHANDLE(i);
      cells[4 * i + 1] = 11;
      cells[4 * i + 2] = CPU_PHANDLE(i);
      cells[4 * i + 3] = 9;
    }
    prop_cells(&w, "interrupts-extended", cells, 4 * vm->nvcpu);
    prop_u32(&w, "phandle", 3);
    node_end(&w);
  } else {
    // APLIC: machine domain (root, with the supervisor domain as its child)
    node_begin(&w, "interrupt-controller@c000000");
    prop_str(&w, "compatible", "riscv,aplic");
    prop_reg(&w, G_APLIC_M, 0x8000);
    prop_u32(&w, "riscv,num-sources", VAPLIC_NSRC);
    prop_empty(&w, "interrupt-controller");
    prop_u32(&w, "#interrupt-cells", 2);
    prop_u32(&w, "riscv,children", 4);
    cells[0] = 4;
    cells[1] = 1;
    cells[2] = VAPLIC_NSRC - 1;
    prop_cells(&w, "riscv,delegation", cells, 3);
    if (vm->vic_kind == VIC_IMSIC) {
      prop_u32(&w, "msi-parent", 7);
    } else {
      for (int i = 0; i < vm->nvcpu; i++) {
        cells[2 * i] = CPU_PHANDLE(i);
        cells[2 * i + 1] = 11;
      }
      prop_cells(&w, "interrupts-extended", cells, 2 * vm->nvcpu);
    }
    prop_u32(&w, "phandle", 5);
    node_end(&w);

    node_begin(&w, "interrupt-controller@d000000");
    prop_str(&w, "compatible", "riscv,aplic");
    prop_reg(&w, G_APLIC_S, 0x8000);
    prop_u32(&w, "riscv,num-sources", VAPLIC_NSRC);
    prop_empty(&w, "interrupt-controller");
    prop_u32(&w, "#interrupt-cells", 2);
    if (vm->vic_kind == VIC_IMSIC) {
      prop_u32(&w, "msi-parent", 6);
    } else {
      for (int i = 0; i < vm->nvcpu; i++) {
        cells[2 * i] = CPU_PHANDLE(i);
        cells[2 * i + 1] = 9;
      }
      prop_cells(&w, "interrupts-extended", cells, 2 * vm->nvcpu);
    }
    prop_u32(&w, "phandle", 4);
    node_end(&w);

    if (vm->vic_kind == VIC_IMSIC) {
      node_begin(&w, "interrupt-controller@24000000");
      prop_str(&w, "compatible", "riscv,imsics");
      prop_reg(&w, G_IMSIC_M, 0x1000L * vm->nvcpu);
      for (int i = 0; i < vm->nvcpu; i++) {
        cells[2 * i] = CPU_PHANDLE(i);
        cells[2 * i + 1] = 11;
      }
      prop_cells(&w, "interrupts-extended", cells, 2 * vm->nvcpu);
      prop_u32(&w, "riscv,num-ids", VIMSIC_NIDS - 1);
      prop_u32(&w, "riscv,guest-index-bits", 0);
      prop_u32(&w, "riscv,hart-index-bits", hart_bits);
      prop_u32(&w, "riscv,group-index-bits", 0);
      prop_empty(&w, "interrupt-controller");
      prop_empty(&w, "msi-controller");
      prop_u32(&w, "#msi-cells", 0);
      prop_u32(&w, "#interrupt-cells", 0);
      prop_u32(&w, "phandle", 7);
      node_end(&w);

      node_begin(&w, "interrupt-controller@28000000");
      prop_str(&w, "compatible", "riscv,imsics");
      prop_reg(&w, G_IMSIC_S, 0x1000L * vm->nvcpu);
      for (int i = 0; i < vm->nvcpu; i++) {
        cells[2 * i] = CPU_PHANDLE(i);
        cells[2 * i + 1] = 9;
      }
      prop_cells(&w, "interrupts-extended", cells, 2 * vm->nvcpu);
      prop_u32(&w, "riscv,num-ids", VIMSIC_NIDS - 1);
      prop_u32(&w, "riscv,guest-index-bits", 0);
      prop_u32(&w, "riscv,hart-index-bits", hart_bits);
      prop_u32(&w, "riscv,group-index-bits", 0);
      prop_empty(&w, "interrupt-controller");
      prop_empty(&w, "msi-controller");
      prop_u32(&w, "#msi-cells", 0);
      prop_u32(&w, "#interrupt-cells", 0);
      prop_u32(&w, "phandle", 6);
      node_end(&w);
    }
  }

  node_begin(&w, "serial@10000000");
  prop_str(&w, "compatible", "ns16550a");
  prop_reg(&w, G_UART_BASE, 0x100);
  prop_u32(&w, "interrupt-parent", ic_phandle);
  cells[0] = G_UART_IRQ;
  cells[1] = 4;
  prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
  prop_u32(&w, "clock-frequency", 3686400);
  prop_u32(&w, "reg-shift", 0);
  prop_u32(&w, "reg-io-width", 1);
  node_end(&w);

  node_begin(&w, "virtio_mmio@10001000");
  prop_str(&w, "compatible", "virtio,mmio");
  prop_reg(&w, G_VIRTIO_BASE, 0x1000);
  prop_u32(&w, "interrupt-parent", ic_phandle);
  cells[0] = G_VIRTIO_IRQ;
  cells[1] = 4;
  prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
  node_end(&w);

  node_end(&w); // soc
  node_end(&w); // root
  put32(&w, FDT_END);

  if (w.err) {
    kfree(scratch);
    return -1;
  }

  // assemble: header (40) + reserve map (16, empty) + struct + strings.
  int off_struct = 56;
  int off_strings = off_struct + w.stpos;
  int total = off_strings + w.sspos;
  if (total > max) {
    kfree(scratch);
    return -1;
  }
  memset(buf, 0, 56);
  be32(buf + 0, FDT_MAGIC);
  be32(buf + 4, total);
  be32(buf + 8, off_struct);
  be32(buf + 12, off_strings);
  be32(buf + 16, 40);     // off_mem_rsvmap
  be32(buf + 20, 17);     // version
  be32(buf + 24, 16);     // last_comp_version
  be32(buf + 28, 0);      // boot_cpuid_phys
  be32(buf + 32, w.sspos); // size_dt_strings
  be32(buf + 36, w.stpos); // size_dt_struct
  memmove(buf + off_struct, w.st, w.stpos);
  memmove(buf + off_strings, w.ss, w.sspos);
  kfree(scratch);
  return total;
}
