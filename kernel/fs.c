// the inode table and path resolution: what is left of the file system
// once the format is somebody else's business.
//
// an inode here is a handle, not a layout. it carries a device and an
// inode number, the fields every file system agrees on (type, size,
// link count), a pointer to the operations of the file system it came
// from, and whatever that file system hangs on fsdata. Everything that
// knows what a block looks like lives in a driver: kernel/ext4_*.c for
// ext4, kernel/procfs.c for /proc.
//
// what stays here is the part no format owns: the table that keeps one
// in-memory inode per (device, inode number) so that two processes see
// the same file, the sleep lock that serialises them, the walk from a
// path to an inode, and the crossing from a mount point into whatever
// is mounted on it.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "vfs.h"
#include "errno.h"


// Blocks.

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void
iinit()
{
  int i = 0;

  initlock(&itable.lock, "itable");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

struct inode *iget(uint dev, uint inum);


// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
struct inode *
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  // the slot may still hold what another file system attached to the
  // inode that used to live here. it has no references left, so this is
  // the last moment anything can hand it back.
  if (ip->fsdata && ip->ops && ip->ops->release)
    ip->ops->release(ip);
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  ip->ops = 0;
  ip->sb = 0;
  ip->fsdata = 0;
  release(&itable.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

void
ilock(struct inode *ip)
{
  if (ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  // the fields come from whichever file system the inode belongs to
  if (ip->valid == 0) {
    if (ip->ops == 0 || ip->ops->load == 0)
      panic("ilock: an inode with no file system");
    ip->ops->load(ip);
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  // Last reference of an unlinked inode?  Capture dev/inum before ref--,
  // since once ref hits 0, ip may be recycled by a concurrent iget()
  // for a different inum.
  int last = (ip->ref == 1 && ip->valid && ip->nlink == 0);
  uint inum = ip->inum;
  struct super *sb = ip->sb;
  int isdir = ip->type == T_DIR;

  if (last) {
    // ip->ref == 1 means no other process can have ip locked.
    acquiresleep(&ip->lock);
    release(&itable.lock);

    itrunc(ip); // free the data blocks (type stays nonzero on disk)
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);

  // after the reference is gone the slot may already belong to another
  // inode, so everything this needs was taken above.
  if (last && sb && sb->ops && sb->ops->ifree)
    sb->ops->ifree(sb, inum, isdir);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, NAME_MAX);
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ) {
    // longer than any name a file system here can hold. the old format
    // had a fixed 14-byte field and left such a name unterminated; a
    // name is a string now, so what fits is copied and terminated and
    // the lookup fails, rather than matching a shorter name by chance.
    memmove(name, s, DIRSIZ - 1);
    name[DIRSIZ - 1] = 0;
  } else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// a directory with a file system mounted on it is that file system's
// root instead, which is how a path crosses a mount point.
static struct inode *
cross_mount(struct inode *ip)
{
  for (int depth = 0; depth < 8; depth++) {
    struct super *sb = vfs_mounted_on(ip);
    if (sb == 0 || sb->root == 0)
      return ip;
    struct inode *root = idup(sb->root);
    iput(ip);
    ip = root;
  }
  return ip;
}

static struct inode *
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if (*path == '/') {
    struct super *rsb = vfs_root_super();
    if (rsb == 0 || rsb->root == 0)
      panic("namex: no root file system");
    ip = idup(rsb->root);
  } else {
    ip = proc_cwd(myproc());
  }
  ip = cross_mount(ip);

  while ((path = skipelem(path, name)) != 0) {
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return 0;
    }
    if (ip->nlink == 0) {
      iunlockput(ip);
      return 0;
    }
    if (nameiparent && *path == '\0') {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if ((next = dirlookup(ip, name, 0)) == 0) {
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = cross_mount(next);
  }
  if (nameiparent) {
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode *
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}


// ---- dispatch ----
//
// every inode carries the operations of the file system it belongs to,
// and the names the rest of the kernel calls are these, which forward.
// there is no default any more: an inode reaches the rest of the kernel
// through whichever file system produced it, or not at all.

int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  if (ip->ops == 0 || ip->ops->read == 0)
    return -EIO;
  return ip->ops->read(ip, user_dst, dst, off, n);
}

int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  if (ip->ops == 0 || ip->ops->write == 0)
    return -EROFS;
  return ip->ops->write(ip, user_src, src, off, n);
}

struct inode *
dirlookup(struct inode *dp, char *name, uint *poff)
{
  if (dp->ops == 0 || dp->ops->lookup == 0)
    return 0;
  return dp->ops->lookup(dp, name, poff);
}

void
itrunc(struct inode *ip)
{
  if (ip->ops && ip->ops->trunc)
    ip->ops->trunc(ip);
}

void
iupdate(struct inode *ip)
{
  if (ip->ops && ip->ops->update)
    ip->ops->update(ip);
}

// walk a directory, for getdents
int
idirread(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  if (dp->type != T_DIR)
    return -ENOTDIR;
  if (dp->ops == 0 || dp->ops->readdir == 0)
    return -ENOTSUP;
  return dp->ops->readdir(dp, pos, name, namemax, ino, type);
}
