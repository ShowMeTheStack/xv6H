//
// a syscall filter: what a process may ask the kernel for.
//
// A container's processes are hostile and root inside. The namespaces
// bound what they can name and the control group bounds what they can
// take; this bounds what they can *call*. It is the shape of seccomp
// without the bytecode: a bitmap of the calls a process may make, set
// once, inherited by its children, and never widened -- a process may
// give up a call and may not take it back, which is what makes it safe
// to hand the bitmap to a program that is about to exec something it
// does not trust.
//
// The default profile for a container (§8.2) denies what a container has
// no business doing even as root inside it:
//
//   the hypervisor        vm_*: a container does not run machines
//   the gateways          netroute, netgw: the machine's network
//   pivot_root, chroot    it has already pivoted; doing it again is a way
//                         out rather than in
//   setns                 joining somebody else's namespace
//   reboot                the machine's life is not its to end
//   sethostname           after the start: its name is its runtime's
//   ptrace                another process's registers
//   (mount and umount are not here: what a container may mount is a
//    short list -- a tmpfs, a procfs, a bind of what it can already
//    reach -- and the list is enforced in sys_mount, where it holds
//    whether or not this filter is on)
//   sched_group           the machine's scheduler groups: its share of
//                         the processor is its control group's
//
// What it does not deny is everything a program actually does: files,
// processes, pipes, signals, sockets, memory. A profile that denied
// those would be a profile nothing could run under.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "syscall.h"
#include "sysfilter.h"
#include "errno.h"

// the calls the container profile denies. One bit per call number, in
// the words of a bitmap that is copied into every process that asks for
// the profile.
static const int deny_container[] = {
    SYS_vm_create, SYS_vm_run,   SYS_vm_destroy, SYS_vm_ctl,
    SYS_vm_cons,   SYS_vm_info,  SYS_vm_stats,   SYS_vm_open,
    SYS_netroute,  SYS_netgw,    SYS_pivot_root, SYS_chroot,
    SYS_setns,     SYS_reboot,   SYS_sethostname, SYS_ptrace,
    SYS_consraw,   SYS_sched_group,
};

void
sysfilter_clear(struct sysfilter *f)
{
  if (f)
    memset(f, 0, sizeof(*f));
}

int
sysfilter_denies(const struct sysfilter *f, int num)
{
  if (f == 0 || !f->on || num < 0 || num >= NSYSCALL)
    return 0;
  return (f->deny[num / 64] & (1UL << (num % 64))) != 0;
}

// deny one call. A filter that is on may only ever deny more.
int
sysfilter_deny(struct sysfilter *f, int num)
{
  if (f == 0 || num <= 0 || num >= NSYSCALL)
    return -EINVAL;
  f->deny[num / 64] |= 1UL << (num % 64);
  f->on = 1;
  return 0;
}

// the profiles a program may ask for by name. There is one, which is the
// one a container gets; a program may also deny calls one at a time.
int
sysfilter_profile(struct sysfilter *f, int profile)
{
  if (f == 0)
    return -EINVAL;
  switch (profile) {
  case SYSFILTER_NONE:
    return 0;
  case SYSFILTER_CONTAINER:
    for (int i = 0; i < (int)(sizeof(deny_container) / sizeof(int)); i++)
      sysfilter_deny(f, deny_container[i]);
    return 0;
  default:
    return -EINVAL;
  }
}
