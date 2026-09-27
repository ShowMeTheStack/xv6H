//
// /sys: the kernel's objects as a tree of small files.
//
// procfs next door is a table of names with a function behind each
// one, which suits a flat directory of reports. /sys is not flat: it
// is a tree, and most of it is one line per attribute of an object
// that may or may not be there -- a disk, an interface, a mounted file
// system. so this file builds the tree instead of declaring it. every
// lookup and every readdir rebuilds the table from what the kernel has
// right now, which is a hundred entries and costs nothing, and means a
// disk that appears is in /sys without anyone having to tell it.
//
// an inode number is a hash of the path, kept unique at build time by
// walking on from a collision. that way a number stays the same when
// something unrelated appears or goes away, which an index into the
// table would not.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "defs.h"
#include "nsproxy.h"
#include "cgroup.h"
#include "blk.h"
#include "vfs.h"
#include "platform.h"
#include "page.h"
#include "vmm.h"
#include "hyp.h"
#include "errno.h"

#define SYSFS_DEV 34
#define SINO_ROOT 1

#define SYSFS_MAX_ENTRIES 256
#define SYSFS_PATHMAX 64

// what a file holds. the kind says which of these to print and the
// argument which object it belongs to.
enum {
  S_DIR = 0,
  S_TEXT,        // a fixed string, in `text`
  S_OSTYPE,
  S_OSRELEASE,
  S_VERSION,
  S_HOSTNAME,
  S_PAGESIZE,
  S_NCPU,
  S_MEM_TOTAL,
  S_MEM_FREE,
  S_UPTIME,
  S_DISK_SIZE,   // arg: the disk's index
  S_DISK_RO,
  S_DISK_REMOVABLE,
  S_DISK_SECTORSZ,
  S_DISK_BLOCKSZ,
  S_DISK_PARTITION,
  S_MOUNT_TYPE,  // arg: the mount's index
  S_MOUNT_SOURCE,
  S_MOUNT_PATH,
  S_MOUNT_RO,
  S_HYP_PRESENT,
  S_HYP_VMS,
  S_VECTOR_PRESENT,
  S_VECTOR_VLEN,
  S_MACHINE_SBI,
  // the settable ones: what is written here is applied at once
  S_W_HOSTNAME,
  S_W_CACHE_PCT,
  S_W_FLUSH_SECS,
  S_W_TICK_HZ,
  S_W_ASLR,
  // the control groups. `arg' is the group's slot, so a read or a write
  // knows which group it is about without looking it up by name.
  S_CG_CPU_WEIGHT,
  S_CG_CPU_MAX,
  S_CG_MEM_MAX,
  S_CG_MEM_CUR,
  S_CG_PIDS_MAX,
  S_CG_PIDS_CUR,
  S_CG_FREEZE,
  S_CG_PROCS,
  S_CG_STAT,
  S_W_CG_CREATE,
  S_W_CG_DESTROY,
};

struct sysent {
  char path[SYSFS_PATHMAX]; // relative to /sys, without a leading slash
  uint inum;
  uchar kind;
  uchar isdir;
  uchar writable; // root may set it, and the kernel applies it at once
  int arg;
  const char *text;
};

static struct sleeplock sysfs_lock;
static struct sysent table[SYSFS_MAX_ENTRIES];
static int ntable;
static const struct inode_ops sysfs_iops;
static struct super *sys_sb;

// ---- building the tree ----

static uint
path_hash(const char *s)
{
  uint h = 2166136261u;

  while (*s)
    h = (h ^ (uchar)*s++) * 16777619u;
  h &= 0x7fffffff;
  return h < 16 ? h + 16 : h; // leave room for the root and a margin
}

static struct sysent *
add_rw(const char *path, int kind, int isdir, int arg, const char *text, int writable);

static struct sysent *
add(const char *path, int kind, int isdir, int arg, const char *text)
{
  return add_rw(path, kind, isdir, arg, text, 0);
}

static struct sysent *
add_rw(const char *path, int kind, int isdir, int arg, const char *text, int writable)
{
  struct sysent *e;

  if (ntable >= SYSFS_MAX_ENTRIES)
    return 0;
  e = &table[ntable];
  memset(e, 0, sizeof(*e));
  safestrcpy(e->path, path, SYSFS_PATHMAX);
  e->kind = (uchar)kind;
  e->isdir = (uchar)isdir;
  e->arg = arg;
  e->text = text;
  e->writable = (uchar)writable;

  // the number is the hash of the path, moved on until it is this
  // table's alone. the walk is deterministic, so a path keeps its
  // number as long as the paths around it do.
  e->inum = path_hash(e->path);
  for (;;) {
    int clash = (e->inum == SINO_ROOT);
    for (int i = 0; i < ntable && !clash; i++)
      if (table[i].inum == e->inum)
        clash = 1;
    if (!clash)
      break;
    e->inum++;
  }
  ntable++;
  return e;
}

// a path made of two or three pieces, without a printf
static void
join(char *out, int max, const char *a, const char *b, const char *c)
{
  int n = 0;

  for (const char *p = a; p && *p && n < max - 1; p++)
    out[n++] = *p;
  if (b && *b && n < max - 1) {
    out[n++] = '/';
    for (const char *p = b; *p && n < max - 1; p++)
      out[n++] = *p;
  }
  if (c && *c && n < max - 1) {
    out[n++] = '/';
    for (const char *p = c; *p && n < max - 1; p++)
      out[n++] = *p;
  }
  out[n] = 0;
}

struct mount_collect {
  int n;
  char type[8][16];
  char source[8][MOUNTPATH];
  char path[8][MOUNTPATH];
  int readonly[8];
};

static struct mount_collect mc;

static void
mount_note(const char *source, const char *path, const char *type, int readonly, void *arg)
{
  struct mount_collect *c = arg;

  if (c->n >= 8)
    return;
  safestrcpy(c->type[c->n], type, 16);
  safestrcpy(c->source[c->n], source, MOUNTPATH);
  safestrcpy(c->path[c->n], path, MOUNTPATH);
  c->readonly[c->n] = readonly;
  c->n++;
}

// a mount's directory name: its path with the slashes turned into
// dashes, which is what "/" itself being a name forces.
static void
mount_dirname(const char *path, char *out, int max)
{
  int n = 0;

  if (strncmp(path, "/", 2) == 0) {
    safestrcpy(out, "root", max);
    return;
  }
  for (const char *p = path; *p && n < max - 1; p++)
    out[n++] = *p == '/' ? '-' : *p;
  out[n] = 0;
  if (out[0] == '-' && out[1])
    memmove(out, out + 1, strlen(out)); // no leading dash
}

static void
rebuild(void)
{
  char p1[SYSFS_PATHMAX], p2[SYSFS_PATHMAX];

  ntable = 0;

  // what the kernel is
  add("kernel", S_DIR, 1, 0, 0);
  add("kernel/ostype", S_OSTYPE, 0, 0, 0);
  add("kernel/osrelease", S_OSRELEASE, 0, 0, 0);
  add("kernel/version", S_VERSION, 0, 0, 0);
  add_rw("kernel/hostname", S_W_HOSTNAME, 0, 0, 0, 1);
  add("kernel/pagesize", S_PAGESIZE, 0, 0, 0);
  add("kernel/ncpu", S_NCPU, 0, 0, 0);
  add("kernel/uptime", S_UPTIME, 0, 0, 0);
  add("mm", S_DIR, 1, 0, 0);
  add("kernel/mm", S_DIR, 1, 0, 0);
  add("kernel/mm/total_pages", S_MEM_TOTAL, 0, 0, 0);
  add("kernel/mm/free_pages", S_MEM_FREE, 0, 0, 0);
  add_rw("mm/cache_pct", S_W_CACHE_PCT, 0, 0, 0, 1);
  add("fs", S_DIR, 1, 0, 0);
  // the control groups, as they are now
  add("fs/cgroup", S_DIR, 1, 0, 0);
  add_rw("fs/cgroup/cgroup.create", S_W_CG_CREATE, 0, 0, 0, 1);
  add_rw("fs/cgroup/cgroup.destroy", S_W_CG_DESTROY, 0, 0, 0, 1);
  for (int i = 0; i < NCGROUP; i++) {
    struct cgroup *cg = cg_at(i);
    if (cg == 0)
      continue;
    char dir[SYSFS_PATHMAX], f[SYSFS_PATHMAX];
    safestrcpy(dir, "fs/cgroup/", sizeof(dir));
    safestrcpy(dir + 10, cg->name, sizeof(dir) - 10);
    add(dir, S_DIR, 1, 0, 0);
    static const struct {
      const char *name;
      int kind;
      int writable;
    } cgfiles[] = {
        {"cpu.weight", S_CG_CPU_WEIGHT, 1}, {"cpu.max", S_CG_CPU_MAX, 1},
        {"memory.max", S_CG_MEM_MAX, 1},    {"memory.current", S_CG_MEM_CUR, 0},
        {"pids.max", S_CG_PIDS_MAX, 1},     {"pids.current", S_CG_PIDS_CUR, 0},
        {"freeze", S_CG_FREEZE, 1},         {"procs", S_CG_PROCS, 1},
        {"stat", S_CG_STAT, 0},             {0, 0, 0},
    };
    for (int k = 0; cgfiles[k].name; k++) {
      int n = strlen(dir);
      safestrcpy(f, dir, sizeof(f));
      if (n < (int)sizeof(f) - 2) {
        f[n++] = '/';
        f[n] = 0;
        safestrcpy(f + n, cgfiles[k].name, sizeof(f) - n);
      }
      add_rw(f, cgfiles[k].kind, 0, i, 0, cgfiles[k].writable);
    }
  }
  add_rw("fs/writeback_secs", S_W_FLUSH_SECS, 0, 0, 0, 1);
  add_rw("kernel/tick_hz", S_W_TICK_HZ, 0, 0, 0, 1);
  add_rw("kernel/randomize_va_space", S_W_ASLR, 0, 0, 0, 1);

  // what the machine has
  add("hypervisor", S_DIR, 1, 0, 0);
  add("hypervisor/present", S_HYP_PRESENT, 0, 0, 0);
  add("hypervisor/vms", S_HYP_VMS, 0, 0, 0);
  add("vector", S_DIR, 1, 0, 0);
  add("vector/present", S_VECTOR_PRESENT, 0, 0, 0);
  add("vector/vlen", S_VECTOR_VLEN, 0, 0, 0);
  add("machine", S_DIR, 1, 0, 0);
  add("machine/sbi", S_MACHINE_SBI, 0, 0, 0);     // the SBI extensions on offer (F22)

  // one directory per disk, as Linux's /sys/block is
  add("block", S_DIR, 1, 0, 0);
  for (int i = 0; i < MAXDISK; i++) {
    struct gendisk *d = blk_by_index(i);
    if (d == 0)
      continue;
    join(p1, sizeof(p1), "block", d->name, 0);
    add(p1, S_DIR, 1, i, 0);
    join(p2, sizeof(p2), p1, "size", 0);
    add(p2, S_DISK_SIZE, 0, i, 0);
    join(p2, sizeof(p2), p1, "ro", 0);
    add(p2, S_DISK_RO, 0, i, 0);
    join(p2, sizeof(p2), p1, "removable", 0);
    add(p2, S_DISK_REMOVABLE, 0, i, 0);
    join(p2, sizeof(p2), p1, "partitions", 0);
    add(p2, S_DISK_PARTITION, 0, i, 0);
    join(p2, sizeof(p2), p1, "queue", 0);
    add(p2, S_DIR, 1, i, 0);
    join(p2, sizeof(p2), p1, "queue", "hw_sector_size");
    add(p2, S_DISK_SECTORSZ, 0, i, 0);
    join(p2, sizeof(p2), p1, "queue", "logical_block_size");
    add(p2, S_DISK_BLOCKSZ, 0, i, 0);
  }

  // one directory per mounted file system, under the same fs/ the
  // tunables are in
  memset(&mc, 0, sizeof(mc));
  vfs_list(mount_note, &mc);
  for (int i = 0; i < mc.n; i++) {
    char name[MOUNTPATH];
    mount_dirname(mc.path[i], name, sizeof(name));
    join(p1, sizeof(p1), "fs", name, 0);
    add(p1, S_DIR, 1, i, 0);
    join(p2, sizeof(p2), p1, "type", 0);
    add(p2, S_MOUNT_TYPE, 0, i, 0);
    join(p2, sizeof(p2), p1, "source", 0);
    add(p2, S_MOUNT_SOURCE, 0, i, 0);
    join(p2, sizeof(p2), p1, "mountpoint", 0);
    add(p2, S_MOUNT_PATH, 0, i, 0);
    join(p2, sizeof(p2), p1, "readonly", 0);
    add(p2, S_MOUNT_RO, 0, i, 0);
  }
}

// ---- generating a file ----

struct gen {
  char *buf;
  int len, cap;
};

static void
gs(struct gen *g, const char *s)
{
  while (s && *s && g->len < g->cap - 1)
    g->buf[g->len++] = *s++;
}

static void
gn(struct gen *g, uint64 v)
{
  char tmp[24];
  int n = 0;

  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  }
  while (n > 0 && g->len < g->cap - 1)
    g->buf[g->len++] = tmp[--n];
}

// one line per process in a group, for its `procs' file
static void
cg_emit_pid(int pid, void *arg)
{
  struct gen *g = arg;

  gn(g, (uint64)pid);
  gs(g, "\n");
}



static void
generate(struct gen *g, struct sysent *e)
{
  struct gendisk *d = e->kind >= S_DISK_SIZE && e->kind <= S_DISK_PARTITION
                          ? blk_by_index(e->arg)
                          : 0;

  switch (e->kind) {
  case S_TEXT:
    gs(g, e->text);
    break;
  case S_OSTYPE:
    gs(g, "xv6H\n");
    break;
  case S_OSRELEASE:
    gs(g, XV6H_RELEASE);
    gs(g, "\n");
    break;
  case S_VERSION:
    gs(g, XV6H_VERSION);
    gs(g, "\n");
    break;
  case S_HOSTNAME:
  case S_W_HOSTNAME:
    gs(g, uts_hostname());
    gs(g, "\n");
    break;
  case S_W_CACHE_PCT:
    gn(g, blk_cache_pct());
    gs(g, "\n");
    break;
  case S_W_FLUSH_SECS:
    gn(g, blk_flush_secs);
    gs(g, "\n");
    break;
  case S_W_TICK_HZ:
    gn(g, tick_hz);
    gs(g, "\n");
    break;
  case S_W_ASLR:
    gn(g, randomize_va_space);
    gs(g, "\n");
    break;
  case S_PAGESIZE:
    gn(g, PGSIZE);
    gs(g, "\n");
    break;
  case S_NCPU:
    gn(g, plat.ncpu);
    gs(g, "\n");
    break;
  case S_UPTIME:
    gn(g, clock_monotonic_ns() / 1000000000UL);
    gs(g, "\n");
    break;

  // ---- the control groups ----
  case S_CG_CPU_WEIGHT:
  case S_CG_CPU_MAX:
  case S_CG_MEM_MAX:
  case S_CG_MEM_CUR:
  case S_CG_PIDS_MAX:
  case S_CG_PIDS_CUR:
  case S_CG_FREEZE: {
    struct cgroup *cg = cg_at(e->arg);
    static const int which[] = {CG_CPU_WEIGHT, CG_CPU_MAX, CG_MEMORY_MAX,
                                CG_MEMORY_CURRENT, CG_PIDS_MAX,
                                CG_PIDS_CURRENT, CG_FREEZE};
    if (cg == 0)
      break;
    gn(g, cg_get(cg, which[e->kind - S_CG_CPU_WEIGHT]));
    gs(g, "\n");
    break;
  }
  case S_CG_PROCS: {
    struct cgroup *cg = cg_at(e->arg);
    if (cg)
      cg_walk_procs(cg, cg_emit_pid, g);
    break;
  }
  case S_CG_STAT: {
    struct cgroup *cg = cg_at(e->arg);
    if (cg == 0)
      break;
    gs(g, "memory.current ");
    gn(g, cg_get(cg, CG_MEMORY_CURRENT));
    gs(g, "\nmemory.refused ");
    gn(g, cg_get(cg, CG_MEM_REFUSED));
    gs(g, "\npids.current ");
    gn(g, cg_get(cg, CG_PIDS_CURRENT));
    gs(g, "\npids.refused ");
    gn(g, cg_get(cg, CG_PID_REFUSED));
    gs(g, "\ncpu.throttles ");
    gn(g, cg_get(cg, CG_CPU_THROTTLES));
    gs(g, "\n");
    break;
  }
  case S_W_CG_CREATE:
  case S_W_CG_DESTROY:
    gs(g, "(write a name here)\n");
    break;
  case S_MEM_TOTAL:
  case S_MEM_FREE: {
    struct meminfo mi;
    kmem_info(&mi);
    gn(g, e->kind == S_MEM_TOTAL ? mi.total_pages : mi.free_pages);
    gs(g, "\n");
    break;
  }
  // a disk's size is in 512-byte sectors, as Linux reports it
  case S_DISK_SIZE:
    gn(g, d ? d->nsectors : 0);
    gs(g, "\n");
    break;
  case S_DISK_RO:
    gn(g, d && d->readonly ? 1 : 0);
    gs(g, "\n");
    break;
  case S_DISK_REMOVABLE:
    gs(g, "0\n");
    break;
  case S_DISK_PARTITION:
    // how many partitions the block layer found on it
    gn(g, d ? d->nparts : 0);
    gs(g, "\n");
    break;
  case S_DISK_SECTORSZ:
    gn(g, d ? d->sector_size : 512);
    gs(g, "\n");
    break;
  case S_DISK_BLOCKSZ:
    gn(g, BLKSIZE);
    gs(g, "\n");
    break;
  case S_MOUNT_TYPE:
    gs(g, e->arg < mc.n ? mc.type[e->arg] : "");
    gs(g, "\n");
    break;
  case S_MOUNT_SOURCE:
    gs(g, e->arg < mc.n ? mc.source[e->arg] : "");
    gs(g, "\n");
    break;
  case S_MOUNT_PATH:
    gs(g, e->arg < mc.n ? mc.path[e->arg] : "");
    gs(g, "\n");
    break;
  case S_MOUNT_RO:
    gs(g, e->arg < mc.n && mc.readonly[e->arg] ? "1\n" : "0\n");
    break;
  case S_HYP_PRESENT:
    gs(g, plat.has_h ? "1\n" : "0\n");
    break;
  case S_HYP_VMS:
    gn(g, vm_count());
    gs(g, "\n");
    break;
  case S_VECTOR_PRESENT:
    gs(g, plat.has_v ? "1\n" : "0\n");
    break;
  case S_VECTOR_VLEN:
    gn(g, plat.has_v ? plat.vlenb * 8 : 0);
    gs(g, "\n");
    break;
  case S_MACHINE_SBI: {
    char names[64];
    machine_sbi_names(names, sizeof(names));
    gs(g, names);
    gs(g, "\n");
    break;
  }
  default:
    break;
  }
}

// ---- the file system ----

static struct inode *
sysfs_iget(uint inum)
{
  struct inode *ip = iget(SYSFS_DEV, inum);

  if (ip) {
    ip->sb = sys_sb;
    ip->ops = &sysfs_iops;
  }
  return ip;
}

static struct sysent *
find_inum(uint inum)
{
  for (int i = 0; i < ntable; i++)
    if (table[i].inum == inum)
      return &table[i];
  return 0;
}

static struct sysent *
find_path(const char *path)
{
  for (int i = 0; i < ntable; i++)
    if (strncmp(table[i].path, path, SYSFS_PATHMAX) == 0)
      return &table[i];
  return 0;
}

// the part of `path` after `dir/`, if `path` is directly inside `dir`.
// a deeper path is not: /sys/block/vda is in /sys/block, /sys/block/
// vda/size is not.
static const char *
child_of(const char *dir, const char *path)
{
  int n = dir ? strlen(dir) : 0;

  if (n > 0) {
    if (strncmp(path, dir, n) != 0 || path[n] != '/')
      return 0;
    path += n + 1;
  }
  for (const char *p = path; *p; p++)
    if (*p == '/')
      return 0;
  return *path ? path : 0;
}

static void
sysfs_load(struct inode *ip)
{
  struct sysent *e;

  acquiresleep(&sysfs_lock);
  rebuild();
  ip->nlink = 1;
  // nothing here is stored, so the mode is the one every file of
  // this file system has: readable by all, written by root alone.
  ip->mode = 0444;
  ip->uid = ip->gid = 0;
  ip->major = ip->minor = 0;
  ip->size = 0;
  if (ip->inum == SINO_ROOT) {
    ip->type = T_DIR;
    ip->mode = 0555;
    ip->nlink = 2;
  } else if ((e = find_inum(ip->inum)) != 0) {
    ip->type = e->isdir ? T_DIR : T_FILE;
    if (e->writable)
      ip->mode = 0644; // root may set it; everyone may read it
    if (!e->isdir) {
      // the length is what generating it produces; a program that
      // trusts stat before reading gets the right number
      char *page = kalloc();
      if (page) {
        struct gen g = {page, 0, PGSIZE};
        generate(&g, e);
        ip->size = g.len;
        kfree(page);
      }
    }
  } else {
    ip->type = 0;
  }
  releasesleep(&sysfs_lock);
  ip->valid = 1;
}

static void
sysfs_update(struct inode *ip)
{
  (void)ip; // nothing here is stored
}

static int
sysfs_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  struct sysent *e;
  char *page;
  int r = 0;

  if (ip->inum == SINO_ROOT)
    return 0;
  if ((page = kalloc()) == 0)
    return -ENOMEM;
  acquiresleep(&sysfs_lock);
  rebuild();
  if ((e = find_inum(ip->inum)) != 0 && !e->isdir) {
    struct gen g = {page, 0, PGSIZE};
    generate(&g, e);
    if (off < (uint)g.len) {
      uint avail = g.len - off;
      if (n > avail)
        n = avail;
      r = either_copyout(user_dst, dst, page + off, n) < 0 ? -EFAULT : (int)n;
    }
  }
  releasesleep(&sysfs_lock);
  kfree(page);
  return r;
}

// writing to one of the settable files applies it at once, which is
// the point of having them: a number in a file that nothing reads
// would be a worse way of saying the same thing than a system call.
static int
sysfs_write(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  struct sysent *e;
  char buf[128];
  int r = -EACCES;

  uint whole = n;   // what the caller wrote, which is what it is told

  if (off != 0 || n == 0 || n >= sizeof(buf))
    return -EINVAL;
  if (either_copyin(buf, user_src, src, n) < 0)
    return -EFAULT;
  buf[n] = 0;
  // a value written with a newline is the same value. The newline is
  // still bytes the caller wrote, though: reporting fewer bytes than
  // were handed over sends stdio round again with the tail, which
  // arrives at a non-zero offset and is refused.
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
    buf[--n] = 0;

  acquiresleep(&sysfs_lock);
  rebuild();
  if ((e = find_inum(ip->inum)) != 0 && e->writable) {
    // the machine's own settings are the machine's, whoever asks. The
    // file's mode says root, and root inside a container holds
    // CAP_DAC_OVERRIDE, which goes through a mode: without this a
    // container could change the tick rate, the share of memory the
    // block cache may hold, how often the machine writes back, or how
    // every process after it is laid out in memory.
    if ((e->kind == S_W_CACHE_PCT || e->kind == S_W_FLUSH_SECS ||
         e->kind == S_W_TICK_HZ || e->kind == S_W_ASLR) &&
        (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root())) {
      releasesleep(&sysfs_lock);
      return -EPERM;
    }
    switch (e->kind) {
    case S_W_HOSTNAME:
      // the same door as sethostname(2), and the same rule: privileged,
      // and in a user namespace the name it may change is its own
      if (!cap_ok(CAP_SYS_ADMIN) || (!userns_in_root() && uts_is_root())) {
        r = -EPERM;
        break;
      }
      r = sethostname_str(buf, n) < 0 ? -EINVAL : (int)whole;
      break;

    case S_W_CG_CREATE: {
      // the tree is the machine's: a container is inside one of these
      // groups and does not get to edit it. Root there is root for the
      // container, and this is the other kind of root.
      if (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root()) {
        r = -EPERM;
        break;
      }
      int e2 = cg_create(buf, cg_current(), 0);
      r = e2 < 0 ? e2 : (int)whole;
      break;
    }
    case S_W_CG_DESTROY: {
      // the tree is the machine's: a container is inside one of these
      // groups and does not get to edit it. Root there is root for the
      // container, and this is the other kind of root.
      if (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root()) {
        r = -EPERM;
        break;
      }
      struct cgroup *cg = cg_find(buf);
      if (cg == 0 || cg == cg_root()) {
        r = -ENOENT;
        break;
      }
      int e2 = cg_destroy(cg);
      r = e2 < 0 ? e2 : (int)whole;
      break;
    }
    case S_CG_PROCS: {
      // a process joins a group by being written into it: "echo $$ >
      // /sys/fs/cgroup/g/procs", which is how this has always been done.
      // the tree is the machine's: a container is inside one of these
      // groups and does not get to edit it. Root there is root for the
      // container, and this is the other kind of root.
      if (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root()) {
        r = -EPERM;
        break;
      }
      struct cgroup *cg = cg_at(e->arg);
      if (cg == 0) {
        r = -ENOENT;
        break;
      }
      int pid = 0, ok = n > 0;
      for (int i = 0; i < (int)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          ok = 0;
          break;
        }
        pid = pid * 10 + (buf[i] - '0');
      }
      if (!ok) {
        r = -EINVAL;
        break;
      }
      struct proc *t = proc_get_ns(pidns_current(), pid);
      if (t == 0) {
        r = -ESRCH;
        break;
      }
      int e2 = cg_join(t, cg);
      proc_release(t);
      r = e2 < 0 ? e2 : (int)whole;
      break;
    }
    case S_CG_CPU_WEIGHT:
    case S_CG_CPU_MAX:
    case S_CG_MEM_MAX:
    case S_CG_PIDS_MAX:
    case S_CG_FREEZE: {
      // the tree is the machine's: a container is inside one of these
      // groups and does not get to edit it. Root there is root for the
      // container, and this is the other kind of root.
      if (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root()) {
        r = -EPERM;
        break;
      }
      struct cgroup *cg = cg_at(e->arg);
      if (cg == 0) {
        r = -ENOENT;
        break;
      }
      uint64 v = 0;
      int ok = n > 0;
      for (int i = 0; i < (int)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          ok = 0;
          break;
        }
        v = v * 10 + (uint64)(buf[i] - '0');
      }
      if (!ok) {
        r = -EINVAL;
        break;
      }
      static const int which[] = {CG_CPU_WEIGHT, CG_CPU_MAX, CG_MEMORY_MAX,
                                  CG_PIDS_MAX, CG_FREEZE};
      static const int kinds[] = {S_CG_CPU_WEIGHT, S_CG_CPU_MAX, S_CG_MEM_MAX,
                                  S_CG_PIDS_MAX, S_CG_FREEZE};
      int w = -1;
      for (int i = 0; i < 5; i++)
        if (kinds[i] == e->kind)
          w = which[i];
      int e2 = w < 0 ? -EINVAL : cg_set_limit(cg, w, v);
      r = e2 < 0 ? e2 : (int)whole;
      break;
    }
    case S_W_CACHE_PCT: {
      int v = 0;
      for (int i = 0; i < (int)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          v = -1;
          break;
        }
        v = v * 10 + (buf[i] - '0');
      }
      r = (v < 0 || blk_set_cache_pct(v) < 0) ? -EINVAL : (int)whole;
      break;
    }
    case S_W_FLUSH_SECS: {
      int v = 0;
      for (int i = 0; i < (int)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          v = -1;
          break;
        }
        v = v * 10 + (buf[i] - '0');
      }
      r = (v < 1 || v > 600) ? -EINVAL : (blk_flush_secs = v, (int)whole);
      break;
    }
    case S_W_TICK_HZ: {
      int v = 0;
      for (int i = 0; i < (int)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          v = -1;
          break;
        }
        v = v * 10 + (buf[i] - '0');
      }
      // ticks already counted keep the meaning they had; what this
      // changes is what the ones from here on are worth. It is a knob
      // for measuring, not something to turn under a running load.
      r = (v < 1 || tick_set_hz(v) < 0) ? -EINVAL : (int)whole;
      break;
    }
    case S_W_ASLR: {
      int v = 0;
      for (int i = 0; i < (int)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          v = -1;
          break;
        }
        v = v * 10 + (buf[i] - '0');
      }
      // 0 lays every address out the same way every time, which is what
      // a debugger wants; 1 moves the stack and the mmap area; 2 moves
      // the text of a position-independent executable as well. Only
      // processes started after the write are affected -- the ones
      // already running keep the layout they were given.
      r = (v < 0 || v > 2) ? -EINVAL : (randomize_va_space = v, (int)whole);
      break;
    }
    default:
      r = -EACCES;
      break;
    }
  }
  releasesleep(&sysfs_lock);
  return r;
}

static struct inode *
sysfs_lookup(struct inode *dp, char *name, uint *poff)
{
  struct sysent *e, *d;
  struct inode *ip = 0;
  char want[SYSFS_PATHMAX];

  if (poff)
    *poff = 0;
  acquiresleep(&sysfs_lock);
  rebuild();
  if (namecmp(name, ".") == 0) {
    ip = sysfs_iget(dp->inum);
    goto out;
  }
  if (namecmp(name, "..") == 0) {
    // the parent is the path with its last element cut off
    if (dp->inum == SINO_ROOT || (d = find_inum(dp->inum)) == 0) {
      ip = sysfs_iget(SINO_ROOT);
      goto out;
    }
    safestrcpy(want, d->path, sizeof(want));
    char *slash = 0;
    for (char *p = want; *p; p++)
      if (*p == '/')
        slash = p;
    if (slash == 0) {
      ip = sysfs_iget(SINO_ROOT);
      goto out;
    }
    *slash = 0;
    e = find_path(want);
    ip = sysfs_iget(e ? e->inum : SINO_ROOT);
    goto out;
  }
  if (dp->inum == SINO_ROOT) {
    safestrcpy(want, name, sizeof(want));
  } else {
    if ((d = find_inum(dp->inum)) == 0 || !d->isdir)
      goto out;
    join(want, sizeof(want), d->path, name, 0);
  }
  if ((e = find_path(want)) != 0)
    ip = sysfs_iget(e->inum);

out:
  releasesleep(&sysfs_lock);
  return ip;
}

static int
sysfs_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  struct sysent *d = 0;
  const char *dir = 0;
  uint64 i = *pos;
  int r = 0;

  acquiresleep(&sysfs_lock);
  rebuild();
  if (dp->inum != SINO_ROOT) {
    if ((d = find_inum(dp->inum)) == 0 || !d->isdir) {
      releasesleep(&sysfs_lock);
      return 0;
    }
    dir = d->path;
  }
  if (i == 0 || i == 1) {
    safestrcpy(name, i == 0 ? "." : "..", namemax);
    *ino = dp->inum;
    *type = 4;
    *pos = i + 1;
    releasesleep(&sysfs_lock);
    return 1;
  }
  // then every entry of the table that is directly inside this one
  uint64 want = i - 2, seen = 0;
  for (int k = 0; k < ntable; k++) {
    const char *child = child_of(dir, table[k].path);
    if (child == 0)
      continue;
    if (seen++ != want)
      continue;
    safestrcpy(name, child, namemax);
    *ino = table[k].inum;
    *type = table[k].isdir ? 4 : 8;
    *pos = i + 1;
    r = 1;
    break;
  }
  releasesleep(&sysfs_lock);
  return r;
}

static const struct inode_ops sysfs_iops = {
    .load = sysfs_load,
    .update = sysfs_update,
    .read = sysfs_read,
    .write = sysfs_write,
    .lookup = sysfs_lookup,
    .readdir = sysfs_readdir,
};

static struct inode *
sysfs_sb_iget(struct super *sb, uint inum)
{
  (void)sb;
  return sysfs_iget(inum);
}

static void
sysfs_sb_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
                uint64 *bsize, uint64 *namelen)
{
  (void)sb;
  *blocks = *bfree = *files = *ffree = 0;
  *bsize = PGSIZE;
  if (namelen)
    *namelen = NAME_MAX;
}

static const struct super_ops sysfs_sops = {
    .iget = sysfs_sb_iget,
    .statfs = sysfs_sb_statfs,
};

static struct super *
sysfs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)disk;
  (void)source;
  (void)readonly;
  (void)opts;

  struct super *sb = vfs_super_alloc();
  if (sb == 0)
    return 0;
  sb->ops = &sysfs_sops;
  sb->iops = &sysfs_iops;
  if (sys_sb == 0)
    sys_sb = sb;        // the first mount owns the inodes
  sb->root = sysfs_iget(SINO_ROOT);
  if (sb->root == 0) {
    vfs_super_free(sb);
    return 0;
  }
  return sb;
}

static struct fs_type sysfs_type = {
    .name = "sysfs",
    .mount = sysfs_mount,
};

void
sysfs_init(void)
{
  initsleeplock(&sysfs_lock, "sysfs");
  vfs_register(&sysfs_type);
}
