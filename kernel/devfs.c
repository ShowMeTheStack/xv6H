//
// /dev: the devices the kernel has, as files.
//
// nothing here is stored either. the names are a fixed table of
// character devices and one entry per disk the block layer knows
// about, so a disk that appears -- a partition found by a scan, a loop
// device attached -- is in /dev the moment it exists, and one that
// goes away is gone. that is what Linux's devtmpfs is for, and it is
// why nothing has to remember to call mknod.
//
// the inode number says what the file is: a small fixed number for a
// character device, and the disk's index in the block layer's table
// plus a base for a block device.
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

#define DEVFS_DEV 33

#define DINO_ROOT       1
#define DINO_FIRST_CHAR 2
#define DINO_FIRST_BLK  64

struct chardev {
  const char *name;
  short major;
  short minor;
  uint mode;   // what may be done to it
  uint gid;    // and by which group, besides root
};

// the groups /etc/group gives these: a disk belongs to `disk` and a
// terminal to `tty`, so that an account can be allowed one without
// being allowed the other.
#define GID_TTY  5
#define GID_DISK 6

// the character devices, which are the same on every machine
static const struct chardev chardevs[] = {
    {"console", CONSOLE, 0, 0620, GID_TTY},
    {"tty", CONSOLE, 0, 0666, GID_TTY},
    {"null", DEV_NULL, 0, 0666, 0},
    {"zero", DEV_ZERO, 0, 0666, 0},
    {"full", DEV_FULL, 0, 0666, 0},
    {"random", DEV_RANDOM, 0, 0666, 0},
    {"urandom", DEV_RANDOM, 0, 0666, 0},
    {"loop-control", DEV_LOOPCTL, 0, 0660, GID_DISK},
    {"log", DEV_LOG, 0, 0666, 0},
    {"kmsg", DEV_KMSG, 0, 0640, 0},
    {"pktcap", DEV_PKTCAP, 0, 0600, 0}, // every frame: root alone
    {"audit", DEV_AUDIT, 0, 0600, 0},   // and the audit log: root alone
};
#define NCHARDEV ((int)(sizeof(chardevs) / sizeof(chardevs[0])))

static const struct inode_ops devfs_iops;
static struct super *dev_sb;

static struct inode *
devfs_iget(uint inum)
{
  struct inode *ip = iget(DEVFS_DEV, inum);

  if (ip) {
    ip->sb = dev_sb;
    ip->ops = &devfs_iops;
  }
  return ip;
}

static void
devfs_load(struct inode *ip)
{
  ip->nlink = 1;
  ip->size = 0;
  // nothing here is stored, so the mode is the one every file of
  // this file system has: readable by all, written by root alone.
  ip->mode = 0666; // the table below says what each one really is
  ip->uid = ip->gid = 0;
  ip->major = 0;
  ip->minor = 0;

  if (ip->inum == DINO_ROOT) {
    ip->type = T_DIR;
    ip->mode = 0755;
    ip->nlink = 2;
  } else if (ip->inum >= DINO_FIRST_BLK) {
    struct gendisk *d = blk_by_index((int)(ip->inum - DINO_FIRST_BLK));
    ip->type = T_DEVICE;
    ip->major = DEV_BLOCK;
    ip->minor = (short)(ip->inum - DINO_FIRST_BLK);
    // the size of a block device is what it is: every tool that asks
    // stat() how big a disk is expects to be told.
    ip->size = d ? d->nsectors * (uint64)d->sector_size : 0;
    // a disk is root's and the disk group's: an account in `disk` can
    // mount and image one without being able to do anything else
    ip->mode = 0660;
    ip->gid = GID_DISK;
  } else {
    int i = (int)(ip->inum - DINO_FIRST_CHAR);
    ip->type = T_DEVICE;
    if (i >= 0 && i < NCHARDEV) {
      ip->major = chardevs[i].major;
      ip->minor = chardevs[i].minor;
      ip->mode = chardevs[i].mode;
      ip->gid = chardevs[i].gid;
    }
  }
  ip->valid = 1;
}

static struct inode *
devfs_lookup(struct inode *dp, char *name, uint *poff)
{
  if (poff)
    *poff = 0;
  if (dp->inum != DINO_ROOT)
    return 0;
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    return devfs_iget(DINO_ROOT);

  for (int i = 0; i < NCHARDEV; i++)
    if (namecmp(name, (char *)chardevs[i].name) == 0)
      return devfs_iget(DINO_FIRST_CHAR + i);

  for (int i = 0; i < MAXDISK; i++) {
    struct gendisk *d = blk_by_index(i);
    if (d && strncmp(d->name, name, DISKNAME) == 0)
      return devfs_iget(DINO_FIRST_BLK + i);
  }
  return 0;
}

static int
devfs_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  uint64 i = *pos;

  if (dp->inum != DINO_ROOT)
    return 0;
  // "." and ".." first, then the character devices, then the disks
  if (i == 0) {
    safestrcpy(name, ".", namemax);
    *ino = DINO_ROOT;
    *type = T_DIR;
    *pos = 1;
    return 1;
  }
  if (i == 1) {
    safestrcpy(name, "..", namemax);
    *ino = DINO_ROOT;
    *type = T_DIR;
    *pos = 2;
    return 1;
  }
  i -= 2;
  if (i < NCHARDEV) {
    safestrcpy(name, chardevs[i].name, namemax);
    *ino = DINO_FIRST_CHAR + (uint)i;
    *type = T_DEVICE;
    *pos += 1;
    return 1;
  }
  for (uint64 k = i - NCHARDEV; k < MAXDISK; k++) {
    struct gendisk *d = blk_by_index((int)k);
    if (d == 0)
      continue;
    safestrcpy(name, d->name, namemax);
    *ino = DINO_FIRST_BLK + (uint)k;
    *type = T_DEVICE;
    *pos = 2 + NCHARDEV + k + 1;
    return 1;
  }
  return 0;
}

static void
devfs_update(struct inode *ip)
{
  (void)ip; // nothing here is stored, so nothing is written back
}

static int
devfs_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  (void)ip;
  (void)user_dst;
  (void)dst;
  (void)off;
  (void)n;
  return 0; // the directory itself; a device is read through its file
}

static const struct inode_ops devfs_iops = {
    .load = devfs_load,
    .update = devfs_update,
    .read = devfs_read,
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
};

static struct inode *
devfs_sb_iget(struct super *sb, uint inum)
{
  (void)sb;
  return devfs_iget(inum);
}

static void
devfs_sb_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
                uint64 *bsize, uint64 *namelen)
{
  (void)sb;
  *blocks = *bfree = *files = *ffree = 0;
  *bsize = BLKSIZE;
  if (namelen)
    *namelen = DISKNAME - 1;
}

static const struct super_ops devfs_sops = {
    .iget = devfs_sb_iget,
    .statfs = devfs_sb_statfs,
};

static struct super *
devfs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)disk;
  (void)source;
  (void)readonly;
  (void)opts;

  struct super *sb = vfs_super_alloc();
  if (sb == 0)
    return 0;
  sb->ops = &devfs_sops;
  sb->iops = &devfs_iops;
  if (dev_sb == 0)
    dev_sb = sb;        // the first mount owns the inodes (see above)
  sb->root = devfs_iget(DINO_ROOT);
  if (sb->root == 0) {
    vfs_super_free(sb);
    return 0;
  }
  return sb;
}

static struct fs_type devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
};

void
devfs_init(void)
{
  vfs_register(&devfs_type);
}
