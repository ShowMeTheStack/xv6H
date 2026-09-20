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
#include "blk.h"
#include "vfs.h"
#include "errno.h"

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
  if (ip == 0)
    return 0;
  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (sb->used && sb->covered && sb->covered->dev == ip->dev && sb->covered->inum == ip->inum) {
      release(&vfs_lock);
      return sb;
    }
  }
  release(&vfs_lock);
  return 0;
}

// mount `source` (a device name, or a name of its own for a file
// system with no disk) of type `type` on the directory `target`.
struct super *
vfs_mount(const char *source, const char *target, const char *type, int readonly, const char *opts)
{
  struct fs_type *t = vfs_find_type(type);

  if (t == 0) {
    printk("mount: no file system of type %s\n", type);
    return 0;
  }

  struct gendisk *disk = 0;
  if (source && *source) {
    const char *name = source;
    // a path like /dev/vda1 names the disk by its last component
    for (const char *p = source; *p; p++)
      if (*p == '/')
        name = p + 1;
    disk = blk_find(name);
    if (disk == 0 && strncmp(type, "ext4", 5) == 0) {
      printk("mount: no such disk: %s\n", source);
      return 0;
    }
  }

  // the directory it goes over must exist, and nothing may be mounted
  // there already
  struct inode *cover = 0;
  if (strncmp(target, "/", 2) != 0) {
    cover = namei((char *)target);
    if (cover == 0) {
      printk("mount: no such directory: %s\n", target);
      return 0;
    }
    ilock(cover);
    if (cover->type != T_DIR) {
      iunlockput(cover);
      printk("mount: not a directory: %s\n", target);
      return 0;
    }
    iunlock(cover);
    if (vfs_mounted_on(cover)) {
      iput(cover);
      printk("mount: %s is already a mount point\n", target);
      return 0;
    }
  }

  struct super *sb = t->mount(disk, source, readonly, opts);
  if (sb == 0) {
    if (cover)
      iput(cover);
    return 0;
  }
  sb->covered = cover;
  safestrcpy(sb->type, type, sizeof(sb->type));
  safestrcpy(sb->source, source ? source : type, sizeof(sb->source));
  safestrcpy(sb->path, target, sizeof(sb->path));
  sb->readonly = readonly;
  printk("mount: %s on %s type %s%s\n", sb->source, sb->path, sb->type, readonly ? " (ro)" : "");
  return sb;
}

int
vfs_umount(const char *target)
{
  acquire(&vfs_lock);
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (!sb->used || strncmp(sb->path, target, MOUNTPATH) != 0)
      continue;
    if (sb == root_super) {
      release(&vfs_lock);
      return -EBUSY;
    }
    release(&vfs_lock);
    if (sb->ops && sb->ops->sync)
      sb->ops->sync(sb);
    if (sb->ops && sb->ops->unmount)
      sb->ops->unmount(sb);
    if (sb->covered) {
      iput(sb->covered);
      sb->covered = 0;
    }
    vfs_super_free(sb);
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
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *sb = &supers[i];
    if (sb->used)
      emit(sb->source, sb->path, sb->type, sb->readonly, arg);
  }
}
