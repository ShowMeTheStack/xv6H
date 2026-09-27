//
// virtual APLIC, following the AIA specification and qemu's behaviour:
//
//  - two domains: the machine-level root at 0x0c000000 (the guest's
//    emulated machine mode delegates sources through it) and the
//    supervisor-level child at 0x0d000000 which actually delivers.
//  - direct delivery mode: per-vcpu IDC registers (idelivery, iforce,
//    ithreshold, topi, claimi); a pending+enabled source whose target
//    is the vcpu raises its external interrupt. level-sensitive sources
//    have their pending bit follow the rectified input (as qemu does).
//  - MSI delivery mode: a source becoming pending+enabled sends its
//    target's eiid to that vcpu's IMSIC and clears the pending bit;
//    software re-arms a level source by writing setipnum.
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
#include "aplic.h"
#include "hyp.h"

#define DOM_M 0
#define DOM_S 1

static int
msimode(struct vm *vm)
{
  return vm->vic_kind == VIC_IMSIC;
}

// every one of these takes a source number that may have come
// straight out of a guest's store instruction -- SETIPNUM and its
// kind are registers whose *value* is the source -- so the range is
// checked here rather than trusted. A number outside it names no
// source, which is what "inactive" means.
static int
sm_of(struct vaplic *a, int irq)
{
  if (irq <= 0 || irq >= VAPLIC_NSRC)
    return APLIC_SM_INACTIVE;
  return a->sourcecfg[irq] & 7;
}

static int
is_level(int sm)
{
  return sm == APLIC_SM_LEVEL1 || sm == APLIC_SM_LEVEL0;
}

static int
is_active(struct vaplic *a, int irq)
{
  if (irq <= 0 || irq >= VAPLIC_NSRC)
    return 0;
  int sm = sm_of(a, irq);
  return sm != APLIC_SM_INACTIVE && !(a->sourcecfg[irq] & APLIC_SOURCECFG_D);
}

// rectified input: level1/edge1 sources are active-high, level0/edge0 low.
static int
rectified(struct vaplic *a, int irq)
{
  if (irq <= 0 || irq >= VAPLIC_NSRC)
    return 0;
  int sm = sm_of(a, irq);
  int in = (a->level >> irq) & 1;
  if (sm == APLIC_SM_LEVEL0 || sm == APLIC_SM_EDGE0)
    return !in;
  return in;
}

// the vcpu targeted by a source (direct mode), or -1.
static int
target_hart(struct vm *vm, int irq)
{
  if (irq <= 0 || irq >= VAPLIC_NSRC)
    return -1;
  return vm_vcpu_of_hart(vm, vm->aplic.target[irq] >> 18);   // F9: a hart id
}

static int
target_prio(struct vm *vm, int irq)
{
  if (irq <= 0 || irq >= VAPLIC_NSRC)
    return 1;
  int p = vm->aplic.target[irq] & 0xff;
  return p == 0 ? 1 : p;
}

// direct mode: best deliverable source for a vcpu, or 0. caller holds vm->lock.
static int
topi(struct vm *vm, int vcpu, int *prio_out)
{
  struct vaplic *a = &vm->aplic;
  int best = 0, bestprio = 256;
  if (!(a->domaincfg[DOM_S] & APLIC_DOMAINCFG_IE))
    return 0;
  uint64 cand = a->pending & a->enable;
  for (int i = 1; i < VAPLIC_NSRC; i++) {
    if (!((cand >> i) & 1) || target_hart(vm, i) != vcpu)
      continue;
    int p = target_prio(vm, i);
    if (p < bestprio) {
      best = i;
      bestprio = p;
    }
  }
  if (best) {
    uint32 th = a->ithreshold[vcpu];
    if (th != 0 && bestprio >= th)
      return 0;
    if (prio_out)
      *prio_out = bestprio;
  }
  return best;
}

int
vaplic_seip(struct vcpu *v)
{
  struct vm *vm = v->vm;
  struct vaplic *a = &vm->aplic;
  if (msimode(vm))
    return 0; // MSIs go to the IMSIC model
  if (!a->idelivery[v->id])
    return 0;
  if (a->iforce[v->id])
    return 1;
  return topi(vm, v->id, 0) != 0;
}

// MSI mode: deliver every pending+enabled source. caller holds vm->lock.
static void
msi_flush(struct vm *vm)
{
  struct vaplic *a = &vm->aplic;
  if (!(a->domaincfg[DOM_S] & APLIC_DOMAINCFG_IE))
    return;
  uint64 cand = a->pending & a->enable;
  for (int i = 1; i < VAPLIC_NSRC; i++) {
    if (!((cand >> i) & 1))
      continue;
    uint32 t = a->target[i];
    int vcpu = vm_vcpu_of_hart(vm, t >> 18), eiid = t & 0x7ff;   // F9: the target is a hart id
    a->pending &= ~(1UL << i);
    if (vm->hart_shift && plat_bootarg("aplicdbg"))
      printk("aplic: vm %d msi irq %d -> hart %d vcpu %d eiid %d\n", vm->id, i, (int)(t >> 18), vcpu, eiid);
    if (vcpu >= 0 && eiid != 0)
      vic_msi(vm, vcpu, eiid);
  }
}

// something changed: deliver MSIs, or kick vcpus that now have an
// interrupt to take. caller holds vm->lock.
static void
notify(struct vm *vm)
{
  if (msimode(vm)) {
    msi_flush(vm);
    return;
  }
  for (int i = 0; i < vm->nvcpu; i++) {
    struct vcpu *v = &vm->vcpu[i];
    if (vaplic_seip(v))
      vcpu_kick(v);
  }
}

// set the pending bit of a source, honouring the source-mode rules.
// caller holds vm->lock.
static void
set_pending(struct vm *vm, int irq, int pending)
{
  struct vaplic *a = &vm->aplic;
  if (!is_active(a, irq))
    return;
  int sm = sm_of(a, irq);
  if (sm == APLIC_SM_DETACHED) {
    // detached sources can only be pended by software.
  } else if (is_level(sm)) {
    if (msimode(vm)) {
      // a level source can be pended by software only while the input
      // is asserted; clearing is always allowed.
      if (pending && !rectified(a, irq))
        return;
    } else {
      // direct mode: pending follows the rectified input.
      return;
    }
  }
  if (pending)
    a->pending |= 1UL << irq;
  else
    a->pending &= ~(1UL << irq);
}

// a virtual device changed its interrupt line. caller holds vm->lock.
void
vaplic_set_level(struct vm *vm, int irq, int level)
{
  struct vaplic *a = &vm->aplic;
  if (irq <= 0 || irq >= VAPLIC_NSRC)
    return;
  int old = (a->level >> irq) & 1;
  if (vm->hart_shift && plat_bootarg("aplicdbg"))     // only once the harts have moved: the boot would drown
    printk("aplic: vm %d irq %d level %d->%d active %d pend %lx en %lx ie %d tgt %x\n", vm->id, irq,
           old, level, is_active(a, irq), (unsigned long)a->pending,
           (unsigned long)a->enable, (a->domaincfg[DOM_S] & APLIC_DOMAINCFG_IE) != 0, a->target[irq]);
  if (old == level)
    return;
  if (level)
    a->level |= 1UL << irq;
  else
    a->level &= ~(1UL << irq);
  if (!is_active(a, irq)) {
    // not configured yet: remember an asserted level for when it is
    // (resync() pends it once the source becomes a level source).
    return;
  }
  int sm = sm_of(a, irq);
  int rect = rectified(a, irq);
  if (is_level(sm)) {
    if (msimode(vm)) {
      if (rect)
        a->pending |= 1UL << irq; // rising rectified edge pends the source
    } else {
      if (rect)
        a->pending |= 1UL << irq;
      else
        a->pending &= ~(1UL << irq);
    }
  } else if (sm == APLIC_SM_EDGE1 || sm == APLIC_SM_EDGE0) {
    if (rect)
      a->pending |= 1UL << irq;
  }
  notify(vm);
}

// after a sourcecfg change, re-derive the pending bit of a level source.
static void
resync(struct vm *vm, int irq)
{
  struct vaplic *a = &vm->aplic;
  if (!is_active(a, irq)) {
    a->pending &= ~(1UL << irq);
    a->enable &= ~(1UL << irq);
    return;
  }
  if (is_level(sm_of(a, irq))) {
    // direct mode: pending follows the input. MSI mode: an input that is
    // already asserted when the source is configured counts as a rising
    // edge (as it does on qemu, which records the input state early).
    if (rectified(a, irq))
      a->pending |= 1UL << irq;
    else if (!msimode(vm))
      a->pending &= ~(1UL << irq);
  }
}

// ---- register access ----

static int
vaplic_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  struct vaplic *a = &vm->aplic;
  int dom = (int)(uint64)d->priv;
  uint64 v = 0;

  if (size != 4)
    return -1;
  acquire(&vm->lock);
  if (off == APLIC_DOMAINCFG) {
    v = APLIC_DOMAINCFG_RO80 | (a->domaincfg[dom] & APLIC_DOMAINCFG_IE) |
        (msimode(vm) ? APLIC_DOMAINCFG_DM : 0);
  } else if (off >= 0x0004 && off < 0x0004 + 4 * (VAPLIC_NSRC - 1)) {
    int irq = (off - 0x0004) / 4 + 1;
    v = (dom == DOM_M) ? a->msourcecfg[irq] : a->sourcecfg[irq];
  } else if (off == APLIC_MMSIADDRCFG) {
    v = a->mmsiaddrcfg;
  } else if (off == APLIC_MMSIADDRCFGH) {
    v = a->mmsiaddrcfgh;
  } else if (off == APLIC_SMSIADDRCFG) {
    v = a->smsiaddrcfg;
  } else if (off == APLIC_SMSIADDRCFGH) {
    v = a->smsiaddrcfgh;
  } else if (off >= APLIC_SETIP && off < APLIC_SETIP + 4 * 32) {
    int w = (off - APLIC_SETIP) / 4;
    if (dom == DOM_S && w < 2)
      v = (uint32)(a->pending >> (32 * w));
  } else if (off >= APLIC_IN_CLRIP && off < APLIC_IN_CLRIP + 4 * 32) {
    int w = (off - APLIC_IN_CLRIP) / 4;
    if (dom == DOM_S && w < 2) {
      uint64 rect = 0;
      for (int i = 1; i < VAPLIC_NSRC; i++)
        if (is_active(a, i) && rectified(a, i))
          rect |= 1UL << i;
      v = (uint32)(rect >> (32 * w));
    }
  } else if (off >= APLIC_SETIE && off < APLIC_SETIE + 4 * 32) {
    int w = (off - APLIC_SETIE) / 4;
    if (dom == DOM_S && w < 2)
      v = (uint32)(a->enable >> (32 * w));
  } else if (off == APLIC_GENMSI) {
    v = 0;
  } else if (off >= 0x3004 && off < 0x3004 + 4 * (VAPLIC_NSRC - 1)) {
    int irq = (off - 0x3004) / 4 + 1;
    v = (dom == DOM_S && is_active(a, irq)) ? a->target[irq] : 0;
  } else if (off >= 0x4000 && off < 0x4000 + 32 * NVCPU) {
    int h = vm_vcpu_of_hart(vm, (off - 0x4000) / 32);   // F9: IDCs are per hart id
    int reg = (off - 0x4000) % 32;
    if (dom == DOM_S && h >= 0 && !msimode(vm)) {
      int prio = 0, irq;
      switch (reg) {
      case APLIC_IDC_IDELIVERY: v = a->idelivery[h]; break;
      case APLIC_IDC_IFORCE: v = a->iforce[h]; break;
      case APLIC_IDC_ITHRESHOLD: v = a->ithreshold[h]; break;
      case APLIC_IDC_TOPI:
        irq = topi(vm, h, &prio);
        v = irq ? ((uint32)irq << 16) | prio : 0;
        break;
      case APLIC_IDC_CLAIMI:
        irq = topi(vm, h, &prio);
        if (irq) {
          v = ((uint32)irq << 16) | prio;
          set_pending(vm, irq, 0); // no effect on level sources (input-driven)
        } else {
          v = 0;
          a->iforce[h] = 0; // reading claimi with nothing pending clears iforce
        }
        break;
      }
    }
  }
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vaplic_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  struct vaplic *a = &vm->aplic;
  int dom = (int)(uint64)d->priv;
  uint32 w = (uint32)val;

  if (size != 4)
    return -1;
  acquire(&vm->lock);
  if (off == APLIC_DOMAINCFG) {
    a->domaincfg[dom] = w & APLIC_DOMAINCFG_IE;
  } else if (off >= 0x0004 && off < 0x0004 + 4 * (VAPLIC_NSRC - 1)) {
    int irq = (off - 0x0004) / 4 + 1;
    if (dom == DOM_M) {
      // the root domain: D=1 delegates the source to the child.
      a->msourcecfg[irq] = w & (APLIC_SOURCECFG_D | 0x3ff);
    } else {
      // the child cannot delegate further; keep the source mode.
      if (w & APLIC_SOURCECFG_D)
        w = APLIC_SM_INACTIVE;
      int sm = w & 7;
      if (sm == 2 || sm == 3)
        sm = APLIC_SM_INACTIVE; // reserved encodings
      a->sourcecfg[irq] = sm;
      resync(vm, irq);
    }
  } else if (off == APLIC_MMSIADDRCFG) {
    a->mmsiaddrcfg = w;
  } else if (off == APLIC_MMSIADDRCFGH) {
    a->mmsiaddrcfgh = w;
  } else if (off == APLIC_SMSIADDRCFG) {
    a->smsiaddrcfg = w;
  } else if (off == APLIC_SMSIADDRCFGH) {
    a->smsiaddrcfgh = w;
  } else if (dom != DOM_S) {
    // the rest of the machine domain's registers do nothing useful.
  } else if (off >= APLIC_SETIP && off < APLIC_SETIP + 4 * 32) {
    int wd = (off - APLIC_SETIP) / 4;
    if (wd < 2)
      for (int b = 0; b < 32; b++)
        if (w & (1u << b))
          set_pending(vm, 32 * wd + b, 1);
  } else if (off == APLIC_SETIPNUM || off == APLIC_SETIPNUM_LE) {
    set_pending(vm, w, 1);
  } else if (off >= APLIC_IN_CLRIP && off < APLIC_IN_CLRIP + 4 * 32) {
    int wd = (off - APLIC_IN_CLRIP) / 4;
    if (wd < 2)
      for (int b = 0; b < 32; b++)
        if (w & (1u << b))
          set_pending(vm, 32 * wd + b, 0);
  } else if (off == APLIC_CLRIPNUM) {
    set_pending(vm, w, 0);
  } else if (off >= APLIC_SETIE && off < APLIC_SETIE + 4 * 32) {
    int wd = (off - APLIC_SETIE) / 4;
    if (wd < 2)
      for (int b = 0; b < 32; b++)
        if ((w & (1u << b)) && is_active(a, 32 * wd + b))
          a->enable |= 1UL << (32 * wd + b);
  } else if (off == APLIC_SETIENUM) {
    if (w > 0 && w < VAPLIC_NSRC && is_active(a, w))
      a->enable |= 1UL << w;
  } else if (off >= APLIC_CLRIE && off < APLIC_CLRIE + 4 * 32) {
    int wd = (off - APLIC_CLRIE) / 4;
    if (wd < 2)
      a->enable &= ~((uint64)w << (32 * wd));
  } else if (off == APLIC_CLRIENUM) {
    if (w > 0 && w < VAPLIC_NSRC)
      a->enable &= ~(1UL << w);
  } else if (off == APLIC_GENMSI) {
    if (msimode(vm)) {
      int hart = w >> 18, eiid = w & 0x7ff;
      if (hart < vm->nvcpu && eiid)
        vic_msi(vm, hart, eiid);
    }
  } else if (off >= 0x3004 && off < 0x3004 + 4 * (VAPLIC_NSRC - 1)) {
    int irq = (off - 0x3004) / 4 + 1;
    if (msimode(vm))
      a->target[irq] = w & ((0x3fffu << 18) | (0x3f << 12) | 0x7ff);
    else
      a->target[irq] = (w & (0x3fffu << 18)) | ((w & 0xff) ? (w & 0xff) : 1);
    if (vm->hart_shift && plat_bootarg("aplicdbg"))
      printk("aplic: vm %d target irq %d <- %x\n", vm->id, irq, a->target[irq]);
  } else if (off >= 0x4000 && off < 0x4000 + 32 * NVCPU) {
    int h = vm_vcpu_of_hart(vm, (off - 0x4000) / 32);   // F9
    int reg = (off - 0x4000) % 32;
    if (h >= 0 && !msimode(vm)) {
      switch (reg) {
      case APLIC_IDC_IDELIVERY: a->idelivery[h] = w & 1; break;
      case APLIC_IDC_IFORCE: a->iforce[h] = w & 1; break;
      case APLIC_IDC_ITHRESHOLD: a->ithreshold[h] = w & 0xff; break;
      }
    }
  }
  notify(vm);
  release(&vm->lock);
  return 0;
}

void
vaplic_init(struct vm *vm, int msi)
{
  memset(&vm->aplic, 0, sizeof(vm->aplic));
  vm_add_dev(vm, "aplic-m", -1, OBS_INTC, G_APLIC_M + vm->intc_shift, APLIC_MMIO_SIZE, vaplic_read, vaplic_write, (void *)(uint64)DOM_M);
  vm_add_dev(vm, "aplic-s", -1, OBS_INTC, G_APLIC_S + vm->intc_shift, APLIC_MMIO_SIZE, vaplic_read, vaplic_write, (void *)(uint64)DOM_S);
}
