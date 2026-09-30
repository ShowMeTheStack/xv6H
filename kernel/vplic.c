//
// virtual PLIC. models the gateway behaviour of level-triggered sources:
// a source becomes pending when its line is high, is taken out of the
// pending set by claim, and (if the line is still high) becomes pending
// again on completion.
//
// contexts: 2*vcpu is the vcpu's M-mode context (accepted, never
// delivers), 2*vcpu+1 is its S-mode context.
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
#include "hyp.h"

#define PRIORITY_BASE 0x000000
#define PENDING_BASE  0x001000
#define ENABLE_BASE   0x002000
#define ENABLE_STRIDE 0x80
#define CONTEXT_BASE  0x200000
#define CONTEXT_STRIDE 0x1000

// A context has two names (defect 29). The PLIC numbers its contexts by their
// order in the description: a stock Linux programs contexts 2*i+1 for the i-th
// hart it was told of, whatever that hart's id. xv6 (the guest and this host)
// numbers them by hart id, 2*hartid+1, which is the same thing on a machine
// whose harts are 0..n-1 and another thing on one numbered from four (F9): a
// Linux booted on such a host programmed contexts 1 and 3 and this model
// delivered on 9 and 11, and its disk's first interrupt never came. The model
// keeps one state per vcpu, under the index name, and answers to both names
// -- and to the name under the previous numbering while a renumbering is
// being taken (the hart alias). A shift smaller than the vcpu count would make
// the names collide, and is refused (hyp.c).
static int
canon(struct vm *vm, int ctx)
{
  int m = ctx & 1, c = ctx >> 1;
  if (c < vm->nvcpu)
    return ctx;                                          // the index name
  if (vm->hart_shift && c >= vm->hart_shift && c - vm->hart_shift < vm->nvcpu)
    return 2 * (c - vm->hart_shift) + m;                 // the hart-id name
  if (vm->hart_alias_on && vm->hart_alias_shift && c >= vm->hart_alias_shift &&
      c - vm->hart_alias_shift < vm->nvcpu)
    return 2 * (c - vm->hart_alias_shift) + m;           // the old hart-id name
  return ctx;                                            // nobody's: an empty context
}

// best claimable irq for a context, or 0. caller holds vm->lock.
static int
best_irq(struct vplic *p, int ctx)
{
  uint32 cand = p->pending & p->enable[ctx];
  int best = 0;
  uint32 bestprio = p->threshold[ctx];
  for (int i = 1; i < VPLIC_NIRQ; i++) {
    if ((cand & (1u << i)) && p->prio[i] > bestprio) {
      best = i;
      bestprio = p->prio[i];
    }
  }
  return best;
}

int
vplic_seip(struct vcpu *v)
{
  struct vm *vm = v->vm;
  // one state per vcpu, under the index name, whichever name the guest used
  return best_irq(&vm->plic, 2 * v->id + 1) != 0;
}

// caller holds vm->lock.
static void
notify(struct vm *vm)
{
  for (int i = 0; i < vm->nvcpu; i++) {
    struct vcpu *v = &vm->vcpu[i];
    if (vplic_seip(v))
      vcpu_kick(v);
  }
}

void
vplic_set_level(struct vm *vm, int irq, int level)
{
  struct vplic *p = &vm->plic;
  if (irq <= 0 || irq >= VPLIC_NIRQ)
    return;
  uint32 bit = 1u << irq;
  if (level) {
    if (p->level & bit)
      return;
    p->level |= bit;
    if (!(p->inservice & bit))
      p->pending |= bit;
    notify(vm);
  } else {
    p->level &= ~bit;
    // a pending level interrupt whose line dropped is withdrawn.
    p->pending &= ~bit;
  }
}

static int
vplic_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  struct vplic *p = &vm->plic;
  uint64 v = 0;

  if (size != 4)
    return -1;
  acquire(&vm->lock);
  if (off < PENDING_BASE) {
    int irq = off / 4;
    if (irq < VPLIC_NIRQ)
      v = p->prio[irq];
  } else if (off < ENABLE_BASE) {
    if (off == PENDING_BASE)
      v = p->pending;
  } else if (off < CONTEXT_BASE) {
    int ctx = canon(vm, (off - ENABLE_BASE) / ENABLE_STRIDE);
    int word = ((off - ENABLE_BASE) % ENABLE_STRIDE) / 4;
    if (ctx < 2 * NVCPU && word == 0)
      v = p->enable[ctx];
  } else {
    int ctx = canon(vm, (off - CONTEXT_BASE) / CONTEXT_STRIDE);
    int reg = (off - CONTEXT_BASE) % CONTEXT_STRIDE;
    if (ctx < 2 * NVCPU) {
      if (reg == 0) {
        v = p->threshold[ctx];
      } else if (reg == 4) {
        // claim
        int irq = best_irq(p, ctx);
        if (irq) {
          p->pending &= ~(1u << irq);
          p->inservice |= (1u << irq);
        }
        v = irq;
      }
    }
  }
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vplic_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  struct vplic *p = &vm->plic;

  if (size != 4)
    return -1;
  acquire(&vm->lock);
  if (off < PENDING_BASE) {
    int irq = off / 4;
    if (irq > 0 && irq < VPLIC_NIRQ)
      p->prio[irq] = val & 7;
    notify(vm);
  } else if (off < ENABLE_BASE) {
    // pending array is read-only
  } else if (off < CONTEXT_BASE) {
    int ctx = canon(vm, (off - ENABLE_BASE) / ENABLE_STRIDE);
    int word = ((off - ENABLE_BASE) % ENABLE_STRIDE) / 4;
    if (ctx < 2 * NVCPU && word == 0)
      p->enable[ctx] = val & ~1u;
    notify(vm);
  } else {
    int ctx = canon(vm, (off - CONTEXT_BASE) / CONTEXT_STRIDE);
    int reg = (off - CONTEXT_BASE) % CONTEXT_STRIDE;
    if (ctx < 2 * NVCPU) {
      if (reg == 0) {
        p->threshold[ctx] = val & 7;
        notify(vm);
      } else if (reg == 4) {
        // complete
        int irq = val;
        if (irq > 0 && irq < VPLIC_NIRQ) {
          uint32 bit = 1u << irq;
          p->inservice &= ~bit;
          if (p->level & bit)
            p->pending |= bit;
          notify(vm);
        }
      }
    }
  }
  release(&vm->lock);
  return 0;
}

void
vplic_init(struct vm *vm)
{
  memset(&vm->plic, 0, sizeof(vm->plic));
  vm_add_dev(vm, "plic", -1, OBS_INTC, G_PLIC_BASE + vm->intc_shift, G_PLIC_SIZE, vplic_read, vplic_write, 0);
}
