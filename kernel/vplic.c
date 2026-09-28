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
  if (best_irq(&vm->plic, 2 * (v->id + vm->hart_shift) + 1) != 0)   // F9: the guest's context numbers
    return 1;
  // and the context it had under the previous numbering, while the revision
  // is pending: its enables are still there, and so is the line that tells
  // it about the renumbering (hyp.h vm_vcpu_of_hart)
  if (vm->hart_alias_on && vm->revise_acked != vm->revise_gen)
    return best_irq(&vm->plic, 2 * (v->id + vm->hart_alias_shift) + 1) != 0;
  return 0;
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
    int ctx = (off - ENABLE_BASE) / ENABLE_STRIDE;
    int word = ((off - ENABLE_BASE) % ENABLE_STRIDE) / 4;
    if (ctx < 2 * NVCPU && word == 0)
      v = p->enable[ctx];
  } else {
    int ctx = (off - CONTEXT_BASE) / CONTEXT_STRIDE;
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
    int ctx = (off - ENABLE_BASE) / ENABLE_STRIDE;
    int word = ((off - ENABLE_BASE) % ENABLE_STRIDE) / 4;
    if (ctx < 2 * NVCPU && word == 0)
      p->enable[ctx] = val & ~1u;
    notify(vm);
  } else {
    int ctx = (off - CONTEXT_BASE) / CONTEXT_STRIDE;
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
