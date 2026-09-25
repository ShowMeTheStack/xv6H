//
// virtual interrupt controller front end: routes to the vPLIC, vAPLIC
// or vAPLIC-MSI + vIMSIC models depending on the VM's kind.
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

void
vic_init(struct vm *vm)
{
  switch (vm->vic_kind) {
  case VIC_PLIC:
    vplic_init(vm);
    break;
  case VIC_APLIC:
    vaplic_init(vm, 0);
    break;
  case VIC_IMSIC:
    vaplic_init(vm, 1);
    vimsic_init(vm);
    break;
  }
}

// a virtual device's interrupt line changed. caller holds vm->lock.
void
vic_set_level(struct vm *vm, int irq, int level)
{
  // Count the rising edges per interrupt line. This is the one place every
  // device model's interrupt goes through, and what it answers is whether
  // there is anything for VIRTIO_F_EVENT_IDX to coalesce: a device that
  // raises one interrupt per request it completes has nothing to save, and
  // one that raises fewer already coalesces. Cheap enough to leave in --
  // an interrupt costs the guest four exits to take.
  if (level && irq >= 0 && irq < VPLIC_NIRQ)
    vm->irq_raised[irq]++;
  switch (vm->vic_kind) {
  case VIC_PLIC:
    vplic_set_level(vm, irq, level);
    break;
  case VIC_APLIC:
  case VIC_IMSIC:
    vaplic_set_level(vm, irq, level);
    break;
  }
}

// is an external interrupt deliverable to v right now? caller holds vm->lock.
int
vic_seip(struct vcpu *v)
{
  switch (v->vm->vic_kind) {
  case VIC_PLIC:
    return vplic_seip(v);
  case VIC_APLIC:
    return vaplic_seip(v);
  case VIC_IMSIC:
    return vimsic_seip(v);
  }
  return 0;
}

// deliver an MSI (interrupt identity eiid) to a vcpu's interrupt file.
// caller holds vm->lock.
void
vic_msi(struct vm *vm, int vcpuid, int eiid)
{
  if (vcpuid < 0 || vcpuid >= vm->nvcpu)
    return;
  vimsic_send(&vm->vcpu[vcpuid], eiid);
}

// per-entry setup; returns the hvip bits to inject for this entry.
uint64
vic_vcpu_enter(struct vcpu *v)
{
  switch (v->vm->vic_kind) {
  case VIC_PLIC:
    return vplic_seip(v) ? HVIP_VSEIP : 0;
  case VIC_APLIC:
    return vaplic_seip(v) ? HVIP_VSEIP : 0;
  case VIC_IMSIC:
    return vimsic_enter(v);
  }
  return 0;
}

void
vic_vcpu_exit(struct vcpu *v)
{
  if (v->vm->vic_kind == VIC_IMSIC)
    vimsic_exit(v);
}

int
vic_csr(struct vcpu *v, uint32 inst)
{
  if (v->vm->vic_kind == VIC_IMSIC)
    return vimsic_csr(v, inst);
  return -1;
}
