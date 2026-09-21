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
  // the chattr flags belong to the file that used to be here; a file
  // system that has no such thing never sets them, so leaving them
  // would make some later file immutable at random
  ip->attr = 0;
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
  st->mode = ip->mode;
  st->uid = ip->uid;
  st->gid = ip->gid;
  st->atime = ip->atime;
  st->mtime = ip->mtime;
  st->ctime = ip->ctime;
  st->blocks = (ip->size + 511) / 512;
  st->rdev = ip->type == T_DEVICE ? ((ip->major << 8) | (ip->minor & 0xff)) : 0;
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

// how many symbolic links one path may go through before the kernel
// decides they lead in a circle. Linux allows forty.
#define SYMLINK_MAX 16

// walk a path. `nofollow` says the last element is not followed if it
// turns out to be a symbolic link, which is what lstat and O_NOFOLLOW
// want; everything before the last is always followed, because a
// symbolic link in the middle of a path is a directory or nothing.
// the walk itself. `myroot` is what "/" means for this process, or
// zero for the file system's own root; it is borrowed, not owned, so
// that the recursion a symbolic link causes does not have to look it
// up again or give it back.
static struct inode *
namex_root(struct inode *start, char *path, int nameiparent, int nofollow, char *name,
           struct inode *myroot)
{
  struct inode *ip, *next;
  char buf[MAXPATH], target[MAXPATH];
  int links = 0;

  if (*path == '/') {
    if (myroot) {
      ip = idup(myroot);
    } else {
      struct super *rsb = vfs_root_super();
      if (rsb == 0 || rsb->root == 0)
        panic("namex: no root file system");
      ip = idup(rsb->root);
    }
    if (start)
      iput(start);
  } else if (start) {
    ip = start;
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

    // ".." at the process's own root stays there: a process that has
    // been put in a subtree cannot climb out of it by asking for its
    // parent, which is the whole point of chroot.
    if (namecmp(name, "..") == 0 && myroot && ip->dev == myroot->dev &&
        ip->inum == myroot->inum) {
      iunlock(ip);
      continue;
    }
    // ".." out of the root of a mounted file system is the parent of
    // the directory it covers, not the file system's own root looking
    // at itself.
    if (namecmp(name, "..") == 0) {
      struct super *msb = vfs_root_of(ip);
      if (msb && msb->covered) {
        struct inode *cov = idup(msb->covered);
        iunlockput(ip);
        ip = cov;
        ilock(ip);
      }
    }
    // every directory on the way has to be one this process may search
    if (inode_permitted(ip, 1) < 0) {
      iunlockput(ip);
      return 0;
    }
    if ((next = dirlookup(ip, name, 0)) == 0) {
      iunlockput(ip);
      return 0;
    }
    iunlock(ip); // still held: a relative link starts from here

    // a symbolic link stands for what it names. the last element is
    // left alone when the caller asked for it to be.
    ilock(next);
    int islink = next->type == T_SYMLINK;
    iunlock(next);
    if (islink && !(nofollow && *path == '\0')) {
      int n;

      if (++links > SYMLINK_MAX) {
        iput(next);
        iput(ip);
        return 0;
      }
      ilock(next);
      n = next->ops && next->ops->readlink ? next->ops->readlink(next, target, sizeof(target) - 1)
                                           : -1;
      iunlockput(next);
      if (n <= 0 || n >= (int)sizeof(target)) {
        iput(ip);
        return 0;
      }
      target[n] = 0;

      // the rest of the path is appended to what the link names, and
      // the walk starts again from there: from the root if the target
      // is absolute, from this directory if it is not.
      if (strlen(target) + strlen(path) + 2 > sizeof(buf)) {
        iput(ip);
        return 0;
      }
      safestrcpy(buf, target, sizeof(buf));
      if (*path) {
        int len = strlen(buf);
        buf[len++] = '/';
        safestrcpy(buf + len, path, sizeof(buf) - len);
      }
      // namex_at takes the reference to ip: it is the directory a
      // relative target is resolved from, and it releases it.
      return namex_root(ip, buf, nameiparent, nofollow, name, myroot);
    }

    iput(ip);
    ip = cross_mount(next);
  }
  if (nameiparent) {
    iput(ip);
    return 0;
  }
  return ip;
}

static struct inode *
namex_at(struct inode *start, char *path, int nameiparent, int nofollow, char *name)
{
  struct inode *myroot = proc_root(myproc());
  struct inode *ip = namex_root(start, path, nameiparent, nofollow, name, myroot);

  if (myroot)
    iput(myroot);
  return ip;
}

static struct inode *
namex(char *path, int nameiparent, char *name)
{
  return namex_at(0, path, nameiparent, 0, name);
}

// the same, but a symbolic link named by the last element is returned
// as itself rather than followed: lstat and O_NOFOLLOW.
struct inode *
namei_nofollow(char *path)
{
  char name[DIRSIZ];

  return namex_at(0, path, 0, 1, name);
}

// a path resolved from a directory that is not the current one: the
// *at family hands in the directory it was given.
struct inode *
namei_from(struct inode *dir, char *path, int nameiparent, int nofollow, char *name)
{
  return namex_at(dir, path, nameiparent, nofollow, name);
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

// set a file's length, when the file system it is on can do it
int
isetsize(struct inode *ip, uint64 len)
{
  if (ip->ops == 0 || ip->ops->setsize == 0)
    return -ENOTSUP;
  return ip->ops->setsize(ip, len);
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
