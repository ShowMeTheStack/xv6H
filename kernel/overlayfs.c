//
// overlayfs: one directory tree laid over another.
//
//   mount -t overlay overlay -o lowerdir=/ro,upperdir=/rw,workdir=/w /m
//
// what is read comes from the upper tree if it is there and the lower
// tree if it is not; what is written always goes to the upper tree. a
// file that exists only below and is about to be changed is copied up
// first, whole, because a file system cannot hold half of one file in
// each layer. a name that is removed cannot be removed from a lower
// tree that may be read-only or shared, so a whiteout takes its place
// in the upper tree -- a device node with no device, which is what
// Linux uses -- and the lookup treats that as "not there".
//
// this is what a container image is: layers that are read-only and one
// writable layer over them, so that a hundred containers from one
// image share it and each has its own changes.
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

#define OVL_DEV_BASE 48
#define OVL_MAX_MOUNTS 4
#define OVL_ROOT_INO 1

// a directory in the upper tree that hides everything the lower tree
// has under the same name, which is what a removed-and-remade
// directory has to do
#define OVL_OPAQUE_XATTR "trusted.overlay.opaque"

struct ovl_node {
  struct ovl_node *next;
  struct ovl_node *parent;
  uint inum;
  short type;
  char name[DIRSIZ];    // within its parent, for copying up
  struct inode *upper;  // a reference, or zero
  struct inode *lower;  // a reference, or zero
};

struct ovl_mount {
  struct sleeplock lock;
  uint dev;
  struct inode *upper_root;
  struct inode *lower_root;
  struct inode *work_root;
  struct ovl_node *nodes;
  uint next_ino;
  int inuse;
};

static struct ovl_mount mounts[OVL_MAX_MOUNTS];
static struct super *supers[OVL_MAX_MOUNTS];
static const struct inode_ops ovl_iops;

static struct ovl_mount *
mount_of(struct inode *ip)
{
  int i = (int)ip->dev - OVL_DEV_BASE;

  if (i < 0 || i >= OVL_MAX_MOUNTS || !mounts[i].inuse)
    return 0;
  return &mounts[i];
}

static struct ovl_node *
node_find(struct ovl_mount *m, uint inum)
{
  for (struct ovl_node *n = m->nodes; n; n = n->next)
    if (n->inum == inum)
      return n;
  return 0;
}

static struct ovl_node *
node_child(struct ovl_mount *m, struct ovl_node *parent, const char *name)
{
  for (struct ovl_node *n = m->nodes; n; n = n->next)
    if (n->parent == parent && strncmp(n->name, name, DIRSIZ) == 0)
      return n;
  return 0;
}

// ---- the underlying trees ----

// a whiteout: a device node with no device behind it. a name that has
// one in the upper tree is not there, whatever the lower tree holds.
static int
is_whiteout(struct inode *ip)
{
  int r;

  if (ip == 0)
    return 0;
  ilock(ip);
  r = ip->type == T_DEVICE && ip->major == 0 && ip->minor == 0;
  iunlock(ip);
  return r;
}

static int
is_opaque(struct inode *dir)
{
  char buf[4];
  int r = 0;

  if (dir == 0)
    return 0;
  ilock(dir);
  if (dir->ops && dir->ops->getxattr)
    r = dir->ops->getxattr(dir, OVL_OPAQUE_XATTR, buf, sizeof(buf)) > 0;
  iunlock(dir);
  return r;
}

// a name in one of the underlying directories
static struct inode *
under_lookup(struct inode *dir, const char *name)
{
  struct inode *ip;

  if (dir == 0)
    return 0;
  ilock(dir);
  ip = dir->ops && dir->ops->lookup ? dir->ops->lookup(dir, (char *)name, 0) : 0;
  iunlock(dir);
  return ip;
}

static short
type_of(struct inode *ip)
{
  short t;

  ilock(ip);
  t = ip->type;
  iunlock(ip);
  return t;
}

// ---- copying up ----

// the upper directory this node's parent is, making it if it is not
// there yet. every copy-up walks to the root this way, so a file deep
// in the lower tree brings its directories with it.
static int copy_up(struct ovl_mount *m, struct ovl_node *n);

static int
copy_up_parent(struct ovl_mount *m, struct ovl_node *n)
{
  if (n->parent == 0)
    return 0; // the root's upper is the mount's own
  return copy_up(m, n->parent);
}

// one file's contents, from the lower layer to the upper one
static int
copy_contents(struct inode *from, struct inode *to)
{
  char *page = kalloc();
  uint off = 0;
  int r = 0;

  if (page == 0)
    return -ENOMEM;
  for (;;) {
    ilock(from);
    int n = readi(from, 0, (uint64)page, off, PGSIZE);
    iunlock(from);
    if (n <= 0) {
      r = n < 0 ? n : 0;
      break;
    }
    ilock(to);
    int w = writei(to, 0, (uint64)page, off, n);
    iunlock(to);
    if (w != n) {
      r = w < 0 ? w : -ENOSPC;
      break;
    }
    off += n;
  }
  kfree(page);
  return r;
}

// every attribute the lower file has, on the upper one
static void
copy_xattrs(struct inode *from, struct inode *to)
{
  char *names = kalloc();
  char *value = kalloc();

  if (names == 0 || value == 0)
    goto out;
  ilock(from);
  int n = from->ops && from->ops->listxattr ? from->ops->listxattr(from, names, PGSIZE) : 0;
  iunlock(from);
  if (n <= 0)
    goto out;
  for (int i = 0; i < n;) {
    const char *name = names + i;
    int len = strlen(name);
    ilock(from);
    int vn = from->ops->getxattr ? from->ops->getxattr(from, name, value, PGSIZE) : -1;
    iunlock(from);
    if (vn >= 0) {
      ilock(to);
      if (to->ops && to->ops->setxattr)
        to->ops->setxattr(to, name, value, vn, 0);
      iunlock(to);
    }
    i += len + 1;
  }
out:
  if (names)
    kfree(names);
  if (value)
    kfree(value);
}

// make this node's upper copy, if it does not have one
static int
copy_up(struct ovl_mount *m, struct ovl_node *n)
{
  struct inode *pdir, *ip;
  int r;

  if (n->upper)
    return 0;
  if (m->upper_root == 0)
    return -EROFS;
  if (n->lower == 0)
    return -ENOENT;
  if ((r = copy_up_parent(m, n)) < 0)
    return r;

  pdir = n->parent ? n->parent->upper : m->upper_root;
  if (pdir == 0)
    return -EIO;

  short type = type_of(n->lower);
  if (type == T_SYMLINK) {
    char target[MAXPATH];
    ilock(n->lower);
    int tn = n->lower->ops && n->lower->ops->readlink
                 ? n->lower->ops->readlink(n->lower, target, sizeof(target) - 1)
                 : -EINVAL;
    iunlock(n->lower);
    if (tn < 0)
      return tn;
    target[tn] = 0;
    ilock(pdir);
    r = pdir->ops && pdir->ops->symlink ? pdir->ops->symlink(pdir, n->name, target) : -EPERM;
    iunlock(pdir);
    if (r < 0)
      return r;
    n->upper = under_lookup(pdir, n->name);
    return n->upper ? 0 : -EIO;
  }

  ilock(pdir);
  ip = pdir->ops && pdir->ops->create ? pdir->ops->create(pdir, n->name, type, 0, 0) : 0;
  iunlock(pdir);
  if (ip == 0)
    return -EIO;
  iunlock(ip); // create hands it back locked; the copy below locks it again

  if (type == T_FILE && (r = copy_contents(n->lower, ip)) < 0) {
    iput(ip);
    return r;
  }
  copy_xattrs(n->lower, ip);
  n->upper = ip;
  return 0;
}

// ---- inodes ----

static struct inode *
ovl_iget(struct ovl_mount *m, uint inum)
{
  struct inode *ip = iget(m->dev, inum);
  int i = (int)m->dev - OVL_DEV_BASE;

  if (ip) {
    ip->sb = supers[i];
    ip->ops = &ovl_iops;
  }
  return ip;
}

// the node behind an overlay inode, with the layer that answers for it
static struct inode *
active(struct ovl_node *n)
{
  return n->upper ? n->upper : n->lower;
}

static void
ovl_load(struct inode *ip)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *n;

  ip->type = 0;
  ip->nlink = 1;
  ip->size = 0;
  ip->major = ip->minor = 0;
  if (m == 0)
    return;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) != 0) {
    struct inode *a = active(n);
    if (a) {
      ilock(a);
      ip->type = a->type;
      ip->nlink = a->nlink;
      ip->size = a->size;
      ip->mode = a->mode;
      ip->uid = a->uid;
      ip->gid = a->gid;
      ip->atime = a->atime;
      ip->mtime = a->mtime;
      ip->ctime = a->ctime;
      ip->major = a->major;
      ip->minor = a->minor;
      iunlock(a);
    }
  }
  releasesleep(&m->lock);
  ip->valid = 1;
}

static void
ovl_update(struct inode *ip)
{
  (void)ip; // what is stored is stored by the layer underneath
}

static int
ovl_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *node;
  struct inode *a;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  node = node_find(m, ip->inum);
  a = node ? active(node) : 0;
  releasesleep(&m->lock);
  if (a == 0)
    return -EIO;
  ilock(a);
  r = readi(a, user_dst, dst, off, n);
  iunlock(a);
  return r;
}

static int
ovl_write(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *node;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((node = node_find(m, ip->inum)) == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  // a file that lives only below is copied up whole before the first
  // byte of it is changed
  if ((r = copy_up(m, node)) < 0) {
    releasesleep(&m->lock);
    return r;
  }
  struct inode *u = node->upper;
  releasesleep(&m->lock);

  ilock(u);
  r = writei(u, user_src, src, off, n);
  if (r > 0)
    ip->size = u->size;
  iunlock(u);
  return r;
}

static void
ovl_trunc(struct inode *ip)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *node;

  if (m == 0)
    return;
  acquiresleep(&m->lock);
  if ((node = node_find(m, ip->inum)) == 0 || copy_up(m, node) < 0) {
    releasesleep(&m->lock);
    return;
  }
  struct inode *u = node->upper;
  releasesleep(&m->lock);
  ilock(u);
  itrunc(u);
  iunlock(u);
  ip->size = 0;
}

static int
ovl_fsync(struct inode *ip, int datasync)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *node;
  struct inode *u;
  int r = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  node = node_find(m, ip->inum);
  u = node ? node->upper : 0;
  releasesleep(&m->lock);
  if (u) {
    ilock(u);
    if (u->ops && u->ops->fsync)
      r = u->ops->fsync(u, datasync);
    iunlock(u);
  }
  return r;
}

// ---- names ----

// the node for `name` inside `dir`, made if this is the first time it
// has been asked for, so that a name keeps its inode number.
static struct ovl_node *
lookup_locked(struct ovl_mount *m, struct ovl_node *dir, const char *name)
{
  struct inode *du = dir->upper;
  struct inode *dl = dir->lower;
  struct inode *up = 0, *lo = 0;
  struct ovl_node *n;

  if ((n = node_child(m, dir, name)) != 0)
    return n;

  up = under_lookup(du, name);
  if (up && is_whiteout(up)) {
    // the name was removed here; what is below does not show through
    iput(up);
    return 0;
  }
  if (!(du && is_opaque(du)))
    lo = under_lookup(dl, name);
  if (up == 0 && lo == 0)
    return 0;
  // a directory below and a file above: the file wins and the
  // directory is not merged into it
  if (up && lo && type_of(up) != T_DIR) {
    iput(lo);
    lo = 0;
  }

  if ((n = (struct ovl_node *)kalloc()) == 0) {
    if (up)
      iput(up);
    if (lo)
      iput(lo);
    return 0;
  }
  memset(n, 0, sizeof(*n));
  n->inum = m->next_ino++;
  n->parent = dir;
  safestrcpy(n->name, name, DIRSIZ);
  n->upper = up;
  n->lower = lo;
  n->type = type_of(active(n));
  n->next = m->nodes;
  m->nodes = n;
  return n;
}

static struct inode *
ovl_lookup(struct inode *dp, char *name, uint *poff)
{
  struct ovl_mount *m = mount_of(dp);
  struct ovl_node *dir, *n;
  struct inode *ip = 0;

  if (poff)
    *poff = 0;
  if (m == 0)
    return 0;
  acquiresleep(&m->lock);
  dir = node_find(m, dp->inum);
  if (dir == 0) {
    releasesleep(&m->lock);
    return 0;
  }
  if (namecmp(name, ".") == 0) {
    releasesleep(&m->lock);
    return ovl_iget(m, dp->inum);
  }
  if (namecmp(name, "..") == 0) {
    uint up = dir->parent ? dir->parent->inum : OVL_ROOT_INO;
    releasesleep(&m->lock);
    return ovl_iget(m, up);
  }
  if ((n = lookup_locked(m, dir, name)) != 0)
    ip = ovl_iget(m, n->inum);
  releasesleep(&m->lock);
  return ip;
}

// walking a merged directory: the upper layer first, then the names of
// the lower layer that the upper one does not already have and has not
// whited out. the position is the upper layer's while it lasts and the
// lower layer's after that, with a bit to say which.
#define OVL_POS_LOWER (1UL << 62)

static int
ovl_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  struct ovl_mount *m = mount_of(dp);
  struct ovl_node *dir, *n;
  struct inode *du, *dl;
  char buf[DIRSIZ];
  uint uino;
  uchar utype;
  int r = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  dir = node_find(m, dp->inum);
  if (dir == 0) {
    releasesleep(&m->lock);
    return 0;
  }
  du = dir->upper;
  dl = dir->lower;

  if (!(*pos & OVL_POS_LOWER) && du) {
    uint64 p = *pos;
    while (du) {
      ilock(du);
      int got = idirread(du, &p, buf, sizeof(buf), &uino, &utype);
      iunlock(du);
      if (got <= 0)
        break;
      // a whiteout is not a name this directory has
      struct inode *e = under_lookup(du, buf);
      int white = is_whiteout(e);
      if (e)
        iput(e);
      if (white)
        continue;
      if (namecmp(buf, ".") != 0 && namecmp(buf, "..") != 0) {
        n = lookup_locked(m, dir, buf);
        if (n == 0)
          continue;
        uino = n->inum;
      } else {
        uino = namecmp(buf, ".") == 0 ? dp->inum
                                      : (dir->parent ? dir->parent->inum : OVL_ROOT_INO);
      }
      safestrcpy(name, buf, namemax);
      *ino = uino;
      *type = utype;
      *pos = p;
      releasesleep(&m->lock);
      return 1;
    }
    *pos = OVL_POS_LOWER; // the upper layer is done; the lower one follows
  }

  if (dl && !(du && is_opaque(du))) {
    uint64 p = *pos & ~OVL_POS_LOWER;
    while (dl) {
      ilock(dl);
      int got = idirread(dl, &p, buf, sizeof(buf), &uino, &utype);
      iunlock(dl);
      if (got <= 0)
        break;
      if (namecmp(buf, ".") == 0 || namecmp(buf, "..") == 0)
        continue;
      // a name the upper layer already gave, or one it has whited out
      struct inode *e = under_lookup(du, buf);
      if (e) {
        iput(e);
        continue;
      }
      n = lookup_locked(m, dir, buf);
      if (n == 0)
        continue;
      safestrcpy(name, buf, namemax);
      *ino = n->inum;
      *type = utype;
      *pos = p | OVL_POS_LOWER;
      releasesleep(&m->lock);
      return 1;
    }
    *pos = p | OVL_POS_LOWER;
  }
  releasesleep(&m->lock);
  return r;
}

// the upper directory a new name goes into, made if it is not there
static int
upper_dir(struct ovl_mount *m, struct ovl_node *dir, struct inode **out)
{
  int r;

  if (m->upper_root == 0)
    return -EROFS;
  if ((r = copy_up(m, dir)) < 0)
    return r;
  *out = dir->upper;
  return 0;
}

static struct inode *
ovl_create(struct inode *dp, char *name, short type, short major, short minor)
{
  struct ovl_mount *m = mount_of(dp);
  struct ovl_node *dir, *n;
  struct inode *du, *ip;

  if (m == 0)
    return 0;
  acquiresleep(&m->lock);
  dir = node_find(m, dp->inum);
  if (dir == 0)
    goto fail;
  if (upper_dir(m, dir, &du) < 0)
    goto fail;

  // a whiteout in the way is removed first: the name is being made
  // again, and what is below it must stay hidden, so a directory made
  // over one below is marked opaque.
  struct inode *old = under_lookup(du, name);
  int had_white = 0;
  if (old && is_whiteout(old)) {
    had_white = 1;
    ilock(du);
    if (du->ops && du->ops->unlink)
      du->ops->unlink(du, name);
    iunlock(du);
  }
  if (old)
    iput(old);

  ilock(du);
  ip = du->ops && du->ops->create ? du->ops->create(du, name, type, major, minor) : 0;
  iunlock(du);
  if (ip == 0)
    goto fail;
  iunlock(ip);
  if (had_white && type == T_DIR) {
    ilock(ip);
    if (ip->ops && ip->ops->setxattr)
      ip->ops->setxattr(ip, OVL_OPAQUE_XATTR, "y", 1, 0);
    iunlock(ip);
  }

  // the node keeps the upper inode; anything the lookup had cached for
  // this name is replaced, because the name is a different file now.
  if ((n = node_child(m, dir, name)) != 0) {
    if (n->upper)
      iput(n->upper);
    n->upper = ip;
    if (had_white && n->lower) {
      iput(n->lower);
      n->lower = 0;
    }
    n->type = type;
  } else {
    if ((n = (struct ovl_node *)kalloc()) == 0) {
      iput(ip);
      goto fail;
    }
    memset(n, 0, sizeof(*n));
    n->inum = m->next_ino++;
    n->parent = dir;
    safestrcpy(n->name, name, DIRSIZ);
    n->upper = ip;
    n->type = type;
    n->next = m->nodes;
    m->nodes = n;
  }
  uint inum = n->inum;
  releasesleep(&m->lock);

  struct inode *ovl = ovl_iget(m, inum);
  if (ovl == 0)
    return 0;
  ilock(ovl);
  return ovl;

fail:
  releasesleep(&m->lock);
  return 0;
}

static int
ovl_link(struct inode *dp, char *name, struct inode *ip)
{
  struct ovl_mount *m = mount_of(dp);
  struct ovl_node *dir, *target;
  struct inode *du;
  int r;

  if (m == 0 || ip->dev != dp->dev)
    return -EXDEV;
  acquiresleep(&m->lock);
  dir = node_find(m, dp->inum);
  target = node_find(m, ip->inum);
  if (target == 0 || dir == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  // both ends have to be in the upper layer: a link is one inode with
  // two names, and the lower layer is not ours to change
  if ((r = copy_up(m, target)) < 0 || (r = upper_dir(m, dir, &du)) < 0) {
    releasesleep(&m->lock);
    return r;
  }
  struct inode *u = target->upper;
  releasesleep(&m->lock);

  ilock(du);
  r = du->ops && du->ops->link ? du->ops->link(du, name, u) : -EPERM;
  iunlock(du);
  return r;
}

// a name that is gone. if the lower layer still has it, a whiteout has
// to stand in the upper layer or the name would come back.
static int
ovl_unlink(struct inode *dp, char *name)
{
  struct ovl_mount *m = mount_of(dp);
  struct ovl_node *dir, *n;
  struct inode *du, *lo;
  int r = 0;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  dir = node_find(m, dp->inum);
  if (dir == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  if ((r = upper_dir(m, dir, &du)) < 0) {
    releasesleep(&m->lock);
    return r;
  }
  n = node_child(m, dir, name);
  lo = n ? n->lower : under_lookup(dir->lower, name);
  int lower_owned = n != 0; // a node's lower reference is the node's

  struct inode *up = n ? n->upper : under_lookup(du, name);
  if (up == 0 && lo == 0) {
    if (!lower_owned && lo)
      iput(lo);
    releasesleep(&m->lock);
    return -ENOENT;
  }

  if (up) {
    ilock(du);
    r = du->ops && du->ops->unlink ? du->ops->unlink(du, name) : -EPERM;
    iunlock(du);
    if (n) {
      iput(n->upper);
      n->upper = 0;
    } else {
      iput(up);
    }
    if (r < 0) {
      if (!lower_owned && lo)
        iput(lo);
      releasesleep(&m->lock);
      return r;
    }
  }

  if (lo) {
    // the lower layer still has the name, so the upper layer has to
    // say that it does not: a device node with no device behind it.
    ilock(du);
    struct inode *w =
        du->ops && du->ops->create ? du->ops->create(du, name, T_DEVICE, 0, 0) : 0;
    iunlock(du);
    if (w)
      iunlockput(w);
    else
      r = -EIO;
    if (n) {
      iput(n->lower);
      n->lower = 0;
    } else {
      iput(lo);
    }
  }

  // the node is empty now: drop it, so the name can be made again
  if (n && n->upper == 0 && n->lower == 0) {
    struct ovl_node **pp = &m->nodes;
    while (*pp && *pp != n)
      pp = &(*pp)->next;
    if (*pp)
      *pp = n->next;
    kfree(n);
  }
  releasesleep(&m->lock);
  return r;
}

static int
ovl_symlink(struct inode *dp, char *name, const char *target)
{
  struct ovl_mount *m = mount_of(dp);
  struct ovl_node *dir;
  struct inode *du;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  dir = node_find(m, dp->inum);
  if (dir == 0) {
    releasesleep(&m->lock);
    return -EIO;
  }
  if ((r = upper_dir(m, dir, &du)) < 0) {
    releasesleep(&m->lock);
    return r;
  }
  releasesleep(&m->lock);
  ilock(du);
  r = du->ops && du->ops->symlink ? du->ops->symlink(du, name, target) : -EPERM;
  iunlock(du);
  return r;
}

static int
ovl_readlink(struct inode *ip, char *buf, int max)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *n;
  struct inode *a;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  n = node_find(m, ip->inum);
  a = n ? active(n) : 0;
  releasesleep(&m->lock);
  if (a == 0)
    return -EIO;
  ilock(a);
  r = a->ops && a->ops->readlink ? a->ops->readlink(a, buf, max) : -EINVAL;
  iunlock(a);
  return r;
}

// ---- extended attributes ----

static int
ovl_getxattr(struct inode *ip, const char *name, char *buf, int size)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *n;
  struct inode *a;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  n = node_find(m, ip->inum);
  a = n ? active(n) : 0;
  releasesleep(&m->lock);
  if (a == 0)
    return -EIO;
  ilock(a);
  r = a->ops && a->ops->getxattr ? a->ops->getxattr(a, name, buf, size) : -EOPNOTSUPP;
  iunlock(a);
  return r;
}

static int
ovl_setxattr(struct inode *ip, const char *name, const char *value, int size, int flags)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *n;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  if ((n = node_find(m, ip->inum)) == 0 || (r = copy_up(m, n)) < 0) {
    releasesleep(&m->lock);
    return n ? r : -EIO;
  }
  struct inode *u = n->upper;
  releasesleep(&m->lock);
  ilock(u);
  r = u->ops && u->ops->setxattr ? u->ops->setxattr(u, name, value, size, flags) : -EOPNOTSUPP;
  iunlock(u);
  return r;
}

static int
ovl_listxattr(struct inode *ip, char *buf, int size)
{
  struct ovl_mount *m = mount_of(ip);
  struct ovl_node *n;
  struct inode *a;
  int r;

  if (m == 0)
    return -EIO;
  acquiresleep(&m->lock);
  n = node_find(m, ip->inum);
  a = n ? active(n) : 0;
  releasesleep(&m->lock);
  if (a == 0)
    return -EIO;
  ilock(a);
  r = a->ops && a->ops->listxattr ? a->ops->listxattr(a, buf, size) : -EOPNOTSUPP;
  iunlock(a);
  return r;
}

static const struct inode_ops ovl_iops = {
    .load = ovl_load,
    .update = ovl_update,
    .read = ovl_read,
    .write = ovl_write,
    .trunc = ovl_trunc,
    .fsync = ovl_fsync,
    .lookup = ovl_lookup,
    .readdir = ovl_readdir,
    .create = ovl_create,
    .link = ovl_link,
    .unlink = ovl_unlink,
    .symlink = ovl_symlink,
    .readlink = ovl_readlink,
    .getxattr = ovl_getxattr,
    .setxattr = ovl_setxattr,
    .listxattr = ovl_listxattr,
};

// ---- the mount ----

static struct inode *
ovl_sb_iget(struct super *sb, uint inum)
{
  struct ovl_mount *m = sb->priv;

  return m ? ovl_iget(m, inum) : 0;
}

static void
ovl_sb_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
              uint64 *bsize, uint64 *namelen)
{
  struct ovl_mount *m = sb->priv;
  struct super *usb = m && m->upper_root ? m->upper_root->sb : 0;

  // what is free is what the upper layer has: that is where a write
  // goes, whichever layer the file came from
  if (usb && usb->ops && usb->ops->statfs) {
    usb->ops->statfs(usb, blocks, bfree, files, ffree, bsize, namelen);
    return;
  }
  *blocks = *bfree = *files = *ffree = 0;
  *bsize = BLKSIZE;
  if (namelen)
    *namelen = NAME_MAX;
}

static int
ovl_sb_sync(struct super *sb)
{
  struct ovl_mount *m = sb->priv;
  struct super *usb = m && m->upper_root ? m->upper_root->sb : 0;

  return usb && usb->ops && usb->ops->sync ? usb->ops->sync(usb) : 0;
}

static void
ovl_sb_unmount(struct super *sb)
{
  struct ovl_mount *m = sb->priv;
  struct ovl_node *n, *next;

  if (m == 0)
    return;
  acquiresleep(&m->lock);
  for (n = m->nodes; n; n = next) {
    next = n->next;
    if (n->upper)
      iput(n->upper);
    if (n->lower)
      iput(n->lower);
    kfree(n);
  }
  m->nodes = 0;
  if (m->upper_root)
    iput(m->upper_root);
  if (m->lower_root)
    iput(m->lower_root);
  if (m->work_root)
    iput(m->work_root);
  m->upper_root = m->lower_root = m->work_root = 0;
  m->inuse = 0;
  releasesleep(&m->lock);
  supers[m->dev - OVL_DEV_BASE] = 0;
  sb->priv = 0;
}

static const struct super_ops ovl_sops = {
    .iget = ovl_sb_iget,
    .statfs = ovl_sb_statfs,
    .sync = ovl_sb_sync,
    .unmount = ovl_sb_unmount,
};

// name=value out of a comma-separated list
static int
opt_str(const char *opts, const char *name, char *out, int max)
{
  int n = strlen(name);

  for (const char *p = opts; p && *p;) {
    if (strncmp(p, name, n) == 0 && p[n] == '=') {
      int k = 0;
      p += n + 1;
      while (*p && *p != ',' && k < max - 1)
        out[k++] = *p++;
      out[k] = 0;
      return k;
    }
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
  }
  return 0;
}

// a directory named by an option, which has to be one
static struct inode *
opt_dir(const char *opts, const char *name)
{
  char path[MAXPATH];
  struct inode *ip;

  if (opt_str(opts, name, path, sizeof(path)) == 0)
    return 0;
  if ((ip = namei(path)) == 0) {
    printk("overlay: no such directory: %s\n", path);
    return 0;
  }
  ilock(ip);
  int isdir = ip->type == T_DIR;
  iunlock(ip);
  if (!isdir) {
    iput(ip);
    printk("overlay: not a directory: %s\n", path);
    return 0;
  }
  return ip;
}

static struct super *
ovl_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  struct ovl_mount *m = 0;
  struct super *sb;
  int i;

  (void)disk;
  (void)source;
  for (i = 0; i < OVL_MAX_MOUNTS; i++)
    if (!mounts[i].inuse) {
      m = &mounts[i];
      break;
    }
  if (m == 0) {
    printk("overlay: no free mount slot\n");
    return 0;
  }
  memset(m, 0, sizeof(*m));
  initsleeplock(&m->lock, "overlay");
  m->dev = OVL_DEV_BASE + i;
  m->next_ino = OVL_ROOT_INO + 1;

  m->lower_root = opt_dir(opts, "lowerdir");
  if (m->lower_root == 0) {
    printk("overlay: lowerdir= is required\n");
    return 0;
  }
  if (!readonly) {
    m->upper_root = opt_dir(opts, "upperdir");
    if (m->upper_root == 0) {
      iput(m->lower_root);
      printk("overlay: upperdir= is required unless the mount is read-only\n");
      return 0;
    }
    m->work_root = opt_dir(opts, "workdir"); // kept for Linux's sake; nothing needs it here
  }

  if ((sb = vfs_super_alloc()) == 0) {
    iput(m->lower_root);
    if (m->upper_root)
      iput(m->upper_root);
    if (m->work_root)
      iput(m->work_root);
    return 0;
  }
  // the root is a node like every other name here, so that nothing
  // below has to ask whether it is looking at the root
  struct ovl_node *root = (struct ovl_node *)kalloc();
  if (root == 0) {
    iput(m->lower_root);
    if (m->upper_root)
      iput(m->upper_root);
    if (m->work_root)
      iput(m->work_root);
    vfs_super_free(sb);
    return 0;
  }
  memset(root, 0, sizeof(*root));
  root->inum = OVL_ROOT_INO;
  root->type = T_DIR;
  safestrcpy(root->name, "/", DIRSIZ);
  root->upper = m->upper_root ? idup(m->upper_root) : 0;
  root->lower = idup(m->lower_root);
  m->nodes = root;

  m->inuse = 1;
  sb->ops = &ovl_sops;
  sb->iops = &ovl_iops;
  sb->priv = m;
  supers[i] = sb;
  sb->root = ovl_iget(m, OVL_ROOT_INO);
  if (sb->root == 0) {
    supers[i] = 0;
    m->inuse = 0;
    vfs_super_free(sb);
    return 0;
  }
  return sb;
}

static struct fs_type ovl_type = {
    .name = "overlay",
    .mount = ovl_mount,
};

void
overlayfs_init(void)
{
  vfs_register(&ovl_type);
}
