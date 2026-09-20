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
  return blk_submit_wait(disk, &bio);
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
  cache.maxbufs = mi.total_pages * CACHE_TARGET_PCT / 100;
  if (cache.maxbufs < 256)
    cache.maxbufs = 256;
  if (cache.maxbufs > 262144)
    cache.maxbufs = 262144; // a gigabyte of cache is enough for anyone here
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
    if (now - last >= (uint)(BLK_FLUSH_SECS * TICK_HZ)) {
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

uint64
blk_reclaimable(void)
{
  uint64 n = 0;

  if (holding(&cache.lock))
    return 0;
  acquire(&cache.lock);
  for (struct blkbuf *b = cache.lru_head; b; b = b->lru_next)
    if (droppable(b))
      n++;
  release(&cache.lock);
  return n;
}

// give back up to `want` pages, oldest first; zero means everything
// that can go. this is reached from kalloc, which alloc_buf calls, so
// it does nothing at all when this cpu is already inside the cache.
uint64
blk_reclaim(uint64 want)
{
  uint64 freed = 0;
  struct blkbuf *b, *prev;

  if (holding(&cache.lock))
    return 0;
  acquire(&cache.lock);
  for (b = cache.lru_tail; b && (want == 0 || freed < want); b = prev) {
    prev = b->lru_prev;
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
