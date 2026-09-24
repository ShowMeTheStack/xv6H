//
// the virtual file system: the table of file system types, the table
// of mounts, and the small amount of dispatch that path walking needs.
//
// mounting is deliberately simple. a mount covers one directory: the
// inode it covers is remembered, and a path walk that arrives at that
// inode continues at the root of the file system mounted there. ".."
// out of a mount root goes back to the covered directory's parent.
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
#include "blk.h"
#include "loop.h"
#include "vfs.h"
#include "errno.h"
#include "posix.h"

static struct spinlock vfs_lock;
static struct fs_type *types;
static struct super supers[MAXMOUNT];
static struct super *root_super;

void
vfs_init(void)
{
  initlock(&vfs_lock, "vfs");
}

void
vfs_register(struct fs_type *type)
{
  acquire(&vfs_lock);
  type->next = types;
  types = type;
  release(&vfs_lock);
}

struct fs_type *
vfs_find_type(const char *name)
{
  acquire(&vfs_lock);
  for (struct fs_type *t = types; t; t = t->next)
    if (strncmp(t->name, name, sizeof(t->name)) == 0) {
      release(&vfs_lock);
      return t;
    }
  release(&vfs_lock);
  return 0;
}

struct super *
vfs_super_alloc(void)
{
  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    if (!supers[i].used) {
      memset(&supers[i], 0, sizeof(supers[i]));
      supers[i].used = 1;
      supers[i].loopdev = -1; // nothing to detach unless a mount says so
      release(&vfs_lock);
      return &supers[i];
    }
  }
  release(&vfs_lock);
  return 0;
}

void
vfs_super_free(struct super *sb)
{
  acquire(&vfs_lock);
  sb->used = 0;
  release(&vfs_lock);
}

struct super *
vfs_root_super(void)
{
  // what "/" is for the caller: its mount namespace's root if it has
  // pivoted into one, and otherwise the machine's.
  struct super *sb = mntns_rootsb(mntns_current());

  return sb ? sb : root_super;
}

// the machine's own root, whatever the caller is in
struct super *
vfs_machine_root(void)
{
  return root_super;
}

void
vfs_set_root_super(struct super *sb)
{
  root_super = sb;
  safestrcpy(sb->path, "/", sizeof(sb->path));
}

// the file system mounted over this directory, if there is one
struct super *
vfs_mounted_on(struct inode *ip)
{
  struct mntns *mns = mntns_current();

  if (ip == 0)
    return 0;
  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    // a mount the caller's namespace cannot see is not there for it: a
    // path that would cross it stays in the directory underneath
    if (!mntns_sees(mns, i))
      continue;
    if (sb->used && sb->covered && sb->covered->dev == ip->dev && sb->covered->inum == ip->inum) {
      release(&vfs_lock);
      return sb;
    }
  }
  release(&vfs_lock);
  return 0;
}

// which slot a mount is in, for the namespaces (which hold a set of
// slots rather than pointers).
int
vfs_slot_of(struct super *sb)
{
  if (sb == 0)
    return -1;
  int i = (int)(sb - supers);
  return (i >= 0 && i < MAXMOUNT) ? i : -1;
}

struct super *
vfs_slot(int i)
{
  if (i < 0 || i >= MAXMOUNT || !supers[i].used)
    return 0;
  return &supers[i];
}

// the mount whose root this inode is
struct super *
vfs_root_of(struct inode *ip)
{
  if (ip == 0)
    return 0;
  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (sb->used && sb->root && sb->root->dev == ip->dev && sb->root->inum == ip->inum &&
        sb->covered) {
      release(&vfs_lock);
      return sb;
    }
  }
  release(&vfs_lock);
  return 0;
}

struct super *
vfs_find_mount(const char *path)
{
  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (sb->used && strncmp(sb->path, path, MOUNTPATH) == 0) {
      release(&vfs_lock);
      return sb;
    }
  }
  release(&vfs_lock);
  return 0;
}

int
vfs_readonly(struct inode *ip)
{
  return ip && ip->sb && ip->sb->readonly;
}

// one option out of a comma-separated list: is `name` there?
static int
opt_has(const char *opts, const char *name)
{
  int n = strlen(name);

  for (const char *p = opts; p && *p;) {
    if (strncmp(p, name, n) == 0 && (p[n] == 0 || p[n] == ',' || p[n] == '='))
      return 1;
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
  }
  return 0;
}

// the number after name=, or `dflt`
static uint64
opt_num(const char *opts, const char *name, uint64 dflt)
{
  int n = strlen(name);

  for (const char *p = opts; p && *p;) {
    if (strncmp(p, name, n) == 0 && p[n] == '=') {
      uint64 v = 0;
      p += n + 1;
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
  return dflt;
}

// the flags an option list asks for, on top of the ones the call gave
static int
opt_flags(const char *opts, int flags)
{
  if (opt_has(opts, "ro"))
    flags |= MS_RDONLY;
  if (opt_has(opts, "rw"))
    flags &= ~MS_RDONLY;
  if (opt_has(opts, "noexec"))
    flags |= MS_NOEXEC;
  if (opt_has(opts, "nosuid"))
    flags |= MS_NOSUID;
  if (opt_has(opts, "nodev"))
    flags |= MS_NODEV;
  if (opt_has(opts, "bind"))
    flags |= MS_BIND;
  if (opt_has(opts, "remount"))
    flags |= MS_REMOUNT;
  return flags;
}

// the directory a mount goes over: it has to be one, and free
static struct inode *
mount_point(const char *target, int *err)
{
  struct inode *cover;

  *err = 0;
  if (strncmp(target, "/", 2) == 0)
    return 0;
  if ((cover = namei((char *)target)) == 0) {
    printk("mount: no such directory: %s\n", target);
    *err = -ENOENT;
    return 0;
  }
  ilock(cover);
  if (cover->type != T_DIR) {
    iunlockput(cover);
    printk("mount: not a directory: %s\n", target);
    *err = -ENOTDIR;
    return 0;
  }
  iunlock(cover);
  if (vfs_mounted_on(cover)) {
    iput(cover);
    printk("mount: %s is already a mount point\n", target);
    *err = -EBUSY;
    return 0;
  }
  return cover;
}

// mount `source` (a device name, or a name of its own for a file
// system with no disk) of type `type` on the directory `target`.
struct super *
vfs_mount(const char *source, const char *target, const char *type, int flags, const char *opts)
{
  // a container that has gone leaves its mounts behind and nothing
  // can see them any more: their slots come back here, before one is
  // wanted.
  vfs_reclaim_unseen();

  struct fs_type *t;
  struct gendisk *disk = 0;
  struct inode *cover;
  int err, loopdev = -1;
  int readonly;

  flags = opt_flags(opts, flags);
  readonly = (flags & MS_RDONLY) != 0;

  // remount: what is already there keeps its file system and changes
  // only how it may be used. this is how a root mounted read-only for
  // a check becomes writable.
  if (flags & MS_REMOUNT) {
    struct super *sb = vfs_find_mount(target);
    if (sb == 0) {
      printk("mount: nothing is mounted on %s\n", target);
      return 0;
    }
    sb->flags = flags & ~MS_REMOUNT;
    sb->readonly = readonly;
    if (sb->ops && sb->ops->remount && sb->ops->remount(sb, readonly, opts) < 0) {
      printk("mount: %s cannot be remounted %s\n", target, readonly ? "ro" : "rw");
      return 0;
    }
    printk("mount: %s remounted %s\n", sb->path, readonly ? "ro" : "rw");
    return sb;
  }

  // a bind mount is a second name for a directory, not a file system
  // at all: the mount's root is the directory itself, so everything
  // under it is the same inodes reached by another path.
  if (flags & MS_BIND) {
    struct inode *src = namei((char *)source);
    if (src == 0) {
      printk("mount: no such directory: %s\n", source);
      return 0;
    }
    ilock(src);
    int isdir = src->type == T_DIR;
    iunlock(src);
    if (!isdir) {
      iput(src);
      printk("mount: not a directory: %s\n", source);
      return 0;
    }
    if ((cover = mount_point(target, &err)) == 0 && err) {
      iput(src);
      return 0;
    }
    struct super *sb = vfs_super_alloc();
    if (sb == 0) {
      iput(src);
      if (cover)
        iput(cover);
      return 0;
    }
    sb->bind = 1;
    sb->loopdev = -1;
    sb->root = src;       // its own file system's inode, kept as it is
    sb->iops = src->ops;
    sb->covered = cover;
    sb->flags = flags;
    sb->readonly = readonly;
    safestrcpy(sb->type, "bind", sizeof(sb->type));
    mntns_add(mntns_current(), vfs_slot_of(sb));
    safestrcpy(sb->source, source, sizeof(sb->source));
    safestrcpy(sb->path, target, sizeof(sb->path));
    printk("mount: %s on %s type bind\n", sb->source, sb->path);
    return sb;
  }

  if ((t = vfs_find_type(type)) == 0) {
    printk("mount: no file system of type %s\n", type);
    return 0;
  }

  if (source && *source) {
    const char *name = source;
    // a path like /dev/vda1 names the disk by its last component
    for (const char *p = source; *p; p++)
      if (*p == '/')
        name = p + 1;
    disk = blk_find(name);

    // -o loop: the source is a file, and a loop device is attached to
    // it here rather than by hand. offset= starts the file system
    // somewhere other than the beginning, which is what mounting one
    // partition of an image without a partition table needs.
    if (disk == 0 && (opt_has(opts, "loop") || opt_has(opts, "offset"))) {
      struct inode *ip = namei((char *)source);
      if (ip == 0) {
        printk("mount: no such file: %s\n", source);
        return 0;
      }
      loopdev = loop_get_free();
      if (loopdev < 0 ||
          loop_set_fd(loopdev, ip, opt_num(opts, "offset", 0), opt_num(opts, "sizelimit", 0),
                      readonly) < 0) {
        iput(ip);
        printk("mount: no loop device for %s\n", source);
        return 0;
      }
      iput(ip);
      loop_set_name(loopdev, source);
      char lname[16];
      safestrcpy(lname, "loop0", sizeof(lname));
      lname[4] = '0' + (char)loopdev;
      disk = blk_find(lname);
    }
    if (disk == 0 && strncmp(type, "ext4", 5) == 0) {
      printk("mount: no such disk: %s\n", source);
      return 0;
    }
    // a disk a guest has been given is the guest's alone: mounting it
    // here would have two kernels writing the same blocks
    if (disk && disk->claimed) {
      printk("mount: %s is in use by a guest\n", source);
      if (loopdev >= 0)
        loop_clr_fd(loopdev);
      return 0;
    }
  }

  if ((cover = mount_point(target, &err)) == 0 && err) {
    if (loopdev >= 0)
      loop_clr_fd(loopdev);
    return 0;
  }

  struct super *sb = t->mount(disk, source, readonly, opts);
  if (sb == 0) {
    if (cover)
      iput(cover);
    if (loopdev >= 0)
      loop_clr_fd(loopdev);
    return 0;
  }
  sb->covered = cover;
  sb->loopdev = loopdev;
  safestrcpy(sb->type, type, sizeof(sb->type));
  safestrcpy(sb->source, source ? source : type, sizeof(sb->source));
  safestrcpy(sb->path, target, sizeof(sb->path));
  sb->readonly = readonly;
  sb->flags = flags;
  // the mount belongs to the namespace that made it, and to no other:
  // what a container mounts is not mounted on the machine.
  mntns_add(mntns_current(), vfs_slot_of(sb));
  printk("mount: %s on %s type %s%s\n", sb->source, sb->path, sb->type, readonly ? " (ro)" : "");
  return sb;
}

// the work of taking a mount down, with no lock held: it syncs, it puts
// inodes and it may detach a loop device, none of which can be done
// under vfs_lock.
static void
super_teardown(struct super *sb)
{
  if (sb->bind) {
    // nothing was mounted: the root is another file system's directory,
    // and all this holds is a reference to it
    if (sb->root) {
      iput(sb->root);
      sb->root = 0;
    }
  } else {
    if (sb->ops && sb->ops->sync)
      sb->ops->sync(sb);
    if (sb->ops && sb->ops->unmount)
      sb->ops->unmount(sb);
  }
  if (sb->covered) {
    iput(sb->covered);
    sb->covered = 0;
  }
  int loopdev = sb->loopdev;
  int slot = vfs_slot_of(sb);
  vfs_super_free(sb);
  if (slot >= 0)
    mntns_drop(mntns_root(), slot);
  // a loop device the mount attached is detached with it
  if (loopdev >= 0)
    loop_clr_fd(loopdev);
}

// a mount no live namespace can see is one a container made and then
// went away from: take it down, so that its slot comes back. Called
// where a slot is about to be wanted, which is a place that may sleep.
void
vfs_reclaim_unseen(void)
{
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = 0;
    acquire(&vfs_lock);
    if (supers[i].used && supers[i].covered && &supers[i] != root_super)
      sb = &supers[i];
    release(&vfs_lock);
    if (sb && !mntns_anyone_sees(i))
      super_teardown(sb);
  }
}

int
vfs_umount(const char *target)
{
  struct mntns *mns = mntns_current();

  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (!sb->used || strncmp(sb->path, target, MOUNTPATH) != 0)
      continue;
    // what the caller cannot see it cannot unmount
    if (!mntns_sees(mns, i))
      continue;
    if (sb == root_super || sb == mntns_rootsb(mns)) {
      release(&vfs_lock);
      return -EBUSY;
    }
    // in a namespace of its own, unmounting means "not here any more";
    // the mount itself stays until the namespace that made it is the
    // last to let go of it.
    if (mns != mntns_root()) {
      release(&vfs_lock);
      mntns_drop(mns, i);
      return 0;
    }
    release(&vfs_lock);
    super_teardown(sb);
    return 0;
  }
  release(&vfs_lock);
  return -EINVAL;
}

int
vfs_sync_all(void)
{
  int r = 0;

  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (sb->used && sb->ops && sb->ops->sync) {
      int e = sb->ops->sync(sb);
      if (e < 0)
        r = e;
    }
  }
  blk_sync_all();
  return r;
}

// mount the root: the first disk, which holds ext4. everything above
// reaches its files through the super block this leaves behind, and a
// disk that holds something else is a panic, because there is nothing
// for the system to run.
void
root_mount(void)
{
  struct gendisk *disk = blk_find("vda");
  struct super *sb;

  blk_start_flusher();

  // a handful of loop devices, so /dev/loop0 is there for losetup to
  // use rather than something it has to ask for first. Linux makes
  // eight; four is enough here and each costs a thread.
  for (int i = 0; i < 4; i++)
    loop_add(i);

  if (disk == 0)
    panic("root_mount: no root disk");
  if (!ext4_probe(disk))
    panic("root_mount: the root disk does not hold an ext4 file system");
  sb = vfs_mount("vda", "/", "ext4", 0, 0);
  if (sb == 0)
    panic("root_mount: the root file system cannot be mounted");
  vfs_set_root_super(sb);
}

void
vfs_list(void (*emit)(const char *source, const char *path, const char *type, int readonly,
                      void *arg),
         void *arg)
{
  struct mntns *mns = mntns_current();

  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (sb->used && mntns_sees(mns, i))
      emit(sb->source, sb->path, sb->type, sb->readonly, arg);
  }
}
