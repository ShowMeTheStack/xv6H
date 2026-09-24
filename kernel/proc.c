//
// processes. there is no process table: a process is an object from a
// slab cache, allocated at fork and freed when reaped. processes are
// found by pid through a hash table, walked (rarely) through a global
// list, and their children are a list per parent. sleeping processes
// wait on hashed wait queues, runnable ones sit on a run queue, and
// every kernel stack is a slot in the kernel stack region (kstack.c).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "nsproxy.h"
#include "cgroup.h"
#include "platform.h"
#include "slab.h"
#include "page.h"
#include "mm.h"
#include "signal.h"
#include "thread.h"
#include "errno.h"
#include "posix.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "file.h"

struct cpu cpus[NCPU];

struct proc *initproc;


extern void forkret(void);
static void freeproc(struct proc *p);
static void proc_put(struct proc *p);
static void waitq_cancel(struct proc *p);
static void unlink_child(struct proc *parent, struct proc *child);
static int wait_match(struct proc *parent, struct proc *child, int wpid);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// ---- the set of processes ----

static struct kmem_cache *proc_cache;
static struct kmem_cache *fdt_cache, *fs_cache, *tg_cache;

// every live process, and the pid hash. proc_lock is acquired before
// any p->lock.
static struct spinlock proc_lock;
static struct proc *all_procs;      // doubly linked, unordered
#define PIDHASH 4096
static struct proc *pid_hash[PIDHASH];
static uint64 nprocs;               // live processes

// pids: a bitmap up to pid_max, allocated cyclically.
uint64 pid_max = PID_MAX;
static uint64 *pid_bitmap;
static uint64 pid_next_hint = 1;
static struct spinlock pid_lock;

// ---- wait queues ----

#define NWAITQ 256
struct waitq {
  struct spinlock lock;
  struct proc *head;   // doubly linked through wq_next/wq_prev
};
static struct waitq waitqs[NWAITQ];

static struct waitq *
waitq_of(void *chan)
{
  uint64 h = (uint64)chan;
  h ^= h >> 17;
  h *= 0x9E3779B97F4A7C15UL;
  return &waitqs[(h >> 40) % NWAITQ];
}

// the run queue lives in sched.c: a red-black tree per hart, ordered
// by virtual time.

// ---- init ----

void
procinit(void)
{
  initlock(&pid_lock, "pid");
  initlock(&wait_lock, "wait_lock");
  initlock(&proc_lock, "procs");
  for (int i = 0; i < NWAITQ; i++)
    initlock(&waitqs[i].lock, "waitq");
  proc_cache = kmem_cache_create("proc", sizeof(struct proc), 64, 0);
  fdt_cache = kmem_cache_create("fdtable", sizeof(struct fdtable), 8, 0);
  fs_cache = kmem_cache_create("fsinfo", sizeof(struct fsinfo), 8, 0);
  tg_cache = kmem_cache_create("tgroup", sizeof(struct tgroup), 8, 0);

  // the pid bitmap: pid_max bits (pidmax=N on the command line, for tests)
  int order = 0;
  while (((uint64)PGSIZE << order) < (pid_max + 63) / 64 * 8)
    order++;
  pid_bitmap = kalloc_order(order);
  if (pid_bitmap == 0)
    panic("procinit: pid bitmap");
  memset(pid_bitmap, 0, (uint64)PGSIZE << order);
  pid_bitmap[0] |= 1; // pid 0 is nobody
}

// pidmax=N on the host's command line sets a small pid space (tests).
void
procinit_early(void)
{
  const char *s = plat.bootargs;
  while (*s) {
    while (*s == ' ')
      s++;
    if (memcmp(s, "pidmax=", 7) == 0) {
      uint64 v = 0;
      for (s += 7; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (*s - '0');
      if (v >= 64 && v <= PID_MAX)
        pid_max = v;
    }
    while (*s && *s != ' ')
      s++;
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// ---- pids ----

int
allocpid()
{
  acquire(&pid_lock);
  for (uint64 tries = 0; tries < pid_max; tries++) {
    uint64 pid = pid_next_hint + tries;
    if (pid >= pid_max)
      pid = 1 + (pid - pid_max) % (pid_max - 1);
    if ((pid_bitmap[pid / 64] & (1UL << (pid % 64))) == 0) {
      pid_bitmap[pid / 64] |= 1UL << (pid % 64);
      pid_next_hint = pid + 1;
      release(&pid_lock);
      return (int)pid;
    }
  }
  release(&pid_lock);
  return -1;
}

static void
freepid(int pid)
{
  acquire(&pid_lock);
  pid_bitmap[pid / 64] &= ~(1UL << (pid % 64));
  release(&pid_lock);
}

// is this number in use in this namespace? (kernel/nsproxy.c asks when
// it hands one out; the table of processes is here.)
int
proc_pid_taken_in_ns(struct pidns *ns, int nr)
{
  int taken = 0;

  acquire(&proc_lock);
  for (struct proc *p = all_procs; p; p = p->all_next) {
    if (p->state == UNUSED)
      continue;
    for (int i = 0; i < p->npids; i++)
      if (p->pids[i].ns == ns && p->pids[i].nr == nr) {
        taken = 1;
        break;
      }
    if (taken)
      break;
  }
  release(&proc_lock);
  return taken;
}

// the process that answers to this number in this namespace, held, or 0.
struct proc *
proc_get_ns(struct pidns *ns, int nr)
{
  if (ns == 0 || ns == pidns_root())
    return proc_get(nr);
  if (nr <= 0)
    return 0;
  acquire(&proc_lock);
  for (struct proc *p = all_procs; p; p = p->all_next) {
    if (p->state == UNUSED || p->state == ZOMBIE)
      continue;
    if (pid_in_ns(p, ns) == nr)
      return p;            // proc_lock stays held, as proc_get leaves it
  }
  release(&proc_lock);
  return 0;
}

// the live process with this pid, or 0 (proc_lock held by the caller).
static struct proc *
pid_lookup(int pid)
{
  // a number nobody can have. It matters that this is checked and not
  // assumed: C's remainder keeps the sign of its left operand, so a
  // negative pid indexed in front of the table, and the pointer read
  // out of whatever is there was followed. getsid(-1) panicked the
  // machine, from any process, in or out of a container.
  if (pid <= 0)
    return 0;
  for (struct proc *p = pid_hash[pid % PIDHASH]; p; p = p->pid_next)
    if (p->pid == pid)
      return p;
  return 0;
}

// ---- file descriptor tables and the file-system view ----


struct fdtable *
fdtable_alloc(void)
{
  struct fdtable *t = kmem_cache_alloc(fdt_cache);
  if (t == 0)
    return 0;
  memset(t, 0, sizeof(*t));
  initlock(&t->lock, "fdtable");
  t->ref = 1;
  return t;
}

struct fdtable *
fdtable_dup(struct fdtable *t)
{
  acquire(&t->lock);
  t->ref++;
  release(&t->lock);
  return t;
}

void
fdtable_put(struct fdtable *t)
{
  if (t == 0)
    return;
  acquire(&t->lock);
  int n = --t->ref;
  release(&t->lock);
  if (n > 0)
    return;
  for (int fd = 0; fd < t->n; fd++)
    if (t->ofile[fd])
      fileclose(t->ofile[fd]);
  if (t->ofile)
    kfree(t->ofile);
  if (t->flags)
    kfree(t->flags);
  kmem_cache_free(fdt_cache, t);
}

// grow the table to at least n entries (t->lock held)
static int
fdt_grow(struct fdtable *t, int n)
{
  if (n > NOFILE)
    return -1;
  int cap = t->n ? t->n : NOFILE_INIT;
  while (cap < n)
    cap *= 2;
  if (cap > NOFILE)
    cap = NOFILE;
  if (cap == t->n)
    return 0;
  int order = 0;
  while (((uint64)PGSIZE << order) < (uint64)cap * sizeof(struct file *))
    order++;
  struct file **nf = kalloc_order(order);
  if (nf == 0)
    return -1;
  memset(nf, 0, (uint64)PGSIZE << order);
  int forder = 0;
  while (((uint64)PGSIZE << forder) < (uint64)cap)
    forder++;
  char *nl = kalloc_order(forder);
  if (nl == 0) {
    kfree_order(nf, order);
    return -1;
  }
  memset(nl, 0, (uint64)PGSIZE << forder);
  if (t->ofile) {
    memmove(nf, t->ofile, t->n * sizeof(struct file *));
    memmove(nl, t->flags, t->n);
    kfree(t->ofile);
    kfree(t->flags);
  }
  t->ofile = nf;
  t->flags = nl;
  t->n = cap;
  return 0;
}

// a private copy for fork()
struct fdtable *
fdtable_copy(struct fdtable *src)
{
  struct fdtable *t = fdtable_alloc();
  if (t == 0)
    return 0;
  acquire(&src->lock);
  if (fdt_grow(t, src->n ? src->n : NOFILE_INIT) < 0) {
    release(&src->lock);
    fdtable_put(t);
    return 0;
  }
  for (int fd = 0; fd < src->n; fd++) {
    if (src->ofile[fd]) {
      t->ofile[fd] = filedup(src->ofile[fd]);
      t->flags[fd] = src->flags[fd];
    }
  }
  release(&src->lock);
  return t;
}

int
fdtable_grow_to(struct proc *p, int n)
{
  struct fdtable *t = p->fdt;
  acquire(&t->lock);
  int r = fdt_grow(t, n);
  release(&t->lock);
  return r;
}

// the descriptor flags (FD_CLOEXEC) of fd, or -1.
int
fdflags_get(struct proc *p, int fd)
{
  struct fdtable *t = p->fdt;
  acquire(&t->lock);
  int r = (fd < 0 || fd >= t->n || t->ofile[fd] == 0) ? -1 : t->flags[fd];
  release(&t->lock);
  return r;
}

int
fdflags_set(struct proc *p, int fd, int flags)
{
  struct fdtable *t = p->fdt;
  acquire(&t->lock);
  int r = -1;
  if (fd >= 0 && fd < t->n && t->ofile[fd]) {
    t->flags[fd] = flags & FD_CLOEXEC;
    r = 0;
  }
  release(&t->lock);
  return r;
}

// allocate a descriptor at or above `from` for f.
int
fdalloc_from(struct file *f, int from)
{
  struct fdtable *t = myproc()->fdt;
  if (from < 0)
    return -EINVAL;
  acquire(&t->lock);
  for (;;) {
    for (int fd = from; fd < t->n; fd++) {
      if (t->ofile[fd] == 0) {
        t->ofile[fd] = f;
        t->flags[fd] = 0;
        release(&t->lock);
        return fd;
      }
    }
    if (t->n >= NOFILE || fdt_grow(t, t->n ? t->n * 2 : NOFILE_INIT) < 0) {
      release(&t->lock);
      return -EMFILE;
    }
  }
}

int
fdalloc(struct file *f)
{
  return fdalloc_from(f, 0);
}

// the file behind fd, or 0.
struct file *
fdget(struct proc *p, int fd)
{
  struct fdtable *t = p->fdt;
  acquire(&t->lock);
  struct file *f = (fd < 0 || fd >= t->n) ? 0 : t->ofile[fd];
  release(&t->lock);
  return f;
}

// does this process hold a descriptor for this object? The hypervisor
// asks it of a VM: a descriptor passed over a socket is what lets a
// process work a machine it does not own (kernel/hyp.c, vm_access).
int
proc_has_file(struct proc *p, int type, void *obj)
{
  struct fdtable *t = p->fdt;
  int found = 0;

  acquire(&t->lock);
  for (int i = 0; i < t->n; i++) {
    struct file *f = t->ofile[i];
    if (f && f->type == type && (void *)f->vm == obj) {
      found = 1;
      break;
    }
  }
  release(&t->lock);
  return found;
}

// take fd out of the table and return what was there.
struct file *
fdtake(struct proc *p, int fd)
{
  struct fdtable *t = p->fdt;
  acquire(&t->lock);
  struct file *f = 0;
  if (fd >= 0 && fd < t->n) {
    f = t->ofile[fd];
    t->ofile[fd] = 0;
    t->flags[fd] = 0;
  }
  release(&t->lock);
  return f;
}

// put f at fd, closing whatever was there (for dup2 and pipe cleanup).
struct file *
fdput(struct proc *p, int fd, struct file *f, int flags)
{
  struct fdtable *t = p->fdt;
  acquire(&t->lock);
  struct file *old = 0;
  if (fd >= 0 && fd < t->n) {
    old = t->ofile[fd];
    t->ofile[fd] = f;
    t->flags[fd] = flags & FD_CLOEXEC;
  }
  release(&t->lock);
  return old;
}

// exec: close the descriptors marked close-on-exec.
void
fdtable_cloexec(struct proc *p)
{
  struct fdtable *t = p->fdt;
  for (int fd = 0; fd < t->n; fd++) {
    acquire(&t->lock);
    struct file *f = 0;
    if (t->ofile[fd] && (t->flags[fd] & FD_CLOEXEC)) {
      f = t->ofile[fd];
      t->ofile[fd] = 0;
      t->flags[fd] = 0;
    }
    release(&t->lock);
    if (f)
      fileclose(f);
  }
}

// ---- thread groups ----

static struct tgroup *
tgroup_alloc(int tgid)
{
  struct tgroup *tg = kmem_cache_alloc(tg_cache);
  if (tg == 0)
    return 0;
  memset(tg, 0, sizeof(*tg));
  initlock(&tg->lock, "tgroup");
  tg->ref = 1;
  tg->nthreads = 1;
  tg->tgid = tgid;
  return tg;
}

static struct tgroup *
tgroup_join(struct tgroup *tg)
{
  acquire(&tg->lock);
  tg->ref++;
  tg->nthreads++;
  release(&tg->lock);
  return tg;
}

// this thread stopped running: returns 1 if it was the last one. the
// group object itself lives until every thread's proc is freed.
static int
tgroup_leave(struct tgroup *tg)
{
  acquire(&tg->lock);
  int last = (--tg->nthreads == 0);
  release(&tg->lock);
  return last;
}

static void
tgroup_put(struct tgroup *tg)
{
  acquire(&tg->lock);
  int n = --tg->ref;
  release(&tg->lock);
  if (n == 0)
    kmem_cache_free(tg_cache, tg);
}

// ask every other thread of this group to exit, as exit_group() and
// exec() do. the caller must hold no process locks.
void
tgroup_kill_others(struct proc *p)
{
  if (proc_nthreads(p) <= 1)
    return;
  proc_iter_start();
  for (struct proc *q = proc_iter_next(0); q; q = proc_iter_next(q)) {
    if (q != p && q->tg == p->tg && q->state != UNUSED && q->state != ZOMBIE)
      signal_send(q, SIGKILL, 0);
  }
  proc_iter_end();
}

// how many threads the group has (for the tests and /proc)
int
proc_nthreads(struct proc *p)
{
  acquire(&p->tg->lock);
  int n = p->tg->nthreads;
  release(&p->tg->lock);
  return n;
}

// ---- the file-system view ----

struct fsinfo *
fsinfo_alloc(void)
{
  struct fsinfo *fs = kmem_cache_alloc(fs_cache);
  if (fs == 0)
    return 0;
  memset(fs, 0, sizeof(*fs));
  initlock(&fs->lock, "fsinfo");
  fs->ref = 1;
  return fs;
}

struct fsinfo *
fsinfo_dup(struct fsinfo *fs)
{
  acquire(&fs->lock);
  fs->ref++;
  release(&fs->lock);
  return fs;
}

struct fsinfo *
fsinfo_copy(struct fsinfo *src)
{
  struct fsinfo *fs = fsinfo_alloc();
  if (fs == 0)
    return 0;
  acquire(&src->lock);
  fs->cwd = src->cwd ? idup(src->cwd) : 0;
  fs->root = src->root ? idup(src->root) : 0;
  release(&src->lock);
  return fs;
}

void
fsinfo_put(struct fsinfo *fs)
{
  if (fs == 0)
    return;
  acquire(&fs->lock);
  int n = --fs->ref;
  struct inode *cwd = fs->cwd;
  struct inode *root = fs->root;
  release(&fs->lock);
  if (n > 0)
    return;
  if (cwd) {
    iput(cwd);
  }
  if (root) {
    iput(root);
  }
  kmem_cache_free(fs_cache, fs);
}

// the working directory, with a reference for the caller to drop.
struct inode *
proc_cwd(struct proc *p)
{
  struct fsinfo *fs = p->fs;
  acquire(&fs->lock);
  struct inode *ip = fs->cwd ? idup(fs->cwd) : 0;
  release(&fs->lock);
  return ip;
}

// what "/" means for this process, or zero for the file system's own
// root. the caller owns the reference.
struct inode *
proc_root(struct proc *p)
{
  struct fsinfo *fs = p->fs;

  if (fs == 0)
    return 0;
  acquire(&fs->lock);
  struct inode *ip = fs->root ? idup(fs->root) : 0;
  release(&fs->lock);
  return ip;
}

// chroot: "/" becomes this directory. the caller owns `ip` and this
// takes it over.
void
proc_set_root(struct proc *p, struct inode *ip)
{
  struct fsinfo *fs = p->fs;
  struct inode *old;

  acquire(&fs->lock);
  old = fs->root;
  fs->root = ip;
  release(&fs->lock);
  if (old)
    iput(old);
}

// change it; the old one is returned for the caller to release.
struct inode *
proc_set_cwd(struct proc *p, struct inode *ip)
{
  struct fsinfo *fs = p->fs;
  acquire(&fs->lock);
  struct inode *old = fs->cwd;
  fs->cwd = ip;
  release(&fs->lock);
  return old;
}

// ---- allocation and release ----

// Create a process: initialize state required to run in the kernel,
// and return with p->lock held. Returns 0 if memory runs out.
static struct proc *
allocproc(void)
{
  static uint64 complaints;
  const char *why = 0;
  struct proc *p = kmem_cache_alloc(proc_cache);
  if (p == 0) {
    why = "no memory for the process object";
    goto complain;
  }
  memset(p, 0, sizeof(*p));
  initlock(&p->lock, "proc");
  // no hart yet: the scheduler places it where it is made, not on
  // hart 0, which is what a zeroed field would mean.
  p->cpu = -1;
  p->cpus = ~0UL;    // no affinity: any hart will do
  p->pid = allocpid();
  if (p->pid < 0) {
    kmem_cache_free(proc_cache, p);
    why = "no free pid";
    goto complain;
  }

  // its numbers. Until something puts it in a namespace of its own it
  // has exactly one: the machine's.
  p->pids[0].ns = pidns_root();
  p->pids[0].nr = p->pid;
  p->npids = 1;

  // on the lists, so kill() and ps see it; state USED until it runs.
  acquire(&proc_lock);
  p->all_next = all_procs;
  p->all_prev = 0;
  if (all_procs)
    all_procs->all_prev = p;
  all_procs = p;
  p->pid_next = pid_hash[p->pid % PIDHASH];
  pid_hash[p->pid % PIDHASH] = p;
  nprocs++;
  release(&proc_lock);

  acquire(&p->lock);
  p->state = USED;

  p->kstack = kstack_alloc();
  if (p->kstack == 0) {
    why = "no kernel stack";
    goto fail;
  }

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    why = "no memory for the trapframe";
    goto fail;
  }
  memset(p->trapframe, 0, sizeof(*p->trapframe));
  if (plat.has_v) {
    if ((p->vstate = kalloc()) == 0) {
      why = "no memory for the vector state";
      goto fail;
    }
    memset(p->vstate, 0, PGSIZE);
  }

  // An empty address space.
  p->mm = proc_mm_create(p);
  p->tf_va = TRAPFRAME; // the only thread of a fresh address space
  if (p->mm == 0) {
    why = "no memory for the page table";
    goto fail;
  }

  p->fdt = fdtable_alloc();
  p->fs = fsinfo_alloc();
  if (p->fdt == 0 || p->fs == 0 || fdtable_grow_to(p, NOFILE_INIT) < 0) {
    why = "no memory for the file table";
    goto fail;
  }
  p->tg = tgroup_alloc(p->pid);
  if (p->tg == 0) {
    why = "no memory for the thread group";
    goto fail;
  }

  p->sighand = sighand_alloc();
  if (p->sighand == 0) {
    why = "no memory for the signal handlers";
    goto fail;
  }
  p->sigaltstk.ss_flags = SS_DISABLE;

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + KSTACK_PAGES * PGSIZE;

  return p;

fail:
  freeproc(p);
  release(&p->lock);
  proc_put(p);
complain:
  if (complaints++ < 8)
    printk("fork: %s (%ld processes, %ld free pages)\n", why, proc_count(), kmem_free_pages());
  return 0;
}

// free the data hanging from a proc, including user pages, and mark it
// UNUSED. p->lock must be held. the object itself and its kernel stack
// are released by proc_put() once the lock is dropped.
// p->lock is held: the address space is detached here and released by
// the caller (proc_put), because dropping inode references may sleep.
static void
freeproc(struct proc *p)
{
  // out of its scheduler group: what it is counted for there goes
  // with it (kernel/sched.c)
  if (p->sg)
    sched_group_join(p, 0);
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->vstate)
    kfree(p->vstate);
  p->vstate = 0;
  if (p->mm && p->tf_va && p->tf_va != TRAPFRAME)
    mm_del_trapframe(p->mm, p); // a thread's own trapframe page
  p->detached_mm = p->mm;
  p->mm = 0;
  p->detached_fdt = p->fdt;
  p->fdt = 0;
  p->detached_fs = p->fs;
  p->fs = 0;
  sighand_put(p->sighand);
  p->sighand = 0;
  cg_leave(p);
  pidns_leave(p);
  ns_put(p->ns);
  p->ns = 0;
  if (p->tg) {
    tgroup_put(p->tg);
    p->tg = 0;
  }
  p->clear_tid = 0;
  p->set_tid = 0;
  p->futex_until = 0;
  p->autoreap = 0;
  sigq_free_all(p);
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// unlink a freed proc and give back its stack, pid and object. the
// caller must not hold p->lock, and must not be running on p's stack.
static void
proc_put(struct proc *p)
{
  if (p->detached_mm) {
    proc_mm_free(p->detached_mm); // may sleep: no locks are held here
    p->detached_mm = 0;
  }
  if (p->detached_fdt) {
    fdtable_put(p->detached_fdt);
    p->detached_fdt = 0;
  }
  if (p->detached_fs) {
    fsinfo_put(p->detached_fs);
    p->detached_fs = 0;
  }
  acquire(&proc_lock);
  if (p->all_prev)
    p->all_prev->all_next = p->all_next;
  else
    all_procs = p->all_next;
  if (p->all_next)
    p->all_next->all_prev = p->all_prev;
  struct proc **pp = &pid_hash[p->pid % PIDHASH];
  while (*pp && *pp != p)
    pp = &(*pp)->pid_next;
  if (*pp)
    *pp = p->pid_next;
  nprocs--;
  release(&proc_lock);
  if (p->kstack)
    kstack_free(p->kstack);
  p->kstack = 0;
  freepid(p->pid);
  kmem_cache_free(proc_cache, p);
}

// Create an address space for a process, with no user memory but with
// the trampoline and trapframe pages mapped.
struct mm *
proc_mm_create(struct proc *p)
{
  struct mm *mm = mm_create();
  if (mm == 0)
    return 0;
  pagetable_t pagetable = mm->pagetable;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    proc_mm_free(mm);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  extern char sigtramp[];
  if (mappages(pagetable, SIGTRAMP, PGSIZE, (uint64)sigtramp, PTE_R | PTE_X | PTE_U) < 0) {
    proc_mm_free(mm);
    return 0;
  }

  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    proc_mm_free(mm);
    return 0;
  }

  return mm;
}

// Release an address space: its areas and pages, the two kernel pages'
// mappings, and the tables.
void
proc_mm_free(struct mm *mm)
{
  pagetable_t pagetable = mm->pagetable;
  if (!mm_free(mm))
    return; // another thread still runs in this address space
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, SIGTRAMP, 1, 0);
  freewalk(pagetable);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  if (p == 0)
    panic("userinit");
  initproc = p;

  // the current directory is set in forkret, not here: userinit runs
  // before any file system is mounted, and what "/" means is not known
  // until root_mount has decided which one the root disk holds.
  p->pgid = p->sid = p->pid; // init leads its own session

  // init is root, with every capability: everything else is descended
  // from it and gets what it is given.
  memset(&p->cred, 0, sizeof(p->cred));
  p->cred.cap_effective = CAP_FULL;
  p->cred.cap_permitted = CAP_FULL;
  p->cred.cap_bounding = CAP_FULL;

  p->state = RUNNABLE;
  sched_enqueue(p, SCHED_NEW);

  release(&p->lock);
}

// Grow or shrink the heap by n bytes (eager: the pages are populated
// now; otherwise on first touch). Return 0 on success, -1 on failure.
int
growproc(int n, int eager)
{
  struct proc *p = myproc();
  uint64 brk = p->mm->brk;
  if (n > 0 && brk + n < brk)
    return -1;
  if (n < 0 && (uint64)(-n) > brk - p->mm->heap_start)
    n = -(int)(brk - p->mm->heap_start);
  return mm_brk(p->mm, brk + n, eager);
}

// Create a new process or thread. The flags say what is shared with the
// caller (thread.h): CLONE_VM the address space, CLONE_FILES the
// descriptors, CLONE_FS the working directory, CLONE_SIGHAND the signal
// handlers, CLONE_THREAD the thread group. Returns the new tid.
int
kclone(uint64 flags, uint64 stack, uint64 ptid, uint64 tls, uint64 ctid)
{
  struct proc *np;
  struct proc *p = myproc();
  int thread = (flags & CLONE_THREAD) != 0;

  if (thread && !(flags & CLONE_VM))
    return -EINVAL; // a thread without the address space makes no sense
  // a namespace of one's own is the machine's to give -- unless it comes
  // with a user namespace, which is a world of its own and which anybody
  // may make: root inside it is nobody outside it.
  if ((flags & CLONE_NEW_ANY) && (flags & CLONE_NEWUSER) == 0 &&
      !cap_ok(CAP_SYS_ADMIN))
    return -EPERM;
  if (flags & CLONE_NEW_UNSUPPORTED)
    return -EINVAL;

  if ((np = allocproc()) == 0)
    return -EAGAIN;

  // the address space: shared or copied
  if (flags & CLONE_VM) {
    proc_mm_free(np->mm); // the empty one allocproc made
    np->mm = mm_dup(p->mm);
    // the new thread needs its own trapframe page mapped; it shares the
    // address space, so the trapframe lives at a different address.
    if (mm_add_trapframe(np->mm, np) < 0) {
      freeproc(np);
      release(&np->lock);
      proc_put(np);
      return -ENOMEM;
    }
  } else if (mm_fork(np->mm, p->mm) < 0) {
    freeproc(np);
    release(&np->lock);
    proc_put(np);
    return -ENOMEM;
  }

  // the descriptors and the working directory
  fdtable_put(np->fdt);
  fsinfo_put(np->fs);
  np->fdt = (flags & CLONE_FILES) ? fdtable_dup(p->fdt) : fdtable_copy(p->fdt);
  np->fs = (flags & CLONE_FS) ? fsinfo_dup(p->fs) : fsinfo_copy(p->fs);
  if (np->fdt == 0 || np->fs == 0) {
    freeproc(np);
    release(&np->lock);
    proc_put(np);
    return -ENOMEM;
  }

  // the signal handlers
  sighand_put(np->sighand);
  np->sighand = (flags & CLONE_SIGHAND) ? sighand_share(p->sighand) : sighand_copy(p->sighand);
  if (np->sighand == 0) {
    freeproc(np);
    release(&np->lock);
    proc_put(np);
    return -ENOMEM;
  }
  np->sigblocked = p->sigblocked;

  // the thread group
  if (thread) {
    tgroup_put(np->tg); // the fresh one allocproc made
    np->tg = tgroup_join(p->tg);
  }

  // the registers: the child returns 0 from the call, on its own stack
  // and with its own thread pointer if it asked for them.
  *(np->trapframe) = *(p->trapframe);
  if (p->vstate)
    memmove(np->vstate, p->vstate, PGSIZE);
  np->trapframe->a0 = 0;
  if (stack)
    np->trapframe->sp = stack;
  if (flags & CLONE_SETTLS)
    np->trapframe->tp = tls;

  np->pgid = p->pgid;
  np->sid = p->sid;
  np->tty = p->tty;
  np->umask = p->umask;
  np->cred = p->cred; // a child is whoever its parent was
  np->netns = p->netns; // and is on the network its parent was on
  // the namespaces: its parent's, or copies of the ones clone asked
  // for. A namespace it cannot have is a clone that does not happen.
  // a child that unshare(CLONE_NEWPID) has been called for goes into a
  // namespace of its own even though clone did not ask: that is what
  // unshare means for this one namespace, since the caller's own number
  // cannot change under everybody who knows it.
  uint64 nsflags = flags;
  if (p->newpidns)
    nsflags |= CLONE_NEWPID;
  np->ns = ns_copy(p->ns, (int)nsflags);
  if (np->ns == 0) {
    freeproc(np);
    release(&np->lock);
    proc_put(np);
    return -ENOSPC;
  }
  if (nsflags & CLONE_NEWUSER) {
    struct cred *cc = &np->cred;
    cc->cap_effective = CAP_FULL;
    cc->cap_permitted = CAP_FULL;
    cc->cap_bounding = CAP_FULL;
    cc->cap_inheritable = 0;
  }
  if (nsflags & CLONE_NEWPID) {
    // it keeps the machine's number it was given and gains one in the
    // new namespace, where it is the first process and so its init
    struct pidns *newns = np->ns->pid;
    pidns_leave(np);
    if (pidns_enter(np, newns, np->pid) < 0) {
      freeproc(np);
      release(&np->lock);
      proc_put(np);
      return -ENOSPC;
    }
    newns->initpid = np->pid;
    p->newpidns = 0;
  } else if (pidns_of(np) != pidns_root()) {
    // and a child of a process that is already in a pid namespace needs
    // a number in it too. Without one it was invisible there: its parent
    // was told the machine's number, which is not a number it can wait
    // for, and nothing in the container's /proc could name it.
    pidns_leave(np);
    if (pidns_enter(np, pidns_of(np), np->pid) < 0) {
      freeproc(np);
      release(&np->lock);
      proc_put(np);
      return -EAGAIN;
    }
  }
  np->nice = p->nice; // and starts with the same claim on the processor
  np->cpus = p->cpus; // and on the same harts
  np->sg = 0;
  sched_group_join(np, p->sg); // and in the same group, if it is in one
  // and in its parent's control group, which is where its pages and its
  // existence are charged. A group that is full is a fork that fails,
  // which is what pids.max means.
  np->cg = 0;
  np->cg_pages = 0;
  np->filter = p->filter;   // what its parent gave up, it has given up
  if (p->cg && cg_join(np, p->cg) < 0) {
    freeproc(np);
    release(&np->lock);
    proc_put(np);
    return -EAGAIN;
  }
  // a child is not watched because its parent is: ptrace(TRACEME) is
  // something a process does for itself
  np->traced = 0;
  np->trace_wants_stop = 0;
  np->trace_at_exit = 0;
  safestrcpy(np->name, p->name, sizeof(p->name));

  // what the caller is told is the child's number in the caller's own
  // namespace. It was the machine's number, which inside a pid namespace
  // is a number the caller cannot wait for: "vmm run" waited for its
  // container, was told at once that there was no such child, and
  // returned while the container was still running.
  int tid = pid_in_ns(np, pidns_of(p));
  if (tid == 0)
    tid = np->pid;   // not visible to the caller: say what the machine calls it
  if (flags & CLONE_CHILD_CLEARTID)
    np->clear_tid = ctid;
  release(&np->lock);

  if ((flags & CLONE_PARENT_SETTID) && ptid && copyout(p->mm, ptid, (char *)&tid, sizeof(tid)) < 0)
    tid = tid; // the caller asked for it but the address is bad: ignore
  if ((flags & CLONE_CHILD_SETTID) && ctid)
    copyout(np->mm, ctid, (char *)&tid, sizeof(tid));

  acquire(&wait_lock);
  np->parent = thread ? 0 : p; // a thread is nobody's child: wait() skips it
  if (!thread) {
    np->sibling = p->children;
    np->sibling_prev = 0;
    if (p->children)
      p->children->sibling_prev = np;
    p->children = np;
  }
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  sched_enqueue(np, SCHED_NEW);
  release(&np->lock);

  return tid;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  return kclone(0, 0, 0, 0, 0);
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp = p->children;
  if (pp == 0)
    return;
  while (pp) {
    pp->parent = initproc;
    if (pp->sibling == 0)
      break;
    pp = pp->sibling;
  }
  // splice the whole children list onto init's ...
  pp->sibling = initproc->children;
  if (initproc->children)
    initproc->children->sibling_prev = pp;
  initproc->children = p->children;
  p->children->sibling_prev = 0;
  p->children = 0;
  // ... and the zombies among them onto init's zombie list.
  if (p->zombies) {
    struct proc *z = p->zombies;
    while (z->zombie_next)
      z = z->zombie_next;
    z->zombie_next = initproc->zombies;
    initproc->zombies = p->zombies;
    p->zombies = 0;
  }
  wakeup(initproc);
}

// remove child from parent's lists (wait_lock held). O(1).
static void
unlink_child(struct proc *parent, struct proc *child)
{
  if (child->sibling_prev)
    child->sibling_prev->sibling = child->sibling;
  else
    parent->children = child->sibling;
  if (child->sibling)
    child->sibling->sibling_prev = child->sibling_prev;
  child->sibling = child->sibling_prev = 0;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // whatever was watching it has nothing left to watch
  ptrace_exit(p);

  // the first process of a pid namespace is that namespace's init, and a
  // container is its init: when it ends, everything it was running ends
  // with it rather than being reparented to the machine's init and left
  // running under a name nobody can use.
  struct pidns *myns = pidns_of(p);
  if (myns != pidns_root() && myns->initpid == p->pid)
    pidns_init_gone(myns);

  // the last thread of a process reports its status to the parent; the
  // others simply vanish. a leader leaving takes the group with it.
  int thread = (p->tg->tgid != p->pid);
  if (!thread)
    tgroup_kill_others(p);
  tgroup_leave(p->tg);

  // CLONE_CHILD_CLEARTID: clear the word and wake pthread_join()
  if (p->clear_tid && p->mm) {
    uint32 zero = 0;
    copyout(p->mm, p->clear_tid, (char *)&zero, sizeof(zero));
    futex_wake(p->clear_tid, 1);
    p->clear_tid = 0;
  }

  // a thread's trapframe page lives in the shared address space
  if (p->mm && p->tf_va && p->tf_va != TRAPFRAME)
    mm_del_trapframe(p->mm, p);

  // let go of the descriptors and the working directory
  fdtable_put(p->fdt);
  p->fdt = 0;
  fsinfo_put(p->fs);
  p->fs = 0;

  // a zombie keeps only its object, its pid and its exit status: the
  // address space, trapframe, vector state and file table go now, and
  // the kernel stack goes as soon as it has stepped off it (see the
  // scheduler). A parent slow to wait() -- init, after a fork storm --
  // holds no memory hostage.
  if (p->mm)
    proc_mm_free(p->mm);
  p->mm = 0;
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->vstate)
    kfree(p->vstate);
  p->vstate = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // a thread that is not its group's leader is nobody's child to wait
  // for: it leaves the parent's list now and the scheduler frees it.
  if (thread || p->parent == 0) {
    if (p->parent)
      unlink_child(p->parent, p);
    p->parent = 0;
    acquire(&p->lock);
    p->autoreap = 1;
    p->xstate = 0;
    p->state = ZOMBIE;
    release(&wait_lock);
    sched();
    panic("zombie thread");
  }

  // Parent might be sleeping in wait(), and wants SIGCHLD.
  wakeup(p->parent);
  if (p->parent) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = SIGCHLD;
    info.si_code = p->exitsig ? CLD_KILLED : CLD_EXITED;
    info.si_pid = p->pid;
    info.si_status = p->exitsig ? p->exitsig : (p->xstate >> 8);
    signal_send(p->parent, SIGCHLD, &info);
  }

  acquire(&p->lock);

  // the POSIX wait status: the exit code in the high byte, or the
  // signal that killed it in the low seven bits.
  p->xstate = p->exitsig ? (p->exitsig & 0x7f) : ((status & 0xff) << 8);
  p->notify = p->exitsig ? CLD_KILLED : CLD_EXITED;
  p->state = ZOMBIE;
  // on the parent's zombie list: wait() reaps in O(1)
  p->zombie_next = p->parent->zombies;
  p->parent->zombies = p;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child to exit (or, with WUNTRACED/WCONTINUED, to stop or
// continue). pid > 0 waits for that child, 0 for the caller's group,
// -1 for any, < -1 for the group -pid. Returns the pid, 0 for WNOHANG
// with nothing to report, or -errno.
int
kwait4(int wpid, uint64 addr, int options, uint64 rusage)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    havekids = 0;
    // a child that stopped or continued, when the caller asked
    for (pp = p->children; pp; pp = pp->sibling) {
      if (!wait_match(p, pp, wpid))
        continue;
      havekids = 1;
      acquire(&pp->lock);
      int n = pp->notify;
      if ((n == CLD_STOPPED && (options & WUNTRACED)) ||
          (n == CLD_CONTINUED && (options & WCONTINUED))) {
        pp->notify = 0;
        int st = (n == CLD_STOPPED) ? ((pp->stopped_sig << 8) | 0x7f) : 0xffff;
        pid = pid_in_ns(pp, pidns_of(p));
        release(&pp->lock);
        release(&wait_lock);
        if (addr != 0 && copyout(p->mm, addr, (char *)&st, sizeof(st)) < 0)
          return -EFAULT;
        return pid;
      }
      release(&pp->lock);
    }
    pp = p->zombies;
    while (pp && !wait_match(p, pp, wpid))
      pp = pp->zombie_next;
    if (pp) {
      havekids = 1;
      // make sure the child isn't still in exit() or swtch().
      acquire(&pp->lock);
      pid = pid_in_ns(pp, pidns_of(p));
      if (addr != 0 &&
          copyout(p->mm, addr, (char *)&pp->xstate, sizeof(pp->xstate)) < 0) {
        release(&pp->lock);
        release(&wait_lock);
        return -EFAULT;
      }
      struct proc **zp = &p->zombies;
      while (*zp && *zp != pp)
        zp = &(*zp)->zombie_next;
      if (*zp)
        *zp = pp->zombie_next;
      pp->zombie_next = 0;
      unlink_child(p, pp);
      pp->parent = 0;
      freeproc(pp);
      release(&pp->lock);
      release(&wait_lock);
      proc_put(pp);
      return pid;
    }

    // No point waiting if we don't have any children.
    if (!havekids)
      break;
    if (options & WNOHANG) {
      release(&wait_lock);
      return 0;
    }
    if (killed(p) || signal_pending(p)) {
      release(&wait_lock);
      return killed(p) ? -ECHILD : -EINTR;
    }

    // Wait for a child to exit.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep_intr();
    acquire(&wait_lock);
  }
  release(&wait_lock);
  return -ECHILD;
}

// does this child match the pid argument of wait4? (wait_lock held)
static int
wait_match(struct proc *parent, struct proc *child, int wpid)
{
  if (wpid == -1)
    return 1;
  if (wpid > 0)
    return pid_in_ns(child, pidns_of(parent)) == wpid;
  if (wpid == 0)
    return child->pgid == parent->pgid;
  return child->pgid == -wpid;
}

int
kwait(uint64 addr)
{
  return kwait4(-1, addr, 0, 0);
}

// the boundary a frozen task stops at: on the way back to user code, and
// between one entry to a guest and the next. Nothing is held here, which
// is the whole point -- the task can be left standing for as long as
// somebody wants without anything else waiting on it. A kill gets
// through, so a container that is frozen can still be stopped.
void
freeze_point(void)
{
  struct proc *p = myproc();

  if (p == 0 || p->sg == 0)
    return;
  while (sched_group_frozen(p->sg) && !killed(p)) {
    // prepared, then checked again: an unfreeze between the two would
    // otherwise be a wakeup nobody was waiting for yet
    sleep_prepare(p->sg);
    if (sched_group_frozen(p->sg) && !killed(p))
      sleep_intr();
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    // the runnable task of this hart with the least virtual time,
    // or one stolen from a busier hart (see sched.c)
    p = sched_pick();
    if (p == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
      continue;
    }
    acquire(&p->lock);
    uint64 zombie_stack = 0;
    if (p->state == RUNNABLE) {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      p->state = RUNNING;
      c->proc = p;
      c->resched = 0;
      sched_begin(p);
      swtch(&c->context, &p->context);
      sched_end(p);

      // Don't re-enable interrupts on release.
      mycpu()->intena = 0;

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      if (p->state == ZOMBIE && p->autoreap) {
        // a thread nobody will wait for: free it here, off its stack.
        freeproc(p);
        release(&p->lock);
        proc_put(p);
        continue;
      }
      if (p->state == ZOMBIE && p->kstack) {
        // and a zombie that somebody will wait for has no use for a
        // stack either: it has just left the CPU for the last time,
        // and this is the first moment it is not standing on it.
        //
        // it matters more than it sounds. Eighty thousand orphans of
        // a fork storm are init's to reap, one at a time, and until
        // it gets to each one the thirty-two kilobytes underneath it
        // are the host's memory held hostage -- two gigabytes of an
        // eight gigabyte machine, which is what usertests was
        // measuring when it said pages had been lost.
        //
        // the address is taken here and the stack is freed below,
        // after the lock: giving a stack back ends in an IPI round
        // that waits for every hart to answer, and a hart holding a
        // lock is a hart with interrupts off, which cannot. Clearing
        // p->kstack under the lock is what keeps the parent's wait()
        // from freeing it as well.
        zombie_stack = p->kstack;
        p->kstack = 0;
      }
    } else {
      // picked, and by the time its lock was taken it was not
      // runnable any more. sched_pick took it off the tree; this puts
      // the hart's books straight again (sched.c).
      sched_unpick(p);
    }
    release(&p->lock);
    if (zombie_stack)
      kstack_free(zombie_stack);
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  // giving up the processor is not sleeping: it keeps the virtual
  // time it has, so a task cannot gain by yielding in a loop. It goes
  // back in the tree in sched_end, once it is really off the
  // processor -- see kernel/sched.c.
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    // the disks come first now: the root may be ext4, and finding out
    // means reading a block through the block layer.
    virtio_disk_register(); // the disks and their partitions, which needs to sleep
    root_mount();
    p->fs->cwd = namei("/");
    if (p->fs->cwd == 0)
      panic("forkret: no root directory");

    // ensure other cores see first=0.
    __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    // /sbin/init is where it lives; /init is where it used to, and a
    // disk made before the move still boots.
    p->trapframe->a0 = kexec("/sbin/init", (char *[]){"/sbin/init", 0});
    if (p->trapframe->a0 < 0)
      p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 < 0)
      panic("exec: no init");
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  extern char userret[];
  uint64 satp = MAKE_SATP(p->mm->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// ---- helpers for signals and job control ----

// p->lock held: make a sleeping or stopped process runnable.
void
proc_make_runnable(struct proc *p)
{
  if (p->state == SLEEPING || p->state == STOPPED) {
    int stopped = p->state == STOPPED;
    p->state = RUNNABLE;
    p->stopped_sig = 0;
    // a process that has marked itself STOPPED but has not yet
    // switched away is still running on its own hart. putting it on
    // the run queue as well would let a second hart into the same
    // process, with one trapframe and one kernel stack between them.
    // it will see that it is RUNNABLE when it takes its lock again
    // and simply carry on, so there is nothing to queue.
    if (stopped && !p->stop_parked)
      return;
    sched_enqueue(p, SCHED_WAKE);
  }
}

// tell the parent that p stopped or continued: SIGCHLD and a wakeup.
void
proc_notify_parent(struct proc *p)
{
  acquire(&wait_lock);
  struct proc *parent = p->parent;
  if (parent) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = SIGCHLD;
    info.si_code = p->notify;
    info.si_pid = p->pid;
    info.si_status = p->stopped_sig;
    wakeup(parent);
    release(&wait_lock);
    signal_send(parent, SIGCHLD, &info);
    return;
  }
  release(&wait_lock);
}

// switch away from a process that has just marked itself STOPPED
// (p->lock held, as sched() requires).
void
sched_stopped(void)
{
  sched();
}

// free every queued signal of p (p->lock held)
void
sigq_free_all(struct proc *p)
{
  while (p->sigqueue) {
    struct sigq *q = p->sigqueue;
    p->sigqueue = q->next;
    sigq_free(q);
  }
  p->nsigq = 0;
  p->sigpending = 0;
}

// the parent's pid, or 0.
int
proc_ppid(struct proc *p)
{
  acquire(&wait_lock);
  int pid = p->parent ? p->parent->pid : 0;
  release(&wait_lock);
  return pid;
}

// look a process up by pid and keep it alive until proc_release().
struct proc *
proc_get(int pid)
{
  acquire(&proc_lock);
  struct proc *p = pid_lookup(pid);
  if (p == 0) {
    release(&proc_lock);
    return 0;
  }
  return p; // proc_lock stays held
}

void
proc_release(struct proc *p)
{
  release(&proc_lock);
}

// walk every process: proc_iter_start(), then proc_iter_next(0) and
// proc_iter_next(prev) until 0, then proc_iter_end().
void
proc_iter_start(void)
{
  acquire(&proc_lock);
}

struct proc *
proc_iter_next(struct proc *prev)
{
  return prev ? prev->all_next : all_procs;
}

void
proc_iter_end(void)
{
  release(&proc_lock);
}

// ---- kernel threads ----

// the first scheduling of a kernel thread lands here (see kthread_create).
static void
kthread_entry(void)
{
  struct proc *p = myproc();
  release(&p->lock); // held since the scheduler switched to us
  p->kfn(p->karg);
  kexit(0);
}

// a process without an address space that runs fn(arg) in the kernel,
// scheduled like any other. it ends when fn returns (its exit status is
// reaped by init, to which it belongs).
int
kthread_create(void (*fn)(void *), void *arg, const char *name)
{
  struct proc *p = allocproc();
  if (p == 0)
    return -1;
  // no user memory, no files, no cwd
  if (p->mm) {
    proc_mm_free(p->mm);
    p->mm = 0;
  }
  p->kfn = fn;
  p->karg = arg;
  safestrcpy(p->name, (char *)name, sizeof(p->name));
  p->context.ra = (uint64)kthread_entry;
  int pid = p->pid;
  release(&p->lock);

  acquire(&wait_lock);
  p->parent = initproc;
  p->sibling = initproc->children;
  p->sibling_prev = 0;
  if (initproc->children)
    initproc->children->sibling_prev = p;
  initproc->children = p;
  release(&wait_lock);

  acquire(&p->lock);
  p->state = RUNNABLE;
  sched_enqueue(p, SCHED_NEW);
  release(&p->lock);
  return pid;
}

// exec() from a thread that is not the group's leader: the caller
// takes over the leader's identity, the way Linux's de_thread does, so
// that the process keeps the pid and the parent it had before. the
// leader is already on its way out and leaves as an ordinary thread.
//
// wait_lock covers the whole thing: the leader cannot become a zombie
// (or be freed) while we hold it, so it is either still running, and
// we may take its place, or already gone, and we adopt init instead.
void
tgroup_take_over(struct proc *p)
{
  if (p->tg->tgid == p->pid)
    return;

  acquire(&wait_lock);
  acquire(&proc_lock);
  struct proc *l = pid_lookup(p->tg->tgid);
  if (l && (l->tg != p->tg || l == p))
    l = 0;
  if (l) {
    acquire(&l->lock);
    int gone = (l->state == ZOMBIE || l->state == UNUSED);
    release(&l->lock);
    if (gone)
      l = 0;
  }

  if (l == 0) {
    // the leader has already been reaped, or died: the exec'd image
    // becomes a process of its own, and init will wait for it.
    release(&proc_lock);
    p->tg->tgid = p->pid;
    if (p->parent == 0 && p != initproc) {
      p->parent = initproc;
      p->sibling = initproc->children;
      p->sibling_prev = 0;
      if (initproc->children)
        initproc->children->sibling_prev = p;
      initproc->children = p;
    }
    release(&wait_lock);
    return;
  }

  // swap the two pids, and their places in the hash, so that the
  // parent still waits for the pid it was given by fork().
  struct proc **pp = &pid_hash[l->pid % PIDHASH];
  while (*pp && *pp != l)
    pp = &(*pp)->pid_next;
  if (*pp)
    *pp = l->pid_next;
  pp = &pid_hash[p->pid % PIDHASH];
  while (*pp && *pp != p)
    pp = &(*pp)->pid_next;
  if (*pp)
    *pp = p->pid_next;

  int lpid = l->pid;
  l->pid = p->pid;
  p->pid = lpid;

  l->pid_next = pid_hash[l->pid % PIDHASH];
  pid_hash[l->pid % PIDHASH] = l;
  p->pid_next = pid_hash[p->pid % PIDHASH];
  pid_hash[p->pid % PIDHASH] = p;
  release(&proc_lock);

  // the leader's place among its parent's children is ours, and so
  // are its children. with no parent left, it exits as a thread does.
  struct proc *parent = l->parent;
  if (parent) {
    unlink_child(parent, l);
    l->parent = 0;
    p->parent = parent;
    p->sibling = parent->children;
    p->sibling_prev = 0;
    if (parent->children)
      parent->children->sibling_prev = p;
    parent->children = p;
  }
  while (l->children) {
    struct proc *c = l->children;
    l->children = c->sibling;
    if (c->sibling)
      c->sibling->sibling_prev = 0;
    c->parent = p;
    c->sibling = p->children;
    c->sibling_prev = 0;
    if (p->children)
      p->children->sibling_prev = c;
    p->children = c;
  }
  p->pgid = l->pgid;
  p->sid = l->sid;
  p->tty = l->tty;
  release(&wait_lock);
}

// a boot-time check of kernel threads: two threads hand a counter back
// and forth, then report.
static volatile int kt_turn, kt_count;

static void
kt_worker(void *arg)
{
  int me = (int)(uint64)arg;
  for (int i = 0; i < 50; i++) {
    while (kt_turn != me)
      yield();
    kt_count++;
    kt_turn = 1 - me;
  }
  if (me == 0) {
    while (kt_turn != 0)
      yield();
    printk("kthread: ok (%d switches)\n", kt_count);
  }
}

void
kthread_selftest(void)
{
  kt_turn = 0;
  kt_count = 0;
  if (kthread_create(kt_worker, (void *)0, "kt0") < 0 || kthread_create(kt_worker, (void *)1, "kt1") < 0)
    panic("kthread_selftest");
}

// ---- sleep and wakeup ----

// take p off its wait queue, if it is on one (p->lock not held).
static void
waitq_cancel(struct proc *p)
{
  struct waitq *wq = p->wq;
  if (wq == 0)
    return;
  acquire(&wq->lock);
  acquire(&p->lock);
  if (p->wq == wq) {
    if (p->wq_prev)
      p->wq_prev->wq_next = p->wq_next;
    else
      wq->head = p->wq_next;
    if (p->wq_next)
      p->wq_next->wq_prev = p->wq_prev;
    p->wq = 0;
    p->wq_next = p->wq_prev = 0;
    p->chan = 0;
  }
  release(&p->lock);
  release(&wq->lock);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  if (chan == 0)
    panic("sleep_prepare: zero chan");
  if (p->wq)
    waitq_cancel(p); // a previous prepare without a sleep
  struct waitq *wq = waitq_of(chan);
  acquire(&wq->lock);
  acquire(&p->lock);
  p->chan = chan;
  p->wq = wq;
  p->wq_prev = 0;
  p->wq_next = wq->head;
  if (wq->head)
    wq->head->wq_prev = p;
  wq->head = p;
  release(&p->lock);
  release(&wq->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
  // woken by wakeup(): already off the queue. woken by kill(): not.
  if (p->wq)
    waitq_cancel(p);
}

// Like sleep(), but for a wait a signal may interrupt: the check for a
// deliverable signal happens under p->lock, which the sender also holds,
// so a signal that arrives between the caller's test and this call
// cannot be missed.
void
sleep_intr(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0 && !p->killed && (p->sigpending & ~p->sigblocked) == 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
  if (p->wq)
    waitq_cancel(p);
}

// take p off its wait queue and make it runnable again, wherever it is
// sleeping (a futex deadline, for instance).
void
wake_proc(struct proc *p)
{
  struct waitq *wq = p->wq;
  if (wq == 0)
    return;
  acquire(&wq->lock);
  acquire(&p->lock);
  if (p->wq == wq) {
    if (p->wq_prev)
      p->wq_prev->wq_next = p->wq_next;
    else
      wq->head = p->wq_next;
    if (p->wq_next)
      p->wq_next->wq_prev = p->wq_prev;
    p->wq = 0;
    p->wq_next = p->wq_prev = 0;
    p->chan = 0;
    if (p->state == SLEEPING) {
      p->state = RUNNABLE;
      sched_enqueue(p, SCHED_WAKE);
    }
  }
  release(&p->lock);
  release(&wq->lock);
}

// give up a sleep_prepare() without sleeping.
void
sleep_cancel(void)
{
  struct proc *p = myproc();
  if (p->wq)
    waitq_cancel(p);
}

// wake up to n processes sleeping on chan, and report how many. n < 0
// means all of them.
int
wakeup_n(void *chan, int n)
{
  struct waitq *wq = waitq_of(chan);
  int woken = 0;
  acquire(&wq->lock);
  struct proc *p = wq->head;
  while (p && (n < 0 || woken < n)) {
    struct proc *next = p->wq_next;
    if (p->chan == chan) {
      acquire(&p->lock);
      if (p->wq_prev)
        p->wq_prev->wq_next = p->wq_next;
      else
        wq->head = p->wq_next;
      if (p->wq_next)
        p->wq_next->wq_prev = p->wq_prev;
      p->wq = 0;
      p->wq_next = p->wq_prev = 0;
      p->chan = 0;
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
        sched_enqueue(p, SCHED_WAKE);
      }
      release(&p->lock);
      woken++;
    }
    p = next;
  }
  release(&wq->lock);
  return woken;
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct waitq *wq = waitq_of(chan);
  acquire(&wq->lock);
  struct proc *p = wq->head;
  while (p) {
    struct proc *next = p->wq_next;
    if (p->chan == chan) {
      acquire(&p->lock);
      // unlink
      if (p->wq_prev)
        p->wq_prev->wq_next = p->wq_next;
      else
        wq->head = p->wq_next;
      if (p->wq_next)
        p->wq_next->wq_prev = p->wq_prev;
      p->wq = 0;
      p->wq_next = p->wq_prev = 0;
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;
      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNABLE.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
        sched_enqueue(p, SCHED_WAKE);
      }
      release(&p->lock);
    }
    p = next;
  }
  release(&wq->lock);
}

// ---- kill ----

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
// the old kill(pid): terminate that process.
int
kkill(int pid)
{
  return signal_kill(pid, SIGKILL, 0);
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->mm, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Fill either a user address or a kernel address with one byte,
// depending on user_dst. this is what a read of a hole does: the file
// has nothing there and the caller's buffer gets zeros.
// Returns 0 on success, -1 on error.
int
either_memset(int user_dst, uint64 dst, int c, uint64 len)
{
  struct proc *p = myproc();

  if (!user_dst) {
    memset((char *)dst, c, len);
    return 0;
  }
  // a page at a time, through a buffer of zeros on the stack
  char zeros[128];
  memset(zeros, c, sizeof(zeros));
  while (len > 0) {
    uint64 k = len < sizeof(zeros) ? len : sizeof(zeros);
    if (copyout(p->mm, dst, zeros, k) < 0)
      return -1;
    dst += k;
    len -= k;
  }
  return 0;
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->mm, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = all_procs; p; p = p->all_next) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %s %s pgid %d chan %p pend %lx blk %lx", p->pid, state, p->name, p->pgid, p->chan,
           p->sigpending, p->sigblocked);
    printk("\n");
  }
}

// how many processes exist (for sysconf and the tests).
uint64
proc_count(void)
{
  acquire(&proc_lock);
  uint64 n = nprocs;
  release(&proc_lock);
  return n;
}
