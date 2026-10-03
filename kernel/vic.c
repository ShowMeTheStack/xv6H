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
#include "imsic.h"
#include "aplic.h"

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

// the three models by name, for the messages that report a change
const char *
vic_kindname(int kind)
{
  switch (kind) {
  case VIC_PLIC:  return "plic";
  case VIC_APLIC: return "aplic";
  case VIC_IMSIC: return "imsic";
  }
  return "?";
}

// how many interrupt lines a level set holds down. There is no libgcc in the
// kernel, so no __builtin_popcountll.
int
vic_held(uint64 levels)
{
  int n = 0;
  for (uint64 b = levels; b; b &= b - 1)
    n++;
  return n;
}

// ---- changing the kind of interrupt controller under a running guest ----
//
// The thing that makes this tractable is what does *not* have to be carried
// over. A guest's controller configuration -- which sources are enabled, at
// what priority, targeted at which hart -- is the guest's own, and the guest
// rebuilds all of it when it re-enumerates (intc_init() re-enables the sources
// it cares about). So translating PLIC enable bits into IMSIC EIE bits would be
// copying something that is about to be overwritten.
//
// What must be carried is the part the *host* owns: which interrupt lines are
// currently asserted. Every virtual device here is level-triggered -- vvirtio
// raises the line when intr_status becomes non-zero and drops it when the guest
// acknowledges -- so the set of asserted lines is a faithful summary of what the
// devices want, independent of any controller. Re-asserting them into the new
// model is exact rather than approximate.
//
// What cannot be carried: an interrupt the guest has claimed but not completed.
// The guest is inside its handler, holding an identity that the new controller
// will not recognise, and it will complete it against a controller that no
// longer exists. So a revision is refused while any source is in service, which
// is a bounded wait -- handlers are short.
//
// Returns 0, or -1 if this is not a safe moment.
int
vic_levels_snapshot(struct vm *vm, uint64 *levels)
{
  *levels = 0;
  switch (vm->vic_kind) {
  case VIC_PLIC:
    if (vm->plic.inservice)
      return -1;                       // mid-handler; ask again shortly
    *levels = vm->plic.level;
    return 0;
  case VIC_APLIC:
  case VIC_IMSIC:
    *levels = vm->aplic.level;
    return 0;
  }
  return -1;
}

// Build the new model from nothing and re-assert what the devices are holding.
// caller holds vm->lock; vm->vic_kind has already been changed. `from' is the
// kind the guest was bound to and `shift' where its registers were: on a live
// exchange that model is in the table and becomes the ghost; on an arrival it
// never was (this host built its own kind at creation), so the ghost is made
// from nothing at the source's address (defect 24). -1: the table's own.
void
vic_levels_restore(struct vm *vm, uint64 levels, int from, uint64 shift)
{
  // the outgoing controller's device models first, or they alias the incoming
  // ones (PLIC and APLIC-M both live at 0x0c000000) and the guest goes on
  // talking to the one it is leaving
  // ... kept as ghosts until the guest has acknowledged (hypmmio.c): a
  // stale ghost from an exchange the guest never acknowledged goes first
  vm_del_ghosts(vm);
  vm_ghost_dev(vm, "plic");
  vm_ghost_dev(vm, "aplic-m");
  vm_ghost_dev(vm, "aplic-s");
  vm_ghost_dev(vm, "imsic-m");
  vm_ghost_dev(vm, "imsic-s");
  if (from == VIC_PLIC)
    vm_add_ghost(vm, "plic", G_PLIC_BASE + shift, G_PLIC_SIZE);
  else if (from == VIC_APLIC || from == VIC_IMSIC) {
    vm_add_ghost(vm, "aplic-m", G_APLIC_M + shift, APLIC_MMIO_SIZE);
    vm_add_ghost(vm, "aplic-s", G_APLIC_S + shift, APLIC_MMIO_SIZE);
    if (from == VIC_IMSIC) {
      vm_add_ghost(vm, "imsic-m", G_IMSIC_M + shift, 0x1000L * NVCPU);
      vm_add_ghost(vm, "imsic-s", G_IMSIC_S + shift, 0x1000L * NVCPU);
    }
  }
  vic_init(vm);
  for (int irq = 1; irq < VPLIC_NIRQ; irq++)
    if (levels & (1ull << irq))
      vic_set_level(vm, irq, 1);
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
  // The quiet window (F3). While the guest is taking a renumbering -- it has
  // read the description and not yet acknowledged -- the two sides disagree on
  // the numbers: the host still raises on the old ones, the guest is moving
  // its handlers to the new ones, one device at a time. Where a device's old
  // number is another device's new one, an interrupt raised then runs the
  // wrong handler, which finds nothing, and the line is raised again: a storm
  // (hunt4g crossing 11: 1.65 M exits, CPU 0 stalled, the RTC never
  // re-registered, the ack never sent). Invariant 4 cannot serve both values
  // of a line; so it serves neither: the level is held here and raised on the
  // new number at the ack (vm_irq_alias_end). The anchor's own line and a
  // foreign guest's pinned console line are not renumbered and go through.
  if (vm->irq_quiet && irq > 0 && irq < VPLIC_NIRQ && irq != G_MACHREV_IRQ &&
      !(irq == G_UART_IRQ && VM_FOREIGN(vm))) {
    if (level) {
      vm->quiet_set |= 1ull << irq;
      vm->irq_quiet_held++;
      return;
    }
    vm->quiet_set &= ~(1ull << irq);
  }
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
  // An IMSIC the guest is leaving, as a ghost, like the windows of any controller it leaves:
  // its registers read as zero and ignore writes until the guest acknowledges. Its registers
  // are CSRs, and the guest's driver (patch 0005) silences the backend it leaves before it
  // brings up the new one -- by these CSRs, after this host has already rebuilt the model as
  // a PLIC. Refused, they were an illegal instruction inside the guest's cross-call: every
  // imsic -> plic exchange hung (lx-revise-intc, 3/3, with the guest of 3 October).
  int csr = inst >> 20;
  if (vm_revising(v->vm) &&
      (csr == CSR_SISELECT || csr == CSR_SIREG || csr == CSR_STOPEI || csr == CSR_STOPI ||
       csr == CSR_SISELECT + 0x100 || csr == CSR_SIREG + 0x100 || csr == CSR_STOPEI + 0x100 ||
       csr == CSR_STOPI + 0x100)) {
    int rdn = (inst >> 7) & 31;
    if (rdn)
      v->regs[rdn] = 0;
    v->pc += 4;
    return 0;
  }
  return -1;
}
