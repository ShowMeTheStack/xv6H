//
// loop devices: a file made to look like a disk.
//
// /dev/loop0 and its fellows are gendisks whose blocks are a file's
// bytes, so an image can be partitioned, have a file system made on it
// and be mounted without being written to a disk first. what makes it
// possible at all is that a file is already a sequence of bytes the
// kernel can address; what makes it delicate is that reading one is
// itself a file system operation, and it must not be done from inside
// whatever operation asked for the block.
//
// so a request is handed to a thread of the device's own. the caller
// waits for it. that thread is the only place the backing file's inode
// is locked, which is what keeps the loop device's file system and the
// file system the image lives on from waiting for each other.
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

#define NLOOP 8

struct loopdev {
  struct spinlock lock;
  int index;
  int inuse;          // a device exists (it is in /dev)
  int bound;          // it has a file behind it
  struct inode *ip;   // the file, held for as long as it is bound
  uint64 offset;      // the first byte of the file this device starts at
  uint64 sizelimit;   // how many bytes of it the device is, or 0 for all
  int readonly;
  int autoclear;      // let go of the file when the last user closes it

  // copy-on-write over a base image: the base is read and never
  // written, the delta (l->ip) holds the blocks that have been changed,
  // and the map says where each of them went.
  struct inode *base;
  uint32 *map;        // map[i] is the delta block holding base block i, plus one
  uint32 nblocks;     // blocks of the base
  uint32 nmapped;     // delta blocks in use
  uint64 data_off;    // where the delta's data blocks start
  char name[DISKNAME];
  char backing[LO_NAME_SIZE]; // the file's name, for whoever asks what is attached
  struct gendisk *disk;

  // the queue between whoever wants a block and the thread that reads
  // it. one request at a time is enough: the caller waits anyway, and
  // a second caller waits for the first.
  struct bio *req;
  int busy;
  int stop;
  int running;
};

static struct loopdev loops[NLOOP];
static struct spinlock looplock; // guards which devices exist

static const struct disk_ops loop_ops;

// the bytes of the file this device covers
static uint64
loop_bytes(struct loopdev *l)
{
  uint64 size;
  struct inode *ip = l->base ? l->base : l->ip;

  if (ip == 0)
    return 0;
  // a copy-on-write device is as large as the base it covers, whatever
  // the delta happens to hold
  ilock(ip);
  size = ip->size;
  iunlock(ip);
  if (l->base)
    return size;
  if (l->offset >= size)
    return 0;
  size -= l->offset;
  if (l->sizelimit > 0 && l->sizelimit < size)
    size = l->sizelimit;
  return size;
}

// ---- copy-on-write over a base image ----
//
// the delta file is a header, then one 32-bit entry per block of the
// base saying which delta block holds it (zero for "still the base's"),
// then the delta blocks themselves in the order they were first
// written. the map is read into memory when the device is attached and
// written through on every change, so an interrupted write costs one
// block and not the file.

#define COW_MAGIC 0x574f4358u // "XCOW"

struct cow_header {
  uint32 magic;
  uint32 version;
  uint32 block_size;
  uint32 nblocks;  // of the base
  uint32 nmapped;  // delta blocks in use
  uint32 reserved[3];
};

// the delta's layout, from the number of blocks the base has
static uint64
cow_index_off(void)
{
  return BLKSIZE;
}

static uint64
cow_index_bytes(uint32 nblocks)
{
  return ((uint64)nblocks * 4 + BLKSIZE - 1) & ~(uint64)(BLKSIZE - 1);
}

// read or write the delta at a byte offset, whole blocks only
static int
cow_rw(struct inode *ip, int write, uint64 off, void *buf, uint32 n)
{
  int r;

  ilock(ip);
  r = write ? writei(ip, 0, (uint64)buf, off, n) : readi(ip, 0, (uint64)buf, off, n);
  iunlock(ip);
  if (r < 0)
    return r;
  if (write && r != (int)n)
    return -ENOSPC;
  if (!write && r < (int)n)
    memset((uchar *)buf + r, 0, n - r);
  return 0;
}

// one changed block's place in the delta, allocated if this is the
// first time the block has been written
static int
cow_alloc(struct loopdev *l, uint32 blk, uint32 *slot)
{
  uint32 entry;
  int r;

  if (l->map[blk] != 0) {
    *slot = l->map[blk] - 1;
    return 0;
  }
  *slot = l->nmapped;
  entry = *slot + 1;
  // the map entry first: a crash after it points at a block whose
  // contents are whatever the file held, which is a block of zeros in
  // a file that has just grown. the other order would lose a write.
  if ((r = cow_rw(l->ip, 1, cow_index_off() + (uint64)blk * 4, &entry, 4)) < 0)
    return r;
  l->map[blk] = entry;
  l->nmapped++;

  struct cow_header h;
  if ((r = cow_rw(l->ip, 0, 0, &h, sizeof(h))) < 0)
    return r;
  h.nmapped = l->nmapped;
  return cow_rw(l->ip, 1, 0, &h, sizeof(h));
}

// one page of a copy-on-write device
static int
cow_page(struct loopdev *l, uint64 off, void *page, int write)
{
  uint32 blk = (uint32)(off / BLKSIZE);
  uint32 slot;
  int r;

  if (blk >= l->nblocks)
    return -EIO;
  if (!write) {
    if (l->map[blk] != 0)
      return cow_rw(l->ip, 0, l->data_off + (uint64)(l->map[blk] - 1) * BLKSIZE, page, BLKSIZE);
    return cow_rw(l->base, 0, (uint64)blk * BLKSIZE, page, BLKSIZE);
  }
  if ((r = cow_alloc(l, blk, &slot)) < 0)
    return r;
  return cow_rw(l->ip, 1, l->data_off + (uint64)slot * BLKSIZE, page, BLKSIZE);
}

// one request, on the loop thread: the file is read or written here
// and nowhere else.
static int
loop_do(struct loopdev *l, struct bio *bio)
{
  uint64 off = l->offset + bio->sector * 512;
  int r = 0;

  if (l->ip == 0)
    return -ENXIO;
  if (bio->write && l->readonly)
    return -EROFS;

  if (l->base) {
    for (int i = 0; i < bio->npages && r == 0; i++) {
      r = cow_page(l, off, bio->pages[i], bio->write);
      off += BLKSIZE;
    }
    return r;
  }

  for (int i = 0; i < bio->npages && r == 0; i++) {
    int n;
    ilock(l->ip);
    if (bio->write)
      n = writei(l->ip, 0, (uint64)bio->pages[i], off, BLKSIZE);
    else
      n = readi(l->ip, 0, (uint64)bio->pages[i], off, BLKSIZE);
    iunlock(l->ip);
    if (bio->write) {
      if (n != BLKSIZE)
        r = n < 0 ? n : -EIO;
    } else if (n < 0) {
      r = n;
    } else if (n < BLKSIZE) {
      // past the end of the file reads as zeros, which is what a short
      // image mapped onto a larger device has to look like
      memset((uchar *)bio->pages[i] + (n < 0 ? 0 : n), 0, BLKSIZE - n);
    }
    off += BLKSIZE;
  }
  return r;
}

static void
loop_thread(void *arg)
{
  struct loopdev *l = arg;

  acquire(&l->lock);
  l->running = 1;
  for (;;) {
    while (l->req == 0 && !l->stop) {
      sleep_prepare(&l->req);
      release(&l->lock);
      sleep();
      acquire(&l->lock);
    }
    if (l->stop && l->req == 0)
      break;
    struct bio *bio = l->req;
    release(&l->lock);

    int r = loop_do(l, bio);

    acquire(&l->lock);
    bio->error = r;
    bio->done = 1;
    l->req = 0;
    release(&l->lock);
    wakeup(bio);
    acquire(&l->lock);
  }
  l->running = 0;
  release(&l->lock);
  wakeup(l);
}

// the block layer's entry point: hand the request to the thread and
// wait. nothing of the backing file is touched here.
static int
loop_submit(struct gendisk *disk, struct bio *bio)
{
  struct loopdev *l = disk->priv;

  acquire(&l->lock);
  if (!l->bound) {
    release(&l->lock);
    bio->error = -ENXIO;
    bio->done = 1;
    return -ENXIO;
  }
  while (l->busy) {
    sleep_prepare(&l->busy);
    release(&l->lock);
    sleep();
    acquire(&l->lock);
  }
  l->busy = 1;
  l->req = bio;
  release(&l->lock);
  wakeup(&l->req);

  acquire(&l->lock);
  while (!bio->done) {
    sleep_prepare(bio);
    release(&l->lock);
    sleep();
    acquire(&l->lock);
  }
  l->busy = 0;
  release(&l->lock);
  wakeup(&l->busy);

  if (bio->callback)
    bio->callback(bio);
  return bio->error;
}

static int
loop_flush(struct gendisk *disk)
{
  (void)disk;
  return 0; // the backing file's own file system decides when to write
}

static const struct disk_ops loop_ops = {
    .submit = loop_submit,
    .flush = loop_flush,
};

// ---- creating and destroying devices ----

static struct loopdev *
loop_get(int index)
{
  if (index < 0 || index >= NLOOP)
    return 0;
  return &loops[index];
}

// make /dev/loopN exist, with nothing behind it yet
int
loop_add(int index)
{
  struct loopdev *l = loop_get(index);

  if (l == 0)
    return -EINVAL;
  acquire(&looplock);
  if (l->inuse) {
    release(&looplock);
    return -EEXIST;
  }
  l->inuse = 1;
  release(&looplock);

  // "loop0" .. "loop7"; the kernel has no snprintf and does not need
  // one for a name of two parts.
  safestrcpy(l->name, "loop", sizeof(l->name));
  l->name[4] = (char)('0' + index);
  l->name[5] = 0;
  l->index = index;
  l->disk = blk_register(l->name, &loop_ops, l, 0, 512);
  if (l->disk == 0) {
    acquire(&looplock);
    l->inuse = 0;
    release(&looplock);
    return -ENOMEM;
  }
  if (!l->running && kthread_create(loop_thread, l, "loop") < 0) {
    blk_unregister(l->disk);
    l->disk = 0;
    acquire(&looplock);
    l->inuse = 0;
    release(&looplock);
    return -ENOMEM;
  }
  return index;
}

// the lowest device with no file behind it, made if it does not exist
int
loop_get_free(void)
{
  for (int i = 0; i < NLOOP; i++) {
    struct loopdev *l = &loops[i];
    acquire(&looplock);
    int free_here = l->inuse && !l->bound;
    release(&looplock);
    if (free_here)
      return i;
  }
  for (int i = 0; i < NLOOP; i++) {
    acquire(&looplock);
    int missing = !loops[i].inuse;
    release(&looplock);
    if (missing)
      return loop_add(i);
  }
  return -ENOSPC;
}

int
loop_remove(int index)
{
  struct loopdev *l = loop_get(index);

  if (l == 0)
    return -EINVAL;
  acquire(&looplock);
  if (!l->inuse || l->bound) {
    int r = l->bound ? -EBUSY : -ENXIO;
    release(&looplock);
    return r;
  }
  l->inuse = 0;
  release(&looplock);
  if (l->disk) {
    blk_unregister(l->disk);
    l->disk = 0;
  }
  return 0;
}

// ---- binding a file ----

int
loop_set_fd(int index, struct inode *ip, uint64 offset, uint64 sizelimit, int readonly)
{
  struct loopdev *l = loop_get(index);
  uint64 bytes;

  if (l == 0 || ip == 0)
    return -EINVAL;
  acquire(&looplock);
  if (!l->inuse) {
    release(&looplock);
    return -ENXIO;
  }
  if (l->bound) {
    release(&looplock);
    return -EBUSY;
  }
  l->bound = 1;
  release(&looplock);

  l->ip = idup(ip);
  l->offset = offset;
  l->sizelimit = sizelimit;
  l->readonly = readonly;
  bytes = loop_bytes(l);
  blk_resize(l->disk, bytes / 512, readonly);
  // an image may carry a partition table; the devices it names appear
  // now, the same as for a real disk.
  blk_scan_partitions(l->disk);
  return 0;
}

int
loop_clr_fd(int index)
{
  struct loopdev *l = loop_get(index);
  struct inode *ip;

  if (l == 0)
    return -EINVAL;
  acquire(&looplock);
  if (!l->inuse || !l->bound) {
    release(&looplock);
    return -ENXIO;
  }
  l->bound = 0;
  release(&looplock);

  blk_sync(l->disk);
  blk_forget(l->disk);
  blk_drop_partitions(l->disk);
  blk_resize(l->disk, 0, 0);
  l->backing[0] = 0;
  struct inode *base = l->base;
  uint32 *map = l->map;
  uint32 nblocks = l->nblocks;
  l->base = 0;
  l->map = 0;
  l->nblocks = 0;
  l->nmapped = 0;
  if (map) {
    int order = 0;
    while (((uint64)PGSIZE << order) < cow_index_bytes(nblocks))
      order++;
    kfree_order(map, order);
  }
  if (base)
    iput(base);
  ip = l->ip;
  l->ip = 0;
  l->offset = 0;
  l->sizelimit = 0;
  if (ip)
    iput(ip);
  return 0;
}

int
loop_status(int index, struct loop_info64 *info)
{
  struct loopdev *l = loop_get(index);

  if (l == 0)
    return -EINVAL;
  acquire(&looplock);
  if (!l->inuse || !l->bound) {
    release(&looplock);
    return -ENXIO;
  }
  release(&looplock);
  memset(info, 0, sizeof(*info));
  info->lo_offset = l->offset;
  info->lo_sizelimit = l->sizelimit;
  info->lo_number = (uint32)index;
  info->lo_flags = (l->readonly ? LO_FLAGS_READ_ONLY : 0) |
                   (l->autoclear ? LO_FLAGS_AUTOCLEAR : 0);
  safestrcpy((char *)info->lo_file_name, l->backing, LO_NAME_SIZE);
  // a copy-on-write device says how much of the base it has changed,
  // which is the one number worth knowing about one
  if (l->base)
    info->lo_init[0] = ((uint64)l->nmapped << 32) | l->nblocks;
  return 0;
}

// attach a base image and a delta to this device: the base is read and
// never written, the delta takes every change. an empty delta is made
// into one here, so that creating an overlay is opening a new file.
int
loop_set_cow(int index, struct inode *base, struct inode *delta)
{
  struct loopdev *l = loop_get(index);
  struct cow_header h;
  uint64 basesize, idxbytes;
  uint32 nblocks;
  uint32 *map;
  int order = 0, r;

  if (l == 0 || base == 0 || delta == 0)
    return -EINVAL;
  acquire(&looplock);
  if (!l->inuse) {
    release(&looplock);
    return -ENXIO;
  }
  if (l->bound) {
    release(&looplock);
    return -EBUSY;
  }
  l->bound = 1;
  release(&looplock);

  ilock(base);
  basesize = base->size;
  iunlock(base);
  nblocks = (uint32)((basesize + BLKSIZE - 1) / BLKSIZE);
  if (nblocks == 0 || nblocks > (1u << 20)) { // four gigabytes of base
    r = -EINVAL;
    goto fail;
  }
  idxbytes = cow_index_bytes(nblocks);
  while (((uint64)PGSIZE << order) < idxbytes)
    order++;
  if ((map = kalloc_order(order)) == 0) {
    r = -ENOMEM;
    goto fail;
  }
  memset(map, 0, (uint)((uint64)PGSIZE << order));

  uint64 dsize;
  ilock(delta);
  dsize = delta->size;
  iunlock(delta);

  if (dsize == 0) {
    // a new overlay: the header and an empty map, so that every later
    // write lands at the end of the file and the file system never has
    // to make a hole
    memset(&h, 0, sizeof(h));
    h.magic = COW_MAGIC;
    h.version = 1;
    h.block_size = BLKSIZE;
    h.nblocks = nblocks;
    char *zero = kalloc();
    if (zero == 0) {
      kfree_order(map, order);
      r = -ENOMEM;
      goto fail;
    }
    memset(zero, 0, PGSIZE);
    memmove(zero, &h, sizeof(h));
    r = cow_rw(delta, 1, 0, zero, BLKSIZE);
    memset(zero, 0, PGSIZE);
    for (uint64 off = cow_index_off(); r == 0 && off < cow_index_off() + idxbytes; off += BLKSIZE)
      r = cow_rw(delta, 1, off, zero, BLKSIZE);
    kfree(zero);
    if (r < 0) {
      kfree_order(map, order);
      goto fail;
    }
  } else {
    if ((r = cow_rw(delta, 0, 0, &h, sizeof(h))) < 0) {
      kfree_order(map, order);
      goto fail;
    }
    if (h.magic != COW_MAGIC || h.version != 1 || h.block_size != BLKSIZE ||
        h.nblocks != nblocks) {
      kfree_order(map, order);
      r = -EINVAL;
      goto fail;
    }
    if ((r = cow_rw(delta, 0, cow_index_off(), map, (uint32)idxbytes)) < 0) {
      kfree_order(map, order);
      goto fail;
    }
    l->nmapped = h.nmapped;
  }

  l->ip = idup(delta);
  l->base = idup(base);
  l->map = map;
  l->nblocks = nblocks;
  l->data_off = cow_index_off() + idxbytes;
  l->offset = 0;
  l->sizelimit = 0;
  l->readonly = 0;
  blk_resize(l->disk, basesize / 512, 0);
  blk_scan_partitions(l->disk);
  return 0;

fail:
  acquire(&looplock);
  l->bound = 0;
  release(&looplock);
  return r;
}

// the name of the file behind a device, for a caller that has one to
// give: the ioctl path takes it in the status structure, and a mount
// that attached the device itself sets it here.
int
loop_set_name(int index, const char *name)
{
  struct loopdev *l = loop_get(index);

  if (l == 0)
    return -EINVAL;
  safestrcpy(l->backing, name, LO_NAME_SIZE);
  return 0;
}

int
loop_set_status(int index, const struct loop_info64 *info)
{
  struct loopdev *l = loop_get(index);

  if (l == 0)
    return -EINVAL;
  acquire(&looplock);
  if (!l->inuse || !l->bound) {
    release(&looplock);
    return -ENXIO;
  }
  release(&looplock);
  if (info->lo_file_name[0])
    safestrcpy(l->backing, (const char *)info->lo_file_name, LO_NAME_SIZE);
  if (l->base)
    return 0; // a copy-on-write device's geometry is the base's, not the caller's
  l->offset = info->lo_offset;
  l->sizelimit = info->lo_sizelimit;
  l->readonly = (info->lo_flags & LO_FLAGS_READ_ONLY) != 0;
  l->autoclear = (info->lo_flags & LO_FLAGS_AUTOCLEAR) != 0;
  blk_resize(l->disk, loop_bytes(l) / 512, l->readonly);
  blk_drop_partitions(l->disk);
  blk_scan_partitions(l->disk);
  return 0;
}

// which loop device a gendisk is, or -1
int
loop_index_of(struct gendisk *d)
{
  for (int i = 0; i < NLOOP; i++)
    if (loops[i].inuse && loops[i].disk == d)
      return i;
  return -1;
}

// ---- /dev/loop-control ----
//
// the device that makes and unmakes loop devices. it answers three
// ioctls and nothing else; reading or writing it does nothing.

static int
loopctl_read(int user, uint64 addr, int n)
{
  (void)user;
  (void)addr;
  (void)n;
  return 0;
}

static int
loopctl_write(int user, uint64 addr, int n)
{
  (void)user;
  (void)addr;
  (void)n;
  return -EINVAL;
}

static int
loopctl_ioctl(uint64 req, uint64 arg)
{
  switch (req) {
  case LOOP_CTL_GET_FREE:
    return loop_get_free();
  case LOOP_CTL_ADD:
    return loop_add((int)arg);
  case LOOP_CTL_REMOVE:
    return loop_remove((int)arg);
  default:
    return -ENOTTY;
  }
}

void
loopinit(void)
{
  initlock(&looplock, "loop");
  for (int i = 0; i < NLOOP; i++)
    initlock(&loops[i].lock, "loopdev");
  devsw[DEV_LOOPCTL].read = loopctl_read;
  devsw[DEV_LOOPCTL].write = loopctl_write;
  devsw[DEV_LOOPCTL].ioctl = loopctl_ioctl;
}
