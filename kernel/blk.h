#ifndef XV6_BLK_H
#define XV6_BLK_H
#include "types.h"
//
// the block layer: disks, the requests that go to them, and the cache
// of blocks that sits in front of them.
//
// a block here is 4096 bytes, which is what ext4 uses and what the
// page allocator hands out, so one cached block is one page and a
// request can be built out of pages without copying.
//

#define BLKSIZE 4096
#define BLKSHIFT 12
#define SECTOR_SIZE 512
#define SECTORS_PER_BLOCK (BLKSIZE / SECTOR_SIZE)
#define MAXPART 16
#define MAXDISK 16
#define DISKNAME 16

struct gendisk;
struct bio;
#include "sleeplock.h"

// what a driver must provide. `submit` starts a request and calls the
// request's done() when the hardware has finished with it; a driver
// that can only work synchronously calls done() before returning.
struct disk_ops {
  int (*submit)(struct gendisk *disk, struct bio *bio);
  int (*flush)(struct gendisk *disk);
  int (*ioctl)(struct gendisk *disk, uint64 req, uint64 arg);
};

struct partition {
  uint64 start;      // first sector of the partition within the disk
  uint64 nsectors;
  char name[DISKNAME];
  int type;          // the MBR type byte, or 0 for GPT
  uchar guid[16];    // GPT partition type
  int used;
};

struct gendisk {
  char name[DISKNAME];       // vda, vdb, loop0
  const struct disk_ops *ops;
  void *priv;                // the driver's own
  uint64 nsectors;
  int sector_size;
  int readonly;
  int claimed;               // a guest has it exclusively
  struct partition parts[MAXPART];
  int nparts;
  uint64 reads, writes;      // how many requests, for /proc
  uint64 read_blocks, write_blocks;
  int used;
};

// one request: a run of sectors, and the pages they move to or from.
#define BIO_MAXPAGES 32

struct bio {
  struct gendisk *disk;
  uint64 sector;             // in the disk's own sectors, partition offset already added
  int npages;
  void *pages[BIO_MAXPAGES];
  int nsectors;
  int write;
  int flush;
  volatile int done;         // the driver sets it when the request is finished
  int error;
  void (*callback)(struct bio *);
  void *priv;
};

// ---- disks ----

struct gendisk *blk_register(const char *name, const struct disk_ops *ops, void *priv,
                             uint64 nsectors, int sector_size);
void blk_unregister(struct gendisk *disk);         // a disk that is going away
void blk_resize(struct gendisk *disk, uint64 nsectors, int readonly);
void blk_drop_partitions(struct gendisk *disk);
struct gendisk *blk_find(const char *name);       // "vda" or "vda1"
struct gendisk *blk_by_index(int i);
int blk_submit(struct gendisk *disk, struct bio *bio);       // start a request
int blk_submit_wait(struct gendisk *disk, struct bio *bio);  // start it and wait
int blk_read_block(struct gendisk *disk, uint64 blkno, void *page);
int blk_write_block(struct gendisk *disk, uint64 blkno, void *page);
int blk_flush(struct gendisk *disk);
void blk_scan_partitions(struct gendisk *disk);

// ---- the cache ----
//
// a cached block belongs to one disk and one block number. it is
// pinned while a caller holds it and written back when it is dirty and
// old, or when someone asks.

// flags on a cached block. BLK_JBD2_JOURNALED is set while the block
// belongs to a journal transaction that is not yet on the log: the
// writeback path must leave such a block alone, because writing it to
// its home location is exactly the write the journal exists to order.
// jbd2 sets it before the block is marked dirty and clears it after the
// block is home; see jbd2_is_journaled() and kernel/jbd2.md.
#define BLK_JBD2_JOURNALED 0x0001

struct blkbuf {
  struct gendisk *disk;
  uint64 blkno;
  void *data;            // one page
  int refs;
  int dirty;
  int valid;
  int flags;
  struct sleeplock lock;
  struct blkbuf *hash_next, *hash_prev;
  struct blkbuf *lru_next, *lru_prev;
  // the dirty blocks are a list of their own, so that writing them
  // back does not mean walking a cache that may hold a million clean
  // ones to find the few that need it.
  struct blkbuf *dirty_next, *dirty_prev;
};

void blkinit(void);
struct blkbuf *blk_get(struct gendisk *disk, uint64 blkno);   // cached, locked, contents valid
struct blkbuf *blk_get_zero(struct gendisk *disk, uint64 blkno); // no read: the caller fills it
struct blkbuf *blk_get_fill(struct gendisk *disk, uint64 blkno); // no read and no zeroing either
// read a run of blocks in before anything asks for them
void blk_readahead(struct gendisk *disk, uint64 blkno, int n);
int blk_cached(struct gendisk *disk, uint64 blkno);
void blk_invalidate_clean(struct gendisk *disk, uint64 blkno);
// a valid cached block, referenced but not locked, for reading only
struct blkbuf *blk_peek(struct gendisk *disk, uint64 blkno);
void blk_unpeek(struct blkbuf *b);
void blk_put(struct blkbuf *b);                               // release the lock and the reference
void blk_mark_dirty(struct blkbuf *b);
int blk_sync(struct gendisk *disk);          // write back everything dirty for one disk
int blk_sync_block(struct gendisk *disk, uint64 blkno);  // one block, for fsync
int blk_sync_range(struct gendisk *disk, uint64 start, uint64 n); // a run of them, batched
int blk_sync_all(void);
void blk_forget(struct gendisk *disk);       // drop a disk's cached blocks
void blk_cache_stats(uint64 *hits, uint64 *misses, uint64 *dirty);
// how many pages the cache is holding: one per cached block, plus its
// own bookkeeping. it never gives them back on its own, so anything
// counting free memory has to know about them.
uint64 blk_cache_pages(void);
// pages the cache would give back if it had to, and giving them back
void blk_start_flusher(void);   // the writeback thread
uint64 blk_reclaimable(void);
// the two tunables /sys/mm and /sys/fs expose
int blk_set_cache_pct(int pct);
int blk_cache_pct(void);
extern int blk_flush_secs;
uint64 blk_reclaim(uint64 want);

#endif
