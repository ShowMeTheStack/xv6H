//
// ext4 as a file system the kernel can mount.
//
// the reading itself is in ext4_read.c, which knows the format; this
// file is the shape the rest of the kernel expects: a file system
// type, a super block, and inodes with operations. every mounted ext4
// gets a device number of its own so that the inode table, which is
// keyed by device and inode number, keeps its files apart.
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
#include "ext4.h"
#include "ext4_ondisk.h"
#include "slab.h"
#include "errno.h"

// the device numbers ext4 mounts use. the old format has 1; these
// start well above it so the two cannot collide.
#define EXT4_DEV_BASE 64

// how many metadata blocks one operation may dirty. a transaction is
// refused if it asks for more than the log can hold, so these are the
// worst case of each operation and not a guess at the common one: an
// inode table block, an inode bitmap, a block bitmap and a group
// descriptor per group touched, the superblock, a directory block or
// two and the extent blocks along one path.
#define EXT4_CREDITS_SMALL   8    // one inode written back
#define EXT4_CREDITS_CREATE  32   // a name, an inode, and the blocks behind them
#define EXT4_CREDITS_WRITE   128  // extents and bitmaps for one write
#define EXT4_CREDITS_BIG     128  // freeing a file: a bitmap per group it spans

// in data=journal every block of a write is on the log too, so one
// write's credits have to cover its data as well as its metadata.
// file.c hands the file system at most FILEWRITE_MAX bytes at a time,
// which is what makes that a number and not a hope.
_Static_assert(FILEWRITE_MAX / BLKSIZE + 32 <= EXT4_CREDITS_WRITE,
               "a write in data=journal must fit its transaction");

static struct kmem_cache *info_cache;

struct ext4_mount {
  struct ext4_fs *fs;
  uint dev;
};

static const struct inode_ops ext4_iops;

// the in-memory inode of an ext4 file lives in fsdata
static struct ext4_inode_info *
info_of(struct inode *ip)
{
  return ip->fsdata;
}

static struct ext4_mount *
mount_of(struct inode *ip)
{
  return ip->sb ? ip->sb->priv : 0;
}

// give the kernel's inode the fields it keeps of its own
static void
fill_inode(struct inode *ip, struct ext4_inode_info *info)
{
  uint32 fmt = info->mode & 0xf000;

  if (fmt == 0x4000)
    ip->type = T_DIR;
  else if (fmt == 0x2000 || fmt == 0x6000)
    ip->type = T_DEVICE;
  else
    ip->type = T_FILE;
  ip->nlink = info->nlink;
  ip->size = info->size;
  ip->major = 0;
  ip->minor = 0;
  if (fmt == 0x2000 || fmt == 0x6000) {
    int major = 0, minor = 0;
    ext4_device_of(info, &major, &minor);
    ip->major = (short)major;
    ip->minor = (short)minor;
  }
}

static void
ext4_vfs_load(struct inode *ip)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info;

  if (m == 0)
    panic("ext4_vfs_load: no mount");
  info = ip->fsdata;
  if (info == 0) {
    info = kmem_cache_alloc(info_cache);
    if (info == 0)
      panic("ext4_vfs_load: out of memory");
    ip->fsdata = info;
  }
  if (ext4_read_inode(m->fs, ip->inum, info) < 0) {
    // a broken inode is reported as an empty file rather than a panic:
    // the image may be someone else's and may be damaged.
    memset(info, 0, sizeof(*info));
    info->mode = 0x8000;
  }
  fill_inode(ip, info);
  ip->valid = 1;
}

static void
ext4_vfs_update(struct inode *ip)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info = info_of(ip);

  if (m == 0 || info == 0 || m->fs->readonly)
    return;
  if (ext4_trans_begin(m->fs, EXT4_CREDITS_SMALL) < 0)
    return;
  info->size = ip->size;
  info->nlink = ip->nlink;
  ext4_write_inode(m->fs, ip->inum, info);
  ext4_trans_end(m->fs);
}

static int
ext4_vfs_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info = info_of(ip);
  int done;

  if (m == 0 || info == 0)
    return -EIO;
  if (off > info->size)
    return 0;
  if (off + n > info->size)
    n = info->size - off;
  if (n == 0)
    return 0;

  // the whole read in one call, copied straight out of the cached
  // blocks. it used to go through a bounce buffer a block at a time,
  // which was an extra copy of every byte and a second loop to no
  // purpose.
  done = ext4_read_file(m->fs, info, off, user_dst, dst, n);
  if (done > 0)
    ext4_readahead(m->fs, info, off / m->fs->blocksize, (off + done - 1) / m->fs->blocksize);
  return done;
}

static int
ext4_vfs_write(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info = info_of(ip);
  int done;

  if (m == 0 || info == 0)
    return -EIO;
  if (m->fs->readonly)
    return -EROFS;
  // a write may start at the end of the file but not past it. ext4
  // itself has no trouble with a hole, but this is the rule the rest of
  // the kernel keeps -- writei has always refused an offset beyond the
  // size -- and a file system that kept a different one would make the
  // same program behave differently depending on where its file is.
  if (off > info->size || off + n < off)
    return -EINVAL;
  if (n == 0)
    return 0;

  // one transaction for the whole write. the data itself is not on the
  // log -- only the extents, the bitmaps and the inode are -- so what
  // the transaction bounds is how much metadata one write can change.
  int e = ext4_trans_begin(m->fs, EXT4_CREDITS_WRITE);
  if (e < 0)
    return e;

  // the whole write in one call: the file system sees how many blocks
  // are coming and can ask the allocator for them in one run, and the
  // bytes go straight from the caller into the cached blocks. a copy
  // that faults part way leaves what it managed to read in the file
  // and reports how much of the write actually happened.
  done = ext4_write_file(m->fs, info, ip->inum, off, user_src, src, n);
  ip->size = info->size;
  ext4_trans_end(m->fs);
  return done;
}

static void
ext4_vfs_trunc(struct inode *ip)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info = info_of(ip);

  if (m == 0 || info == 0 || m->fs->readonly)
    return;
  if (ext4_trans_begin(m->fs, EXT4_CREDITS_BIG) < 0)
    return;
  ext4_truncate(m->fs, info, ip->inum, 0);
  ext4_trans_end(m->fs);
  ip->size = 0;
}

// this file, on the disk. the data goes first and the journal second,
// so that the metadata naming those blocks only becomes durable after
// the blocks themselves do: a crash in between leaves the file as it
// was, not pointing at blocks that were never written. that is what
// data=ordered means, and fsync is where it matters most.
static int
ext4_vfs_fsync(struct inode *ip, int datasync)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info = info_of(ip);
  uint64 nblocks;
  int r = 0;

  (void)datasync; // the inode goes with the journal either way
  if (m == 0 || info == 0)
    return -EIO;
  if (m->fs->readonly)
    return 0;

  nblocks = (info->size + m->fs->blocksize - 1) / m->fs->blocksize;
  for (uint64 i = 0; i < nblocks; i++) {
    uint64 diskblk = 0;
    if (ext4_bmap(m->fs, info, i, &diskblk) < 0)
      break;
    if (diskblk == 0)
      continue; // a hole has nothing to write
    int e = blk_sync_block(m->fs->disk, diskblk);
    if (e < 0)
      r = e;
  }
  if (r < 0)
    return r;
  r = ext4_journal_commit(m->fs);
  if (r < 0)
    return r;
  return blk_flush(m->fs->disk);
}

// the inode of one name in a directory
static struct inode *
ext4_vfs_lookup(struct inode *dp, char *name, uint *poff)
{
  struct ext4_mount *m = mount_of(dp);
  struct ext4_inode_info *info = info_of(dp);
  uint64 ino = 0;
  uchar type = 0;

  if (poff)
    *poff = 0;
  if (m == 0 || info == 0)
    return 0;
  if (ext4_lookup(m->fs, info, name, strlen(name), &ino, &type) < 0)
    return 0;
  struct inode *ip = iget(m->dev, (uint)ino);
  if (ip) {
    ip->sb = dp->sb;
    ip->ops = &ext4_iops;
  }
  return ip;
}

static int
ext4_vfs_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  struct ext4_mount *m = mount_of(dp);
  struct ext4_inode_info *info = info_of(dp);
  uint64 inum = 0;
  uchar t = 0;

  if (m == 0 || info == 0)
    return -EIO;
  int r = ext4_readdir(m->fs, info, pos, name, namemax, &inum, &t);
  if (r <= 0)
    return r;
  *ino = (uint)inum;
  *type = t;
  return 1;
}

static int
ext4_vfs_readlink(struct inode *ip, char *buf, int max)
{
  struct ext4_mount *m = mount_of(ip);
  struct ext4_inode_info *info = info_of(ip);

  if (m == 0 || info == 0)
    return -EIO;
  return ext4_readlink(m->fs, info, buf, max);
}

static void
ext4_vfs_release(struct inode *ip)
{
  if (ip->fsdata) {
    kmem_cache_free(info_cache, ip->fsdata);
    ip->fsdata = 0;
  }
}

// the file type byte a directory entry carries
static uchar
ftype_of(short type)
{
  switch (type) {
  case T_DIR:
    return EXT4_FT_DIR;
  case T_DEVICE:
    return EXT4_FT_CHRDEV;
  default:
    return EXT4_FT_REG_FILE;
  }
}

static struct inode *
ext4_vfs_create(struct inode *dp, char *name, short type, short major, short minor)
{
  struct ext4_mount *m = mount_of(dp);
  struct ext4_inode_info *dirinfo = info_of(dp);
  struct ext4_inode_info newinfo;
  uint64 ino;

  if (m == 0 || dirinfo == 0)
    return 0;
  if (m->fs->readonly)
    return 0;

  uint16 mode = (type == T_DIR) ? 0x41ed : (type == T_DEVICE ? 0x21b6 : 0x81a4);
  if (ext4_trans_begin(m->fs, EXT4_CREDITS_CREATE) < 0)
    return 0;
  if (ext4_make_inode(m->fs, dp->inum, mode, &ino, &newinfo) < 0) {
    ext4_trans_end(m->fs);
    return 0;
  }
  if (type == T_DIR && ext4_make_dir(m->fs, dirinfo, dp->inum, ino, &newinfo) < 0) {
    ext4_free_inode(m->fs, ino, 1);
    ext4_trans_end(m->fs);
    return 0;
  }
  if (type == T_DEVICE)
    ext4_set_device(m->fs, &newinfo, ino, major, minor);
  int added = ext4_add_entry(m->fs, dirinfo, dp->inum, name, strlen(name), ino, ftype_of(type));

  // the directory may have grown a block, and the kernel's copy of its
  // inode has to learn that whether the name went in or not: iupdate
  // writes ip->size back, and a stale one there would shorten the
  // directory again and lose every entry in the blocks it added. an
  // add that fails part way still grew it.
  dp->size = dirinfo->size;
  dp->nlink = dirinfo->nlink;

  if (added < 0) {
    ext4_free_inode(m->fs, ino, type == T_DIR);
    ext4_trans_end(m->fs);
    return 0;
  }
  // the transaction ends before the new inode is locked: the file
  // system's lock is always the innermost one, so that nothing holding
  // an inode can be waiting for it.
  ext4_trans_end(m->fs);

  struct inode *ip = iget(m->dev, (uint)ino);
  if (ip == 0)
    return 0;
  ip->sb = dp->sb;
  ip->ops = &ext4_iops;
  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  return ip;
}

static int
ext4_vfs_unlink(struct inode *dp, char *name)
{
  struct ext4_mount *m = mount_of(dp);
  struct ext4_inode_info *dirinfo = info_of(dp);
  uint64 ino = 0;

  if (m == 0 || dirinfo == 0)
    return -EIO;
  if (m->fs->readonly)
    return -EROFS;

  // what is being removed, so that its links can be counted down
  uchar type = 0;
  if (ext4_lookup(m->fs, dirinfo, name, strlen(name), &ino, &type) < 0)
    return -ENOENT;

  struct ext4_inode_info victim;
  if (ext4_read_inode(m->fs, ino, &victim) < 0)
    return -EIO;
  int is_dir = (victim.mode & 0xf000) == 0x4000;
  if (is_dir) {
    int empty = ext4_dir_empty(m->fs, &victim);
    if (empty < 0)
      return empty;
    if (!empty)
      return -ENOTEMPTY;
  }

  // everything above only read; from here on the file system changes,
  // and all of it is one transaction: after a crash the entry is gone
  // and the inode is free, or neither is.
  int e = ext4_trans_begin(m->fs, EXT4_CREDITS_BIG);
  if (e < 0)
    return e;

  int r = ext4_remove_entry(m->fs, dirinfo, dp->inum, name, strlen(name), &ino);
  if (r < 0) {
    ext4_trans_end(m->fs);
    return r;
  }

  if (is_dir) {
    // a directory loses both its own entry and the "." inside it
    victim.nlink = 0;
    if (dp->nlink > 1) {
      dp->nlink--;
      dirinfo->nlink = dp->nlink;
      ext4_write_inode(m->fs, dp->inum, dirinfo);
    }
  } else if (victim.nlink > 0) {
    victim.nlink--;
  }
  ext4_write_inode(m->fs, ino, &victim);
  dp->size = dirinfo->size;
  dp->nlink = dirinfo->nlink;
  ext4_trans_end(m->fs);

  // the blocks and the inode itself are not freed here. a name is only
  // a name: a program that still has the file open must go on reading
  // and writing it, and unlinking it must not pull the disk out from
  // under it. iput frees an inode that has no links and no references
  // left, and this is what tells it the links are gone.
  if (victim.nlink == 0) {
    struct inode *ip = iget(m->dev, (uint)ino);
    if (ip) {
      ip->sb = dp->sb;
      ip->ops = &ext4_iops;
      ilock(ip);
      ip->nlink = 0;
      iunlockput(ip);
    }
  }
  return 0;
}

static int
ext4_vfs_link(struct inode *dp, char *name, struct inode *ip)
{
  struct ext4_mount *m = mount_of(dp);
  struct ext4_inode_info *dirinfo = info_of(dp);
  struct ext4_inode_info *target = info_of(ip);

  if (m == 0 || dirinfo == 0 || target == 0)
    return -EIO;
  if (m->fs->readonly)
    return -EROFS;
  if (ip->type == T_DIR)
    return -EPERM;
  // only the name: the caller has already raised the link count and
  // written the inode, and undoes both if this fails.
  int e = ext4_trans_begin(m->fs, EXT4_CREDITS_CREATE);
  if (e < 0)
    return e;
  target->nlink = ip->nlink;
  int r = ext4_add_entry(m->fs, dirinfo, dp->inum, name, strlen(name), ip->inum,
                         ftype_of(ip->type));
  dp->size = dirinfo->size; // it may have grown, whether or not this worked
  ext4_trans_end(m->fs);
  return r;
}

static const struct inode_ops ext4_iops = {
    .load = ext4_vfs_load,
    .update = ext4_vfs_update,
    .release = ext4_vfs_release,
    .read = ext4_vfs_read,
    .write = ext4_vfs_write,
    .trunc = ext4_vfs_trunc,
    .fsync = ext4_vfs_fsync,
    .lookup = ext4_vfs_lookup,
    .readdir = ext4_vfs_readdir,
    .readlink = ext4_vfs_readlink,
    .create = ext4_vfs_create,
    .link = ext4_vfs_link,
    .unlink = ext4_vfs_unlink,
};

// ---- the super block ----

static struct inode *
ext4_sb_iget(struct super *sb, uint inum)
{
  struct ext4_mount *m = sb->priv;
  struct inode *ip = iget(m->dev, inum);

  if (ip) {
    ip->sb = sb;
    ip->ops = &ext4_iops;
  }
  return ip;
}

static void
ext4_sb_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
               uint64 *bsize, uint64 *namelen)
{
  struct ext4_mount *m = sb->priv;

  ext4_statfs(m->fs, blocks, bfree, files, ffree, bsize);
  if (namelen)
    *namelen = 255;
}

static int
ext4_sb_sync(struct super *sb)
{
  struct ext4_mount *m = sb->priv;
  int r;

  // the journal first: it writes its blocks to their home locations
  // itself, and blk_sync would otherwise find nothing of them to write.
  r = ext4_journal_commit(m->fs);
  if (r < 0)
    return r;
  return blk_sync(sb->disk);
}

static void
ext4_sb_unmount(struct super *sb)
{
  struct ext4_mount *m = sb->priv;

  if (sb->root) {
    iput(sb->root);
    sb->root = 0;
  }
  if (m) {
    ext4_journal_close(m->fs); // commits what is left and empties the log
    ext4_unmount(m->fs);
    blk_forget(sb->disk);
    kfree(m);
  }
}

// the last link and the last reference are both gone: the inode goes
// back to the free list, and with it whatever the truncate that ran
// just before this one left behind.
static void
ext4_sb_ifree(struct super *sb, uint inum, int isdir)
{
  struct ext4_mount *m = sb->priv;

  if (m == 0 || m->fs->readonly)
    return;
  if (ext4_trans_begin(m->fs, EXT4_CREDITS_SMALL) < 0)
    return;
  ext4_free_inode(m->fs, inum, isdir);
  ext4_trans_end(m->fs);
}

static const struct super_ops ext4_sops = {
    .iget = ext4_sb_iget,
    .statfs = ext4_sb_statfs,
    .sync = ext4_sb_sync,
    .unmount = ext4_sb_unmount,
    .ifree = ext4_sb_ifree,
};

// the mount options this file system understands. only one so far:
//
//   data=ordered     the data is on the disk before the metadata that
//                    names it becomes durable. the default.
//   data=writeback   the data is written when the writeback thread
//                    gets to it, in no particular order. fastest, and
//                    a crash can leave a file pointing at blocks it
//                    never wrote.
//   data=journal     the data goes on the log as well, so a write is
//                    atomic against a crash. everything is written
//                    twice.
static int
ext4_data_mode(const char *opts)
{
  if (opts == 0)
    return EXT4_DATA_ORDERED;
  for (const char *p = opts; *p; p++) {
    if (p != opts && p[-1] != ',')
      continue;
    if (strncmp(p, "data=writeback", 14) == 0)
      return EXT4_DATA_WRITEBACK;
    if (strncmp(p, "data=journal", 12) == 0)
      return EXT4_DATA_JOURNAL;
    if (strncmp(p, "data=ordered", 12) == 0)
      return EXT4_DATA_ORDERED;
  }
  return EXT4_DATA_ORDERED;
}

static const char *
data_mode_name(int mode)
{
  switch (mode) {
  case EXT4_DATA_WRITEBACK: return "writeback";
  case EXT4_DATA_JOURNAL:   return "journal";
  default:                  return "ordered";
  }
}

static struct super *
ext4_fs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)source;

  if (disk == 0)
    return 0;
  if (info_cache == 0) {
    info_cache = kmem_cache_create("ext4inode", sizeof(struct ext4_inode_info), 8, 0);
    if (info_cache == 0)
      return 0;
  }

  struct ext4_fs *fs = ext4_mount(disk, readonly);
  if (fs == 0)
    return 0;

  fs->data_mode = ext4_data_mode(opts);
  printk("ext4: %s: data=%s\n", disk->name, data_mode_name(fs->data_mode));

  // the journal comes before anything else reads the file system: what
  // is on the log is newer than what is at the home blocks, and a read
  // that came first would see the older of the two.
  if (ext4_journal_open(fs) < 0) {
    ext4_unmount(fs);
    return 0;
  }

  struct super *sb = vfs_super_alloc();
  if (sb == 0) {
    ext4_unmount(fs);
    return 0;
  }
  struct ext4_mount *m = kalloc();
  if (m == 0) {
    vfs_super_free(sb);
    ext4_unmount(fs);
    return 0;
  }
  m->fs = fs;
  // a device number of its own, so the inode table keeps the mounts apart
  m->dev = EXT4_DEV_BASE + (uint)(sb - (struct super *)0) % 64;
  static uint next_dev;
  m->dev = EXT4_DEV_BASE + (next_dev++ % 64);

  sb->ops = &ext4_sops;
  sb->iops = &ext4_iops;
  sb->disk = disk;
  sb->priv = m;
  sb->readonly = readonly;
  sb->root = ext4_sb_iget(sb, (uint)ext4_root_ino(fs));
  if (sb->root == 0) {
    kfree(m);
    vfs_super_free(sb);
    ext4_unmount(fs);
    return 0;
  }
  return sb;
}

static struct fs_type ext4_type = {
    .name = "ext4",
    .mount = ext4_fs_mount,
};

void
ext4_init(void)
{
  vfs_register(&ext4_type);
}
