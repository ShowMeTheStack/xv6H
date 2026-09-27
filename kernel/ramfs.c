//
// ramfs: a file system that is nothing but memory.
//
// there is no disk and no format. an inode is a structure, a
// directory is a list of names, and a file's contents are pages held
// in a two-level table, so a file is as large as the pages it has and
// a hole in one costs nothing. it is what /tmp wants, what an initial
// root wants before a real disk is mounted, and what the upper layer
// of an overlay wants.
//
// the same code answers to two names. "ramfs" grows until memory runs
// out; "tmpfs" is the same thing with a limit, which is what the size=
// mount option sets. everything else about them is identical, as it is
// in Linux.
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
#include "xattr.h"
#include "errno.h"

#define RAMFS_DEV_BASE 40
#define RAMFS_MAX_MOUNTS 8
#define RAMFS_ROOT_INO 1

// a file's pages, in two levels: a table of tables. one page holds 512
// pointers, so two levels reach 512 * 512 pages, which is a gigabyte.
#define PTRS_PER_PAGE (PGSIZE / (int)sizeof(void *))
#define RAMFS_MAX_PAGES ((uint64)PTRS_PER_PAGE * PTRS_PER_PAGE)

struct ramfs_dirent {
  struct ramfs_dirent *next;
  uint inum;
  uchar type;
  char name[DIRSIZ];
};

struct ramfs_xattr {
  struct ramfs_xattr *next;
  char *value;
  int vlen;
  char name[XATTR_NAME_MAX + 1];
};

struct ramfs_node {
  struct ramfs_node *next; // every node of this mount, in one list
  uint inum;
  short type;
  short major, minor;
  short nlink;
  uint mode;               // the permission bits
  uint attr;               // the chattr flags: immutable, append-only
  uint uid, gid;
  uint64 size;
  uint64 atime, mtime, ctime;
  char ***l1;                    // the page table, allocated on first write
  char *link;                    // a symbolic link's target
  struct ramfs_dirent *entries;  // a directory's names
  struct ramfs_xattr *xattrs;
};

struct ramfs_mount {
  struct sleeplock lock;
  uint dev;
  struct ramfs_node *nodes;
  uint next_ino;
  uint64 pages_used;
  uint64 pages_max; // 0 for no limit, which is what ramfs is
  int half;         // no size= given: the limit is half the machine, whatever size the machine is now (F13/F14)
  int inuse;
};

static struct ramfs_mount mounts[RAMFS_MAX_MOUNTS];

// the limit of a tmpfs. Given with size=, it is fixed; left to the default
// it is half the machine -- the machine as it is now, since a revision can
// change what it has (F13/F14). Linux freezes it at mount time; a machine
// whose description is revisable should not.
static uint64
limit(struct ramfs_mount *m)
{
  if (m->half) {
    uint64 h = kmem_total_pages() / 2;
    return h ? h : 1;
  }
  return m->pages_max;
}
static const struct inode_ops ramfs_iops;
static struct super *supers[RAMFS_MAX_MOUNTS];

static struct ramfs_mount *
mount_of(struct inode *ip)
{
  int i = (int)ip->dev - RAMFS_DEV_BASE;

  if (i < 0 || i >= RAMFS_MAX_MOUNTS || !mounts[i].inuse)
    return 0;
  return &mounts[i];
}

static struct ramfs_node *
node_find(struct ramfs_mount *m, uint inum)
{
  for (struct ramfs_node *n = m->nodes; n; n = n->next)
    if (n->inum == inum)
      return n;
  return 0;
}

// ---- the pages of one file ----

// the page holding file offset `idx`, allocated if asked for and if
// the mount has room for another one.
static char *
node_page(struct ramfs_mount *m, struct ramfs_node *n, uint64 idx, int alloc)
{
  char **l2;
  char *p;

  if (idx >= RAMFS_MAX_PAGES)
    return 0;
  if (n->l1 == 0) {
    if (!alloc)
      return 0;
    if ((n->l1 = (char ***)kalloc()) == 0)
      return 0;
    memset(n->l1, 0, PGSIZE);
  }
  uint64 hi = idx / PTRS_PER_PAGE, lo = idx % PTRS_PER_PAGE;
  l2 = n->l1[hi];
  if (l2 == 0) {
    if (!alloc)
      return 0;
    if ((l2 = (char **)kalloc()) == 0)
      return 0;
    memset(l2, 0, PGSIZE);
    n->l1[hi] = l2;
  }
  p = l2[lo];
  if (p == 0 && alloc) {
    if (m->pages_max && m->pages_used >= limit(m))
      return 0;
    if ((p = kalloc()) == 0)
      return 0;
    memset(p, 0, PGSIZE);
    l2[lo] = p;
    m->pages_used++;
  }
  return p;
}

// give back every page from `from` on, which is what truncation is
static void
node_drop_pages(struct ramfs_mount *m, struct ramfs_node *n, uint64 from)
{
  if (n->l1 == 0)
    return;
  for (uint64 hi = 0; hi < PTRS_PER_PAGE; hi++) {
    char **l2 = n->l1[hi];
    if (l2 == 0)
      continue;
    int any = 0;
    for (uint64 lo = 0; lo < PTRS_PER_PAGE; lo++) {
      uint64 idx = hi * PTRS_PER_PAGE + lo;
      if (l2[lo] == 0)
        continue;
      if (idx >= from) {
        kfree(l2[lo]);
        l2[lo] = 0;
        m->pages_used--;
      } else {
        any = 1;
      }
    }
    if (!any) {
      kfree(l2);
      n->l1[hi] = 0;
    }
  }
  if (from == 0) {
    kfree(n->l1);
    n->l1 = 0;
  }
}

static void
node_free(struct ramfs_mount *m, struct ramfs_node *n)
{
  struct ramfs_dirent *d, *dn;
  struct ramfs_xattr *x, *xn;

  node_drop_pages(m, n, 0);
  for (d = n->entries; d; d = dn) {
    dn = d->next;
    kfree(d);
  }
  for (x = n->xattrs; x; x = xn) {
    xn = x->next;
    if (x->value)
      kfree(x->value);
    kfree(x);
  }
  if (n->link)
    kfree(n->link);
  kfree(n);
}

// ---- inodes ----

static uint64
now_seconds(void)
{
  return clock_realtime_ns() / 1000000000UL;
}

static struct ramfs_node *
node_alloc(struct ramfs_mount *m, short type)
{
  struct ramfs_node *n = (struct ramfs_node *)kalloc();

  if (n == 0)
    return 0;
  memset(n, 0, sizeof(*n));
  n->inum = m->next_ino++;
  n->type = type;
  n->nlink = 1;
  n->mode = type == T_DIR ? 0777 : (type == T_SYMLINK ? 0777 : 0666);
  n->atime = n->mtime = n->ctime = now_seconds();
  n->next = m->nodes;
  m->nodes = n;
  return n;
}

static struct inode *
ramfs_iget(struct ramfs_mount *m, uint inum)
{
  struct inode *ip = iget(m->dev, inum);
  int i = (int)m->dev - RAMFS_DEV_BASE;

  if (ip) {
    ip->sb = supers[i];
    ip->ops = &ramfs_iops;
  }
  return ip;
}

static void
ramfs_load(struct inode *ip)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;

  ip->type = 0;
  ip->nlink = 0;
  ip->size = 0;
  ip->major = ip->minor = 0;
  if (m == 0)
    return;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0) {
    ip->type = n->type;
    ip->nlink = n->nlink;
    ip->size = n->size;
    ip->mode = n->mode;
    ip->attr = n->attr;
    ip->uid = n->uid;
    ip->gid = n->gid;
    ip->atime = n->atime;
    ip->mtime = n->mtime;
    ip->ctime = n->ctime;
    ip->major = n->major;
    ip->minor = n->minor;
  }
  releasesleep(&m->lock);
  ip->valid = 1;
}

static void
ramfs_update(struct inode *ip)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;

  if (m == 0)
    return;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0) {
    n->nlink = ip->nlink;
    n->major = ip->major;
    n->minor = ip->minor;
    n->mode = ip->mode;
    n->attr = ip->attr;
    n->uid = ip->uid;
    n->gid = ip->gid;
    if (n->type == T_FILE && ip->size < n->size) {
      // the only shrink that reaches here is a truncate the layer
      // above has already decided on
      node_drop_pages(m, n, (ip->size + PGSIZE - 1) / PGSIZE);
      n->size = ip->size;
    }
    n->ctime = now_seconds();
  }
  releasesleep(&m->lock);
}

// ---- reading and writing ----

static int
ramfs_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *node;
  uint done = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((node = node_find(m, ip->inum)) == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  if (off >= node->size) {
    releasesleep(&m->lock);
    return 0;
  }
  if (off + n > node->size)
    n = (uint)(node->size - off);

  while (done < n) {
    uint64 idx = (off + done) / PGSIZE;
    uint within = (off + done) % PGSIZE;
    uint chunk = PGSIZE - within;
    if (chunk > n - done)
      chunk = n - done;
    char *p = node_page(m, node, idx, 0);
    int e;
    if (p == 0) {
      // a hole: it reads as zeroes, and a page of them is where they
      // come from without making one up on the disk
      char *zero = kalloc();
      if (zero == 0) {
        releasesleep(&m->lock);
        return done > 0 ? (int)done : -ENOMEM;
      }
      memset(zero, 0, PGSIZE);
      e = either_copyout(user_dst, dst + done, zero, chunk);
      kfree(zero);
    } else {
      e = either_copyout(user_dst, dst + done, p + within, chunk);
    }
    if (e < 0)
      break;
    done += chunk;
  }
  node->atime = now_seconds();
  releasesleep(&m->lock);
  return done > 0 ? (int)done : (n == 0 ? 0 : -EFAULT);
}

static int
ramfs_write(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *node;
  uint done = 0;
  int err = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((node = node_find(m, ip->inum)) == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  if (off > node->size) {
    releasesleep(&m->lock);
    return -EINVAL;
  }
  while (done < n) {
    uint64 idx = (off + done) / PGSIZE;
    uint within = (off + done) % PGSIZE;
    uint chunk = PGSIZE - within;
    if (chunk > n - done)
      chunk = n - done;
    char *p = node_page(m, node, idx, 1);
    if (p == 0) {
      err = -ENOSPC;
      break;
    }
    if (either_copyin(p + within, user_src, src + done, chunk) < 0) {
      err = -EFAULT;
      break;
    }
    done += chunk;
  }
  if (off + done > node->size)
    node->size = off + done;
  node->mtime = node->ctime = now_seconds();
  ip->size = node->size;
  releasesleep(&m->lock);
  if (done > 0)
    return (int)done;
  return err ? err : 0;
}

// ramfs pages are kept in a sparse table, so a hole costs nothing and
// growing is only a change of the recorded size
static int
ramfs_setsize(struct inode *ip, uint64 len)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;
  int r = 0;

  if (m == 0)
    return -EIO;
  if (ip->type != T_FILE)
    return -EINVAL;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) == 0) {
    r = -ENOENT;
  } else if (m->pages_max && (len + PGSIZE - 1) / PGSIZE > limit(m)) {
    r = -EFBIG; // a tmpfs with a size limit will not be grown past it
  } else {
    if (len < n->size) {
      // the pages wholly past the new end go back, and what is left in
      // the last one is zeroed so a later write does not uncover it
      node_drop_pages(m, n, (len + PGSIZE - 1) / PGSIZE);
      uint64 tail = len % PGSIZE;
      if (tail) {
        char *pg = node_page(m, n, len / PGSIZE, 0);
        if (pg)
          memset(pg + tail, 0, PGSIZE - tail);
      }
    }
    n->size = len;
    n->mtime = n->ctime = now_seconds();
    ip->size = len;
  }
  releasesleep(&m->lock);
  return r;
}

static void
ramfs_trunc(struct inode *ip)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;

  if (m == 0)
    return;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0) {
    node_drop_pages(m, n, 0);
    n->size = 0;
    n->mtime = n->ctime = now_seconds();
  }
  releasesleep(&m->lock);
  ip->size = 0;
}

static int
ramfs_fsync(struct inode *ip, int datasync)
{
  (void)ip;
  (void)datasync;
  return 0; // there is no disk for it to be on
}

// ---- directories ----

static struct ramfs_dirent *
dirent_find(struct ramfs_node *dir, const char *name, struct ramfs_dirent **prev)
{
  struct ramfs_dirent *p = 0;

  for (struct ramfs_dirent *d = dir->entries; d; p = d, d = d->next)
    if (strncmp(d->name, name, DIRSIZ) == 0) {
      if (prev)
        *prev = p;
      return d;
    }
  return 0;
}

static int
dirent_add(struct ramfs_node *dir, const char *name, uint inum, uchar type)
{
  struct ramfs_dirent *d;

  if (dirent_find(dir, name, 0))
    return -EEXIST;
  if ((d = (struct ramfs_dirent *)kalloc()) == 0)
    return -ENOMEM;
  memset(d, 0, sizeof(*d));
  safestrcpy(d->name, name, DIRSIZ);
  d->inum = inum;
  d->type = type;
  d->next = dir->entries;
  dir->entries = d;
  dir->size += sizeof(struct ramfs_dirent);
  dir->mtime = dir->ctime = now_seconds();
  return 0;
}

static struct inode *
ramfs_lookup(struct inode *dp, char *name, uint *poff)
{
  struct ramfs_mount *m = mount_of(dp);
  struct ramfs_node *dir;
  struct inode *ip = 0;

  if (poff)
    *poff = 0;
  if (m == 0)
    return 0;
  acquiresleep(&m->lock);
  if ((dir = node_find(m, dp->inum)) != 0 && dir->type == T_DIR) {
    if (namecmp(name, ".") == 0) {
      ip = ramfs_iget(m, dp->inum);
    } else {
      struct ramfs_dirent *d = dirent_find(dir, name, 0);
      if (d)
        ip = ramfs_iget(m, d->inum);
    }
  }
  releasesleep(&m->lock);
  return ip;
}

static int
ramfs_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  struct ramfs_mount *m = mount_of(dp);
  struct ramfs_node *dir;
  uint64 i = 0;
  int r = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((dir = node_find(m, dp->inum)) == 0 || dir->type != T_DIR) {
    releasesleep(&m->lock);
    return 0;
  }
  for (struct ramfs_dirent *d = dir->entries; d; d = d->next, i++) {
    if (i < *pos)
      continue;
    safestrcpy(name, d->name, namemax);
    *ino = d->inum;
    *type = d->type;
    *pos = i + 1;
    r = 1;
    break;
  }
  releasesleep(&m->lock);
  return r;
}

static uchar
dtype_of(short type)
{
  switch (type) {
  case T_DIR:
    return 2;
  case T_SYMLINK:
    return 7;
  case T_DEVICE:
    return 3;
  default:
    return 1;
  }
}

static struct inode *
ramfs_create(struct inode *dp, char *name, short type, short major, short minor)
{
  struct ramfs_mount *m = mount_of(dp);
  struct ramfs_node *dir, *n;
  struct inode *ip = 0;
  uint inum;

  if (m == 0)
    return 0;
  acquiresleep(&m->lock);
  if ((dir = node_find(m, dp->inum)) == 0 || dir->type != T_DIR)
    goto out;
  if (dirent_find(dir, name, 0))
    goto out;
  if ((n = node_alloc(m, type)) == 0)
    goto out;
  n->major = major;
  n->minor = minor;
  if (type == T_DIR) {
    // "." and ".." are entries here as they are on a disk, so that a
    // walk out of a directory works without a special case
    n->nlink = 2;
    if (dirent_add(n, ".", n->inum, 2) < 0 || dirent_add(n, "..", dir->inum, 2) < 0) {
      m->nodes = n->next;
      node_free(m, n);
      goto out;
    }
    dir->nlink++;
  }
  if (dirent_add(dir, name, n->inum, dtype_of(type)) < 0) {
    m->nodes = n->next;
    node_free(m, n);
    goto out;
  }
  inum = n->inum;
  dp->nlink = dir->nlink;
  dp->size = dir->size;
  releasesleep(&m->lock);

  ip = ramfs_iget(m, inum);
  if (ip == 0)
    return 0;
  ilock(ip);
  return ip;

out:
  releasesleep(&m->lock);
  return 0;
}

static int
ramfs_link(struct inode *dp, char *name, struct inode *ip)
{
  struct ramfs_mount *m = mount_of(dp);
  struct ramfs_node *dir, *n;
  int r;

  if (m == 0 || ip->dev != dp->dev)
    return -EXDEV;
  acquiresleep(&m->lock);
  if ((dir = node_find(m, dp->inum)) == 0 || dir->type != T_DIR ||
      (n = node_find(m, ip->inum)) == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  r = dirent_add(dir, name, n->inum, dtype_of(n->type));
  dp->size = dir->size;
  releasesleep(&m->lock);
  return r;
}

static int
ramfs_unlink(struct inode *dp, char *name)
{
  struct ramfs_mount *m = mount_of(dp);
  struct ramfs_node *dir, *n;
  struct ramfs_dirent *d, *prev = 0;
  uint victim = 0;
  short left = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((dir = node_find(m, dp->inum)) == 0 || dir->type != T_DIR) {
    releasesleep(&m->lock);
    return -EIO;
  }
  if ((d = dirent_find(dir, name, &prev)) == 0) {
    releasesleep(&m->lock);
    return -ENOENT;
  }
  n = node_find(m, d->inum);
  if (n && n->type == T_DIR) {
    // a directory goes only when nothing but "." and ".." is in it
    for (struct ramfs_dirent *e = n->entries; e; e = e->next)
      if (strncmp(e->name, ".", DIRSIZ) != 0 && strncmp(e->name, "..", DIRSIZ) != 0) {
        releasesleep(&m->lock);
        return -ENOTEMPTY;
      }
  }
  if (prev)
    prev->next = d->next;
  else
    dir->entries = d->next;
  kfree(d);
  if (dir->size >= sizeof(struct ramfs_dirent))
    dir->size -= sizeof(struct ramfs_dirent);
  dir->mtime = dir->ctime = now_seconds();
  if (n) {
    victim = n->inum;
    if (n->type == T_DIR) {
      n->nlink = 0; // "." went with the entry, and ".." with the directory
      if (dir->nlink > 0)
        dir->nlink--;
    } else if (n->nlink > 0) {
      n->nlink--;
    }
    left = n->nlink;
  }
  dp->nlink = dir->nlink;
  dp->size = dir->size;
  releasesleep(&m->lock);

  // the node itself goes when its last link and its last reference are
  // both gone, which is the inode layer's decision: take a reference,
  // tell it the new link count, and drop the reference again.
  if (victim) {
    struct inode *ip = ramfs_iget(m, victim);
    if (ip) {
      ilock(ip);
      ip->nlink = left;
      iunlockput(ip);
    }
  }
  return 0;
}

static int
ramfs_symlink(struct inode *dp, char *name, const char *target)
{
  struct ramfs_mount *m = mount_of(dp);
  struct ramfs_node *dir, *n;
  int len = strlen(target);
  int r;

  if (m == 0)
    return -EIO;
  if (len >= MAXPATH)
    return -ENAMETOOLONG;
  acquiresleep(&m->lock);
  if ((dir = node_find(m, dp->inum)) == 0 || dir->type != T_DIR) {
    releasesleep(&m->lock);
    return -EIO;
  }
  if ((n = node_alloc(m, T_SYMLINK)) == 0) {
    releasesleep(&m->lock);
    return -ENOMEM;
  }
  if ((n->link = kalloc()) == 0) {
    m->nodes = n->next;
    node_free(m, n);
    releasesleep(&m->lock);
    return -ENOMEM;
  }
  memmove(n->link, target, len + 1);
  n->size = len;
  if ((r = dirent_add(dir, name, n->inum, dtype_of(T_SYMLINK))) < 0) {
    m->nodes = n->next;
    node_free(m, n);
    releasesleep(&m->lock);
    return r;
  }
  dp->size = dir->size;
  releasesleep(&m->lock);
  return 0;
}

static int
ramfs_readlink(struct inode *ip, char *buf, int max)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;
  int len = -EINVAL;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0 && n->type == T_SYMLINK && n->link) {
    len = strlen(n->link);
    if (len > max)
      len = max;
    memmove(buf, n->link, len);
  }
  releasesleep(&m->lock);
  return len;
}

// ---- extended attributes ----
//
// a list per inode. every name is kept, whatever its prefix: nothing
// here goes on a disk, so there is no format to refuse one.

static int
ramfs_getxattr(struct inode *ip, const char *name, char *buf, int size)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;
  int r = -ENODATA;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0)
    for (struct ramfs_xattr *x = n->xattrs; x; x = x->next)
      if (strncmp(x->name, name, sizeof(x->name)) == 0) {
        if (size == 0)
          r = x->vlen;
        else if (size < x->vlen)
          r = -ERANGE;
        else {
          memmove(buf, x->value, x->vlen);
          r = x->vlen;
        }
        break;
      }
  releasesleep(&m->lock);
  return r;
}

static int
ramfs_setxattr(struct inode *ip, const char *name, const char *value, int size, int flags)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;
  struct ramfs_xattr *x, *prev = 0;
  int r = 0;

  if (m == 0)
    return -EIO;
  if (strlen(name) > XATTR_NAME_MAX)
    return -ERANGE;
  if (value && (size < 0 || size > PGSIZE))
    return -E2BIG;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  for (x = n->xattrs; x; prev = x, x = x->next)
    if (strncmp(x->name, name, sizeof(x->name)) == 0)
      break;
  if (x && (flags & XATTR_CREATE))
    r = -EEXIST;
  else if (!x && (flags & XATTR_REPLACE))
    r = -ENODATA;
  else if (value == 0) {
    if (x == 0)
      r = -ENODATA;
    else {
      if (prev)
        prev->next = x->next;
      else
        n->xattrs = x->next;
      if (x->value)
        kfree(x->value);
      kfree(x);
    }
  } else {
    if (x == 0) {
      if ((x = (struct ramfs_xattr *)kalloc()) == 0)
        r = -ENOMEM;
      else {
        memset(x, 0, sizeof(*x));
        safestrcpy(x->name, name, sizeof(x->name));
        x->next = n->xattrs;
        n->xattrs = x;
      }
    }
    if (r == 0) {
      if (x->value == 0 && (x->value = kalloc()) == 0)
        r = -ENOMEM;
      else {
        memmove(x->value, value, size);
        x->vlen = size;
      }
    }
  }
  releasesleep(&m->lock);
  return r;
}

static int
ramfs_listxattr(struct inode *ip, char *buf, int size)
{
  struct ramfs_mount *m = mount_of(ip);
  struct ramfs_node *n;
  int used = 0, r = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0)
    for (struct ramfs_xattr *x = n->xattrs; x; x = x->next) {
      int len = strlen(x->name);
      if (size > 0) {
        if (used + len + 1 > size) {
          r = -ERANGE;
          break;
        }
        memmove(buf + used, x->name, len + 1);
      }
      used += len + 1;
    }
  releasesleep(&m->lock);
  return r < 0 ? r : used;
}

static const struct inode_ops ramfs_iops = {
    .load = ramfs_load,
    .update = ramfs_update,
    .read = ramfs_read,
    .write = ramfs_write,
    .trunc = ramfs_trunc,
    .setsize = ramfs_setsize,
    .fsync = ramfs_fsync,
    .lookup = ramfs_lookup,
    .readdir = ramfs_readdir,
    .create = ramfs_create,
    .link = ramfs_link,
    .unlink = ramfs_unlink,
    .symlink = ramfs_symlink,
    .readlink = ramfs_readlink,
    .getxattr = ramfs_getxattr,
    .setxattr = ramfs_setxattr,
    .listxattr = ramfs_listxattr,
};

// ---- the mount ----

static struct inode *
ramfs_sb_iget(struct super *sb, uint inum)
{
  struct ramfs_mount *m = sb->priv;

  return m ? ramfs_iget(m, inum) : 0;
}

static void
ramfs_sb_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
                uint64 *bsize, uint64 *namelen)
{
  struct ramfs_mount *m = sb->priv;
  uint64 nodes = 0;

  *bsize = PGSIZE;
  if (namelen)
    *namelen = NAME_MAX;
  if (m == 0) {
    *blocks = *bfree = *files = *ffree = 0;
    return;
  }
  acquiresleep(&m->lock);
  for (struct ramfs_node *n = m->nodes; n; n = n->next)
    nodes++;
  // a file system with no limit reports what it holds and says that
  // what is free is what the machine still has
  *blocks = m->pages_max ? limit(m) : m->pages_used + kmem_free_pages();
  *bfree = *blocks - m->pages_used;
  *files = nodes + *bfree;
  *ffree = *bfree;
  releasesleep(&m->lock);
}

static void
ramfs_sb_ifree(struct super *sb, uint inum, int isdir)
{
  struct ramfs_mount *m = sb->priv;
  struct ramfs_node *n, *prev = 0;

  (void)isdir;
  if (m == 0)
    return;
  acquiresleep(&m->lock);
  for (n = m->nodes; n; prev = n, n = n->next)
    if (n->inum == inum) {
      if (prev)
        prev->next = n->next;
      else
        m->nodes = n->next;
      node_free(m, n);
      break;
    }
  releasesleep(&m->lock);
}

static void
ramfs_sb_unmount(struct super *sb)
{
  struct ramfs_mount *m = sb->priv;
  struct ramfs_node *n, *next;

  if (m == 0)
    return;
  acquiresleep(&m->lock);
  for (n = m->nodes; n; n = next) {
    next = n->next;
    node_free(m, n);
  }
  m->nodes = 0;
  m->inuse = 0;
  releasesleep(&m->lock);
  supers[m->dev - RAMFS_DEV_BASE] = 0;
  sb->priv = 0;
}

static const struct super_ops ramfs_sops = {
    .iget = ramfs_sb_iget,
    .statfs = ramfs_sb_statfs,
    .unmount = ramfs_sb_unmount,
    .ifree = ramfs_sb_ifree,
};

// size=N, with the suffixes every tool writes. zero, or no option at
// all, means no limit, which is what "ramfs" always is.
static uint64
parse_size(const char *opts)
{
  const char *p = opts;

  if (opts == 0)
    return 0;
  while (*p) {
    if (strncmp(p, "size=", 5) == 0) {
      uint64 v = 0;
      p += 5;
      while (*p >= '0' && *p <= '9')
        v = v * 10 + (uint64)(*p++ - '0');
      if (*p == 'k' || *p == 'K')
        v *= 1024;
      else if (*p == 'm' || *p == 'M')
        v *= 1024 * 1024;
      else if (*p == 'g' || *p == 'G')
        v *= 1024UL * 1024 * 1024;
      return v;
    }
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
  }
  return 0;
}

static struct super *
ramfs_mount_common(const char *source, const char *opts, int limited)
{
  struct ramfs_mount *m = 0;
  struct super *sb;
  int i;

  for (i = 0; i < RAMFS_MAX_MOUNTS; i++)
    if (!mounts[i].inuse) {
      m = &mounts[i];
      break;
    }
  if (m == 0)
    return 0;

  memset(m, 0, sizeof(*m));
  initsleeplock(&m->lock, "ramfs");
  m->dev = RAMFS_DEV_BASE + i;
  m->next_ino = RAMFS_ROOT_INO;
  m->inuse = 1;
  // tmpfs has a limit and ramfs does not; without size= the limit is
  // half of what the machine has, which is what Linux does
  if (limited) {
    uint64 want = parse_size(opts);
    if (want == 0) {
      want = (kmem_total_pages() * PGSIZE) / 2;
      m->half = 1;
    }
    m->pages_max = want / PGSIZE;
    if (m->pages_max == 0)
      m->pages_max = 1;
  }

  if ((sb = vfs_super_alloc()) == 0) {
    m->inuse = 0;
    return 0;
  }
  sb->ops = &ramfs_sops;
  sb->iops = &ramfs_iops;
  sb->priv = m;
  supers[i] = sb;

  // the root is inode 1, and it is its own parent
  acquiresleep(&m->lock);
  struct ramfs_node *root = node_alloc(m, T_DIR);
  if (root == 0 || dirent_add(root, ".", root->inum, 2) < 0 ||
      dirent_add(root, "..", root->inum, 2) < 0) {
    releasesleep(&m->lock);
    supers[i] = 0;
    m->inuse = 0;
    vfs_super_free(sb);
    return 0;
  }
  root->nlink = 2;
  releasesleep(&m->lock);

  sb->root = ramfs_iget(m, RAMFS_ROOT_INO);
  if (sb->root == 0) {
    supers[i] = 0;
    m->inuse = 0;
    vfs_super_free(sb);
    return 0;
  }
  (void)source;
  return sb;
}

static struct super *
ramfs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)disk;
  (void)readonly;
  return ramfs_mount_common(source, opts, 0);
}

static struct super *
tmpfs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)disk;
  (void)readonly;
  return ramfs_mount_common(source, opts, 1);
}

static struct fs_type ramfs_type = {
    .name = "ramfs",
    .mount = ramfs_mount,
};

static struct fs_type tmpfs_type = {
    .name = "tmpfs",
    .mount = tmpfs_mount,
};

void
ramfs_init(void)
{
  vfs_register(&ramfs_type);
  vfs_register(&tmpfs_type);
}
