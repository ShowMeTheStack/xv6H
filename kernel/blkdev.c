//
// a block device as a file: what /dev/vda is when a program opens it.
//
// reads and writes go through the same cache the file systems use, so
// a program reading /dev/vda1 and a file system mounted on it see the
// same blocks, and `dd` writing a file system image onto a device is
// visible to a mount of that device without a flush in between. the
// offset is the file's own, which is what makes lseek work on a disk.
//
// the ioctls are Linux's, because every tool that asks a device how
// big it is asks the same way.
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
#include "blkdev.h"
#include "loop.h"
#include "errno.h"

uint64
blkdev_size(struct gendisk *d)
{
  return d ? d->nsectors * (uint64)d->sector_size : 0;
}

// one transfer, clipped to the device and done a block at a time. a
// read or write that is not aligned to a block still works: the part
// of the block it touches is copied and the rest is left alone, which
// is what a device file has always promised.
static int
blkdev_rw(struct gendisk *d, int write, int user, uint64 addr, uint64 off, uint64 n)
{
  uint64 size = blkdev_size(d);
  uint64 done = 0;

  if (d == 0)
    return -ENXIO;
  if (write && d->readonly)
    return -EROFS;
  if (off >= size)
    return write ? -ENOSPC : 0;
  if (n > size - off)
    n = size - off;

  while (done < n) {
    uint64 blkno = (off + done) / BLKSIZE;
    uint32 within = (uint32)((off + done) % BLKSIZE);
    uint32 chunk = BLKSIZE - within;
    struct blkbuf *b;
    int e;

    if (chunk > n - done)
      chunk = (uint32)(n - done);

    if (write && within == 0 && chunk == BLKSIZE)
      b = blk_get_fill(d, blkno); // every byte of it is about to be written
    else
      b = blk_get(d, blkno);
    if (b == 0)
      return done > 0 ? (int)done : -EIO;

    if (write) {
      e = either_copyin((uchar *)b->data + within, user, addr + done, chunk);
      blk_mark_dirty(b);
    } else {
      e = either_copyout(user, addr + done, (uchar *)b->data + within, chunk);
    }
    blk_put(b);
    if (e < 0)
      return done > 0 ? (int)done : -EFAULT;
    done += chunk;
  }
  return (int)done;
}

int
blkdev_read(struct gendisk *d, int user, uint64 addr, uint64 off, uint64 n)
{
  return blkdev_rw(d, 0, user, addr, off, n);
}

int
blkdev_write(struct gendisk *d, int user, uint64 addr, uint64 off, uint64 n)
{
  return blkdev_rw(d, 1, user, addr, off, n);
}

// the ioctls, by their Linux numbers. each one that hands a value back
// writes it to the user address the caller passed.
int
blkdev_ioctl(struct gendisk *d, uint64 req, uint64 arg)
{
  uint64 v64;
  int v32;

  if (d == 0)
    return -ENXIO;

  // a loop device answers its own ioctls as well as the block ones
  int loop = loop_index_of(d);
  if (loop >= 0) {
    struct loop_info64 info;
    struct file *f;
    int fd;

    switch (req) {
    case LOOP_SET_FD:
      // the argument is a descriptor: the file this device becomes
      fd = (int)arg;
      f = fdget(myproc(), fd);
      if (f == 0 || f->type != FD_INODE || f->ip == 0)
        return -EBADF;
      return loop_set_fd(loop, f->ip, 0, 0, !f->writable);
    case LOOP_SET_COW: {
      // two descriptors: the base everyone shares and this device's
      // own delta
      struct loop_cow c;
      struct file *bf, *df;
      if (either_copyin(&c, 1, arg, sizeof(c)) < 0)
        return -EFAULT;
      bf = fdget(myproc(), c.base_fd);
      df = fdget(myproc(), c.delta_fd);
      if (bf == 0 || df == 0 || bf->type != FD_INODE || df->type != FD_INODE || bf->ip == 0 ||
          df->ip == 0)
        return -EBADF;
      if (!df->writable)
        return -EACCES;
      return loop_set_cow(loop, bf->ip, df->ip);
    }
    case LOOP_CLR_FD:
      return loop_clr_fd(loop);
    case LOOP_GET_STATUS64: {
      int r = loop_status(loop, &info);
      if (r < 0)
        return r;
      return either_copyout(1, arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    case LOOP_SET_STATUS64:
      if (either_copyin(&info, 1, arg, sizeof(info)) < 0)
        return -EFAULT;
      return loop_set_status(loop, &info);
    default:
      break; // fall through to the block ioctls
    }
  }

  switch (req) {
  case BLKGETSIZE64:
    v64 = blkdev_size(d);
    return either_copyout(1, arg, &v64, sizeof(v64)) < 0 ? -EFAULT : 0;
  case BLKGETSIZE: // in 512-byte sectors, and only 32 bits of them
    v64 = blkdev_size(d) / 512;
    return either_copyout(1, arg, &v64, sizeof(v64)) < 0 ? -EFAULT : 0;
  case BLKSSZGET:
    v32 = d->sector_size;
    return either_copyout(1, arg, &v32, sizeof(v32)) < 0 ? -EFAULT : 0;
  case BLKBSZGET:
    v32 = BLKSIZE;
    return either_copyout(1, arg, &v32, sizeof(v32)) < 0 ? -EFAULT : 0;
  case BLKROGET:
    v32 = d->readonly;
    return either_copyout(1, arg, &v32, sizeof(v32)) < 0 ? -EFAULT : 0;
  case BLKROSET:
    if (either_copyin(&v32, 1, arg, sizeof(v32)) < 0)
      return -EFAULT;
    d->readonly = v32 != 0;
    return 0;
  case BLKFLSBUF:
    return blk_sync(d);
  case BLKRRPART:
    // the table is read again: a tool that has just written one wants
    // the partitions it describes to exist, and the ones it replaced
    // to be gone.
    if (blk_sync(d) < 0)
      return -EIO;
    blk_drop_partitions(d);
    blk_scan_partitions(d);
    return 0;
  default:
    return -ENOTTY;
  }
}
