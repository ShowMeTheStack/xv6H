//
// fill in struct platform from the device tree.
// called once, by hart 0, in machine mode, before anything else runs.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "fdt.h"
#include "defs.h"
#include "platform.h"

uint64 time_mmio;               // riscv.h r_time(): the CLINT's mtime, or 0 for the CSR
uint64 host_timebase = 10000000; // hyp.h HOST_TIMEBASE: this host's timebase, from its tree
int mprobe_time(void);          // mvec.S: does a machine-mode read of the CSR trap?
int mprobe_stimecmp(void);
int mprobe_htimedelta(void);

struct platform plat;
__attribute__((aligned(16))) char fdt_copy[FDT_COPY_MAX];

// used by mvec.S (machine-mode IPI relay) to clear msip.
uint64 clint_msip_base;

// The parser writes through this pointer: the live description at boot, a
// separate structure when a guest re-reads a revised tree (machine.c reread).
// It used to parse into `plat' itself -- zeroed first, restored after -- and a
// second hart reading `plat' in those microseconds saw a description with no
// controller base, no timer kind, no hart ids: the two-hart arrival hang
// (snap-migrate-console-legacy).
static struct platform *pt = &plat;
static int cpu_dropped;   // the cpu node being parsed is `disabled' (F7/F8)

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
  if (pt->nvirtio >= PLAT_MAXVIRTIO)
    return;
  // keep sorted by base address (qemu lists slots highest first).
  int i = pt->nvirtio;
  while (i > 0 && pt->virtio_base[i - 1] > base) {
    pt->virtio_base[i] = pt->virtio_base[i - 1];
    pt->virtio_irq[i] = pt->virtio_irq[i - 1];
    i--;
  }
  pt->virtio_base[i] = base;
  pt->virtio_irq[i] = irq;
  pt->nvirtio++;
}

static void
finalize(struct nodeacc *n)
{
  n->pending = 0;
  if (n->depth == 0)
    return;
  if (fdt_name_is(n->name, "memory") && n->nreg >= 2) {
    pt->ram_base = n->reg[0];
    pt->ram_size = n->reg[1];
    return;
  }
  if (fdt_name_is(n->name, "xv6h-description") && n->nreg >= 2) {
    pt->resv_base = n->reg[0];     // the description's slots (hyp.h desc_gpa)
    pt->resv_size = n->reg[1];
    return;
  }
  if (n->compat == 0)
    return;
  if (fdt_strlist_has(n->compat, n->compat_len, "riscv,plic0") ||
      fdt_strlist_has(n->compat, n->compat_len, "sifive,plic-1.0.0")) {
    pt->plic_base = n->reg[0];
    pt->plic_size = n->reg[1];
    if (pt->intc_kind < INTC_PLIC)
      pt->intc_kind = INTC_PLIC;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "riscv,aplic")) {
    if (n->has_children)
      pt->aplic_m_base = n->reg[0];
    else
      pt->aplic_s_base = n->reg[0];
    pt->aplic_size = n->reg[1];
    pt->aplic_nsrc = n->nsrc;
    if (pt->intc_kind < INTC_APLIC)
      pt->intc_kind = INTC_APLIC;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "riscv,imsics")) {
    // interrupts-extended cause 9 = S-mode file, 11 = M-mode file.
    int is_s = (n->niext >= 2 && n->iext[1] == 9);
    if (is_s) {
      pt->imsic_s_base = n->reg[0];
      pt->imsic_s_size = n->reg[1];
      pt->imsic_guest_bits = n->gib;
      pt->imsic_nids = n->numids;
    } else {
      pt->imsic_m_base = n->reg[0];
    }
    pt->intc_kind = INTC_IMSIC;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "ns16550a")) {
    pt->uart_base = n->reg[0];
    pt->uart_irq = n->nirq ? n->irqs[0] : 0;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "virtio,mmio")) {
    add_virtio(n->reg[0], n->nirq ? n->irqs[0] : 0);
  } else if (fdt_strlist_has(n->compat, n->compat_len, "xv6h,ramdisk")) {
    pt->ramdisk_base = n->reg[0];
    pt->ramdisk_size = n->nreg > 1 ? n->reg[1] : 0;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "google,goldfish-rtc")) {
    pt->rtc_base = n->reg[0];
    pt->rtc_irq = n->nirq ? n->irqs[0] : 0;
  } else if (fdt_strlist_has(n->compat, n->compat_len, "riscv,clint0") ||
             fdt_strlist_has(n->compat, n->compat_len, "sifive,clint0")) {
    pt->clint_base = n->reg[0];
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
      if (item.depth == 2 && fdt_name_is(item.name, "cpu@")) {
        if (pt->ncpu < NCPU)
          pt->hartid[pt->ncpu] = pt->ncpu;   // until its reg says otherwise (F9)
        pt->ncpu++;
        cpu_dropped = 0;
      }
    } else if (item.tok == FDT_END_NODE) {
      if (acc.pending)
        finalize(&acc);
    } else if (item.tok == FDT_PROP) {
      if (item.depth == 0) {
        // root-node properties: the hypervisor marks guests here.
        if (streq(item.prop, "xv6h,guest"))
          pt->is_guest = 1;
        else if (streq(item.prop, "xv6h,vmid") && item.len >= 4)
          pt->guest_id = fdt32(item.data);
        continue;
      }
      if (item.depth == 1 && acc.pending && fdt_name_is(acc.name, "cpus") &&
          streq(item.prop, "timebase-frequency") && item.len >= 4) {
        pt->timebase = fdt32(item.data);
        continue;
      }
      if (item.depth == 2 && acc.pending && fdt_name_is(acc.name, "cpu@") && streq(item.prop, "status")) {
        // F7/F8: a hart described but not running is not one of ours
        if (!cpu_dropped && !(item.len >= 4 && memcmp(item.data, "okay", 4) == 0)) {
          cpu_dropped = 1;
          pt->ncpu--;
        }
        continue;
      }
      if (item.depth == 2 && acc.pending && fdt_name_is(acc.name, "cpu@") && streq(item.prop, "reg") &&
          item.len >= 4 && pt->ncpu >= 1 && pt->ncpu <= NCPU && !cpu_dropped) {
        pt->hartid[pt->ncpu - 1] = fdt32(item.data);   // F9: this cpu node's hart id
        continue;
      }
      if (item.depth == 2 && acc.pending && fdt_name_is(acc.name, "cpu@") && streq(item.prop, "riscv,cboz-block-size") &&
          item.len >= 4) {
        pt->cboz_block = fdt32(item.data);   // F21
        continue;
      }
      if (item.depth == 2 && acc.pending && fdt_name_is(acc.name, "cpu@") && streq(item.prop, "riscv,isa")) {
        // F5: does this machine have Sstc? The string is "rv64imafdc_zicsr_..._sstc..."
        const char *s = item.data;
        for (int i = 0; i + 4 <= item.len; i++)
          if (s[i] == 's' && s[i + 1] == 's' && s[i + 2] == 't' && s[i + 3] == 'c' &&
              (i == 0 || s[i - 1] == '_') && (i + 4 == item.len || s[i + 4] == 0 || s[i + 4] == '_'))
            pt->has_sstc = 1;
        continue;
      }
      if (item.depth == 2 && acc.pending && fdt_name_is(acc.name, "cpu@") && streq(item.prop, "mmu-type")) {
        const char *m = item.data;
        int v = 39;
        if (fdt_strlist_has(m, item.len, "riscv,sv57"))
          v = 57;
        else if (fdt_strlist_has(m, item.len, "riscv,sv48"))
          v = 48;
        if (pt->mmu_max == 0 || v < pt->mmu_max)
          pt->mmu_max = v; // the least capable hart decides
        continue;
      }
      if (item.depth == 1 && acc.pending && fdt_name_is(acc.name, "chosen") && streq(item.prop, "stdout-path")) {
        // F18: which console this machine has
        const char *s = item.data;
        int kind = 0;
        for (uint32 i = 0; i + 6 <= item.len; i++)
          if (memcmp(s + i, "virtio", 6) == 0)
            kind = 1;
        for (uint32 i = 0; i + 3 <= item.len; i++)
          if (memcmp(s + i, "sbi", 3) == 0)
            kind = 2;
        pt->console_kind = kind;
        continue;
      }
      if (item.depth == 1 && acc.pending && fdt_name_is(acc.name, "chosen") && streq(item.prop, "bootargs")) {
        uint32 n = item.len;
        if (n > sizeof(pt->bootargs) - 1)
          n = sizeof(pt->bootargs) - 1;
        for (uint32 i = 0; i < n; i++)
          pt->bootargs[i] = ((const char *)item.data)[i];
        pt->bootargs[n] = 0;
        continue;
      }
      if (item.depth == 1 && acc.pending && fdt_name_is(acc.name, "chosen")) {
        if (streq(item.prop, "xv6h,ip") && item.len >= 16) {
          pt->net_ip = fdt32(item.data);
          pt->net_mask = fdt32((const char *)item.data + 4);
          pt->net_gw = fdt32((const char *)item.data + 8);
          pt->net_dns = fdt32((const char *)item.data + 12);
        }
        if (streq(item.prop, "xv6h,outer") && item.len >= 16) {
          pt->outer_net = fdt32(item.data);
          pt->outer_mask = fdt32((const char *)item.data + 4);
          pt->outer_gw = fdt32((const char *)item.data + 8);
          pt->outer_dns = fdt32((const char *)item.data + 12);
        }
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

// F9: which logical cpu (tp) a hart id is
int
plat_cpu_of_hart(uint64 hartid)
{
  for (int i = 0; i < plat.ncpu; i++)
    if ((uint64)plat.hartid[i] == hartid)
      return i;
  return -1;
}

// the number after `key=' on the command line (`guesttimebase=1000000'), or 0
uint64
plat_bootarg_num(const char *key)
{
  const char *s = plat.bootargs;
  int n = strlen(key);
  while (*s) {
    while (*s == ' ')
      s++;
    const char *e = s;
    while (*e && *e != ' ')
      e++;
    if (e - s > n + 1 && memcmp(s, key, n) == 0 && s[n] == '=') {
      uint64 v = 0;
      for (const char *p = s + n + 1; p < e && *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
      return v;
    }
    s = e;
  }
  return 0;
}

// the text after `key=' on the command line, copied into out (at most max bytes); its length, or 0
int
plat_bootarg_str(const char *key, char *out, int max)
{
  const char *s = plat.bootargs;
  int n = strlen(key);
  while (*s) {
    while (*s == ' ')
      s++;
    const char *e = s;
    while (*e && *e != ' ')
      e++;
    if (e - s > n + 1 && memcmp(s, key, n) == 0 && s[n] == '=') {
      int l = e - (s + n + 1);
      if (l > max)
        l = max;
      memmove(out, s + n + 1, l);
      return l;
    }
    s = e;
  }
  return 0;
}

// does the host's command line contain the word w?
int
plat_bootarg(const char *w)
{
  const char *s = plat.bootargs;
  int n = strlen(w);
  while (*s) {
    while (*s == ' ')
      s++;
    const char *e = s;
    while (*e && *e != ' ')
      e++;
    if (e - s == n && memcmp(s, w, n) == 0)
      return 1;
    s = e;
  }
  return 0;
}

static void
defaults(void)
{
  // used only if there is no usable device tree (should not happen on qemu).
  pt->ram_base = KERNBASE;
  pt->ram_size = 128L * 1024 * 1024;
  pt->ncpu = NCPU;
  pt->intc_kind = INTC_PLIC;
  pt->plic_base = PLIC;
  pt->plic_size = 0x4000000;
  pt->clint_base = CLINT_BASE;
  pt->uart_base = UART0;
  pt->uart_irq = UART0_IRQ;
  pt->nvirtio = 1;
  pt->virtio_base[0] = VIRTIO0;
  pt->virtio_irq[0] = VIRTIO0_IRQ;
}

void
platform_init(uint64 fdt_pa)
{
  const void *fdt = (const void *)fdt_pa;

  pt->intc_kind = -1;
  if (fdt_valid(fdt) && fdt_totalsize(fdt) <= FDT_COPY_MAX) {
    // the blob lives near the top of RAM where the allocator will
    // scribble; keep a private copy.
    uint32 sz = fdt_totalsize(fdt);
    for (uint32 i = 0; i < sz; i++)
      fdt_copy[i] = ((const char *)fdt)[i];
    pt->fdt_addr = fdt_pa;
    pt->fdt_size = sz;
    parse(fdt_copy);
  }
  if (pt->ram_size == 0 || pt->intc_kind < 0 || pt->uart_base == 0)
    defaults();

  pt->ram_end = pt->ram_base + pt->ram_size;
  // the description's slots at the top of RAM are not ours to allocate
  pt->resv_top = 0;
  if (pt->resv_size && pt->resv_base + pt->resv_size == pt->ram_end)
    pt->resv_top = pt->resv_size;

  // paging mode: Sv48 when the hardware has it and the command line does
  // not say "sv39"; Sv39 cannot direct-map RAM beyond 256 GiB.
  if (pt->mmu_max == 0)
    pt->mmu_max = 39;
  pt->mmu_levels = (pt->mmu_max >= 48 && !plat_bootarg("sv39")) ? 4 : 3;
  pt->maxva = pt->mmu_levels == 4 ? (1UL << 47) : (1UL << 38);
  if (pt->mmu_levels == 3 && pt->ram_end > (1UL << 38)) {
    pt->ram_end = 1UL << 38;
    pt->ram_size = pt->ram_end - pt->ram_base;
  }
  if (pt->ncpu <= 0 || pt->ncpu > NCPU) {
    pt->ncpu = NCPU;
    for (int i = 0; i < NCPU; i++)
      pt->hartid[i] = i;
  }
  // the host is its own machine mode and always has a CLINT and Sstc; a guest
  // has what its description says (F4, F5): no CLINT node means SBI IPIs, no
  // sstc in the isa means the SBI timer
  if (pt->clint_base == 0 && !pt->is_guest)
    pt->clint_base = CLINT_BASE;
  // ... and what this hart actually has is asked of the hart, not assumed: a
  // machine-mode read of each CSR behind a one-instruction trap (mvec.S). Rocket
  // (the ZedBoard) has none of the three; QEMU's virt has all three unless told
  // otherwise (sstc=false, zicntr=false: the board's profile, for development).
  if (!pt->is_guest && pt == &plat) {
    pt->has_sstc = mprobe_stimecmp();
    pt->has_time_csr = mprobe_time();
    time_mmio = pt->has_time_csr ? 0 : pt->clint_base + 0xbff8;
  } else if (!pt->is_guest) {
    pt->has_sstc = 1;
    pt->has_time_csr = 1;
  } else {
    pt->has_time_csr = 1;       // a guest's reads are answered by its hypervisor
  }
  if (pt == &plat)
    clint_msip_base = pt->clint_base;

  pt->misa = r_misa();
  pt->has_h = (pt->misa >> 7) & 1;
  if (pt == &plat && !pt->is_guest) {
    pt->has_htimedelta = pt->has_h && mprobe_htimedelta();
    if (pt->timebase)
      host_timebase = pt->timebase;
  }
  pt->has_v = (pt->misa >> 21) & 1;
  pt->has_aia_csrs = (pt->intc_kind == INTC_IMSIC);
  if (pt->has_v) {
    // vlenb is only readable with the vector unit enabled (mstatus.VS).
    uint64 ms = r_mstatus();
    w_mstatus(ms | MSTATUS_VS_INITIAL);
    pt->vlenb = r_vlenb();
    w_mstatus(ms);
    if (pt->vlenb * 32 > 4096)
      pt->has_v = 0; // a register file larger than a page: unsupported
  }

  __sync_synchronize();
  pt->ready = 1;
}

// F-all: parse a (new) tree into `out' without touching the live description
void
platform_parse_into(struct platform *out, uint64 fdt_pa)
{
  pt = out;
  platform_init(fdt_pa);
  pt = &plat;
}

int
platform_virtio_by_irq(int irq)
{
  for (int i = 0; i < plat.nvirtio; i++)
    if (plat.virtio_irq[i] == irq)
      return i;
  return -1;
}
