//
// the block layer and the block cache.
//
// a disk registers itself here and is then reachable by name, whole or
// by partition; a partition is a disk of its own whose requests are
// shifted by its start sector, so nothing above this layer has to know
// the difference.
//
// the cache holds 4096-byte blocks, one page each, in a hash table
// with an LRU list behind it. a block is read on first use and written
// back when the writeback thread finds it dirty and old, or when
// someone asks for a sync. a caller holds a block locked while it
// works on it, which is what serializes readers against the writer of
// the same block.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "blk.h"
#include "page.h"
#include "errno.h"
#include "slab.h"

#define NBUCKET 512
#define CACHE_TARGET_PCT 20   // of the memory the allocator manages

static struct {
  struct spinlock lock;
  struct gendisk disks[MAXDISK];
  int ndisks;
} disks;

// a partition pretends to be a disk of its own: its private pointer is
// the disk it lives on and its offset.
struct partdisk {
  struct gendisk *parent;
  uint64 start;
};

static struct kmem_cache *buf_cache;

static struct {
  struct spinlock lock;
  struct blkbuf *buckets[NBUCKET];
  struct blkbuf *lru_head, *lru_tail;  // least recently used at the tail
  uint64 nbufs, maxbufs;
  uint64 total_pages;  // what the allocator manages, for the percentage
  int pct;             // how much of it the cache may hold
  uint64 hits, misses;
  uint64 ndirty;
  struct blkbuf *dirty_head, *dirty_tail;
} cache;

// ---- disks ----

struct gendisk *
blk_register(const char *name, const struct disk_ops *ops, void *priv, uint64 nsectors,
             int sector_size)
{
  acquire(&disks.lock);
  for (int i = 0; i < MAXDISK; i++) {
    struct gendisk *d = &disks.disks[i];
    if (d->used)
      continue;
    memset(d, 0, sizeof(*d));
    safestrcpy(d->name, name, sizeof(d->name));
    d->ops = ops;
    d->priv = priv;
    d->nsectors = nsectors;
    d->sector_size = sector_size ? sector_size : SECTOR_SIZE;
    d->used = 1;
    if (i >= disks.ndisks)
      disks.ndisks = i + 1;
    release(&disks.lock);
    return d;
  }
  release(&disks.lock);
  return 0;
}

// a disk that is going away: its cached blocks are dropped and its slot
// freed. a loop device that has been detached is the only caller.
void
blk_unregister(struct gendisk *disk)
{
  if (disk == 0)
    return;
  blk_sync(disk);
  blk_forget(disk);
  acquire(&disks.lock);
  disk->used = 0;
  disk->nparts = 0;
  release(&disks.lock);
}

// a disk whose size has changed: a loop device given a file, or given
// a different offset into one.
void
blk_resize(struct gendisk *disk, uint64 nsectors, int readonly)
{
  if (disk == 0)
    return;
  acquire(&disks.lock);
  disk->nsectors = nsectors;
  disk->readonly = readonly;
  release(&disks.lock);
}

struct gendisk *
blk_find(const char *name)
{
  acquire(&disks.lock);
  for (int i = 0; i < MAXDISK; i++) {
    struct gendisk *d = &disks.disks[i];
    if (d->used && strncmp(d->name, name, DISKNAME) == 0) {
      release(&disks.lock);
      return d;
    }
  }
  release(&disks.lock);
  return 0;
}

struct gendisk *
blk_by_index(int i)
{
  if (i < 0 || i >= MAXDISK)
    return 0;
  return disks.disks[i].used ? &disks.disks[i] : 0;
}

// a partition's requests go to its parent with the offset added
static int
part_submit(struct gendisk *disk, struct bio *bio)
{
  struct partdisk *p = disk->priv;

  bio->sector += p->start;
  bio->disk = p->parent;
  return p->parent->ops->submit(p->parent, bio);
}

static int
part_flush(struct gendisk *disk)
{
  struct partdisk *p = disk->priv;

  return p->parent->ops->flush ? p->parent->ops->flush(p->parent) : 0;
}

static const struct disk_ops part_ops = {
    .submit = part_submit,
    .flush = part_flush,
};

// forget the partitions of a disk: the table may be about to be read
// again, or the disk may be about to have nothing behind it.
void
blk_drop_partitions(struct gendisk *disk)
{
  if (disk == 0)
    return;
  acquire(&disks.lock);
  for (int i = 0; i < MAXDISK; i++) {
    struct gendisk *d = &disks.disks[i];
    if (!d->used || d->ops != &part_ops)
      continue;
    struct partdisk *pd = d->priv;
    if (pd && pd->parent == disk) {
      release(&disks.lock);
      blk_sync(d);
      blk_forget(d);
      acquire(&disks.lock);
      d->used = 0;
    }
  }
  disk->nparts = 0;
  release(&disks.lock);
}

// register one partition as a disk of its own
static void
add_partition(struct gendisk *parent, int index, uint64 start, uint64 nsectors, int type,
              const uchar *guid)
{
  char name[DISKNAME];
  int n = strlen(parent->name);

  if (index >= MAXPART)
    return;
  safestrcpy(name, parent->name, sizeof(name));
  // vda1, vda2, ... and loop0p1 for a name that already ends in a digit
  if (n > 0 && parent->name[n - 1] >= '0' && parent->name[n - 1] <= '9') {
    name[n] = 'p';
    name[n + 1] = '1' + index;
    name[n + 2] = 0;
  } else {
    name[n] = '1' + index;
    name[n + 1] = 0;
  }

  struct partdisk *pd = kalloc();
  if (pd == 0)
    return;
  pd->parent = parent;
  pd->start = start;
  struct gendisk *d = blk_register(name, &part_ops, pd, nsectors, parent->sector_size);
  if (d == 0) {
    kfree(pd);
    return;
  }
  d->readonly = parent->readonly;

  struct partition *p = &parent->parts[index];
  p->start = start;
  p->nsectors = nsectors;
  p->type = type;
  if (guid)
    memmove(p->guid, guid, 16);
  safestrcpy(p->name, name, sizeof(p->name));
  p->used = 1;
  if (index >= parent->nparts)
    parent->nparts = index + 1;
  printk("blk: %s: partition %s, %ld sectors at %ld\n", parent->name, name, nsectors, start);
}

// ---- requests ----

static void
bio_wake(struct bio *bio)
{
  wakeup(bio);
}

int
blk_submit(struct gendisk *disk, struct bio *bio)
{
  bio->disk = disk;
  bio->done = 0;
  bio->error = 0;
  if (disk->ops == 0 || disk->ops->submit == 0)
    return -ENODEV;
  if (bio->write)
    disk->writes++;
  else
    disk->reads++;
  return disk->ops->submit(disk, bio);
}

int
blk_submit_wait(struct gendisk *disk, struct bio *bio)
{
  if (bio->callback == 0)
    bio->callback = bio_wake;
  int r = blk_submit(disk, bio);
  if (r < 0)
    return r;
  while (!bio->done) {
    sleep_prepare(bio);
    sleep_intr();
  }
  return bio->error;
}

// one block, read or written straight through with no cache
int
blk_read_block(struct gendisk *disk, uint64 blkno, void *page)
{
  struct bio bio;

  // a cached copy is the truth. the block may have been written
  // through the cache and not yet reached the disk -- dd writing an
  // image onto a partition is exactly that -- and a caller that went
  // straight to the device would read what was there before.
  struct blkbuf *cached = blk_peek(disk, blkno);
  if (cached) {
    memmove(page, cached->data, BLKSIZE);
    blk_unpeek(cached);
    return 0;
  }

  memset(&bio, 0, sizeof(bio));
  bio.sector = blkno * SECTORS_PER_BLOCK;
  bio.nsectors = SECTORS_PER_BLOCK;
  bio.npages = 1;
  bio.pages[0] = page;
  return blk_submit_wait(disk, &bio);
}

int
blk_write_block(struct gendisk *disk, uint64 blkno, void *page)
{
  struct bio bio;

  if (disk->readonly)
    return -EROFS;
  memset(&bio, 0, sizeof(bio));
  bio.sector = blkno * SECTORS_PER_BLOCK;
  bio.nsectors = SECTORS_PER_BLOCK;
  bio.npages = 1;
  bio.pages[0] = page;
  bio.write = 1;
  int r = blk_submit_wait(disk, &bio);

  // and a cached copy of what was just written must not survive with
  // the older content. a dirty one is left alone: whoever dirtied it
  // has a change of their own that has not been written yet.
  if (r == 0)
    blk_invalidate_clean(disk, blkno);
  return r;
}

int
blk_flush(struct gendisk *disk)
{
  if (disk->ops && disk->ops->flush)
    return disk->ops->flush(disk);
  return 0;
}

// ---- the cache ----

static uint
hash_of(struct gendisk *disk, uint64 blkno)
{
  return (uint)(((uint64)disk >> 6) * 31 + blkno) % NBUCKET;
}

static void
lru_remove(struct blkbuf *b)
{
  if (b->lru_prev)
    b->lru_prev->lru_next = b->lru_next;
  else
    cache.lru_head = b->lru_next;
  if (b->lru_next)
    b->lru_next->lru_prev = b->lru_prev;
  else
    cache.lru_tail = b->lru_prev;
  b->lru_next = b->lru_prev = 0;
}

static void
lru_push_front(struct blkbuf *b)
{
  b->lru_prev = 0;
  b->lru_next = cache.lru_head;
  if (cache.lru_head)
    cache.lru_head->lru_prev = b;
  cache.lru_head = b;
  if (cache.lru_tail == 0)
    cache.lru_tail = b;
}

static void
hash_insert(struct blkbuf *b)
{
  uint h = hash_of(b->disk, b->blkno);

  b->hash_prev = 0;
  b->hash_next = cache.buckets[h];
  if (cache.buckets[h])
    cache.buckets[h]->hash_prev = b;
  cache.buckets[h] = b;
}

static void
hash_remove(struct blkbuf *b)
{
  uint h = hash_of(b->disk, b->blkno);

  if (b->hash_prev)
    b->hash_prev->hash_next = b->hash_next;
  else
    cache.buckets[h] = b->hash_next;
  if (b->hash_next)
    b->hash_next->hash_prev = b->hash_prev;
  b->hash_next = b->hash_prev = 0;
}

static struct blkbuf *
hash_find(struct gendisk *disk, uint64 blkno)
{
  for (struct blkbuf *b = cache.buckets[hash_of(disk, blkno)]; b; b = b->hash_next)
    if (b->disk == disk && b->blkno == blkno)
      return b;
  return 0;
}

// how much of memory the cache may hold, as a percentage. changing it
// does not free anything at once: the cache shrinks as pages are
// wanted elsewhere, which is what reclaim is for.
int
blk_set_cache_pct(int pct)
{
  if (pct < 1 || pct > 90)
    return -1;
  acquire(&cache.lock);
  cache.pct = pct;
  cache.maxbufs = cache.total_pages * (uint64)pct / 100;
  if (cache.maxbufs < 256)
    cache.maxbufs = 256;
  if (cache.maxbufs > 262144)
    cache.maxbufs = 262144; // a gigabyte of cache is enough for anyone here
  release(&cache.lock);
  return 0;
}

int
blk_cache_pct(void)
{
  return cache.pct;
}

void
blkinit(void)
{
  initlock(&disks.lock, "disks");
  initlock(&cache.lock, "blkcache");
  buf_cache = kmem_cache_create("blkbuf", sizeof(struct blkbuf), 16, 0);
  if (buf_cache == 0)
    panic("blkinit");

  // the cache is a fraction of what the allocator manages, and never
  // so large that a machine with little memory cannot run.
  struct meminfo mi;
  kmem_info(&mi);
  cache.total_pages = mi.total_pages;
  cache.pct = CACHE_TARGET_PCT;
  blk_set_cache_pct(CACHE_TARGET_PCT);
  printk("blk: cache up to %ld blocks (%ld MB)\n", cache.maxbufs, cache.maxbufs * BLKSIZE >> 20);
}

// the dirty list: a block joins it when it is dirtied and leaves it
// when it has been written back. the cache lock covers both.
static void
dirty_add(struct blkbuf *b)
{
  b->dirty_prev = cache.dirty_tail;
  b->dirty_next = 0;
  if (cache.dirty_tail)
    cache.dirty_tail->dirty_next = b;
  else
    cache.dirty_head = b;
  cache.dirty_tail = b;
}

static void
dirty_remove(struct blkbuf *b)
{
  if (b->dirty_prev)
    b->dirty_prev->dirty_next = b->dirty_next;
  else
    cache.dirty_head = b->dirty_next;
  if (b->dirty_next)
    b->dirty_next->dirty_prev = b->dirty_prev;
  else
    cache.dirty_tail = b->dirty_prev;
  b->dirty_next = b->dirty_prev = 0;
}

// write one block back. the caller holds its lock.
static int
writeback(struct blkbuf *b)
{
  if (!b->dirty)
    return 0;
  int r = blk_write_block(b->disk, b->blkno, b->data);
  if (r == 0) {
    acquire(&cache.lock);
    if (b->dirty) {
      b->dirty = 0;
      cache.ndirty--;
      dirty_remove(b);
    }
    release(&cache.lock);
  }
  return r;
}

// find a buffer to reuse: the oldest clean one, or a new one while the
// cache is still below its size.
static struct blkbuf *
alloc_buf(void)
{
  if (cache.nbufs < cache.maxbufs) {
    release(&cache.lock);
    struct blkbuf *b = kmem_cache_alloc(buf_cache);
    void *page = kalloc();
    acquire(&cache.lock);
    if (b == 0 || page == 0) {
      if (page)
        kfree(page);
      if (b)
        kmem_cache_free(buf_cache, b);
      return 0;
    }
    memset(b, 0, sizeof(*b));
    b->data = page;
    initsleeplock(&b->lock, "blkbuf");
    cache.nbufs++;
    return b;
  }

  // reuse the oldest buffer nobody holds and nothing must be written
  for (struct blkbuf *b = cache.lru_tail; b; b = b->lru_prev) {
    if (b->refs == 0 && !b->dirty) {
      hash_remove(b);
      lru_remove(b);
      b->valid = 0;
      b->flags = 0;
      return b;
    }
  }
  return 0;
}

struct blkbuf *
blk_get(struct gendisk *disk, uint64 blkno)
{
  struct blkbuf *b;

  acquire(&cache.lock);
  b = hash_find(disk, blkno);
  if (b) {
    b->refs++;
    lru_remove(b);
    lru_push_front(b);
    cache.hits++;
    release(&cache.lock);
    acquiresleep(&b->lock);
    return b;
  }
  cache.misses++;
  b = alloc_buf();
  if (b == 0) {
    release(&cache.lock);
    // nothing was free: push the dirty blocks out and try once more
    blk_sync(disk);
    acquire(&cache.lock);
    b = alloc_buf();
    if (b == 0) {
      release(&cache.lock);
      return 0;
    }
  }
  b->disk = disk;
  b->blkno = blkno;
  b->refs = 1;
  b->valid = 0;
  b->dirty = 0;
  hash_insert(b);
  lru_push_front(b);
  release(&cache.lock);

  acquiresleep(&b->lock);
  if (!b->valid) {
    if (blk_read_block(disk, blkno, b->data) < 0) {
      releasesleep(&b->lock);
      acquire(&cache.lock);
      b->refs--;
      hash_remove(b);
      lru_remove(b);
      release(&cache.lock);
      return 0;
    }
    b->valid = 1;
  }
  return b;
}

// a cached block that is already valid, for a caller that will only
// read it. the reference keeps the buffer from being reused; the
// sleeplock is not taken, because what it protects is a block being
// read from or written to the disk, and a valid block's contents do
// not change under a reader except when something writes the same
// block -- which for file data is a race the caller already has, and
// which is why this is only ever used for file data.
//
// returns 0 when the block is not here or not valid yet, and the
// caller falls back to blk_get.
struct blkbuf *
blk_peek(struct gendisk *disk, uint64 blkno)
{
  struct blkbuf *b;

  acquire(&cache.lock);
  b = hash_find(disk, blkno);
  if (b && b->valid) {
    b->refs++;
    lru_remove(b);
    lru_push_front(b);
    cache.hits++;
    release(&cache.lock);
    return b;
  }
  release(&cache.lock);
  return 0;
}

void
blk_unpeek(struct blkbuf *b)
{
  acquire(&cache.lock);
  b->refs--;
  release(&cache.lock);
}

// drop a clean cached copy of one block, for a caller that has written
// it straight to the device and would otherwise leave the cache with
// what was there before.
void
blk_invalidate_clean(struct gendisk *disk, uint64 blkno)
{
  struct blkbuf *b;

  if (holding(&cache.lock))
    return;
  acquire(&cache.lock);
  b = hash_find(disk, blkno);
  if (b && b->refs == 0 && !b->dirty && !(b->flags & BLK_JBD2_JOURNALED)) {
    hash_remove(b);
    lru_remove(b);
    b->valid = 0;
    b->disk = 0;
    lru_push_front(b);
  }
  release(&cache.lock);
}

// is this block in the cache already? asked before readahead does any
// work: a reader walking a file that is entirely cached should not pay
// to map a window of it every time round.
int
blk_cached(struct gendisk *disk, uint64 blkno)
{
  int found;

  acquire(&cache.lock);
  found = hash_find(disk, blkno) != 0;
  release(&cache.lock);
  return found;
}

// read a run of blocks into the cache before anything asks for them.
//
// this is where the multi-page request earns its keep: the blocks a
// sequential reader is about to want come back in the same round trip
// as the one it is waiting for. blocks already in the cache end the
// run -- there is no point reading past what is already there -- and a
// block that cannot be allocated a buffer ends it too, because
// readahead must never take a buffer from someone who needs one now.
//
// nothing here fails in a way the caller has to handle: readahead that
// does not happen only costs the read that follows.
void
blk_readahead(struct gendisk *disk, uint64 blkno, int n)
{
  struct blkbuf *bufs[BIO_MAXPAGES];
  struct bio bio;
  int got = 0;

  if (disk == 0 || n <= 0)
    return;
  if (n > BIO_MAXPAGES)
    n = BIO_MAXPAGES;

  acquire(&cache.lock);
  for (int i = 0; i < n; i++) {
    uint64 blk = blkno + i;
    if (blk >= disk->nsectors / SECTORS_PER_BLOCK)
      break;
    if (hash_find(disk, blk))
      break; // already here: the run the caller would gain from ends
    struct blkbuf *b = alloc_buf();
    if (b == 0)
      break;
    b->disk = disk;
    b->blkno = blk;
    b->refs = 1;
    b->valid = 0;
    b->dirty = 0;
    hash_insert(b);
    lru_push_front(b);
    bufs[got++] = b;
  }
  release(&cache.lock);
  if (got == 0)
    return;

  for (int i = 0; i < got; i++)
    acquiresleep(&bufs[i]->lock);

  memset(&bio, 0, sizeof(bio));
  bio.sector = blkno * SECTORS_PER_BLOCK;
  bio.nsectors = got * SECTORS_PER_BLOCK;
  bio.npages = got;
  for (int i = 0; i < got; i++)
    bio.pages[i] = bufs[i]->data;
  bio.write = 0;

  int r = blk_submit_wait(disk, &bio);

  for (int i = 0; i < got; i++) {
    struct blkbuf *b = bufs[i];
    if (r == 0) {
      b->valid = 1;
      releasesleep(&b->lock);
      acquire(&cache.lock);
      b->refs--;
      release(&cache.lock);
    } else {
      releasesleep(&b->lock);
      acquire(&cache.lock);
      b->refs--;
      hash_remove(b);
      lru_remove(b);
      b->disk = 0;
      b->valid = 0;
      lru_push_front(b);
      release(&cache.lock);
    }
  }
}

// the same again, but the caller is going to write every byte of the
// block, so zeroing it first is wasted work too. a caller that does not
// fill it leaves whatever the buffer last held in a block of its file,
// which is why this is separate from blk_get_zero and not the same
// function with a flag.
struct blkbuf *
blk_get_fill(struct gendisk *disk, uint64 blkno)
{
  struct blkbuf *b;

  acquire(&cache.lock);
  b = hash_find(disk, blkno);
  if (b) {
    b->refs++;
    lru_remove(b);
    lru_push_front(b);
    release(&cache.lock);
    acquiresleep(&b->lock);
    b->valid = 1;
    return b;
  }
  b = alloc_buf();
  if (b == 0) {
    release(&cache.lock);
    blk_sync(disk);
    acquire(&cache.lock);
    b = alloc_buf();
    if (b == 0) {
      release(&cache.lock);
      return 0;
    }
  }
  b->disk = disk;
  b->blkno = blkno;
  b->refs = 1;
  b->dirty = 0;
  b->valid = 1;
  hash_insert(b);
  lru_push_front(b);
  release(&cache.lock);
  acquiresleep(&b->lock);
  return b;
}

// the same, but the caller is going to overwrite the whole block, so
// reading it first would be wasted work.
struct blkbuf *
blk_get_zero(struct gendisk *disk, uint64 blkno)
{
  struct blkbuf *b;

  acquire(&cache.lock);
  b = hash_find(disk, blkno);
  if (b) {
    b->refs++;
    lru_remove(b);
    lru_push_front(b);
    release(&cache.lock);
    acquiresleep(&b->lock);
    memset(b->data, 0, BLKSIZE);
    b->valid = 1;
    return b;
  }
  b = alloc_buf();
  if (b == 0) {
    release(&cache.lock);
    blk_sync(disk);
    acquire(&cache.lock);
    b = alloc_buf();
    if (b == 0) {
      release(&cache.lock);
      return 0;
    }
  }
  b->disk = disk;
  b->blkno = blkno;
  b->refs = 1;
  b->dirty = 0;
  hash_insert(b);
  lru_push_front(b);
  release(&cache.lock);
  acquiresleep(&b->lock);
  memset(b->data, 0, BLKSIZE);
  b->valid = 1;
  return b;
}

void
blk_put(struct blkbuf *b)
{
  if (b == 0)
    return;
  releasesleep(&b->lock);
  acquire(&cache.lock);
  b->refs--;
  release(&cache.lock);
}

// the block is dirty from now until something writes it back. the
// caller holds it, so nothing else can be writing it out.
void
blk_mark_dirty(struct blkbuf *b)
{
  acquire(&cache.lock);
  if (!b->dirty) {
    b->dirty = 1;
    cache.ndirty++;
    dirty_add(b);
  }
  release(&cache.lock);
}

// write a run of dirty blocks that lie next to one another on the disk
// in one request. the caller holds every buffer's lock and a reference
// on each.
//
// what this saves is round trips: a megabyte of dirty blocks is eight
// requests rather than two hundred and fifty-six.
static int
writeback_run(struct blkbuf **bufs, int n)
{
  struct bio bio;
  int r;

  if (n == 1)
    return writeback(bufs[0]);

  memset(&bio, 0, sizeof(bio));
  bio.sector = bufs[0]->blkno * SECTORS_PER_BLOCK;
  bio.nsectors = n * SECTORS_PER_BLOCK;
  bio.npages = n;
  for (int i = 0; i < n; i++)
    bio.pages[i] = bufs[i]->data;
  bio.write = 1;

  r = blk_submit_wait(bufs[0]->disk, &bio);
  if (r == 0) {
    acquire(&cache.lock);
    for (int i = 0; i < n; i++) {
      if (bufs[i]->dirty) {
        bufs[i]->dirty = 0;
        cache.ndirty--;
        dirty_remove(bufs[i]);
      }
    }
    release(&cache.lock);
  }
  return r;
}

// write back everything dirty that belongs to one disk, or to all of
// them when disk is null.
static int
sync_disk(struct gendisk *disk)
{
  int err = 0;

  for (;;) {
    struct blkbuf *found = 0;
    acquire(&cache.lock);
    // the dirty list, not the whole cache: on a machine with a
    // gigabyte of cached blocks and a handful of dirty ones, walking
    // the cache for each of them was the whole cost of a sync.
    for (struct blkbuf *b = cache.dirty_head; b; b = b->dirty_next) {
      // a block a journal transaction owns is not ours to write: the
      // journal writes it home itself, after its commit block is on the
      // disk. see BLK_JBD2_JOURNALED in blk.h.
      if (b->flags & BLK_JBD2_JOURNALED)
        continue;
      if (disk == 0 || b->disk == disk) {
        b->refs++;
        found = b;
        break;
      }
    }
    release(&cache.lock);
    if (found == 0)
      break;

    // the dirty blocks that lie next to this one on the disk go in the
    // same request. the search starts by walking back to the lowest of
    // them, because the list is in order of use and a program writing
    // a file forwards leaves the highest block at the front of it.
    //
    // the first lock is waited for; the rest are taken only if they are
    // free, so this never waits while holding a lock and cannot
    // deadlock against a file system that holds its blocks in some
    // other order.
    struct blkbuf *run[BIO_MAXPAGES];
    int n = 1;

    acquire(&cache.lock);
    for (int k = 1; k < BIO_MAXPAGES && found->blkno >= (uint64)k; k++) {
      struct blkbuf *pv = hash_find(found->disk, found->blkno - k);
      if (pv == 0 || !pv->dirty || (pv->flags & BLK_JBD2_JOURNALED))
        break;
      found->refs--;
      pv->refs++;
      found = pv;
    }
    release(&cache.lock);

    run[0] = found;
    acquiresleep(&found->lock);
    acquire(&cache.lock);
    while (n < BIO_MAXPAGES) {
      struct blkbuf *nx = hash_find(found->disk, found->blkno + n);
      if (nx == 0 || !nx->dirty || (nx->flags & BLK_JBD2_JOURNALED))
        break;
      if (!tryacquiresleep(&nx->lock))
        break;
      nx->refs++;
      run[n++] = nx;
    }
    release(&cache.lock);

    int r = writeback_run(run, n);
    if (r < 0)
      err = r;

    for (int i = n - 1; i >= 0; i--)
      releasesleep(&run[i]->lock);
    acquire(&cache.lock);
    for (int i = 0; i < n; i++)
      run[i]->refs--;
    release(&cache.lock);
    if (r < 0)
      break;
  }
  if (disk)
    blk_flush(disk);
  return err;
}

int
blk_sync(struct gendisk *disk)
{
  return sync_disk(disk);
}

int
blk_sync_all(void)
{
  int r = sync_disk(0);

  for (int i = 0; i < MAXDISK; i++) {
    struct gendisk *d = blk_by_index(i);
    if (d)
      blk_flush(d);
  }
  return r;
}

void
blk_forget(struct gendisk *disk)
{
  acquire(&cache.lock);
  for (struct blkbuf *b = cache.lru_head; b;) {
    struct blkbuf *next = b->lru_next;
    if (b->disk == disk && b->refs == 0) {
      hash_remove(b);
      lru_remove(b);
      if (b->dirty) {
        b->dirty = 0;
        cache.ndirty--;
        dirty_remove(b);
      }
      b->valid = 0;
      b->flags = 0;
      b->disk = 0;
      lru_push_front(b); // keep it for reuse
    }
    b = next;
  }
  release(&cache.lock);
}

// a cached block that nobody holds, that has nothing in it the disk
// does not have, and that the journal does not own, is memory the
// system is only keeping because it might be useful. these two say how
// much of that there is and give it back.
static int
droppable(struct blkbuf *b)
{
  return b->refs == 0 && !b->dirty && !(b->flags & BLK_JBD2_JOURNALED);
}

// write back whatever of [start, start+n) is dirty, in as few requests
// as it can: the blocks that lie next to one another go together. this
// is what fsync is built out of, and writing a file's blocks one
// request at a time was the whole cost of it.
int
blk_sync_range(struct gendisk *disk, uint64 start, uint64 n)
{
  struct blkbuf *run[BIO_MAXPAGES];
  int err = 0;

  for (uint64 i = 0; i < n;) {
    int k = 0;

    acquire(&cache.lock);
    while (i + k < n && k < BIO_MAXPAGES) {
      struct blkbuf *b = hash_find(disk, start + i + k);
      if (b == 0 || !b->dirty || (b->flags & BLK_JBD2_JOURNALED))
        break;
      // the first of a run is waited for; the rest are taken only if
      // they are free, so this never waits while holding a lock.
      if (k == 0) {
        b->refs++;
        release(&cache.lock);
        acquiresleep(&b->lock);
        acquire(&cache.lock);
        if (!b->dirty || (b->flags & BLK_JBD2_JOURNALED)) {
          release(&cache.lock);
          releasesleep(&b->lock);
          acquire(&cache.lock);
          b->refs--;
          break;
        }
      } else {
        if (!tryacquiresleep(&b->lock))
          break;
        b->refs++;
      }
      run[k++] = b;
    }
    release(&cache.lock);

    if (k == 0) {
      i++; // not dirty, or not here: nothing to write
      continue;
    }

    int r = writeback_run(run, k);
    if (r < 0)
      err = r;
    for (int j = k - 1; j >= 0; j--)
      releasesleep(&run[j]->lock);
    acquire(&cache.lock);
    for (int j = 0; j < k; j++)
      run[j]->refs--;
    release(&cache.lock);
    if (r < 0)
      break;
    i += k;
  }
  return err;
}

// write one block back to its home, if it is cached and dirty and the
// journal does not own it. this is what fsync is built out of: a file's
// blocks, one at a time, rather than the whole cache.
int
blk_sync_block(struct gendisk *disk, uint64 blkno)
{
  struct blkbuf *found = 0;
  int r = 0;

  acquire(&cache.lock);
  for (struct blkbuf *b = cache.buckets[hash_of(disk, blkno)]; b; b = b->hash_next) {
    if (b->disk == disk && b->blkno == blkno) {
      if (b->dirty && !(b->flags & BLK_JBD2_JOURNALED)) {
        b->refs++;
        found = b;
      }
      break;
    }
  }
  release(&cache.lock);
  if (found == 0)
    return 0;

  acquiresleep(&found->lock);
  r = writeback(found);
  releasesleep(&found->lock);
  acquire(&cache.lock);
  found->refs--;
  release(&cache.lock);
  return r;
}

// ---- writeback ----
//
// a dirty block is the only copy of what it holds, so it pins its page
// until someone writes it out. without a thread doing that the pages
// accumulate: the kernel looks like it is losing memory, and a crash
// loses everything written since the last sync. so one thread walks the
// cache every few seconds and writes back what is dirty, the way every
// unix has had a flusher since update(8).
//
// the journal's blocks are not its business: they belong to jbd2 until
// its commit puts them home, and sync_disk skips them.
#define BLK_FLUSH_SECS 5

// how often the flusher runs, and how much of memory the cache may
// hold: both settable through /sys, because the right answer depends
// on the machine and on what it is doing.
int blk_flush_secs = BLK_FLUSH_SECS;

static void
blk_flusher(void *arg)
{
  uint last = 0;

  (void)arg;
  for (;;) {
    uint now;

    acquire(&tickslock);
    now = ticks;
    release(&tickslock);
    if (now - last >= (uint)(blk_flush_secs * tick_hz)) {
      last = now;
      if (cache.ndirty > 0)
        sync_disk(0);
    }
    acquire(&tickslock);
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
  }
}

// started once the first process exists, because a kernel thread needs
// one to be forked from.
void
blk_start_flusher(void)
{
  static int started;

  if (started)
    return;
  started = 1;
  if (kthread_create(blk_flusher, 0, "kflushd") < 0)
    printk("blk: no writeback thread; dirty blocks will wait for sync\n");
}

// how many pages the cache would give back. this is the count it
// keeps rather than a walk of the list: a cache of a gigabyte is a
// quarter of a million buffers, and walking them under the cache's
// spinlock to answer a question about free memory would stop every
// other processor for as long as it took. a block that is not dirty is
// very nearly always droppable -- the ones held or owned by the
// journal at any instant are a handful -- and what this number is for
// is a report of how much memory is available.
uint64
blk_reclaimable(void)
{
  uint64 n, d;

  acquire(&cache.lock);
  n = (uint64)cache.nbufs;
  d = cache.ndirty;
  release(&cache.lock);
  return n > d ? n - d : 0;
}

// give back up to `want` pages, oldest first; zero means everything
// that can go. this is reached from kalloc, which alloc_buf calls, so
// it does nothing at all when this cpu is already inside the cache.
//
// it looks at a bounded number of buffers, not all of them. the oldest
// end of the list is where the droppable ones are, so a short look
// finds them if they are there; and a long one would hold the cache's
// spinlock across a quarter of a million buffers every time memory ran
// short, which is exactly when nothing else can afford to wait.
#define BLK_RECLAIM_SCAN 512

uint64
blk_reclaim(uint64 want)
{
  uint64 freed = 0, looked = 0;
  struct blkbuf *b, *prev;

  if (holding(&cache.lock))
    return 0;
  acquire(&cache.lock);
  for (b = cache.lru_tail; b && (want == 0 || freed < want); b = prev) {
    prev = b->lru_prev;
    if (want != 0 && ++looked > BLK_RECLAIM_SCAN)
      break;
    if (!droppable(b))
      continue;
    if (b->disk) // blk_forget leaves a dropped buffer out of the hash
      hash_remove(b);
    lru_remove(b);
    kfree(b->data);
    kmem_cache_free(buf_cache, b);
    cache.nbufs--;
    freed++;
  }
  release(&cache.lock);
  return freed;
}

uint64
blk_cache_pages(void)
{
  uint64 n;

  acquire(&cache.lock);
  n = (uint64)cache.nbufs;
  release(&cache.lock);
  return n;
}

void
blk_cache_stats(uint64 *hits, uint64 *misses, uint64 *dirty)
{
  acquire(&cache.lock);
  if (hits)
    *hits = cache.hits;
  if (misses)
    *misses = cache.misses;
  if (dirty)
    *dirty = cache.ndirty;
  release(&cache.lock);
}

// ---- partition tables ----

struct mbr_entry {
  uchar boot;
  uchar start_chs[3];
  uchar type;
  uchar end_chs[3];
  uint32 start_lba;
  uint32 nsectors;
} __attribute__((packed));

struct gpt_header {
  char signature[8];   // "EFI PART"
  uint32 revision;
  uint32 header_size;
  uint32 header_crc;
  uint32 reserved;
  uint64 current_lba;
  uint64 backup_lba;
  uint64 first_usable;
  uint64 last_usable;
  uchar disk_guid[16];
  uint64 entry_lba;
  uint32 nentries;
  uint32 entry_size;
  uint32 entries_crc;
} __attribute__((packed));

struct gpt_entry {
  uchar type_guid[16];
  uchar part_guid[16];
  uint64 first_lba;
  uint64 last_lba;
  uint64 flags;
  ushort name[36];
} __attribute__((packed));

// read the partition table, if there is one, and give each entry a
// device of its own.
void
blk_scan_partitions(struct gendisk *disk)
{
  char *page = kalloc();

  if (page == 0)
    return;
  if (blk_read_block(disk, 0, page) < 0) {
    kfree(page);
    return;
  }

  // the protective MBR of a GPT disk has one entry of type 0xee
  struct mbr_entry *mbr = (struct mbr_entry *)(page + 446);
  int has_mbr = (uchar)page[510] == 0x55 && (uchar)page[511] == 0xaa;
  int gpt = 0;
  for (int i = 0; i < 4 && has_mbr; i++)
    if (mbr[i].type == 0xee)
      gpt = 1;

  if (gpt) {
    // the GPT header is in the second sector, and the entries follow
    char *hdrpage = kalloc();
    if (hdrpage == 0) {
      kfree(page);
      return;
    }
    // sector 1 lives in the first block, at offset 512
    struct gpt_header *h = (struct gpt_header *)(page + 512);
    if (memcmp(h->signature, "EFI PART", 8) != 0) {
      kfree(hdrpage);
      kfree(page);
      return;
    }
    uint64 entry_lba = h->entry_lba;
    uint32 nentries = h->nentries;
    uint32 esize = h->entry_size;
    if (esize < sizeof(struct gpt_entry) || esize > 512 || nentries > 128)
      nentries = nentries > 128 ? 128 : nentries;

    int index = 0;
    for (uint32 i = 0; i < nentries && index < MAXPART; i++) {
      uint64 off = entry_lba * 512 + (uint64)i * esize;
      uint64 blk = off / BLKSIZE;
      if (blk_read_block(disk, blk, hdrpage) < 0)
        break;
      struct gpt_entry *e = (struct gpt_entry *)(hdrpage + (off % BLKSIZE));
      int empty = 1;
      for (int k = 0; k < 16; k++)
        if (e->type_guid[k])
          empty = 0;
      if (empty)
        continue;
      if (e->last_lba < e->first_lba)
        continue;
      add_partition(disk, index++, e->first_lba, e->last_lba - e->first_lba + 1, 0, e->type_guid);
    }
    kfree(hdrpage);
    kfree(page);
    return;
  }

  if (!has_mbr) {
    kfree(page);
    return;
  }
  int index = 0;
  for (int i = 0; i < 4; i++) {
    if (mbr[i].type == 0 || mbr[i].nsectors == 0)
      continue;
    if (mbr[i].type == 0x05 || mbr[i].type == 0x0f) {
      // an extended partition holds a chain of logical ones
      uint64 base = mbr[i].start_lba, next = base;
      for (int guard = 0; guard < MAXPART && next && index < MAXPART; guard++) {
        char *ext = kalloc();
        if (ext == 0)
          break;
        if (blk_read_block(disk, next / SECTORS_PER_BLOCK, ext) < 0) {
          kfree(ext);
          break;
        }
        struct mbr_entry *le = (struct mbr_entry *)(ext + (next % SECTORS_PER_BLOCK) * 512 + 446);
        if (le[0].type && le[0].nsectors)
          add_partition(disk, index++, next + le[0].start_lba, le[0].nsectors, le[0].type, 0);
        next = le[1].type ? base + le[1].start_lba : 0;
        kfree(ext);
      }
      continue;
    }
    add_partition(disk, index++, mbr[i].start_lba, mbr[i].nsectors, mbr[i].type, 0);
  }
  kfree(page);
}
