//
// MMIO emulation: decode the guest's faulting load or store and route
// it to the VM's virtual device at that guest-physical address.
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

struct memop {
  int len;     // instruction length: 2 or 4
  int size;    // access size in bytes
  int is_load;
  int reg;     // rd for loads, rs2 for stores
  int sign;    // sign-extend loaded value
};

// returns 0 on success, -1 if the instruction is not a plain load/store.
static int
decode(uint32 inst, struct memop *m)
{
  if ((inst & 3) == 3) {
    int opcode = inst & 0x7f;
    int funct3 = (inst >> 12) & 7;
    m->len = 4;
    if (opcode == 0x03) { // LOAD
      m->is_load = 1;
      m->reg = (inst >> 7) & 31;
      m->sign = !(funct3 & 4);
      m->size = 1 << (funct3 & 3);
      if (funct3 == 7)
        return -1;
      return 0;
    } else if (opcode == 0x23) { // STORE
      m->is_load = 0;
      m->reg = (inst >> 20) & 31;
      m->sign = 0;
      if (funct3 > 3)
        return -1;
      m->size = 1 << funct3;
      return 0;
    }
    return -1;
  }
  // compressed
  int q = inst & 3;
  int funct3 = (inst >> 13) & 7;
  m->len = 2;
  m->sign = 1;
  if (q == 0) {
    switch (funct3) {
    case 2: m->is_load = 1; m->size = 4; m->reg = 8 + ((inst >> 2) & 7); return 0; // c.lw
    case 3: m->is_load = 1; m->size = 8; m->reg = 8 + ((inst >> 2) & 7); return 0; // c.ld
    case 6: m->is_load = 0; m->size = 4; m->reg = 8 + ((inst >> 2) & 7); return 0; // c.sw
    case 7: m->is_load = 0; m->size = 8; m->reg = 8 + ((inst >> 2) & 7); return 0; // c.sd
    }
    // Zcb (this machine advertises it, so a guest may use it): the
    // byte and halfword forms of the compressed loads and stores, in
    // quadrant 0 with funct3 = 4. The three bits above rs1' say which,
    // and for the halfword forms bit 6 separates signed from unsigned
    // and load from store. A guest compiled for Zcb reaches a device
    // with these, and before they were decoded such an access was
    // reported as an instruction that could not be emulated.
    if (funct3 == 4) {
      int sel = (inst >> 10) & 7;
      int reg = 8 + ((inst >> 2) & 7);
      switch (sel) {
      case 0: // c.lbu
        m->is_load = 1; m->size = 1; m->sign = 0; m->reg = reg; return 0;
      case 1: // c.lhu (bit 6 clear) and c.lh (bit 6 set)
        m->is_load = 1; m->size = 2; m->sign = (inst >> 6) & 1; m->reg = reg;
        return 0;
      case 2: // c.sb
        m->is_load = 0; m->size = 1; m->sign = 0; m->reg = reg; return 0;
      case 3: // c.sh
        if ((inst >> 6) & 1)
          return -1;
        m->is_load = 0; m->size = 2; m->sign = 0; m->reg = reg; return 0;
      }
    }
  } else if (q == 2) {
    switch (funct3) {
    case 2: m->is_load = 1; m->size = 4; m->reg = (inst >> 7) & 31; return 0; // c.lwsp
    case 3: m->is_load = 1; m->size = 8; m->reg = (inst >> 7) & 31; return 0; // c.ldsp
    case 6: m->is_load = 0; m->size = 4; m->reg = (inst >> 2) & 31; return 0; // c.swsp
    case 7: m->is_load = 0; m->size = 8; m->reg = (inst >> 2) & 31; return 0; // c.sdsp
    }
  }
  return -1;
}

void
vm_add_dev(struct vm *vm, const char *name, int irq, uint32 obs, uint64 base, uint64 size,
           int (*read)(struct vm *, struct vdev *, uint64, int, uint64 *),
           int (*write)(struct vm *, struct vdev *, uint64, int, uint64), void *priv)
{
  if (vm->ndev >= VM_MAXDEV)
    panic("vm_add_dev");
  struct vdev *d = &vm->dev[vm->ndev++];
  d->name = name;
  d->irq0 = irq;
  d->irq = irq < 0 ? irq : vm_irq(vm, irq);   // F3: the numbering this machine has
  d->obs = obs;
  d->loads = d->stores = 0;
  d->base = base;
  d->size = size;
  d->read = read;
  d->write = write;
  d->priv = priv;
}

// Take a device model out of the table.
//
// Needed because an interrupt controller can be *replaced* under a running
// guest (mutable.md), and the models alias each other: a PLIC lives at
// 0x0c000000 and so does the APLIC's M domain. Adding the new one without
// removing the old leaves two models claiming one address, find_dev() returns
// whichever was registered first, and the guest keeps talking to the controller
// it is supposed to be leaving -- while the host delivers through the new one.
// A guest alive and deaf is the symptom.
void
vm_del_dev(struct vm *vm, const char *name)
{
  for (int i = 0; i < vm->ndev; i++) {
    if (vm->dev[i].name == 0 || strncmp(vm->dev[i].name, name, 12) != 0)
      continue;
    for (int j = i; j + 1 < vm->ndev; j++)
      vm->dev[j] = vm->dev[j + 1];
    vm->ndev--;
    memset(&vm->dev[vm->ndev], 0, sizeof(vm->dev[0]));
    i--;
  }
}

// The outgoing controller's window, kept until the acknowledgement. A guest
// stopped in the middle of an interrupt handler and moved to a host of
// another controller kind completes the interrupt on the controller it was
// bound to, which is gone: hunt2c crossing 570, an xv6 guest at
// aplic_complete() storing to 0xd002000 on a PLIC host, a store access fault,
// a panic, and the arrival never acknowledged -- Theorem 1's excluded case,
// met at a migration stop rather than a live exchange, and on the same
// evidence the first version's one arrival in 814. The old names keep
// answering: a ghost reads as zero and takes writes without effect, and the
// interrupt the guest was completing is re-delivered through the new
// controller when it re-enables the line, which a level-triggered handler
// takes without harm.
static int
ghost_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  *val = 0;
  return 0;
}

static int
ghost_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  return 0;
}

void
vm_ghost_dev(struct vm *vm, const char *name)
{
  for (int i = 0; i < vm->ndev; i++) {
    struct vdev *d = &vm->dev[i];
    if (d->name == 0 || strncmp(d->name, name, 12) != 0)
      continue;
    d->ghost = 1;
    d->read = ghost_read;
    d->write = ghost_write;
    d->irq = d->irq0 = -1;
  }
}

void
vm_del_ghosts(struct vm *vm)
{
  for (int i = 0; i < vm->ndev; i++) {
    if (!vm->dev[i].ghost)
      continue;
    for (int j = i; j + 1 < vm->ndev; j++)
      vm->dev[j] = vm->dev[j + 1];
    vm->ndev--;
    memset(&vm->dev[vm->ndev], 0, sizeof(vm->dev[0]));
    i--;
  }
}

static struct vdev *find_dev_(struct vm *vm, uint64 gpa, int *aliased, uint64 *tgpa);

// L20: the alias invariant's cost is a second lookup on a miss; count both
// kinds and their cycles so `vmm info' can say what it is
static struct vdev *
find_dev(struct vm *vm, uint64 gpa, uint64 *tgpa)
{
  // One map, not half of two. A revision moves the windows on another hart
  // while this one looks a device up without the lock (every MMIO exit comes
  // through here); a lookup that saw a base moved and a shift not yet moved
  // found no window, or the wrong register (alias1000 under two kernel builds:
  // 13 wrong reads in 1 250 revisions, 4 of them "load to unmapped gpa" on a
  // window that was there). So the lookup, and the translation of an aliased
  // address, are one snapshot: retried while the map is being moved or has
  // moved under them (hyp.h map_seq).
  int aliased = 0;
  uint64 c0 = r_cycle();
  struct vdev *d;
  uint32 seq;
  do {
    while ((seq = vm->map_seq) & 1)
      ;
    __sync_synchronize();
    aliased = 0;
    *tgpa = gpa;
    d = find_dev_(vm, gpa, &aliased, tgpa);
    __sync_synchronize();
  } while (seq != vm->map_seq);
  uint64 dc = r_cycle() - c0;
  if (aliased) {
    vm->lookup_alias++;
    vm->lookup_alias_cyc += dc;
  } else {
    vm->lookup_direct++;
    vm->lookup_direct_cyc += dc;
  }
  return d;
}

static struct vdev *
find_dev_(struct vm *vm, uint64 gpa, int *aliased, uint64 *tgpa)
{
  // the live models first; a ghost only where nothing live answers (the
  // PLIC and the APLIC's M domain share 0x0c000000)
  for (int i = 0; i < vm->ndev; i++) {
    struct vdev *d = &vm->dev[i];
    if (!d->ghost && gpa >= d->base && gpa < d->base + d->size)
      return d;
  }
  for (int i = 0; i < vm->ndev; i++) {
    struct vdev *d = &vm->dev[i];
    if (d->ghost && gpa >= d->base && gpa < d->base + d->size)
      return d;
  }
  // F15: the old device map, honoured until the guest has taken the new one
  // (hyp.h alias_shift). The virtio windows are the ones that move.
  int pending = vm_revising(vm);
  // F2: the controller's old window, the same way
  if (vm->intc_alias_on && pending && vm->intc_alias_shift != vm->intc_shift) {
    for (int i = 0; i < vm->ndev; i++) {
      struct vdev *d = &vm->dev[i];
      if (d->obs != OBS_INTC || d->ghost)
        continue;
      uint64 ob = d->base - vm->intc_shift + vm->intc_alias_shift;
      if (gpa >= ob && gpa < ob + d->size) {
        *aliased = 1;
        *tgpa = gpa - vm->intc_alias_shift + vm->intc_shift;
        return d;
      }
    }
  }
  if (vm->alias_on && pending &&
      gpa >= G_VIRTIO_BASE + vm->alias_shift && gpa < G_CON_BASE + 0x1000 + vm->alias_shift) {
    uint64 moved = gpa - vm->alias_shift + vm->mmio_shift;
    for (int i = 0; i < vm->ndev; i++) {
      struct vdev *d = &vm->dev[i];
      if (moved >= d->base && moved < d->base + d->size) {
        *aliased = 1;
        *tgpa = moved;
        return d;
      }
    }
  }
  return 0;
}

int
hyp_mmio(struct vcpu *v, uint64 gpa, int is_store, uint32 inst)
{
  struct vm *vm = v->vm;
  struct memop m;

  if (decode(inst, &m) < 0 || (m.is_load == is_store)) {
    if (vm_gripe(&vm->g_mmio))
      printk("hyp: vm %d vcpu %d: cannot emulate inst 0x%x at pc 0x%lx gpa 0x%lx "
             "(%ld times)\n", vm->id, v->id, inst, v->pc, gpa, vm->g_mmio);
    vcpu_inject(v, is_store ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS, gpa);
    return 0;
  }

  uint64 tgpa;
  struct vdev *d = find_dev(vm, gpa, &tgpa);
  if (d == 0) {
    if (vm_gripe(&vm->g_fault))
      printk("hyp: vm %d vcpu %d: %s to unmapped gpa 0x%lx at pc 0x%lx (%ld times)\n",
             vm->id, v->id, is_store ? "store" : "load", gpa, v->pc, vm->g_fault);
    vcpu_inject(v, is_store ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS, gpa);
    return 0;
  }

  gpa = tgpa;                     // reached through an old map (the F15 or the F2 alias): translated in the same snapshot
  uint64 off = gpa - d->base;
  int rc;
  // which device a guest's exits go to is the thing that says what to
  // build next: a count here costs one increment on a path that already
  // costs microseconds.
  if (m.is_load)
    d->loads++;
  else
    d->stores++;
  // and the first access to a device is the moment this guest learned the
  // device is there, which is what decides whether it could still be moved
  // to a machine that has no such device (OBS_* in hyp.h).
  if (d->loads + d->stores == 1)
    vm_observe(vm, d->obs);
  if (m.is_load) {
    uint64 val = 0;
    rc = d->read(vm, d, off, m.size, &val);
    if (rc == 0 && m.reg != 0) {
      switch (m.size) {
      case 1: val = m.sign ? (uint64)(long)(signed char)val : (uint8)val; break;
      case 2: val = m.sign ? (uint64)(long)(short)val : (uint16)val; break;
      case 4: val = m.sign ? (uint64)(long)(int)val : (uint32)val; break;
      }
      v->regs[m.reg] = val;
    }
  } else {
    uint64 val = v->regs[m.reg];
    switch (m.size) {
    case 1: val &= 0xff; break;
    case 2: val &= 0xffff; break;
    case 4: val &= 0xffffffff; break;
    }
    rc = d->write(vm, d, off, m.size, val);
  }
  if (rc < 0) {
    vcpu_inject(v, is_store ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS, gpa);
    return 0;
  }
  v->pc += m.len;
  v->mmio++;
  return 0;
}
