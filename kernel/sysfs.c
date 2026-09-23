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
  // the settable ones: what is written here is applied at once
  S_W_HOSTNAME,
  S_W_CACHE_PCT,
  S_W_FLUSH_SECS,
  S_W_TICK_HZ,
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
  add_rw("fs/writeback_secs", S_W_FLUSH_SECS, 0, 0, 0, 1);
  add_rw("kernel/tick_hz", S_W_TICK_HZ, 0, 0, 0, 1);

  // what the machine has
  add("hypervisor", S_DIR, 1, 0, 0);
  add("hypervisor/present", S_HYP_PRESENT, 0, 0, 0);
  add("hypervisor/vms", S_HYP_VMS, 0, 0, 0);
  add("vector", S_DIR, 1, 0, 0);
  add("vector/present", S_VECTOR_PRESENT, 0, 0, 0);
  add("vector/vlen", S_VECTOR_VLEN, 0, 0, 0);

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

extern char hostname[];

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
    gs(g, hostname);
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
    switch (e->kind) {
    case S_W_HOSTNAME:
      r = sethostname_str(buf, n) < 0 ? -EINVAL : (int)whole;
      break;
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
  sys_sb = sb;
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
