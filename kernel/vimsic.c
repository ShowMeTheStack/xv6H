//
// virtual IMSIC: one supervisor-level interrupt file per vcpu.
//
// software model: pending/enable bit arrays kept in struct vimsic. the
// guest's vsiselect/vsireg/vstopei accesses trap (virtual instruction
// exceptions, because hstatus.VGEIN = 0) and are emulated here; the
// external interrupt is injected through hvip.VSEIP.
//
// hardware model (qemu -M virt,aia=aplic-imsic,aia-guests=N): while a
// vcpu runs, its file lives in one of the hart's guest interrupt files
// (hstatus.VGEIN selects it); MSIs are stores to that file's page, the
// guest's CSR accesses need no traps, and hardware raises VSEIP. on
// exit the file is saved back into the software model and released, so
// a vcpu may resume on any hart.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"

#ifndef CSR_STOPI
#define CSR_STOPI  0xdb0
#define CSR_VSTOPI 0xeb0
#endif
#include "intc.h"
#include "imsic.h"
#include "hyp.h"

// per-hart guest interrupt files.
static struct {
  struct spinlock lock;
  int geilen;          // guest files per hart (0: none)
  uint32 used[NCPU];   // bitmask of files in use, bit g = file g
} gf;

void
vimsic_hartinit(void)
{
  if (cpuid() == 0) {
    initlock(&gf.lock, "gfiles");
    csr_write(CSR_HGEIE, ~0UL);
    uint64 m = csr_read(CSR_HGEIE);
    csr_write(CSR_HGEIE, 0);
    int n = 0;
    while (m) {
      n += m & 1;
      m >>= 1;
    }
    gf.geilen = n;
  }
}

int
vimsic_geilen(void)
{
  return gf.geilen;
}

static int
gfile_alloc(int hart)
{
  int g = 0;
  acquire(&gf.lock);
  for (int i = 1; i <= gf.geilen && i < 32; i++) {
    if (!(gf.used[hart] & (1u << i))) {
      gf.used[hart] |= 1u << i;
      g = i;
      break;
    }
  }
  release(&gf.lock);
  return g;
}

static void
gfile_free(int hart, int g)
{
  acquire(&gf.lock);
  gf.used[hart] &= ~(1u << g);
  release(&gf.lock);
}

// ---- software model ----

// lowest pending+enabled identity below the threshold, or 0.
static int
top_id(struct vimsic *im)
{
  if (!im->eidelivery)
    return 0;
  for (int k = 0; k < VIMSIC_NIDS / 64; k++) {
    uint64 bits = im->eip[k] & im->eie[k];
    if (bits == 0)
      continue;
    int b = 0;
    while (!((bits >> b) & 1))
      b++;
    int id = 64 * k + b;
    if (id == 0)
      continue;
    if (im->eithreshold != 0 && id >= im->eithreshold)
      return 0;
    return id;
  }
  return 0;
}

// the same, for vmm info
int
vimsic_top(struct vimsic *im)
{
  return top_id(im);
}

int
vimsic_seip(struct vcpu *v)
{
  return top_id(&v->imsic) != 0;
}

// software model: with hstatus.VGEIN = 0 a pending VSEIP shows up in
// vstopi only if hvictl names it (IID 9 with a priority), which is what
// an AIA-aware guest reads to find the interrupt. keep hvip and hvictl
// together. runs on the vcpu's own hart.

static void
soft_vseip(int on)
{
  // hvictl names the interrupt for vstopi; on a host without the AIA CSRs there
  // is no vstopi to name it for (the guest's reads of stopei trap here and are
  // answered from top_id), and touching hvictl is an illegal instruction in HS.
  if (on) {
    if (plat.has_aia_csrs)
      csr_write(CSR_HVICTL, HVICTL_VSEIP);
    csr_set(CSR_HVIP, HVIP_VSEIP);
  } else {
    csr_clear(CSR_HVIP, HVIP_VSEIP);
    if (plat.has_aia_csrs)
      csr_write(CSR_HVICTL, 0);
  }
}

// deliver identity eiid to a vcpu. caller holds vm->lock.
void
vimsic_send(struct vcpu *v, int eiid)
{
  if (eiid <= 0 || eiid >= VIMSIC_NIDS)
    return;
  if (v->imsic.hwfile && v->running_hart >= 0) {
    // hardware file in use on that hart: a plain store delivers it.
    *(volatile uint32 *)imsic_file_addr(v->imsic.hwhart, v->imsic.hwfile) = eiid;
    return;
  }
  v->imsic.eip[eiid / 64] |= 1UL << (eiid % 64);
  if (top_id(&v->imsic))
    vcpu_kick(v);
}

// indirect register access through (vs)iselect.
static int
ireg_read(struct vimsic *im, uint64 sel, uint64 *val)
{
  if (sel == IMSIC_EIDELIVERY) {
    *val = im->eidelivery;
  } else if (sel == IMSIC_EITHRESHOLD) {
    *val = im->eithreshold;
  } else if (sel >= IMSIC_EIP0 && sel < IMSIC_EIP0 + 64) {
    int k = sel - IMSIC_EIP0;
    *val = (k & 1) ? 0 : (k / 2 < VIMSIC_NIDS / 64 ? im->eip[k / 2] : 0);
  } else if (sel >= IMSIC_EIE0 && sel < IMSIC_EIE0 + 64) {
    int k = sel - IMSIC_EIE0;
    *val = (k & 1) ? 0 : (k / 2 < VIMSIC_NIDS / 64 ? im->eie[k / 2] : 0);
  } else {
    return -1;
  }
  return 0;
}

static int
ireg_write(struct vimsic *im, uint64 sel, uint64 val)
{
  if (sel == IMSIC_EIDELIVERY) {
    im->eidelivery = val & 1;
  } else if (sel == IMSIC_EITHRESHOLD) {
    im->eithreshold = val & (VIMSIC_NIDS - 1);
  } else if (sel >= IMSIC_EIP0 && sel < IMSIC_EIP0 + 64) {
    int k = sel - IMSIC_EIP0;
    if (!(k & 1) && k / 2 < VIMSIC_NIDS / 64)
      im->eip[k / 2] = val & ~(k == 0 ? 1UL : 0); // id 0 does not exist
  } else if (sel >= IMSIC_EIE0 && sel < IMSIC_EIE0 + 64) {
    int k = sel - IMSIC_EIE0;
    if (!(k & 1) && k / 2 < VIMSIC_NIDS / 64)
      im->eie[k / 2] = val & ~(k == 0 ? 1UL : 0);
  } else {
    return -1;
  }
  return 0;
}

// emulate a CSR instruction on vsiselect / vsireg / vstopei (software
// model). returns 0 if handled, -1 if the guest should get an illegal
// instruction exception.
int
vimsic_csr(struct vcpu *v, uint32 inst)
{
  struct vimsic *im = &v->imsic;
  int funct3 = (inst >> 12) & 7;
  int csr = inst >> 20;
  int rs1 = (inst >> 15) & 31;
  int rdn = (inst >> 7) & 31;
  uint64 old = 0, src, nw;
  int dowrite;

  // in VS mode, siselect/sireg/stopei (0x15x) are the guest's names for
  // vsiselect/vsireg/vstopei (0x25x).
  // and stopi (0xdb0) is the guest's name for vstopi (0xeb0): the same 0x100.
  if (csr == CSR_SISELECT || csr == CSR_SIREG || csr == CSR_STOPEI || csr == CSR_STOPI)
    csr += 0x100;

  // vsiselect itself is a real register the guest writes without
  // trapping; it names the register that a vsireg access means.
  // ... on a CPU that has it. Without the AIA CSRs the guest's writes to siselect
  // arrive here as illegal instructions (the register does not exist) and the
  // selection lives only in im->iselect, which the write below keeps.
  if (plat.has_aia_csrs)
    im->iselect = csr_read(CSR_VSISELECT);

  acquire(&v->vm->lock);
  int rc = 0;
  switch (csr) {
  case CSR_VSISELECT: old = im->iselect; break;
  case CSR_VSIREG: rc = ireg_read(im, im->iselect, &old); break;
  case CSR_VSTOPEI: {
    int id = top_id(im);
    old = id ? ((uint64)id << 16) | id : 0;
    break;
  }
  case CSR_VSTOPI: {
    // Supervisor top interrupt: (iid << 16) | priority for the highest pending
    // and enabled S-level interrupt, 0 if none. On a CPU with the AIA CSRs this
    // is hardware (vstopi, with hvictl naming the external one); on one without,
    // the guest's read arrives here as an illegal instruction. Linux's AIA
    // interrupt entry reads it to learn what it is taking, so a guest arriving on
    // such a host died at its first interrupt until this existed -- xv6 never
    // reads it, which is why an xv6 guest passed the same crossing. Priority
    // order as the ISA gives it for S level: external, software, timer.
    uint64 ie = v->vsie;
    if ((ie & (1L << 9)) && top_id(im) != 0)
      old = (9L << 16) | 1;
    else if ((ie & (1L << 1)) && v->vssip)
      old = (1L << 16) | 1;
    else if ((ie & (1L << 5)) && vcpu_timer_pending(v))
      old = (5L << 16) | 1;
    else
      old = 0;
    break;
  }
  default: rc = -1;
  }
  if (rc < 0) {
    release(&v->vm->lock);
    return -1;
  }
  src = (funct3 & 4) ? (uint64)rs1 : v->regs[rs1];
  switch (funct3 & 3) {
  case 1: nw = src; dowrite = 1; break;
  case 2: nw = old | src; dowrite = (rs1 != 0); break;
  case 3: nw = old & ~src; dowrite = (rs1 != 0); break;
  default: release(&v->vm->lock); return -1;
  }
  if (dowrite) {
    switch (csr) {
    case CSR_VSISELECT:
      im->iselect = nw;
      if (plat.has_aia_csrs)
        csr_write(CSR_VSISELECT, nw);
      break;
    case CSR_VSIREG: rc = ireg_write(im, im->iselect, nw); break;
    case CSR_VSTOPEI: {
      // any write claims the top interrupt: clear its pending bit.
      int id = top_id(im);
      if (id)
        im->eip[id / 64] &= ~(1UL << (id % 64));
      break;
    }
    case CSR_VSTOPI:
      rc = -1;               // read-only: a write is the illegal instruction it looks like
      break;
    }
  }
  // the external interrupt line follows the file's state at once: we are
  // on this vcpu's hart, in its exit path, so hvip is its own. (hardware
  // would drop VSEIP when the file empties; the software model must.)
  soft_vseip(top_id(im) != 0);
  release(&v->vm->lock);
  if (rc < 0)
    return -1;
  if (rdn != 0)
    v->regs[rdn] = old;
  v->pc += 4;
  v->csr_emul++;
  return 0;
}

// ---- hardware guest interrupt files ----

static inline void
w_vsiselect(uint64 x)
{
  csr_write(CSR_VSISELECT, x);
}

static inline void
w_vsireg(uint64 x)
{
  csr_write(CSR_VSIREG, x);
}

static inline uint64
r_vsireg(void)
{
  return csr_read(CSR_VSIREG);
}

// about to enter the guest on this hart. caller holds vm->lock.
// returns the hvip bits to inject (VSEIP for the software model).
uint64
vimsic_enter(struct vcpu *v)
{
  struct vimsic *im = &v->imsic;

  if (!v->vm->imsic_hw || gf.geilen == 0) {
    v->vgein = 0;
    int on = top_id(im) != 0;
    if (plat.has_aia_csrs)
      csr_write(CSR_HVICTL, on ? HVICTL_VSEIP : 0);
    return on ? HVIP_VSEIP : 0;
  }

  int hart = cpuid();
  int g = gfile_alloc(hart);
  if (g == 0) {
    // no free guest file on this hart: run with the software model.
    v->vgein = 0;
    int on = top_id(im) != 0;
    if (plat.has_aia_csrs)
      csr_write(CSR_HVICTL, on ? HVICTL_VSEIP : 0);
    return on ? HVIP_VSEIP : 0;
  }
  csr_write(CSR_HVICTL, 0); // the guest file reports its own interrupts
  im->hwfile = g;
  im->hwhart = hart;
  v->vgein = g;

  // load the file (vsiselect/vsireg address guest file VGEIN while V=0).
  csr_write(CSR_HSTATUS, (uint64)g << HSTATUS_VGEIN_SHIFT);
  w_vsiselect(IMSIC_EIDELIVERY);
  w_vsireg(0);
  w_vsiselect(IMSIC_EITHRESHOLD);
  w_vsireg(im->eithreshold);
  for (int k = 0; k < VIMSIC_NIDS / 64; k++) {
    w_vsiselect(IMSIC_EIE0 + 2 * k);
    w_vsireg(im->eie[k]);
    w_vsiselect(IMSIC_EIP0 + 2 * k);
    w_vsireg(im->eip[k]);
  }
  w_vsiselect(IMSIC_EIDELIVERY);
  w_vsireg(im->eidelivery);
  return 0; // hardware raises VSEIP itself
}

// the guest exited on this hart. caller holds vm->lock; v->running_hart
// is already -1, so new MSIs go to the software model.
void
vimsic_exit(struct vcpu *v)
{
  struct vimsic *im = &v->imsic;
  if (im->hwfile == 0)
    return;
  int g = im->hwfile;
  uint64 hs = csr_read(CSR_HSTATUS);
  csr_write(CSR_HSTATUS, (hs & ~HSTATUS_VGEIN_MASK) | ((uint64)g << HSTATUS_VGEIN_SHIFT));

  w_vsiselect(IMSIC_EIDELIVERY);
  im->eidelivery = r_vsireg() & 1;
  w_vsireg(0);
  w_vsiselect(IMSIC_EITHRESHOLD);
  im->eithreshold = r_vsireg();
  for (int k = 0; k < VIMSIC_NIDS / 64; k++) {
    w_vsiselect(IMSIC_EIE0 + 2 * k);
    im->eie[k] = r_vsireg();
    w_vsireg(0);
    w_vsiselect(IMSIC_EIP0 + 2 * k);
    im->eip[k] = r_vsireg();
    w_vsireg(0);
  }
  csr_write(CSR_HSTATUS, hs & ~HSTATUS_VGEIN_MASK);

  gfile_free(im->hwhart, g);
  im->hwfile = 0;
  v->vgein = 0;
}

void
vimsic_sgei(void)
{
  // hgeie is never set: vcpus hold guest files only while running.
}

// ---- the guest's view of the IMSIC pages (MSI target for its IPIs) ----

static int
vimsic_mmio_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  *val = 0;
  return 0;
}

static int
vimsic_mmio_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  int is_s = (int)(uint64)d->priv;
  int vcpu = vm_vcpu_of_hart(vm, off / 0x1000);   // F9: files are per hart id
  int reg = off % 0x1000;
  if (!is_s || size != 4 || vcpu < 0)
    return 0; // machine-level files: accepted, ignored
  if (reg == IMSIC_SETEIPNUM_LE) {
    acquire(&vm->lock);
    vic_msi(vm, vcpu, (int)val);
    release(&vm->lock);
  }
  return 0;
}

void
vimsic_init(struct vm *vm)
{
  for (int i = 0; i < NVCPU; i++)
    memset(&vm->vcpu[i].imsic, 0, sizeof(struct vimsic));
  vm_add_dev(vm, "imsic-m", -1, OBS_INTC, G_IMSIC_M + vm->intc_shift, 0x1000L * NVCPU,   /* F9: any hart id */ vimsic_mmio_read, vimsic_mmio_write, (void *)0);
  vm_add_dev(vm, "imsic-s", -1, OBS_INTC, G_IMSIC_S + vm->intc_shift, 0x1000L * NVCPU,   /* F9: any hart id */ vimsic_mmio_read, vimsic_mmio_write, (void *)1);
}
