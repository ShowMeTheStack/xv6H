//
// virtual CLINT: only the msip registers matter. the guest's IPI code
// writes another vcpu's msip; hardware would raise a machine software
// interrupt that the guest's own mvec.S turns into SSIP. we short-cut
// that and inject the supervisor software interrupt directly.
// mtime reads return the real time counter (the guest uses Sstc for
// timers, so mtimecmp is accepted and ignored).
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

#define MSIP(i)     (4 * (i))
#define MTIMECMP(i) (0x4000 + 8 * (i))
#define MTIME       0xbff8

static int
vclint_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  *val = 0;
  if (off < 4 * NVCPU) {
    int i = vm_vcpu_of_hart(vm, off / 4);    // F9
    acquire(&vm->lock);
    if (i >= 0)
      *val = vm->clint_msip[i];
    release(&vm->lock);
    return 0;
  }
  if (off >= MTIME && off < MTIME + 8) {
    uint64 t = r_time();
    *val = (off == MTIME) ? t : (t >> 32);
    return 0;
  }
  return 0;
}

static int
vclint_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  if (off < 4 * NVCPU) {
    int i = vm_vcpu_of_hart(vm, off / 4);    // F9
    acquire(&vm->lock);
    if (i >= 0) {
      struct vcpu *v = &vm->vcpu[i];
      vm->clint_msip[i] = val & 1;
      if (val & 1) {
        v->ssip_req = 1;
        vcpu_kick(v);
      }
    }
    release(&vm->lock);
  }
  return 0;
}

// the model as a device of the machine; also what a revision that gives a
// machine its CLINT back registers (F4, hyp.c vm_revise_ipi)
void
vclint_add(struct vm *vm)
{
  vm_add_dev(vm, "clint", -1, OBS_TIMER, G_CLINT_BASE, 0x10000, vclint_read, vclint_write, 0);
}

void
vclint_init(struct vm *vm)
{
  memset(vm->clint_msip, 0, sizeof(vm->clint_msip));
  if (!vm->no_clint)     // F4: a machine without a CLINT sends its IPIs through SBI
    vclint_add(vm);
}
