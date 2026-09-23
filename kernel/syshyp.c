//
// hypervisor system calls; see kernel/vmm.h for the interface.
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
#include "vmm.h"
#include "hwinfo.h"
#include "hyp.h"

uint64
sys_vm_create(void)
{
  uint64 addr;
  struct vmconfig cfg;
  argaddr(0, &addr);
  if (either_copyin(&cfg, 1, addr, sizeof(cfg)) < 0)
    return -1;
  cfg.name[VM_NAMELEN - 1] = 0;
  cfg.kernel[VM_PATHLEN - 1] = 0;
  cfg.disk[VM_PATHLEN - 1] = 0;
  return vm_create(&cfg);
}

uint64
sys_vm_run(void)
{
  int vmid, vcpu;
  argint(0, &vmid);
  argint(1, &vcpu);
  struct vm *vm = vm_lookup(vmid);
  if (vm == 0)
    return -1;
  return vm_run(vm, vcpu);
}

uint64
sys_vm_destroy(void)
{
  int vmid;
  argint(0, &vmid);
  struct vm *vm = vm_lookup(vmid);
  if (vm == 0)
    return -1;
  return vm_destroy(vm);
}

uint64
sys_vm_ctl(void)
{
  int vmid, op;
  uint64 arg;
  argint(0, &vmid);
  argint(1, &op);
  argaddr(2, &arg);
  // same-page merging belongs to the host, so it is answered before a
  // VM is looked up and the id means nothing
  if (op == VMCTL_KSM_ON || op == VMCTL_KSM_OFF)
    return ksm_set(op == VMCTL_KSM_ON);
  struct vm *vm = vm_lookup(vmid);
  if (vm == 0)
    return -1;
  switch (op) {
  case VMCTL_PAUSE:
    return vm_pause(vm);
  case VMCTL_RESUME:
    return vm_resume(vm);
  case VMCTL_BALLOON:
    return vballoon_request(vm, arg * 1024 * 1024);
  case VMCTL_WEIGHT:
    return sched_group_set_weight(vm->sg, (uint32)arg);
  case VMCTL_CPUMAX:
    // hundredths of one processor, 0 for no ceiling
    return sched_group_set_max(vm->sg, arg * 1000, 0);
  }
  return -1;
}

uint64
sys_vm_cons(void)
{
  int vmid, op, n;
  uint64 buf;
  argint(0, &vmid);
  argint(1, &op);
  argaddr(2, &buf);
  argint(3, &n);
  struct vm *vm = vm_lookup(vmid);
  if (vm == 0)
    return -1;
  // output left behind by a VM that already exited stays readable
  // until the slot is reused (short-lived guests would otherwise lose
  // their last words).
  if (vm->state == VM_FREE && op != VMCONS_READ)
    return -1;
  switch (op) {
  case VMCONS_READ:
    return vm_cons_read(vm, 1, buf, n);
  case VMCONS_WRITE:
    return vm_cons_write(vm, 1, buf, n);
  case VMCONS_DETACH:
    acquire(&vm->lock);
    vm->cons_attached = 0;
    vm->cons_detach_req = 1;
    wakeup(&vm->cons_out);
    release(&vm->lock);
    return 0;
  }
  return -1;
}

uint64
sys_vm_info(void)
{
  int vmid;
  uint64 addr;
  struct vminfo info;
  argint(0, &vmid);
  argaddr(1, &addr);
  struct vm *vm = vm_lookup(vmid);
  if (vm == 0) {
    // a slot never used: it exists, and it is free.
    if (vmid < 0 || vmid >= NVM)
      return -1;
    memset(&info, 0, sizeof(info));
    info.id = vmid;
    info.state = VM_FREE;
    return copyout(myproc()->mm, addr, (char *)&info, sizeof(info)) < 0 ? -1 : 0;
  }
  memset(&info, 0, sizeof(info));
  info.id = vm->id;
  info.state = vm->state;
  if (vm->state != VM_FREE) {
    info.nvcpu = vm->nvcpu;
    info.mem_mb = vm->ram_size / (1024 * 1024);
    info.resident_mb = vm->resident / (1024 * 1024);
    info.split_chunks = vm->nsplit;
    info.table_pages = vm->pttables;
    info.balloon_target_mb = (uint64)vm->balloon.target * PGSIZE / (1024 * 1024);
    info.balloon_actual_mb = (uint64)vm->balloon.actual * PGSIZE / (1024 * 1024);
    info.mem_limit_mb = vm->mem_limit / (1024 * 1024);
    {
      uint64 gs[7];
      sched_group_stat(vm->sg, gs, 7);
      info.weight = (int)gs[1];
      info.cpu_max_us = (int)gs[2];
      info.cpu_period_us = (int)gs[3];
      info.throttles = gs[4];
      info.frozen = (int)gs[5];
    }
    info.intc = vm->vic_kind;
    info.imsic_hw = vm->imsic_hw;
    info.cfg = vm->cfg;
    info.entry = vm->entry;
    info.disk_sectors = vm->blk.capacity;
    for (int i = 0; i < vm->nvcpu; i++) {
      struct vcpu *v = &vm->vcpu[i];
      info.exits += v->exits;
      info.mmio += v->mmio;
      info.csr += v->csr_emul;
      info.wfi += v->wfi_sleeps;
      info.injected += v->injected;
      info.exit_throttles += v->exit_throttles;
      info.nudges += v->nudges;
    }
    struct vcpu *v = &vm->vcpu[0];
    info.pc = v->pc;
    info.last_cause = v->last_cause;
    info.last_gpa = v->last_gpa;
    info.running_hart = v->running_hart;
    info.idle = v->idle;
    info.mmode = v->in_mmode;
    info.vsatp = v->vsatp;
    info.vstvec = v->vstvec;
    info.vsepc = v->vsepc;
    info.vscause = v->vscause;
    info.vsstatus = v->vsstatus;
    info.vstval = v->vstval;
    info.sp = v->regs[2];
    info.ra = v->regs[1];
    info.vsie = v->vsie;
    info.vstimecmp = v->vstimecmp;
    info.scounteren = v->scounteren;
    info.senvcfg = v->senvcfg;
    info.hcounteren = csr_read(0x606);
    info.eie0_0 = v->imsic.eie[0];
    info.eip0_0 = v->imsic.eip[0];
    info.deliv0 = v->imsic.eidelivery;
    info.thr0 = v->imsic.eithreshold;
    if (vm->nvcpu > 1) {
      struct vcpu *w = &vm->vcpu[1];
      info.pc1 = w->pc;
      info.cause1 = w->last_cause;
      info.exits1 = w->exits;
      info.idle1 = w->idle;
      info.hsm1 = w->hsm_state;
      info.hart1 = w->running_hart;
      info.eie0_1 = w->imsic.eie[0];
      info.eip0_1 = w->imsic.eip[0];
      info.deliv1 = w->imsic.eidelivery;
      info.thr1 = w->imsic.eithreshold;
    }
  }
  if (either_copyout(1, addr, &info, sizeof(info)) < 0)
    return -1;
  return 0;
}

// hypervisor statistics: buf[0] free guest memory in MB, buf[1] IPIs,
// buf[2+i] exits of vm i, buf[2+NVM] free guest memory in pages, and
// buf[3+NVM]..buf[6+NVM] same-page merging (on, scanned, merged,
// shared).
uint64
sys_vm_stats(void)
{
  uint64 addr, out[7 + NVM];
  int n;
  argaddr(0, &addr);
  argint(1, &n);
  if (n <= 0)
    return -1;
  if (n > 7 + NVM)
    n = 7 + NVM;
  memset(out, 0, sizeof(out));
  hyp_stats(out, n);
  if (either_copyout(1, addr, out, n * sizeof(uint64)) < 0)
    return -1;
  return 0;
}

uint64
sys_hwinfo(void)
{
  uint64 addr;
  struct hwinfo hi;
  argaddr(0, &addr);
  memset(&hi, 0, sizeof(hi));
  hi.misa = plat.misa;
  hi.ncpu = plat.ncpu;
  hi.has_h = plat.has_h;
  hi.has_v = plat.has_v;
  hi.vlenb = plat.vlenb;
  hi.is_guest = plat.is_guest;
  hi.guest_id = plat.guest_id;
  if (either_copyout(1, addr, &hi, sizeof(hi)) < 0)
    return -1;
  return 0;
}

uint64
sys_consraw(void)
{
  int on;
  argint(0, &on);
  consoleraw(on);
  return 0;
}
