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
#include "vmstate.h"

#include "file.h"
#include "fcntl.h"
#include "posix.h"
#include "errno.h"

// every call below names a machine by id. A machine belongs to the
// user who created it: another user gets -EPERM, and learns nothing
// else about it. Holding a descriptor for it (vm_open, or one passed
// over a socket) is the other way in -- that is what a descriptor is
// for.
static struct vm *
vm_arg(int id, int *err)
{
  struct vm *vm = vm_lookup(id);

  *err = 0;
  if (vm == 0 || vm->state == VM_FREE) {
    // a machine that has exited is still readable through vm_cons and
    // still reports VM_FREE through vm_info; ownership of a slot that
    // holds nothing means nothing, so those two decide for themselves.
    *err = -1;
    return vm;
  }
  if (!vm_access(vm)) {
    *err = -EPERM;
    return 0;
  }
  return vm;
}

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
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
  return vm_run(vm, vcpu);
}

// vm_save(id, path): the machine, written into a file that exists
// already -- `vmm save' creates it, so that it belongs to whoever asked
// rather than to root and this kernel needs no second copy of open(2).
uint64
sys_vm_save(void)
{
  int vmid;
  char path[MAXPATH];

  argint(0, &vmid);
  if (argstr(1, path, sizeof(path)) < 0)
    return -EFAULT;
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
  return vm_save(vm, path);
}

// vm_restore(path, name): a machine made from a file, stopped, with the
// new name if one was given. Its id is the answer.
// vm_snapshot(id, path): the same file as a save, written while the
// machine runs (§9).
uint64
sys_vm_snapshot(void)
{
  int vmid;
  char path[MAXPATH];

  argint(0, &vmid);
  if (argstr(1, path, sizeof(path)) < 0)
    return -EFAULT;
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
  return vm_snapshot(vm, path);
}

// vm_clone(id, name, disk): a second machine sharing the first's pages.
uint64
sys_vm_clone(void)
{
  int vmid, newid = -1;
  char name[VM_NAMELEN], disk[MAXPATH];

  argint(0, &vmid);
  name[0] = disk[0] = 0;
  if (argstr(1, name, sizeof(name)) < 0)
    name[0] = 0;
  if (argstr(2, disk, sizeof(disk)) < 0)
    disk[0] = 0;
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
  int r = vm_clone(vm, name, disk, &newid);
  return r < 0 ? r : newid;
}

// vm_migrate(id, fd): the machine, down a connection somebody else has
// already made (§9, E11). The machine is paused for the length of the
// transfer -- a stop-and-copy -- and destroyed here once the other end
// has it, because two of the same machine running is one too many.
uint64
sys_vm_migrate(void)
{
  int vmid, fd;
  struct file *f;

  argint(0, &vmid);
  argint(1, &fd);
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
  if ((f = fdget(myproc(), fd)) == 0)
    return -EBADF;
  int r = vm_save_file(vm, f);
  if (r < 0)
    return r;
  // it is over there now. What is here must stop being a machine before
  // the other end starts it, which is why this is not a copy.
  vm_destroy(vm);
  return 0;
}

// vm_accept(fd, name): a machine arriving down a connection. Its id is
// the answer; it is stopped until somebody runs its vcpus.
uint64
sys_vm_accept(void)
{
  int fd, id = -1;
  char name[VM_NAMELEN];
  struct file *f;

  argint(0, &fd);
  name[0] = 0;
  if (argstr(1, name, sizeof(name)) < 0)
    name[0] = 0;
  if ((f = fdget(myproc(), fd)) == 0)
    return -EBADF;
  int r = vm_restore_file(f, name, &id);
  return r < 0 ? r : id;
}

uint64
sys_vm_restore(void)
{
  char path[MAXPATH], name[VM_NAMELEN];
  int id = -1;

  if (argstr(0, path, sizeof(path)) < 0)
    return -EFAULT;
  name[0] = 0;
  if (argstr(1, name, sizeof(name)) < 0)
    name[0] = 0;
  int r = vm_restore(path, name, &id);
  return r < 0 ? r : id;
}

uint64
sys_vm_destroy(void)
{
  int vmid;
  argint(0, &vmid);
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
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
  int err;
  struct vm *vm = vm_arg(vmid, &err);
  if (err)
    return err;
  switch (op) {
  case VMCTL_PAUSE:
    return vm_pause(vm);
  case VMCTL_RESUME:
    return vm_resume(vm);
  case VMCTL_REVISE:
    // arg is the REV_* mask of what the host intends to change; for now the
    // description is simply rebuilt and the guest asked to look again.
    return vm_revise(vm, (uint32)arg);
  case VMCTL_BALLOON:
    return vballoon_request(vm, arg * 1024 * 1024);
  case VMCTL_WEIGHT:
    return sched_group_set_weight(vm->sg, (uint32)arg);
  case VMCTL_CPUMAX:
    // hundredths of one processor, 0 for no ceiling
    return sched_group_set_max(vm->sg, arg * 1000, 0);
  case VMCTL_PORT_ADD:
    return vm_port_add(vm, (uint8)(arg >> 32), (uint16)(arg >> 16),
                       (uint16)arg);
  case VMCTL_PORT_RM:
    return vm_port_rm(vm, (uint8)(arg >> 32), (uint16)(arg >> 16));
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
  if (!vm_access(vm))
    return -EPERM;
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
  if (vm != 0 && vm->state != VM_FREE && !vm_access(vm)) {
    // somebody else's machine: `vmm ls' shows the slot as empty
    // rather than saying whose it is. /proc/vmm is where a privileged
    // program looks at the whole machine.
    memset(&info, 0, sizeof(info));
    info.id = vmid;
    info.state = VM_FREE;
    return copyout(myproc()->mm, addr, (char *)&info, sizeof(info)) < 0 ? -1 : 0;
  }
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
    acquire(&vm->portlock);
    info.nports = vm->nports;
    for (int i = 0; i < vm->nports; i++)
      info.ports[i] = vm->ports[i];
    release(&vm->portlock);
    info.net_filtered = vm->net_filtered;
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
    info.obs = vm->obs;
    for (int i = 0; i < OBS_COUNT && i < 14; i++)
      info.obs_first[i] = vm->obs_first[i];
    info.obs_now = vm->obs_t0 ? r_time() - vm->obs_t0 : 0;
    info.ndev = vm->ndev < 12 ? vm->ndev : 12;
    for (int i = 0; i < info.ndev; i++) {
      safestrcpy(info.dev[i].name, vm->dev[i].name ? vm->dev[i].name : "?",
                 sizeof(info.dev[i].name));
      info.dev[i].loads = vm->dev[i].loads;
      info.dev[i].stores = vm->dev[i].stores;
      info.dev[i].irqs = vm->dev[i].irq >= 0 && vm->dev[i].irq < VPLIC_NIRQ
                             ? vm->irq_raised[vm->dev[i].irq] : 0;
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
    for (int k = 0; k < XK_KINDS; k++)
      info.exit_hist[k] = v->exit_hist[k];
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

// vm_open(id, name, flags) -> a descriptor for a machine.
//
// A descriptor is the machine as an object: it may be inherited
// through fork, passed to another process over a unix socket with
// SCM_RIGHTS, read and written (the guest's console), and closed. An
// unnamed machine is destroyed when the last descriptor for it goes;
// a named one stays, because its name is how it is found again.
uint64
sys_vm_open(void)
{
  int id, flags;
  uint64 nameaddr;
  char name[VM_NAMELEN];
  struct vm *vm;
  struct file *f;
  int fd;

  argint(0, &id);
  argaddr(1, &nameaddr);
  argint(2, &flags);

  if (nameaddr) {
    if (fetchstr(nameaddr, name, sizeof(name)) < 0)
      return -EFAULT;
    vm = vm_by_name(name);
    if (vm == 0)
      return -ENOENT;
  } else {
    vm = vm_lookup(id);
    if (vm == 0 || vm->state == VM_FREE)
      return -ENOENT;
    if (!vm_access(vm))
      return -EPERM;
  }

  if ((f = filealloc()) == 0)
    return -ENFILE;
  f->type = FD_VM;
  f->readable = 1;
  f->writable = 1;
  f->vm = vm_fdget(vm);
  if ((fd = fdalloc(f)) < 0) {
    fileclose(f);
    return -EMFILE;
  }
  if (flags & O_CLOEXEC)
    fdflags_set(myproc(), fd, FD_CLOEXEC);
  return fd;
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
