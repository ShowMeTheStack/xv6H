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
#include "net.h"

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

// "virtio_mmio@10001000": a node name carrying its unit address, built rather
// than written down, because the address is per machine now (vm->mmio_shift).
static const char *
unit_name(char *buf, const char *prefix, uint64 addr)
{
  int n = 0;
  while (prefix[n]) { buf[n] = prefix[n]; n++; }
  buf[n++] = '@';
  char hex[17]; int k = 0;
  do { hex[k++] = "0123456789abcdef"[addr & 15]; addr >>= 4; } while (addr);
  while (k) buf[n++] = hex[--k];
  buf[n] = 0;
  return buf;
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

// build the guest's device tree and copy it into guest RAM at gpa.
// the structure block gets a page of its own and the strings block
// another, and the blob is copied out in three pieces, so no contiguous
// host buffer is needed. returns the blob's size, or -1.
int
hyp_make_dtb(struct vm *vm, uint64 gpa, int max)
{
  struct fdtw w;
  char *stpage = kalloc(), *sspage = kalloc();
  if (stpage == 0 || sspage == 0) {
    if (stpage)
      kfree(stpage);
    if (sspage)
      kfree(sspage);
    return -1;
  }
  memset(&w, 0, sizeof(w));
  w.st = stpage;
  w.stmax = PGSIZE;
  w.ss = sspage;
  w.ssmax = PGSIZE;

  char uname[32];
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
  if (VM_FOREIGN(vm) && vm->online < vm->nvcpu) {
    // F7/F8 for a guest that boots its own harts: the ones beyond the count
    // stay offline until its driver is told otherwise
    char ba[160];
    int n = 0;
    for (const char *p = vm->cfg.bootargs; *p && n < 140; p++)
      ba[n++] = *p;
    if (n)
      ba[n++] = ' ';
    const char *m = "maxcpus=";
    while (*m)
      ba[n++] = *m++;
    if (vm->online >= 10)
      ba[n++] = '0' + vm->online / 10;
    ba[n++] = '0' + vm->online % 10;
    ba[n] = 0;
    prop_str(&w, "bootargs", ba);
  } else if (vm->cfg.bootargs[0])
    prop_str(&w, "bootargs", vm->cfg.bootargs);
  // F18: which console this machine has. The 16550 is always there as a
  // device (a guest that knows nothing else finds it); what the description
  // names as *the* console is the fact, and the virtio one exists only when
  // it is that.
  if (vm->console_kind == CON_VIRTIO) {
    char path[48] = "/soc/";
    unit_name(path + 5, "virtio_mmio", G_CON_BASE + vm->mmio_shift);
    prop_str(&w, "stdout-path", path);
  } else if (vm->console_kind == CON_SBI) {
    prop_str(&w, "stdout-path", "sbi-dbcn");
  } else {
    prop_str(&w, "stdout-path", "/soc/serial@10000000");
  }
  if (vm->cfg.net == 2) {
    // static address for an xv6 guest: ip, mask, gateway, dns.
    uint32 ipc[4] = {vm->cfg.ip, vm->cfg.mask, vm->cfg.gw, vm->cfg.dns};
    prop_cells(&w, "xv6h,ip", ipc, 4);
  }
  if (vm->gw) {
    // what is on the far side of its gateway: the switch's own subnet,
    // and the two addresses on it that are not ours -- qemu's router,
    // which is the laptop, and qemu's name forwarder. A machine cannot
    // work this out: 10.0.2.15 is the host it may talk to and 10.0.2.2
    // is the laptop it may not, and they differ by one number. With
    // this it may reach the switch through its gateway and still refuse
    // the two, and its own gateway stops looking like the laptop.
    uint32 outer[4] = {netcfg.ip & netcfg.mask, netcfg.mask, netcfg.gw,
                       netcfg.dns};
    prop_cells(&w, "xv6h,outer", outer, 4);
  }
  node_end(&w);

  // cpus
  node_begin(&w, "cpus");
  prop_u32(&w, "#address-cells", 1);
  prop_u32(&w, "#size-cells", 0);
  prop_u32(&w, "timebase-frequency", vm->timebase ? vm->timebase : 10000000);   // F6
  for (int i = 0; i < vm->nvcpu; i++) {
    int hid = i + vm->hart_shift;     // F9: the hart id this machine gives vcpu i
    memmove(name, "cpu@", 4);
    if (hid >= 10) {
      name[4] = '0' + hid / 10;
      name[5] = '0' + hid % 10;
      name[6] = 0;
    } else {
      name[4] = '0' + hid;
      name[5] = 0;
    }
    node_begin(&w, name);
    prop_str(&w, "device_type", "cpu");
    prop_u32(&w, "reg", hid);
    // F7/F8: a hart beyond the online count. An xv6 guest takes `disabled'
    // and re-reads it. Linux drops a disabled cpu from its possible set for
    // good, so for a guest that boots its own harts (VMF_HSM) every hart is
    // `okay', the parked ones carry a mark, and maxcpus= in bootargs keeps
    // them offline; the driver (machrev.ko) brings them up and down.
    if VM_FOREIGN(vm) {
      prop_str(&w, "status", "okay");
      if (i >= vm->online)
        prop_empty(&w, "xv6h,parked");
    } else
      prop_str(&w, "status", i < vm->online ? "okay" : "disabled");
    prop_str(&w, "compatible", "riscv");
    // the ISA as this guest may use it; in IMSIC mode the AIA CSRs
    // (Ssaia/Smaia) are part of it, or Linux's IMSIC driver refuses.
    {
      char isa[96], ext[128];
      int n = 0, e = 0;
      // the vector unit may have been revoked from this machine even though
      // the host has one (mutable.md): the description must agree with the
      // misa this guest is told about, or it would read V in one place and
      // not the other.
      int vec = plat.has_v && !vm->vec_revoked;
      const char *base = vec ? "rv64imafdcv_zicsr_zifencei" : "rv64imafdc_zicsr_zifencei";
      const char *exts = vec ? "i\0m\0a\0f\0d\0c\0v\0zicsr\0zifencei\0"
                             : "i\0m\0a\0f\0d\0c\0zicsr\0zifencei\0";
      int extlen = vec ? 29 : 27;
      for (const char *p = base; *p; p++)
        isa[n++] = *p;
      memmove(ext, exts, extlen);
      e = extlen;
      if (!vm->no_sstc && !vm_scaled_time(vm)) {    // F5/F6: the timer this machine has
        memmove(isa + n, "_sstc", 5);
        n += 5;
        memmove(ext + e, "sstc\0", 5);
        e += 5;
      }
      if (vm->cboz_block) {                        // F21: the cache-block size cbo.zero clears
        memmove(isa + n, "_zicboz", 7);
        n += 7;
        memmove(ext + e, "zicboz\0", 7);
        e += 7;
      }
      if (vm->vic_kind == VIC_IMSIC) {
        memmove(isa + n, "_smaia_ssaia", 12);
        n += 12;
        memmove(ext + e, "smaia\0ssaia\0", 12);
        e += 12;
      }
      isa[n] = 0;
      prop_str(&w, "riscv,isa", isa);
      prop_str(&w, "riscv,isa-base", "rv64i");
      prop(&w, "riscv,isa-extensions", ext, e);
    }
    prop_str(&w, "mmu-type", plat.mmu_max >= 48 ? "riscv,sv48" : "riscv,sv39");
    if (vm->cboz_block)
      prop_u32(&w, "riscv,cboz-block-size", vm->cboz_block);   // F21
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

  // where the description lives: the boot blob and the two revision slots
  // below it (hyp.h: desc_gpa). Linux keeps its hands off no-map memory; xv6
  // reads this node too (platform.c: resv_top)
  node_begin(&w, "reserved-memory");
  prop_u32(&w, "#address-cells", 2);
  prop_u32(&w, "#size-cells", 2);
  prop_empty(&w, "ranges");
  {
    uint64 rbase = vm->dtb_gpa - 2 * VM_DTB_MAX;
    node_begin(&w, unit_name(uname, "xv6h-description", rbase));
    prop_reg(&w, rbase, 3 * VM_DTB_MAX);
    prop_empty(&w, "no-map");
    node_end(&w);
  }
  node_end(&w);

  // soc
  node_begin(&w, "soc");
  prop_u32(&w, "#address-cells", 2);
  prop_u32(&w, "#size-cells", 2);
  prop_str(&w, "compatible", "simple-bus");
  prop_empty(&w, "ranges");

  if (!vm->no_clint) {   // F4
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
  }

  if (vm->vic_kind == VIC_PLIC) {
    node_begin(&w, unit_name(uname, "plic", G_PLIC_BASE + vm->intc_shift));
    prop_str(&w, "compatible", "riscv,plic0");
    prop_reg(&w, G_PLIC_BASE + vm->intc_shift, G_PLIC_SIZE);
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
    node_begin(&w, unit_name(uname, "interrupt-controller", G_APLIC_M + vm->intc_shift));
    prop_str(&w, "compatible", "riscv,aplic");
    prop_reg(&w, G_APLIC_M + vm->intc_shift, 0x8000);
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

    node_begin(&w, unit_name(uname, "interrupt-controller", G_APLIC_S + vm->intc_shift));
    prop_str(&w, "compatible", "riscv,aplic");
    prop_reg(&w, G_APLIC_S + vm->intc_shift, 0x8000);
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
      node_begin(&w, unit_name(uname, "interrupt-controller", G_IMSIC_M + vm->intc_shift));
      prop_str(&w, "compatible", "riscv,imsics");
      prop_reg(&w, G_IMSIC_M + vm->intc_shift, 0x1000L * (vm->nvcpu + vm->hart_shift));
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

      node_begin(&w, unit_name(uname, "interrupt-controller", G_IMSIC_S + vm->intc_shift));
      prop_str(&w, "compatible", "riscv,imsics");
      prop_reg(&w, G_IMSIC_S + vm->intc_shift, 0x1000L * (vm->nvcpu + vm->hart_shift));
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

  // the anchor (vmachrev.c): fixed address, fixed line, whatever F2/F3 do
  node_begin(&w, "machrev@10005000");
  prop_str(&w, "compatible", "xv6h,machrev");
  prop_reg(&w, G_MACHREV_BASE, 0x1000);
  prop_u32(&w, "interrupt-parent", ic_phandle);
  cells[0] = G_MACHREV_IRQ;
  cells[1] = 4;
  prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
  node_end(&w);

  node_begin(&w, "serial@10000000");
  prop_str(&w, "compatible", "ns16550a");
  prop_reg(&w, G_UART_BASE, 0x100);
  prop_u32(&w, "interrupt-parent", ic_phandle);
  cells[0] = vm_irq(vm, G_UART_IRQ);
  cells[1] = 4;
  prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
  prop_u32(&w, "clock-frequency", 3686400);
  prop_u32(&w, "reg-shift", 0);
  prop_u32(&w, "reg-io-width", 1);
  node_end(&w);

  node_begin(&w, unit_name(uname, "virtio_mmio", G_VIRTIO_BASE + vm->mmio_shift));
  prop_str(&w, "compatible", "virtio,mmio");
  prop_reg(&w, G_VIRTIO_BASE + vm->mmio_shift, 0x1000);
  prop_u32(&w, "interrupt-parent", ic_phandle);
  cells[0] = vm_irq(vm, G_VIRTIO_IRQ);
  cells[1] = 4;
  prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
  node_end(&w);

  if (rtc_present() && !vm->no_rtc) {    // F16/F17
    node_begin(&w, "rtc@101000");
    prop_str(&w, "compatible", "google,goldfish-rtc");
    prop_reg(&w, G_RTC_BASE, 0x1000);
    prop_u32(&w, "interrupt-parent", ic_phandle);
    cells[0] = vm_irq(vm, G_RTC_IRQ);
    cells[1] = 4;
    prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
    node_end(&w);
  }

  if (vm->balloon.present) {
    node_begin(&w, unit_name(uname, "virtio_mmio", G_BALLOON_BASE + vm->mmio_shift));
    prop_str(&w, "compatible", "virtio,mmio");
    prop_reg(&w, G_BALLOON_BASE + vm->mmio_shift, 0x1000);
    prop_u32(&w, "interrupt-parent", ic_phandle);
    cells[0] = vm_irq(vm, G_BALLOON_IRQ);
    cells[1] = 4;
    prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
    node_end(&w);
  }

  if (vm->con.present && !vm->con_going) {   // F18: a console on its way out is not described
    // the console as a queue. The guest prefers it to the 16550 when it
    // is here and falls back to the 16550 when it is not, so this node
    // appearing is what makes a guest stop paying two exits a character.
    node_begin(&w, unit_name(uname, "virtio_mmio", G_CON_BASE + vm->mmio_shift));
    prop_str(&w, "compatible", "virtio,mmio");
    prop_reg(&w, G_CON_BASE + vm->mmio_shift, 0x1000);
    prop_u32(&w, "interrupt-parent", ic_phandle);
    cells[0] = vm_irq(vm, G_CON_IRQ);
    cells[1] = 4;
    prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
    node_end(&w);
  }

  if (vm->vnet.present) {
    node_begin(&w, unit_name(uname, "virtio_mmio", G_VNET_BASE + vm->mmio_shift));
    prop_str(&w, "compatible", "virtio,mmio");
    prop_reg(&w, G_VNET_BASE + vm->mmio_shift, 0x1000);
    prop_u32(&w, "interrupt-parent", ic_phandle);
    cells[0] = vm_irq(vm, G_VNET_IRQ);
    cells[1] = 4;
    prop_cells(&w, "interrupts", cells, vm->vic_kind == VIC_PLIC ? 1 : 2);
    node_end(&w);
  }

  node_end(&w); // soc
  node_end(&w); // root
  put32(&w, FDT_END);

  // assemble: header (40) + reserve map (16, empty) + struct + strings.
  int off_struct = 56;
  int off_strings = off_struct + w.stpos;
  int total = off_strings + w.sspos;
  if (w.err || total > max) {
    printk("hyp: guest device tree will not fit (error %d, %d bytes, max %d)\n", w.err, total, max);
    kfree(stpage);
    kfree(sspage);
    return -1;
  }
  char buf[56];
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
  int ret = total;
  if (vm_copyout(vm, gpa, buf, 56) < 0 || vm_copyout(vm, gpa + off_struct, w.st, w.stpos) < 0 ||
      vm_copyout(vm, gpa + off_strings, w.ss, w.sspos) < 0)
    ret = -1;
  kfree(stpage);
  kfree(sspage);
  return ret;
}
