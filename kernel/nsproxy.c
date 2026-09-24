//
// namespaces.
//
// A namespace is an answer to a question about the machine that can be
// different for different processes: what the machine is called, what is
// mounted, who root is, what a process's number is, what the network
// looks like. A process holds one object, the nsproxy, with a pointer to
// each kind; forking copies the pointer, and unsharing copies only the
// kinds named, so two processes that differ in their hostname still
// share everything else.
//
// The root namespaces are the machine's own, are never freed, and are
// what a process gets when it has no nsproxy -- which is what every
// process had before this existed, and is why nothing had to change to
// make room for it.
//
// The pools are fixed, like everything else here: a machine with eight
// different hostnames is doing something unusual, and a fixed pool is a
// bound on what a container can do to the host by asking for more.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "thread.h"
#include "net.h"
#include "nsproxy.h"
#include "errno.h"

// the largest number a container hands out. A container with tens of
// thousands of processes is a machine, and this is what keeps the walk
// that looks for a free one short.
#define PID_IN_NS_MAX 32768

static struct spinlock nslock;
static struct utsns utstab[NUTSNS];
static struct nsproxy nstab[NNSPROXY];

static struct utsns root_uts;
static struct nsproxy root_ns;
static struct pidns pidnstab[NPIDNS];
static struct pidns root_pidns;
static struct mntns mntnstab[NMNTNS];
static struct mntns root_mntns;
static struct userns usernstab[NUSERNS];
static struct userns root_userns;

void
nsinit(void)
{
  initlock(&nslock, "ns");
  safestrcpy(root_uts.nodename, "xv6h", UTS_LEN);
  safestrcpy(root_uts.domainname, "(none)", UTS_LEN);
  root_uts.ref = 1;         // the machine's own: never freed
  root_uts.id = 0;
  root_pidns.ref = 1;       // the machine's own: never freed
  root_pidns.id = 0;
  root_pidns.level = 0;
  root_pidns.parent = 0;
  root_ns.ref = 1;
  root_ns.uts = &root_uts;
  root_ns.net = 0;          // the root network namespace (kernel/net.c)
  root_ns.pid = &root_pidns;
  root_mntns.ref = 1;       // the machine's own: never freed
  root_mntns.id = 0;
  root_mntns.visible = 0;   // filled in as the machine mounts things
  root_mntns.root = 0;      // and its root is the machine's
  root_ns.mnt = &root_mntns;
  root_userns.ref = 1;      // the machine's own: never freed
  root_userns.id = 0;
  root_userns.level = 0;
  root_ns.user = &root_userns;
}

struct nsproxy *
ns_root(void)
{
  return &root_ns;
}

struct nsproxy *
ns_current(void)
{
  struct proc *p = myproc();

  return (p && p->ns) ? p->ns : &root_ns;
}

struct nsproxy *
ns_hold(struct nsproxy *ns)
{
  if (ns == 0 || ns == &root_ns)
    return ns;
  acquire(&nslock);
  ns->ref++;
  release(&nslock);
  return ns;
}

static void pidns_put_locked(struct pidns *ns);
static void mntns_put_locked(struct mntns *ns);
static void userns_put_locked(struct userns *ns);
static struct userns *userns_new_locked(struct userns *parent);
static struct mntns *mntns_clone_locked(struct mntns *from);
static struct pidns *pidns_new_locked(struct pidns *parent);

// caller holds nslock
static void
uts_put_locked(struct utsns *u)
{
  if (u == 0 || u == &root_uts)
    return;
  if (--u->ref <= 0)
    u->ref = 0;             // the slot is free again
}

void
ns_put(struct nsproxy *ns)
{
  if (ns == 0 || ns == &root_ns)
    return;
  acquire(&nslock);
  if (--ns->ref <= 0) {
    uts_put_locked(ns->uts);
    pidns_put_locked(ns->pid);
    mntns_put_locked(ns->mnt);
    userns_put_locked(ns->user);
    struct netns *net = ns->net;
    ns->user = 0;
    ns->uts = 0;
    ns->net = 0;
    ns->pid = 0;
    ns->mnt = 0;
    ns->ref = 0;            // the slot is free again
    release(&nslock);
    // outside the lock: taking a network apart frees packets and touches
    // the gateways, neither of which is nslock's business
    netns_put(net);
    return;
  }
  release(&nslock);
}

// caller holds nslock
static struct utsns *
uts_clone_locked(struct utsns *from)
{
  for (int i = 0; i < NUTSNS; i++) {
    struct utsns *u = &utstab[i];
    if (u->ref)
      continue;
    memset(u, 0, sizeof(*u));
    u->ref = 1;
    u->id = i + 1;
    safestrcpy(u->nodename, from->nodename, UTS_LEN);
    safestrcpy(u->domainname, from->domainname, UTS_LEN);
    return u;
  }
  return 0;
}

struct nsproxy *
ns_copy(struct nsproxy *old, int flags)
{
  if (old == 0)
    old = &root_ns;
  if ((flags & (CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER |
                CLONE_NEWNET)) == 0)
    return ns_hold(old);    // nothing of its own: the same set

  acquire(&nslock);
  struct nsproxy *ns = 0;
  for (int i = 0; i < NNSPROXY; i++) {
    if (nstab[i].ref == 0) {
      ns = &nstab[i];
      break;
    }
  }
  if (ns == 0) {
    release(&nslock);
    return 0;
  }
  memset(ns, 0, sizeof(*ns));
  ns->ref = 1;
  ns->net = old->net;
  ns->uts = old->uts;
  ns->pid = old->pid ? old->pid : &root_pidns;
  ns->mnt = old->mnt ? old->mnt : &root_mntns;
  ns->user = old->user ? old->user : &root_userns;
  if (flags & CLONE_NEWUSER) {
    struct userns *un = userns_new_locked(ns->user);
    if (un == 0) {
      ns->ref = 0;
      release(&nslock);
      return 0;
    }
    ns->user = un;
  } else if (ns->user != &root_userns) {
    ns->user->ref++;
  }
  if (flags & CLONE_NEWNS) {
    struct mntns *mn = mntns_clone_locked(ns->mnt);
    if (mn == 0) {
      ns->ref = 0;
      release(&nslock);
      return 0;
    }
    ns->mnt = mn;
  } else if (ns->mnt != &root_mntns) {
    ns->mnt->ref++;
  }
  if (flags & CLONE_NEWPID) {
    struct pidns *np = pidns_new_locked(ns->pid);
    if (np == 0) {
      ns->ref = 0;
      release(&nslock);
      return 0;
    }
    ns->pid = np;
  } else if (ns->pid != &root_pidns) {
    ns->pid->ref++;
  }
  if (flags & CLONE_NEWUTS) {
    struct utsns *u = uts_clone_locked(old->uts ? old->uts : &root_uts);
    if (u == 0) {
      ns->ref = 0;
      release(&nslock);
      return 0;
    }
    ns->uts = u;
  } else if (ns->uts && ns->uts != &root_uts) {
    ns->uts->ref++;
  }
  release(&nslock);
  // the network last, and outside nslock: making one takes the interface
  // and route locks, and a namespace that cannot have a loopback is a
  // clone that does not happen.
  if (flags & CLONE_NEWNET) {
    struct netns *net = netns_new();
    if (net == 0) {
      ns_put(ns);
      return 0;
    }
    ns->net = net;
  } else {
    netns_hold(ns->net);
  }
  return ns;
}

// unshare(2), for the namespaces this revision has. The process keeps
// everything it did not ask about.
int
ns_unshare(int flags)
{
  struct proc *p = myproc();

  // CLONE_NEWUSER is the unprivileged one: what it creates is a world of
  // its own, and everything else asked for in the same breath belongs to
  // that world. Without it, a namespace is the machine's to give.
  if ((flags & CLONE_NEWUSER) == 0 && !cap_ok(CAP_SYS_ADMIN))
    return -EPERM;
  if (flags & CLONE_NEWPID) {
    // a process cannot leave its own pid namespace: its number there is
    // known to its parent, to whoever is waiting for it and to /proc.
    // What unshare does is put the next child in a new namespace, and
    // that child is that namespace's init.
    p->newpidns = 1;
  }
  if ((flags & (CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWNET)) == 0)
    return 0;
  // the pid namespace is not copied here: a process cannot leave its own
  // (its number there is known to whoever is waiting for it), and the
  // flag above has already said that its next child goes into a new one.
  struct nsproxy *ns = ns_copy(p->ns ? p->ns : &root_ns,
                               flags & ~CLONE_NEWPID);
  if (ns == 0)
    return -ENOSPC;
  struct nsproxy *old = p->ns;
  p->ns = ns;
  ns_put(old);
  if (flags & CLONE_NEWUSER) {
    // root of its own world, and nobody in the one outside: its numbers
    // mean nothing until somebody writes its map, and what its root may
    // do is what cap_ok allows a namespace (its own mounts, its own
    // name, its own processes -- not the machine's clock or its guests).
    struct cred *c = &p->cred;
    c->cap_effective = CAP_FULL;
    c->cap_permitted = CAP_FULL;
    c->cap_bounding = CAP_FULL;
    c->cap_inheritable = 0;
  }
  return 0;
}

// ---- pid namespaces ----
//
// A number is a fact about a namespace. The machine's own namespace
// numbers every process on it, and a process inside a container has a
// second number there -- usually a small one, starting at 1, because
// that is what makes a container look like a machine. Which number a
// process is told is decided by which namespace is asking.
//
// The numbering inside a namespace is its own business: a counter and a
// walk of the table to see whether the number is taken. A namespace
// holds a few dozen processes at most, so the walk costs less than a
// bitmap per namespace would.

// is this number taken in this namespace? caller holds nslock.
static int
pidns_nr_taken(struct pidns *ns, int nr)
{
  return proc_pid_taken_in_ns(ns, nr);
}

struct pidns *
pidns_root(void)
{
  return &root_pidns;
}

struct pidns *
pidns_of(struct proc *p)
{
  if (p == 0)
    return &root_pidns;
  struct nsproxy *ns = p->ns ? p->ns : &root_ns;
  return ns->pid ? ns->pid : &root_pidns;
}

struct pidns *
pidns_current(void)
{
  return pidns_of(myproc());
}

// what this process is called in that namespace, or 0 if it is not in
// it at all -- which is what makes a container's processes invisible
// from inside another one.
int
pid_in_ns(struct proc *p, struct pidns *ns)
{
  if (p == 0)
    return 0;
  if (ns == 0 || ns == &root_pidns)
    return p->pid;
  for (int i = 0; i < p->npids; i++)
    if (p->pids[i].ns == ns)
      return p->pids[i].nr;
  return 0;
}

int
pid_visible(struct proc *p, struct pidns *ns)
{
  return pid_in_ns(p, ns) != 0;
}

// a free number in this namespace. The machine's own numbers come from
// the allocator in proc.c, which has a bitmap; a container's come from
// here.
int
pidns_alloc_nr(struct pidns *ns)
{
  if (ns == 0 || ns == &root_pidns)
    return 0;                      // not ours to give
  acquire(&nslock);
  for (int tries = 0; tries < PID_IN_NS_MAX; tries++) {
    int nr = ns->next + 1;
    ns->next = nr >= PID_IN_NS_MAX ? 0 : nr;
    if (nr <= 0)
      continue;
    if (!pidns_nr_taken(ns, nr)) {
      release(&nslock);
      return nr;
    }
  }
  release(&nslock);
  return 0;
}

void
pidns_free_nr(struct pidns *ns, int nr)
{
  (void)ns;
  (void)nr;                        // the number is free when nobody has it
}

// the numbers a new process gets: one in its own namespace and one in
// each namespace above it, outermost first, so that pids[0] is always
// the machine's own.
int
pidns_enter(struct proc *p, struct pidns *ns, int rootpid)
{
  struct pidns *chain[PIDNS_DEPTH];
  int n = 0;

  if (ns == 0)
    ns = &root_pidns;
  if (ns->dying)
    return -1;
  for (struct pidns *w = ns; w; w = w->parent) {
    if (n >= PIDNS_DEPTH)
      return -1;
    chain[n++] = w;
  }
  // chain is innermost first; the array is outermost first
  p->npids = 0;
  for (int i = n - 1; i >= 0; i--) {
    struct pidns *w = chain[i];
    int nr = (w == &root_pidns) ? rootpid : pidns_alloc_nr(w);
    if (nr == 0) {
      pidns_leave(p);
      return -1;
    }
    p->pids[p->npids].ns = w;
    p->pids[p->npids].nr = nr;
    p->npids++;
  }
  acquire(&nslock);
  for (int i = 0; i < p->npids; i++)
    p->pids[i].ns->nprocs++;
  release(&nslock);
  return 0;
}

void
pidns_leave(struct proc *p)
{
  acquire(&nslock);
  for (int i = 0; i < p->npids; i++)
    if (p->pids[i].ns)
      p->pids[i].ns->nprocs--;
  release(&nslock);
  p->npids = 0;
  memset(p->pids, 0, sizeof(p->pids));
}

// caller holds nslock
static struct pidns *
pidns_new_locked(struct pidns *parent)
{
  if (parent == 0)
    parent = &root_pidns;
  if (parent->level + 1 >= PIDNS_DEPTH)
    return 0;
  for (int i = 0; i < NPIDNS; i++) {
    struct pidns *ns = &pidnstab[i];
    if (ns->ref)
      continue;
    memset(ns, 0, sizeof(*ns));
    ns->ref = 1;
    ns->id = i + 1;
    ns->level = parent->level + 1;
    ns->parent = parent;
    return ns;
  }
  return 0;
}

struct pidns *
pidns_new(struct pidns *parent)
{
  acquire(&nslock);
  struct pidns *ns = pidns_new_locked(parent);
  release(&nslock);
  return ns;
}

// caller holds nslock
static void
pidns_put_locked(struct pidns *ns)
{
  if (ns == 0 || ns == &root_pidns)
    return;
  if (--ns->ref <= 0)
    ns->ref = 0;
}

void
pidns_put(struct pidns *ns)
{
  acquire(&nslock);
  pidns_put_locked(ns);
  release(&nslock);
}

// ---- mount namespaces ----
//
// What a process sees mounted is a set of the machine's mounts and a
// root. A copy takes a snapshot -- what is mounted afterwards in one
// namespace does not appear in the other, which is the "private"
// propagation of §8.3 and the only one this has -- and pivot_root makes
// the set a single mount, which is how the host's mounts stop being
// visible inside a container.

struct mntns *
mntns_root(void)
{
  return &root_mntns;
}

struct mntns *
mntns_current(void)
{
  struct proc *p = myproc();
  struct nsproxy *ns = (p && p->ns) ? p->ns : &root_ns;

  return ns->mnt ? ns->mnt : &root_mntns;
}

int
mntns_sees(struct mntns *ns, int slot)
{
  if (ns == 0)
    ns = &root_mntns;
  if (slot < 0 || slot >= 32)
    return 1;
  return (ns->visible & (1u << slot)) != 0;
}

void
mntns_add(struct mntns *ns, int slot)
{
  if (ns == 0 || slot < 0 || slot >= 32)
    return;
  acquire(&nslock);
  ns->visible |= 1u << slot;
  release(&nslock);
}

void
mntns_drop(struct mntns *ns, int slot)
{
  if (ns == 0 || slot < 0 || slot >= 32)
    return;
  acquire(&nslock);
  ns->visible &= ~(1u << slot);
  release(&nslock);
}

// after a pivot the only thing mounted is the new root. What the
// container mounts next -- its /proc, its /dev, its /tmp -- is mounted
// inside it and appears; what the host has mounted does not, which is
// the point of the call.
void
mntns_pivot(struct mntns *ns, int slot)
{
  mntns_keep(ns, 1u << slot);
}

// the same, with the mounts that are inside the new root kept. What is
// pivoted away is everything *outside* it; a volume the runtime bound
// into the container's root before pivoting is part of that root, and
// dropping it left an empty directory where the data was.
void
mntns_keep(struct mntns *ns, unsigned int visible)
{
  if (ns == 0 || ns == &root_mntns)
    return;
  acquire(&nslock);
  ns->visible = visible;
  release(&nslock);
}

// does any live namespace see this mount? One that nobody sees is one a
// container made and then went away from, and kernel/vfs.c takes its slot
// back when it wants one.
int
mntns_anyone_sees(int slot)
{
  if (slot < 0 || slot >= 32)
    return 1;
  unsigned int bit = 1u << slot;
  int seen = 0;

  acquire(&nslock);
  if (root_mntns.visible & bit)
    seen = 1;
  for (int i = 0; !seen && i < NMNTNS; i++)
    if (mntnstab[i].ref && (mntnstab[i].visible & bit))
      seen = 1;
  release(&nslock);
  return seen;
}

struct super *
mntns_rootsb(struct mntns *ns)
{
  return ns ? ns->root : 0;
}

void
mntns_set_root(struct mntns *ns, struct super *sb)
{
  if (ns == 0 || ns == &root_mntns)
    return;
  acquire(&nslock);
  ns->root = sb;
  release(&nslock);
}

// caller holds nslock
static void
mntns_put_locked(struct mntns *ns)
{
  if (ns == 0 || ns == &root_mntns)
    return;
  if (--ns->ref <= 0)
    ns->ref = 0;
}

// caller holds nslock
static struct mntns *
mntns_clone_locked(struct mntns *from)
{
  for (int i = 0; i < NMNTNS; i++) {
    struct mntns *ns = &mntnstab[i];
    if (ns->ref)
      continue;
    memset(ns, 0, sizeof(*ns));
    ns->ref = 1;
    ns->id = i + 1;
    ns->visible = from->visible;   // a snapshot of what is mounted now
    ns->root = from->root;
    return ns;
  }
  return 0;
}

// ---- user namespaces ----
//
// Inside a container, root is root; outside it, it is an ordinary user
// with no more claim on the machine than any other. That is what the
// mapping is: a range of numbers inside standing for a range of numbers
// outside. The kernel keeps the outside ones -- so every check it makes
// is against the machine's own idea of who owns what -- and translates
// only where a number is given to a program or taken from one.

struct userns *
userns_root(void)
{
  return &root_userns;
}

struct userns *
userns_current(void)
{
  struct proc *p = myproc();
  struct nsproxy *ns = (p && p->ns) ? p->ns : &root_ns;

  return ns->user ? ns->user : &root_userns;
}

int
userns_in_root(void)
{
  return userns_current() == &root_userns;
}

int
uid_to_ns(struct userns *ns, unsigned int kuid)
{
  if (ns == 0 || ns == &root_userns)
    return (int)kuid;
  if (!ns->uid_set)
    return -1;
  if (kuid >= ns->uid_host && kuid < ns->uid_host + ns->uid_count)
    return (int)(ns->uid_inside + (kuid - ns->uid_host));
  return -1;
}

int
gid_to_ns(struct userns *ns, unsigned int kgid)
{
  if (ns == 0 || ns == &root_userns)
    return (int)kgid;
  if (!ns->gid_set)
    return -1;
  if (kgid >= ns->gid_host && kgid < ns->gid_host + ns->gid_count)
    return (int)(ns->gid_inside + (kgid - ns->gid_host));
  return -1;
}

int
uid_from_ns(struct userns *ns, unsigned int uid)
{
  if (ns == 0 || ns == &root_userns)
    return (int)uid;
  if (!ns->uid_set)
    return -1;
  if (uid >= ns->uid_inside && uid < ns->uid_inside + ns->uid_count)
    return (int)(ns->uid_host + (uid - ns->uid_inside));
  return -1;
}

int
gid_from_ns(struct userns *ns, unsigned int gid)
{
  if (ns == 0 || ns == &root_userns)
    return (int)gid;
  if (!ns->gid_set)
    return -1;
  if (gid >= ns->gid_inside && gid < ns->gid_inside + ns->gid_count)
    return (int)(ns->gid_host + (gid - ns->gid_inside));
  return -1;
}

// the map is written once, by somebody privileged in the namespace above
// it: a container cannot widen its own mapping afterwards, which is the
// whole reason the mapping can be trusted.
int
userns_set_map(struct userns *ns, int isuid, unsigned int inside,
               unsigned int host, unsigned int count)
{
  if (ns == 0 || ns == &root_userns)
    return -EINVAL;
  if (count == 0 || count > 65536)
    return -EINVAL;
  acquire(&nslock);
  if (isuid) {
    if (ns->uid_set) {
      release(&nslock);
      return -EPERM;
    }
    ns->uid_inside = inside;
    ns->uid_host = host;
    ns->uid_count = count;
    ns->uid_set = 1;
  } else {
    if (ns->gid_set) {
      release(&nslock);
      return -EPERM;
    }
    ns->gid_inside = inside;
    ns->gid_host = host;
    ns->gid_count = count;
    ns->gid_set = 1;
  }
  release(&nslock);
  return 0;
}

// the mapping, for /proc/<pid>/uid_map. -1 when there is none yet.
int
userns_map_of(struct userns *ns, int isuid, unsigned int *inside,
              unsigned int *host, unsigned int *count)
{
  if (ns == 0 || ns == &root_userns)
    return -1;
  acquire(&nslock);
  int set = isuid ? ns->uid_set : ns->gid_set;
  if (set) {
    *inside = isuid ? ns->uid_inside : ns->gid_inside;
    *host = isuid ? ns->uid_host : ns->gid_host;
    *count = isuid ? ns->uid_count : ns->gid_count;
  }
  release(&nslock);
  return set ? 0 : -1;
}

// caller holds nslock
static void
userns_put_locked(struct userns *ns)
{
  if (ns == 0 || ns == &root_userns)
    return;
  if (--ns->ref <= 0)
    ns->ref = 0;
}

// caller holds nslock
static struct userns *
userns_new_locked(struct userns *parent)
{
  if (parent == 0)
    parent = &root_userns;
  if (parent->level + 1 >= 4)
    return 0;
  for (int i = 0; i < NUSERNS; i++) {
    struct userns *ns = &usernstab[i];
    if (ns->ref)
      continue;
    memset(ns, 0, sizeof(*ns));
    ns->ref = 1;
    ns->id = i + 1;
    ns->level = parent->level + 1;
    ns->parent = parent;
    return ns;
  }
  return 0;
}

// ---- the kinds, and joining one ----

const char *
ns_kind_name(int kind)
{
  switch (kind) {
  case NSK_UTS: return "uts";
  case NSK_NET: return "net";
  case NSK_PID: return "pid";
  case NSK_MNT: return "mnt";
  case NSK_USER: return "user";
  default: return 0;       // ipc is not provided in this revision (§8.3)
  }
}

// which namespace of this kind a process is in. The number is the slot,
// which is enough for what the file is for: the same for two processes
// in one namespace, different for two in different ones.
uint64
ns_id_of(int pid, int kind)
{
  struct proc *p = proc_get(pid);
  uint64 id = 0;

  if (p == 0)
    return 0;
  struct nsproxy *ns = p->ns ? p->ns : &root_ns;
  // the number is what `ls -l /proc/<pid>/ns' shows and what tells two
  // processes in the same namespace from two in different ones. The root
  // of each kind is 0 here as everywhere else.
  switch (kind) {
  case NSK_UTS:
    id = (uint64)(ns->uts ? ns->uts->id : root_uts.id);
    break;
  case NSK_PID:
    id = (uint64)(ns->pid ? ns->pid->id : root_pidns.id);
    break;
  case NSK_MNT:
    id = (uint64)(ns->mnt ? ns->mnt->id : root_mntns.id);
    break;
  case NSK_USER:
    id = (uint64)(ns->user ? ns->user->id : root_userns.id);
    break;
  case NSK_NET:
    id = (uint64)netns_id(ns->net);
    break;
  default:
    break;
  }
  proc_release(p);
  return id;
}

// the init of a pid namespace has gone (§8.3): nothing new may enter it,
// and everything in it goes too. A container is its init -- when that
// process ends, what it was running ends with it, rather than being
// reparented to the machine's init and left running with no way to name
// it. Called from exit, with no locks held.
void
pidns_init_gone(struct pidns *ns)
{
  if (ns == 0 || ns == &root_pidns)
    return;
  acquire(&nslock);
  ns->dying = 1;
  release(&nslock);
  proc_iter_start();
  for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
    if (p == myproc() || p->state == UNUSED || p->state == ZOMBIE)
      continue;
    if (pid_in_ns(p, ns) == 0)
      continue;
    setkilled(p);
  }
  proc_iter_end();
}

// setns(2): join the namespace of `kind' that process `pid' is in. What
// the caller is in of every other kind it stays in, which is what makes
// this one thing at a time.
int
ns_join(int pid, int kind)
{
  struct proc *me = myproc();
  struct proc *t = proc_get_ns(pidns_current(), pid);

  // a process privileged over the machine may name a process by the
  // machine's number even from inside a pid namespace it has joined --
  // which is what joining several namespaces one after another needs.
  if (t == 0 && userns_in_root() && cap_ok(CAP_SYS_ADMIN))
    t = proc_get(pid);
  if (t == 0)
    return -ESRCH;
  // NSK_UTS is 0, so the bound below is not "kind <= 0": the first kind
  // there is was the one it rejected.
  if (kind < 0 || kind >= NS_KINDS || kind == NSK_IPC) {
    proc_release(t);
    return -EINVAL;
  }
  // joining somebody else's namespace is the machine's business, and not
  // a container's however root it is inside one
  if (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root()) {
    proc_release(t);
    return -EPERM;
  }
  struct nsproxy *tns = t->ns ? t->ns : &root_ns;
  struct utsns *want_uts = tns->uts ? tns->uts : &root_uts;
  struct pidns *want_pid = tns->pid ? tns->pid : &root_pidns;
  struct mntns *want_mnt = tns->mnt ? tns->mnt : &root_mntns;
  struct userns *want_user = tns->user ? tns->user : &root_userns;
  struct netns *want_net = tns->net;
  proc_release(t);

  struct nsproxy *mine = me->ns ? me->ns : &root_ns;
  // a set of its own, which is its old one with the one namespace named
  // replaced. Everything it keeps, it keeps a reference to.
  acquire(&nslock);
  struct nsproxy *ns = 0;
  for (int i = 0; i < NNSPROXY; i++)
    if (nstab[i].ref == 0) {
      ns = &nstab[i];
      break;
    }
  if (ns == 0) {
    release(&nslock);
    return -ENOSPC;
  }
  memset(ns, 0, sizeof(*ns));
  ns->ref = 1;
  ns->uts = mine->uts ? mine->uts : &root_uts;
  ns->pid = mine->pid ? mine->pid : &root_pidns;
  ns->mnt = mine->mnt ? mine->mnt : &root_mntns;
  ns->user = mine->user ? mine->user : &root_userns;
  ns->net = mine->net;
  switch (kind) {
  case NSK_UTS:
    ns->uts = want_uts;
    break;
  case NSK_PID:
    // as Linux does it: the caller keeps the number its parent knows it
    // by, and its *children* are born in the namespace it has joined.
    ns->pid = want_pid;
    break;
  case NSK_MNT:
    ns->mnt = want_mnt;
    break;
  case NSK_USER:
    ns->user = want_user;
    break;
  case NSK_NET:
    ns->net = want_net;
    break;
  }
  if (ns->uts != &root_uts)
    ns->uts->ref++;
  if (ns->pid != &root_pidns)
    ns->pid->ref++;
  if (ns->mnt != &root_mntns)
    ns->mnt->ref++;
  if (ns->user != &root_userns)
    ns->user->ref++;
  release(&nslock);
  netns_hold(ns->net);

  struct nsproxy *old = me->ns;
  me->ns = ns;
  ns_put(old);
  // the working directory it had may be a directory the namespace it has
  // joined cannot see. "/" follows the mount namespace, so the honest
  // thing is to put it there and let it walk from a place that exists.
  if (kind == NSK_MNT) {
    struct inode *root = namei("/");
    if (root) {
      struct inode *was = me->fs->cwd;
      me->fs->cwd = root;
      if (was)
        iput(was);
    }
  }
  return 0;
}

// ---- uts ----

// is the caller in the machine's own uts namespace? (A process in a user
// namespace may set the name of a uts namespace of its own, and must not
// be able to set the machine's.)
int
uts_is_root(void)
{
  struct nsproxy *ns = ns_current();

  return (ns->uts ? ns->uts : &root_uts) == &root_uts;
}

struct utsns *
uts_current(void)
{
  struct nsproxy *ns = ns_current();

  return ns->uts ? ns->uts : &root_uts;
}

int
uts_sethostname(const char *s, int n)
{
  struct utsns *u = uts_current();

  if (n < 0 || n >= UTS_LEN)
    return -1;
  acquire(&nslock);
  safestrcpy(u->nodename, s, n + 1);
  release(&nslock);
  return 0;
}

int
uts_setdomainname(const char *s, int n)
{
  struct utsns *u = uts_current();

  if (n < 0 || n >= UTS_LEN)
    return -1;
  acquire(&nslock);
  safestrcpy(u->domainname, s, n + 1);
  release(&nslock);
  return 0;
}

const char *
uts_hostname(void)
{
  return uts_current()->nodename;
}

const char *
uts_domainname(void)
{
  return uts_current()->domainname;
}
