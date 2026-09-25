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
vm_add_dev(struct vm *vm, const char *name, int irq, uint64 base, uint64 size,
           int (*read)(struct vm *, struct vdev *, uint64, int, uint64 *),
           int (*write)(struct vm *, struct vdev *, uint64, int, uint64), void *priv)
{
  if (vm->ndev >= VM_MAXDEV)
    panic("vm_add_dev");
  struct vdev *d = &vm->dev[vm->ndev++];
  d->name = name;
  d->irq = irq;
  d->loads = d->stores = 0;
  d->base = base;
  d->size = size;
  d->read = read;
  d->write = write;
  d->priv = priv;
}

static struct vdev *
find_dev(struct vm *vm, uint64 gpa)
{
  for (int i = 0; i < vm->ndev; i++) {
    struct vdev *d = &vm->dev[i];
    if (gpa >= d->base && gpa < d->base + d->size)
      return d;
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

  struct vdev *d = find_dev(vm, gpa);
  if (d == 0) {
    if (vm_gripe(&vm->g_fault))
      printk("hyp: vm %d vcpu %d: %s to unmapped gpa 0x%lx at pc 0x%lx (%ld times)\n",
             vm->id, v->id, is_store ? "store" : "load", gpa, v->pc, vm->g_fault);
    vcpu_inject(v, is_store ? EXC_STORE_ACCESS : EXC_LOAD_ACCESS, gpa);
    return 0;
  }

  uint64 off = gpa - d->base;
  int rc;
  // which device a guest's exits go to is the thing that says what to
  // build next: a count here costs one increment on a path that already
  // costs microseconds.
  if (m.is_load)
    d->loads++;
  else
    d->stores++;
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
