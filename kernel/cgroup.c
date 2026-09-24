//
// control groups: what a set of processes may take.
//
// A group is a name, a place in a tree, and a few limits. The scheduler
// already had the two that concern the processor -- a weight and a
// ceiling, per group, in kernel/sched.c -- and this puts a tree around
// them and adds the ones that concern memory and processes:
//
//   cpu.weight      its share of the harts, on a task's scale
//   cpu.max         a ceiling on that share
//   memory.max      pages its processes may hold at once
//   memory.current  how many they hold
//   pids.max        processes it may have
//   pids.current    how many it has
//   freeze          stop them all, and start them again
//   procs           which processes are in it
//   stat            what it has used, and what it has been refused
//
// The limits are charged where the resource is taken: a fork into a full
// group fails as the machine's own process limit would, and a page fault
// in a group that is at its memory limit fails as the machine running
// out would. That is the whole of the mechanism -- there is no reclaim
// and no pressure signal -- and it is enough for the thing a container
// needs, which is to be unable to take the machine down.
//
// The tree is shallow and small (a machine with sixteen groups is a busy
// machine) and the whole of it appears under /sys/fs/cgroup.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "cgroup.h"
#include "errno.h"

static struct spinlock cglock;
static struct cgroup cgtab[NCGROUP];
static struct cgroup cgroot;

void
cginit(void)
{
  initlock(&cglock, "cgroup");
  safestrcpy(cgroot.name, "", CG_NAMELEN);
  cgroot.used = 1;
  cgroot.id = 0;
  cgroot.parent = 0;
}

struct cgroup *
cg_root(void)
{
  return &cgroot;
}

struct cgroup *
cg_current(void)
{
  struct proc *p = myproc();

  return (p && p->cg) ? p->cg : &cgroot;
}

struct cgroup *
cg_find(const char *name)
{
  if (name == 0 || name[0] == 0)
    return &cgroot;
  acquire(&cglock);
  for (int i = 0; i < NCGROUP; i++)
    if (cgtab[i].used && strncmp(cgtab[i].name, name, CG_NAMELEN) == 0) {
      release(&cglock);
      return &cgtab[i];
    }
  release(&cglock);
  return 0;
}

struct cgroup *
cg_at(int i)
{
  if (i < 0 || i >= NCGROUP)
    return 0;
  return cgtab[i].used ? &cgtab[i] : 0;
}

// a group of one's own, under `parent'. The scheduler group comes with
// it, because the processor controls live there.
int
cg_create(const char *name, struct cgroup *parent, struct cgroup **out)
{
  if (name == 0 || name[0] == 0)
    return -EINVAL;
  for (const char *p = name; *p; p++)
    if (*p == '/' || *p == ' ')
      return -EINVAL;
  if (cg_find(name))
    return -EEXIST;
  if (parent == 0)
    parent = &cgroot;

  acquire(&cglock);
  struct cgroup *cg = 0;
  for (int i = 0; i < NCGROUP; i++)
    if (!cgtab[i].used) {
      cg = &cgtab[i];
      break;
    }
  if (cg == 0) {
    release(&cglock);
    return -ENOSPC;
  }
  int id = (int)(cg - cgtab) + 1;
  memset(cg, 0, sizeof(*cg));
  cg->used = 1;
  cg->id = id;
  cg->parent = parent;
  safestrcpy(cg->name, name, CG_NAMELEN);
  release(&cglock);

  cg->sg = sched_group_create(name, 1024);
  if (cg->sg == 0) {
    acquire(&cglock);
    cg->used = 0;
    release(&cglock);
    return -ENOSPC;
  }
  if (out)
    *out = cg;
  return 0;
}

int
cg_destroy(struct cgroup *cg)
{
  if (cg == 0 || cg == &cgroot || !cg->used)
    return -EINVAL;
  if (cg->pids_current > 0)
    return -EBUSY;      // a group with processes in it is not taken away
  for (int i = 0; i < NCGROUP; i++)
    if (cgtab[i].used && cgtab[i].parent == cg)
      return -EBUSY;    // nor one with groups under it
  if (cg->sg)
    sched_group_destroy(cg->sg);
  acquire(&cglock);
  cg->used = 0;
  cg->sg = 0;
  release(&cglock);
  return 0;
}

// ---- the limits ----

int
cg_set_limit(struct cgroup *cg, int which, uint64 value)
{
  if (cg == 0 || !cg->used)
    return -EINVAL;
  switch (which) {
  case CG_CPU_WEIGHT:
    return cg == &cgroot ? -EINVAL
                         : sched_group_set_weight(cg->sg, (uint32)value);
  case CG_CPU_MAX:
    // hundredths of one processor, as vmm's --cpu-max is
    return cg == &cgroot ? -EINVAL
                         : sched_group_set_max(cg->sg, value * 1000, 0);
  case CG_MEMORY_MAX:
    acquire(&cglock);
    cg->mem_max = value;      // in pages; 0 is no limit
    release(&cglock);
    return 0;
  case CG_PIDS_MAX:
    acquire(&cglock);
    cg->pids_max = (int)value;
    release(&cglock);
    return 0;
  case CG_FREEZE:
    if (cg == &cgroot)
      return -EINVAL;
    cg->frozen = value != 0;
    return sched_group_freeze(cg->sg, value != 0);
  default:
    return -EINVAL;
  }
}

uint64
cg_get(struct cgroup *cg, int which)
{
  if (cg == 0 || !cg->used)
    return 0;
  uint64 gs[7];
  switch (which) {
  case CG_CPU_WEIGHT:
    if (cg == &cgroot)
      return 1024;
    sched_group_stat(cg->sg, gs, 7);
    return gs[1];
  case CG_CPU_MAX:
    if (cg == &cgroot)
      return 0;
    sched_group_stat(cg->sg, gs, 7);
    return gs[2] / 1000;
  case CG_MEMORY_MAX:     return cg->mem_max;
  case CG_MEMORY_CURRENT: return cg->mem_current;
  case CG_PIDS_MAX:       return (uint64)cg->pids_max;
  case CG_PIDS_CURRENT:   return (uint64)cg->pids_current;
  case CG_FREEZE:         return (uint64)cg->frozen;
  case CG_MEM_REFUSED:    return cg->mem_refused;
  case CG_PID_REFUSED:    return cg->pid_refused;
  case CG_CPU_THROTTLES:
    if (cg == &cgroot)
      return 0;
    sched_group_stat(cg->sg, gs, 7);
    return gs[4];
  default:                return 0;
  }
}

// ---- joining, and what joining costs ----

// a process joins a group: its processor time is accounted there, and
// its pages and its existence are charged to it. -EAGAIN if the group is
// full, which is what a fork into a full group returns.
int
cg_join(struct proc *p, struct cgroup *cg)
{
  struct cgroup *old = p->cg;

  if (cg == 0)
    cg = &cgroot;
  if (cg == old)
    return 0;
  acquire(&cglock);
  if (cg != &cgroot && cg->pids_max > 0 && cg->pids_current >= cg->pids_max) {
    cg->pid_refused++;
    release(&cglock);
    return -EAGAIN;
  }
  if (cg != &cgroot)
    cg->pids_current++;
  if (old && old != &cgroot && old->pids_current > 0)
    old->pids_current--;
  release(&cglock);
  p->cg = cg == &cgroot ? 0 : cg;
  sched_group_join(p, cg == &cgroot ? 0 : cg->sg);
  return 0;
}

// a process that has ended is out of its group
void
cg_leave(struct proc *p)
{
  struct cgroup *cg = p->cg;

  if (cg == 0)
    return;
  acquire(&cglock);
  if (cg->pids_current > 0)
    cg->pids_current--;
  // and the pages it held are not held any more
  if (cg->mem_current >= p->cg_pages)
    cg->mem_current -= p->cg_pages;
  else
    cg->mem_current = 0;
  release(&cglock);
  p->cg_pages = 0;
  p->cg = 0;
}

// a page for this process: may its group have it? The charge is per
// process, so that when the process ends what it held is given back
// without walking anything.
int
cg_charge_page(struct proc *p)
{
  struct cgroup *cg = p ? p->cg : 0;

  if (cg == 0)
    return 0;
  acquire(&cglock);
  if (cg->mem_max && cg->mem_current >= cg->mem_max) {
    cg->mem_refused++;
    release(&cglock);
    return -ENOMEM;
  }
  cg->mem_current++;
  p->cg_pages++;
  release(&cglock);
  return 0;
}

void
cg_uncharge_page(struct proc *p, int n)
{
  struct cgroup *cg = p ? p->cg : 0;

  if (cg == 0 || n <= 0)
    return;
  acquire(&cglock);
  cg->mem_current = cg->mem_current > (uint64)n ? cg->mem_current - n : 0;
  p->cg_pages = p->cg_pages > (uint64)n ? p->cg_pages - n : 0;
  release(&cglock);
}

// which processes are in it, for the `procs' file
void
cg_walk_procs(struct cgroup *cg, void (*fn)(int pid, void *), void *arg)
{
  if (cg == 0)
    return;
  proc_iter_start();
  for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
    if (p->state == UNUSED)
      continue;
    struct cgroup *pc = p->cg ? p->cg : &cgroot;
    if (pc == cg)
      fn(p->pid, arg);
  }
  proc_iter_end();
}
