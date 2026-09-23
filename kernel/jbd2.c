//
// jbd2: the journal, written the way Linux writes it.
//
// the problem the journal solves is that a file system operation touches
// several blocks and a disk makes no promise about which of them survive
// a crash. rename writes two directory blocks and an inode; if the disk
// takes the first and loses the second, the file system is not merely
// stale, it is wrong. the journal fixes this by writing every metadata
// block twice: first into a log, where a whole group of them is made
// durable together, and only then to the place the block really lives.
// after a crash the log is replayed, so either every block of a group is
// on the disk or none of them is.
//
// THE LOG. the journal is a file whose blocks the file system knows how
// to find; `map` turns a journal block number into a disk block number.
// block 0 is the journal superblock, which says how long the log is
// (s_maxlen), where it starts (s_first, 1), which block the oldest
// transaction that still matters begins at (s_start) and what sequence
// number that transaction has (s_sequence). blocks s_first .. s_maxlen-1
// are the log proper, used circularly.
//
// A TRANSACTION on the log is
//
//    [descriptor][data][data]...[descriptor][data]...[revoke]...[commit]
//
// a descriptor block holds a tag per data block that follows it, in the
// same order, naming the home block that data belongs to. a revoke block
// lists home blocks that must not be replayed. the commit block is last,
// and it is the whole point: a transaction whose commit block is on the
// disk and verifies happened, and one whose commit block is missing or
// broken did not. so the commit block must not reach the disk before the
// blocks it commits, which is why there is a blk_flush() before it and
// another after it. without those two flushes the log means nothing.
//
// ESCAPING. recovery finds the end of a transaction by reading log
// blocks and looking for the jbd2 magic. a file system block may hold
// anything, including those four bytes, so a data block that begins with
// the magic is written to the log with them zeroed and its tag marked
// JBD2_FLAG_ESCAPE; replay puts the magic back. this is not optional.
//
// REVOKES. a metadata block that is freed may be handed out again as
// file data. if the log still holds the old metadata for it, replay
// would write that metadata over the new data. so freeing a block
// records a revoke, and replay skips a block revoked by the transaction
// that logged it or by a later one.
//
// CHECKSUMS. with csum_v3 every descriptor, revoke and commit block
// carries a crc32c of itself, seeded with a crc32c of the journal's uuid,
// and every tag carries a crc32c of the data block it names. recovery
// refuses to replay a transaction whose commit block does not verify,
// which is how a torn write at the end of the log is told from a
// complete one.
//
// IN MEMORY there is exactly one transaction. handles join it; a handle
// declares how many blocks it will dirty and the journal refuses to open
// one it cannot fit. a commit closes the running transaction to new
// handles, waits for the open ones to finish, writes the whole thing to
// the log, then writes every block to its home location and moves the
// log's start past it. this is simpler than Linux, which keeps frozen
// copies of the blocks so that a new transaction can run while the old
// one is written; jbd2.md says what that costs and why it is still
// correct.
//
// A COMMIT THREAD (kjournald) commits every five seconds when there is
// anything to commit, and sooner when jbd2_commit asks or the running
// transaction has grown past a quarter of the log.
//
// THE BLOCK CACHE must not write a journalled block to its home location
// on its own: that is exactly the write the log exists to order. the
// writeback path asks jbd2_is_journaled() and skips the block if it says
// yes. jbd2.md states the contract.
//
// NOTHING READ FROM THE LOG IS TRUSTED. every block number out of a tag
// or a revoke record is checked against the size of the device before
// anything is written to it, every loop that follows the log has a bound
// that does not come from the log, and a structure that does not check
// out stops recovery rather than being guessed at.
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
#include "errno.h"
#include "ext4_ondisk.h"
#include "jbd2.h"

// the on-disk layout, proved field by field rather than assumed. nothing
// in the jbd2 format has implicit padding, so a plain structure has the
// layout the disk has.
_Static_assert(sizeof(journal_header_t) == 12, "journal header is not 12 bytes");
_Static_assert(sizeof(journal_block_tag3_t) == 16, "the csum_v3 tag is not 16 bytes");
_Static_assert(sizeof(journal_block_tag_t) == 12, "the old tag is not 12 bytes");
_Static_assert(sizeof(jbd2_journal_revoke_header_t) == 16, "the revoke header is not 16 bytes");
_Static_assert(sizeof(struct jbd2_journal_block_tail) == 4, "the block tail is not 4 bytes");
_Static_assert(sizeof(journal_superblock_t) == 1024, "the journal superblock is not 1024 bytes");
_Static_assert(__builtin_offsetof(journal_superblock_t, s_uuid) == 48, "s_uuid moved");
_Static_assert(__builtin_offsetof(journal_superblock_t, s_checksum) == 252, "s_checksum moved");
_Static_assert(__builtin_offsetof(struct commit_header, h_chksum) == 16, "h_chksum moved");
_Static_assert(__builtin_offsetof(struct commit_header, h_commit_sec) == 48, "h_commit_sec moved");
_Static_assert(__builtin_offsetof(journal_block_tag3_t, t_blocknr_high) ==
               __builtin_offsetof(journal_block_tag_t, t_blocknr_high),
               "the two tag shapes must agree on t_blocknr_high");

// ---- crc32c ----

// the kernel's crc32c (kernel/crc32c.c) is already the raw running
// value with neither complement, which is what jbd2 and ext4 both want:
// each seeds one checksum with the result of another. jbd2_crc32c is
// kept as the name this file uses so that the checksum rules below read
// the way Linux's do.
#define jbd2_crc32c(crc, data, len) crc32c((crc), (data), (int)(len))

// ---- byte order ----
//
// the journal is big-endian and this machine is not, so every field goes
// through one of these. the swap is its own inverse, so the same call
// reads a field and writes one.
//
// the swaps are written out rather than taken from __builtin_bswap,
// which on a riscv without the bit-manipulation extension becomes a call
// into libgcc, and the kernel is not linked against it.

static uint16
jbd2_be16(uint16 v)
{
  return (uint16)((v >> 8) | (v << 8));
}

static uint32
jbd2_be32(uint32 v)
{
  return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
         ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

static uint64
jbd2_be64(uint64 v)
{
  return ((uint64)jbd2_be32((uint32)v) << 32) | jbd2_be32((uint32)(v >> 32));
}

// sequence numbers are 32 bits and wrap, so they are compared by the
// sign of their difference and never by <.
static int
jbd2_tid_gt(uint32 a, uint32 b)
{
  return (int)(a - b) > 0;
}

// the tick counter, which the commit thread's interval is measured in.
// it is read under its own lock and never while the journal's is held.
static uint
jbd2_now(void)
{
  uint t;

  acquire(&tickslock);
  t = ticks;
  release(&tickslock);
  return t;
}

// ---- in-memory state ----

#define JBD2_ORDER 2        // 2^2 pages for struct jbd2; the asserts below check it
#define JBD2_MAX_HANDLES 32 // handles open at once, so that none is allocated

// the running transaction. there is only ever one: a commit closes it,
// writes it and empties it before another may start.
struct jbd2_txn {
  uint32 tid;
  int nblocks;
  int nrevokes;
  int reserved;                              // credits promised to open handles, not yet spent
  struct blkbuf *blocks[JBD2_MAX_TXN_BLOCKS];
  uint64 revokes[JBD2_MAX_REVOKES];
};

struct jbd2_handle {
  struct jbd2 *j;
  int credits;  // what jbd2_start reserved
  int used;     // blocks this handle has taken write access to
  int inuse;
};

struct jbd2 {
  struct gendisk *disk;
  uint64 (*map)(void *priv, uint64 jblk);
  void *priv;
  uint64 devblocks; // blocks on the device: the bound every replay target is checked against

  // what the journal superblock says
  uint64 maxlen;
  uint64 first;
  uint32 features;  // s_feature_incompat, as it is on the disk until recovery upgrades it
  uint32 csum_seed; // crc32c(~0, the journal superblock's own s_uuid, 16)
  uchar uuid[16];
  int tag_bytes;
  int max_txn_blocks; // cut down from JBD2_MAX_TXN_BLOCKS if the log is short
  int max_revokes;

  // the log. there is no separate tail: every commit checkpoints what it
  // wrote before it returns, so the oldest block the log still needs is
  // always the block the next transaction will start at.
  uint64 head;
  uint32 next_tid;

  struct spinlock lock;
  int ordered;   // file data goes to its home blocks before the commit
  int recovered;
  int committing;
  int nhandles;
  int failed; // the journal gave up; no transaction may start
  struct jbd2_txn txn;
  struct jbd2_handle handles[JBD2_MAX_HANDLES];

  // the commit thread
  int stopping;
  int thread_running;
  uint last_commit_tick;
  int commit_request;

  uint64 ntransactions;
  uint64 nblocks_written;

  void *sbuf; // the journal superblock, as it is on the disk
  void *dbuf; // the descriptor block being built, or a data block during recovery
  void *mbuf; // the revoke or commit block being built
  void *ebuf; // an escaped copy of a data block
  void *rbuf; // a log block being read during recovery
};

_Static_assert(sizeof(struct jbd2) <= (1 << JBD2_ORDER) * PGSIZE,
               "struct jbd2 does not fit in its allocation");

// a tag, read out of a descriptor block in whichever shape the journal
// on the disk uses.
struct jbd2_tag {
  uint64 blkno;
  uint32 flags;
  uint32 csum;
  int has_csum;
};

// ---- the log as a ring ----

static uint64
jbd2_log_next(struct jbd2 *j, uint64 blk)
{
  blk++;
  if (blk >= j->maxlen)
    blk = j->first;
  return blk;
}

// turn a journal block number into a disk block number, refusing one
// that is outside the journal or outside the device. every read and
// write of the log goes through here, so a file system that hands back a
// wild mapping cannot make us touch a block that is not ours.
static uint64
jbd2_map_block(struct jbd2 *j, uint64 jblk)
{
  uint64 d;

  if (jblk >= j->maxlen)
    return 0;
  d = j->map(j->priv, jblk);
  if (d == 0 || d >= j->devblocks)
    return 0;
  return d;
}

static int
jbd2_jread(struct jbd2 *j, uint64 jblk, void *page)
{
  uint64 d = jbd2_map_block(j, jblk);

  if (d == 0) {
    printk("jbd2: journal block %lu has no disk block\n", jblk);
    return -EIO;
  }
  return blk_read_block(j->disk, d, page);
}

// ---- writing several blocks in one request ----
//
// a transaction's blocks go to the log one after another, and the log
// is a file whose blocks are usually one extent, so they are usually
// next to each other on the disk. writing them one request at a time
// costs a round trip each; gathered into one request they cost one.
//
// the pages must still be there when the batch is written, so a buffer
// the journal reuses -- the descriptor it is building, the escaped
// copy -- is never put in a batch.
struct jbd2_batch {
  struct gendisk *disk;
  uint64 start; // the disk block of pages[0]
  int n;
  void *pages[BIO_MAXPAGES];
};

static int
jbd2_batch_flush(struct jbd2_batch *bt)
{
  struct bio bio;
  int r;

  if (bt->n == 0)
    return 0;
  memset(&bio, 0, sizeof(bio));
  bio.sector = bt->start * SECTORS_PER_BLOCK;
  bio.nsectors = bt->n * SECTORS_PER_BLOCK;
  bio.npages = bt->n;
  for (int i = 0; i < bt->n; i++)
    bio.pages[i] = bt->pages[i];
  bio.write = 1;
  r = blk_submit_wait(bt->disk, &bio);
  bt->n = 0;
  return r;
}

static int
jbd2_batch_add(struct jbd2_batch *bt, uint64 diskblk, void *page)
{
  if (bt->n > 0 && (diskblk != bt->start + (uint64)bt->n || bt->n == BIO_MAXPAGES)) {
    int r = jbd2_batch_flush(bt);
    if (r < 0)
      return r;
  }
  if (bt->n == 0)
    bt->start = diskblk;
  bt->pages[bt->n++] = page;
  return 0;
}

static int
jbd2_jwrite(struct jbd2 *j, uint64 jblk, void *page)
{
  uint64 d = jbd2_map_block(j, jblk);

  if (d == 0) {
    printk("jbd2: journal block %lu has no disk block\n", jblk);
    return -EIO;
  }
  return blk_write_block(j->disk, d, page);
}

// the same, gathered with whatever came before it
static int
jbd2_jwrite_batch(struct jbd2 *j, struct jbd2_batch *bt, uint64 jblk, void *page)
{
  uint64 d = jbd2_map_block(j, jblk);

  if (d == 0) {
    printk("jbd2: journal block %lu has no disk block\n", jblk);
    return -EIO;
  }
  return jbd2_batch_add(bt, d, page);
}

// ---- checksums over the four kinds of block ----

// a descriptor or a revoke block carries its checksum in the last four
// bytes of the block, over the whole block with those bytes zeroed.
static void
jbd2_desc_csum_set(struct jbd2 *j, void *block)
{
  struct jbd2_journal_block_tail *tail;

  tail = (struct jbd2_journal_block_tail *)((uchar *)block + BLKSIZE - sizeof(*tail));
  tail->t_checksum = 0;
  tail->t_checksum = jbd2_be32(jbd2_crc32c(j->csum_seed, block, BLKSIZE));
}

static int
jbd2_desc_csum_ok(struct jbd2 *j, void *block)
{
  struct jbd2_journal_block_tail *tail;
  uint32 provided, calculated;

  if (!(j->features & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)))
    return 1;
  tail = (struct jbd2_journal_block_tail *)((uchar *)block + BLKSIZE - sizeof(*tail));
  provided = tail->t_checksum;
  tail->t_checksum = 0;
  calculated = jbd2_crc32c(j->csum_seed, block, BLKSIZE);
  tail->t_checksum = provided;
  return provided == jbd2_be32(calculated);
}

// the commit block keeps its checksum in h_chksum[0] instead, and the
// two bytes that used to say which algorithm it was must be zero for the
// sum to come out the same on both sides.
static void
jbd2_commit_csum_set(struct jbd2 *j, void *block)
{
  struct commit_header *h = block;

  h->h_chksum_type = 0;
  h->h_chksum_size = 0;
  h->h_chksum[0] = 0;
  h->h_chksum[0] = jbd2_be32(jbd2_crc32c(j->csum_seed, block, BLKSIZE));
}

static int
jbd2_commit_csum_ok(struct jbd2 *j, void *block)
{
  struct commit_header *h = block;
  uint32 provided, calculated;

  if (!(j->features & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)))
    return 1;
  provided = h->h_chksum[0];
  h->h_chksum[0] = 0;
  calculated = jbd2_crc32c(j->csum_seed, block, BLKSIZE);
  h->h_chksum[0] = provided;
  return provided == jbd2_be32(calculated);
}

// a tag's checksum covers the sequence number and then the data block
// exactly as it goes on the log, which for an escaped block is the copy
// with its first four bytes zeroed.
static uint32
jbd2_tag_csum(struct jbd2 *j, uint32 tid, const void *data)
{
  uint32 seq = jbd2_be32(tid);
  uint32 csum = jbd2_crc32c(j->csum_seed, &seq, sizeof(seq));

  return jbd2_crc32c(csum, data, BLKSIZE);
}

// the journal superblock is the one structure seeded with ~0 rather than
// with the uuid, because it is what the uuid is read from.
static void
jbd2_super_csum_set(struct jbd2 *j, journal_superblock_t *sb)
{
  if (!(j->features & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)))
    return;
  sb->s_checksum = 0;
  sb->s_checksum = jbd2_be32(jbd2_crc32c(0xffffffff, sb, sizeof(*sb)));
}

static int
jbd2_super_csum_ok(struct jbd2 *j, journal_superblock_t *sb)
{
  uint32 provided, calculated;

  if (!(j->features & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)))
    return 1;
  provided = sb->s_checksum;
  sb->s_checksum = 0;
  calculated = jbd2_crc32c(0xffffffff, sb, sizeof(*sb));
  sb->s_checksum = provided;
  return provided == jbd2_be32(calculated);
}

// ---- tags ----

// how many bytes a tag takes. with csum_v3 it is the flat 16-byte tag;
// the older shape is 12 bytes, or 8 when the journal does not carry
// 64-bit block numbers.
static int
jbd2_tag_bytes(uint32 features)
{
  if (features & JBD2_FEATURE_INCOMPAT_CSUM_V3)
    return (int)sizeof(journal_block_tag3_t);
  if (features & JBD2_FEATURE_INCOMPAT_64BIT)
    return (int)sizeof(journal_block_tag_t);
  return (int)sizeof(journal_block_tag_t) - (int)sizeof(uint32);
}

// read one tag. the flags are taken as the big-endian 16-bit value at
// offset 6 in both shapes, because that is where Linux writes them: it
// stores a csum_v3 tag's flags through a pointer to the older tag, so
// the high half of the 32-bit t_flags field is always zero.
static void
jbd2_tag_read(struct jbd2 *j, const void *p, struct jbd2_tag *out)
{
  if (j->features & JBD2_FEATURE_INCOMPAT_CSUM_V3) {
    const journal_block_tag3_t *t = p;
    out->blkno = jbd2_be32(t->t_blocknr);
    out->blkno |= (uint64)jbd2_be32(t->t_blocknr_high) << 32;
    out->flags = jbd2_be32(t->t_flags) & 0xffff;
    out->csum = jbd2_be32(t->t_checksum);
    out->has_csum = 1;
  } else {
    const journal_block_tag_t *t = p;
    out->blkno = jbd2_be32(t->t_blocknr);
    if (j->features & JBD2_FEATURE_INCOMPAT_64BIT)
      out->blkno |= (uint64)jbd2_be32(t->t_blocknr_high) << 32;
    out->flags = jbd2_be16(t->t_flags);
    out->csum = 0;
    out->has_csum = 0;
  }
}

// ---- how much of the log a transaction takes ----

// tags fit in a descriptor block until there is no room for another tag,
// another uuid and the tail checksum. the condition is Linux's, so that
// a descriptor block written here has the same tags in it as one Linux
// would have written for the same transaction.
static int
jbd2_tags_per_desc(struct jbd2 *j)
{
  int csum_size = (int)sizeof(struct jbd2_journal_block_tail);
  int space = BLKSIZE - (int)sizeof(journal_header_t);
  int tags = 0;

  for (;;) {
    space -= j->tag_bytes;
    if (tags == 0)
      space -= 16; // the first tag carries the uuid after it
    tags++;
    if (space < j->tag_bytes + 16 + csum_size)
      return tags;
  }
}

static int
jbd2_revokes_per_block(void)
{
  int csum_size = (int)sizeof(struct jbd2_journal_block_tail);

  return (BLKSIZE - csum_size - (int)sizeof(jbd2_journal_revoke_header_t)) / 8;
}

// the log blocks a transaction of this shape occupies: its data, the
// descriptors that carry its tags, its revoke blocks and its commit
// block.
static int
jbd2_txn_log_blocks(struct jbd2 *j, int nblocks, int nrevokes)
{
  int per_desc = jbd2_tags_per_desc(j);
  int per_revoke = jbd2_revokes_per_block();
  int n;

  if (nblocks == 0 && nrevokes == 0)
    return 0;
  n = nblocks;
  if (nblocks > 0)
    n += (nblocks + per_desc - 1) / per_desc;
  if (nrevokes > 0)
    n += (nrevokes + per_revoke - 1) / per_revoke;
  return n + 1;
}

// ---- the journal superblock ----

// write it with a new start and sequence, and wait for it. the log's
// start is what recovery believes, so it is written after the blocks it
// describes are safe and never before.
static int
jbd2_write_super(struct jbd2 *j, uint64 start, uint32 seq)
{
  journal_superblock_t *sb = j->sbuf;
  int r;

  sb->s_start = jbd2_be32((uint32)start);
  sb->s_sequence = jbd2_be32(seq);
  sb->s_errno = jbd2_be32(j->failed ? (uint32)-EIO : 0);
  jbd2_super_csum_set(j, sb);
  r = jbd2_jwrite(j, 0, j->sbuf);
  if (r < 0)
    return r;
  return blk_flush(j->disk);
}

// ---- recovery ----
//
// three passes over the same stretch of log. scan finds the last
// transaction whose commit block verifies; revoke collects every block
// revoked up to that point, remembering the latest transaction that
// revoked each; replay writes every logged block back to its home unless
// a revoke of the same transaction or a later one says not to.

#define JBD2_PASS_SCAN   0
#define JBD2_PASS_REVOKE 1
#define JBD2_PASS_REPLAY 2

// the revoke table is open-addressed and fixed: a journal that revokes
// more distinct blocks than this in one replay window makes recovery
// fail rather than replay half of it.
#define JBD2_REVOKE_SLOTS 8192
#define JBD2_REVOKE_ORDER 5 // 8192 * 16 bytes is 32 pages

struct jbd2_revoke_ent {
  uint64 blkno;
  uint32 tid;
  uint32 used;
};

_Static_assert(sizeof(struct jbd2_revoke_ent) * JBD2_REVOKE_SLOTS ==
               (1 << JBD2_REVOKE_ORDER) * PGSIZE,
               "the revoke table does not fill its allocation");

struct jbd2_recovery {
  int pass;
  uint32 end_tid; // the first sequence number with no good commit block
  struct jbd2_revoke_ent *revokes;
  int nrevokes;
  uint64 replayed;
};

static uint32
jbd2_revoke_hash(uint64 blkno)
{
  uint64 x = blkno * 0x9e3779b97f4a7c15UL;

  return (uint32)(x >> 48) & (JBD2_REVOKE_SLOTS - 1);
}

// remember that `blkno` was revoked, keeping the latest transaction that
// did so: a later revoke beats an earlier log record, an earlier one
// does not.
static int
jbd2_revoke_set(struct jbd2_recovery *rec, uint64 blkno, uint32 tid)
{
  uint32 h = jbd2_revoke_hash(blkno);

  for (int i = 0; i < JBD2_REVOKE_SLOTS; i++) {
    struct jbd2_revoke_ent *e = &rec->revokes[(h + i) & (JBD2_REVOKE_SLOTS - 1)];
    if (!e->used) {
      if (rec->nrevokes >= JBD2_REVOKE_SLOTS * 3 / 4)
        return -ENOSPC;
      e->used = 1;
      e->blkno = blkno;
      e->tid = tid;
      rec->nrevokes++;
      return 0;
    }
    if (e->blkno == blkno) {
      if (jbd2_tid_gt(tid, e->tid))
        e->tid = tid;
      return 0;
    }
  }
  return -ENOSPC;
}

// whether a block logged by transaction `tid` must be skipped. it must
// be when the revoke is from that same transaction or a later one, which
// is what Linux's jbd2_journal_test_revoke says: a block freed by the
// transaction that also logged it is being freed, not written.
static int
jbd2_revoke_test(struct jbd2_recovery *rec, uint64 blkno, uint32 tid)
{
  uint32 h = jbd2_revoke_hash(blkno);

  if (rec->revokes == 0)
    return 0;
  for (int i = 0; i < JBD2_REVOKE_SLOTS; i++) {
    struct jbd2_revoke_ent *e = &rec->revokes[(h + i) & (JBD2_REVOKE_SLOTS - 1)];
    if (!e->used)
      return 0;
    if (e->blkno == blkno)
      return !jbd2_tid_gt(tid, e->tid);
  }
  return 0;
}

// put one logged block back where it belongs. it goes through the block
// cache rather than straight to the disk so that a copy already cached
// cannot shadow what was just replayed.
static int
jbd2_replay_block(struct jbd2 *j, uint64 blkno, const void *data)
{
  struct blkbuf *b = blk_get_zero(j->disk, blkno);

  if (b == 0)
    return -ENOMEM;
  memmove(b->data, data, BLKSIZE);
  blk_mark_dirty(b);
  blk_put(b);
  return 0;
}

// the records in one revoke block. r_count is the number of bytes of the
// block in use, the header included, and it is checked against the block
// before a single record is read out of it.
static int
jbd2_scan_revokes(struct jbd2 *j, struct jbd2_recovery *rec, void *block, uint32 tid)
{
  jbd2_journal_revoke_header_t *rh = block;
  int csum_size = 0;
  int record_len = (j->features & JBD2_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
  int offset = (int)sizeof(*rh);
  uint32 rcount = jbd2_be32(rh->r_count);
  int max;

  if (j->features & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))
    csum_size = (int)sizeof(struct jbd2_journal_block_tail);
  if (rcount > (uint32)(BLKSIZE - csum_size) || rcount < sizeof(*rh)) {
    printk("jbd2: revoke block of transaction %u claims %u bytes\n", tid, rcount);
    return -EIO;
  }
  max = (int)rcount;
  while (offset + record_len <= max) {
    uint64 blkno;
    int r;

    if (record_len == 4)
      blkno = jbd2_be32(*(uint32 *)((uchar *)block + offset));
    else
      blkno = jbd2_be64(*(uint64 *)((uchar *)block + offset));
    offset += record_len;
    r = jbd2_revoke_set(rec, blkno, tid);
    if (r < 0) {
      printk("jbd2: too many revoked blocks to recover\n");
      return r;
    }
  }
  return 0;
}

// one pass. it starts at the block the journal superblock points at,
// expecting the sequence number the journal superblock gives, and stops
// at the first block that is not the next block of the log: a wrong
// magic, a wrong sequence, an unknown type or a checksum that does not
// hold. the loop is bounded by the length of the log, so a journal that
// points at itself stops rather than spins.
static int
jbd2_do_pass(struct jbd2 *j, struct jbd2_recovery *rec, uint64 start_block, uint32 start_tid)
{
  uint64 loglen = j->maxlen - j->first;
  uint64 next = start_block;
  uint64 consumed = 0;
  uint32 tid = start_tid;
  int csum_size = 0;
  int r = 0;

  if (j->features & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))
    csum_size = (int)sizeof(struct jbd2_journal_block_tail);

  while (consumed < loglen) {
    journal_header_t *hdr;
    uint32 type;

    // the later passes stop where the scan said the log ended
    if (rec->pass != JBD2_PASS_SCAN && !jbd2_tid_gt(rec->end_tid, tid))
      break;

    r = jbd2_jread(j, next, j->rbuf);
    if (r < 0)
      return r;
    consumed++;
    next = jbd2_log_next(j, next);

    hdr = j->rbuf;
    if (jbd2_be32(hdr->h_magic) != JBD2_MAGIC_NUMBER)
      break;
    if (jbd2_be32(hdr->h_sequence) != tid)
      break;
    type = jbd2_be32(hdr->h_blocktype);

    if (type == JBD2_SUPERBLOCK_V1 || type == JBD2_SUPERBLOCK_V2)
      continue; // a copy of the superblock in the log: not a transaction

    if (type == JBD2_COMMIT_BLOCK) {
      // the one block that decides whether the transaction happened
      if (!jbd2_commit_csum_ok(j, j->rbuf)) {
        printk("jbd2: transaction %u has a bad commit block; the log ends here\n", tid);
        break;
      }
      tid++;
      if (rec->pass == JBD2_PASS_SCAN)
        rec->end_tid = tid;
      continue;
    }

    if (type == JBD2_REVOKE_BLOCK) {
      if (!jbd2_desc_csum_ok(j, j->rbuf)) {
        printk("jbd2: transaction %u has a bad revoke block; the log ends here\n", tid);
        break;
      }
      if (rec->pass == JBD2_PASS_REVOKE) {
        r = jbd2_scan_revokes(j, rec, j->rbuf, tid);
        if (r < 0)
          return r;
      }
      continue;
    }

    if (type != JBD2_DESCRIPTOR_BLOCK)
      break; // a type we do not know: treat it as the end of the log

    if (!jbd2_desc_csum_ok(j, j->rbuf)) {
      printk("jbd2: transaction %u has a bad descriptor block; the log ends here\n", tid);
      break;
    }

    {
      uchar *tagp = (uchar *)j->rbuf + sizeof(journal_header_t);
      uchar *limit = (uchar *)j->rbuf + BLKSIZE - csum_size;

      while (tagp + j->tag_bytes <= limit) {
        struct jbd2_tag tag;
        uint64 datablk = next;

        if (consumed >= loglen) {
          printk("jbd2: transaction %u runs off the end of the log\n", tid);
          return -EIO;
        }
        jbd2_tag_read(j, tagp, &tag);
        consumed++;
        next = jbd2_log_next(j, next);

        if (rec->pass == JBD2_PASS_REPLAY) {
          // a home block number out of the log is never believed
          if (tag.blkno >= j->devblocks) {
            printk("jbd2: transaction %u logs block %lu, past the end of the device\n",
                   tid, tag.blkno);
            return -EIO;
          }
          r = jbd2_jread(j, datablk, j->dbuf);
          if (r < 0)
            return r;
          // the tag covers the block as it lies on the log, so it is
          // checked before the escaped magic is put back
          if (tag.has_csum && jbd2_tag_csum(j, tid, j->dbuf) != tag.csum) {
            printk("jbd2: transaction %u has a bad checksum on block %lu\n", tid, tag.blkno);
            return -EIO;
          }
          if (!jbd2_revoke_test(rec, tag.blkno, tid)) {
            if (tag.flags & JBD2_FLAG_ESCAPE)
              *(uint32 *)j->dbuf = jbd2_be32(JBD2_MAGIC_NUMBER);
            r = jbd2_replay_block(j, tag.blkno, j->dbuf);
            if (r < 0)
              return r;
            rec->replayed++;
          }
        }

        tagp += j->tag_bytes;
        if (!(tag.flags & JBD2_FLAG_SAME_UUID))
          tagp += 16; // the first tag of a descriptor block is followed by the uuid
        if (tag.flags & JBD2_FLAG_LAST_TAG)
          break;
      }
    }
  }
  if (rec->pass == JBD2_PASS_SCAN)
    rec->end_tid = tid;
  return 0;
}

int
jbd2_recover(struct jbd2 *j)
{
  journal_superblock_t *sb;
  struct jbd2_recovery rec;
  uint64 start;
  uint32 start_tid;
  uint now;
  int r = 0;

  if (j == 0)
    return -EINVAL;
  if (j->recovered)
    return 0;

  sb = j->sbuf;
  start = jbd2_be32(sb->s_start);
  start_tid = jbd2_be32(sb->s_sequence);
  memset(&rec, 0, sizeof(rec));
  rec.end_tid = start_tid;

  if (start == 0) {
    // a journal with no start is a journal that was emptied on the way
    // out: the file system was unmounted cleanly and there is nothing to
    // replay. this is not an error.
    printk("jbd2: journal is clean at sequence %u\n", start_tid);
  } else if (start < j->first || start >= j->maxlen) {
    printk("jbd2: journal superblock says the log starts at %lu, outside %lu..%lu\n",
           start, j->first, j->maxlen - 1);
    return -EIO;
  } else {
    rec.pass = JBD2_PASS_SCAN;
    r = jbd2_do_pass(j, &rec, start, start_tid);
    if (r < 0)
      return r;

    if (rec.end_tid != start_tid) {
      rec.revokes = kalloc_order(JBD2_REVOKE_ORDER);
      if (rec.revokes == 0)
        return -ENOMEM;
      memset(rec.revokes, 0, (1 << JBD2_REVOKE_ORDER) * PGSIZE);

      rec.pass = JBD2_PASS_REVOKE;
      r = jbd2_do_pass(j, &rec, start, start_tid);
      if (r == 0) {
        rec.pass = JBD2_PASS_REPLAY;
        r = jbd2_do_pass(j, &rec, start, start_tid);
      }
      kfree_order(rec.revokes, JBD2_REVOKE_ORDER);
      rec.revokes = 0;
      if (r < 0)
        return r;

      // the replayed blocks are in the cache and dirty; they have to be
      // on the disk before the journal superblock stops pointing at the
      // log that holds them.
      r = blk_sync(j->disk);
      if (r < 0)
        return r;
      r = blk_flush(j->disk);
      if (r < 0)
        return r;
      printk("jbd2: replayed %lu blocks, transactions %u..%u\n",
             rec.replayed, start_tid, rec.end_tid - 1);
    } else {
      printk("jbd2: nothing on the log after sequence %u\n", start_tid);
    }
  }

  // restart at the sequence after the last one seen, leaving a gap so
  // that a commit block still lying on the log cannot be mistaken for
  // one of ours.
  j->next_tid = rec.end_tid + 1;
  if (j->next_tid == 0)
    j->next_tid = 1;

  // the log is ours and empty now, so this is the moment to write the
  // format this driver writes: revokes, 64-bit tags, csum_v3.
  sb->s_header.h_blocktype = jbd2_be32(JBD2_SUPERBLOCK_V2);
  sb->s_feature_compat = 0;
  sb->s_feature_incompat = jbd2_be32(JBD2_FEATURES_WRITTEN);
  sb->s_feature_ro_compat = 0;
  sb->s_checksum_type = JBD2_CRC32C_CHKSUM;
  sb->s_nr_users = jbd2_be32(1);
  j->features = JBD2_FEATURES_WRITTEN;
  j->tag_bytes = jbd2_tag_bytes(j->features);

  j->head = j->first;
  j->txn.tid = j->next_tid;

  // the superblock points at where the next transaction will start, so
  // that a crash between now and its commit block finds a log that
  // begins with a torn transaction and replays nothing.
  r = jbd2_write_super(j, j->head, j->next_tid);
  if (r < 0)
    return r;

  now = jbd2_now();
  acquire(&j->lock);
  j->recovered = 1;
  j->last_commit_tick = now;
  release(&j->lock);
  return (int)(rec.replayed > 0x7fffffff ? 0x7fffffff : rec.replayed);
}

// ---- writing a transaction to the log ----

// write the descriptor blocks, the data blocks and the revoke blocks,
// then the commit block, with the flush between them that makes the
// commit block mean something. the caller has closed the transaction, so
// nothing here may change under us. `*head_out` is where the log
// continues afterwards.
static int
jbd2_write_txn(struct jbd2 *j, uint32 tid, uint64 *head_out)
{
  struct jbd2_txn *t = &j->txn;
  int csum_size = (int)sizeof(struct jbd2_journal_block_tail);
  uint64 blk = j->head;
  uint64 ns;
  struct commit_header *ch;
  struct jbd2_batch bt;
  int i = 0, k = 0;
  int r;

  memset(&bt, 0, sizeof(bt));
  bt.disk = j->disk;

  while (i < t->nblocks) {
    journal_header_t *dh;
    journal_block_tag3_t *tag = 0;
    uchar *tagp;
    uint64 descblk = blk;
    int space_left;
    int first_tag = 1;

    // the descriptor block is written last of its group even though it
    // comes first on the log. nothing on the log means anything until
    // the commit block, and one flush covers the whole group, so the
    // order the blocks are submitted in does not matter.
    blk = jbd2_log_next(j, blk);

    memset(j->dbuf, 0, BLKSIZE);
    dh = j->dbuf;
    dh->h_magic = jbd2_be32(JBD2_MAGIC_NUMBER);
    dh->h_blocktype = jbd2_be32(JBD2_DESCRIPTOR_BLOCK);
    dh->h_sequence = jbd2_be32(tid);

    tagp = (uchar *)j->dbuf + sizeof(journal_header_t);
    space_left = BLKSIZE - (int)sizeof(journal_header_t);

    while (i < t->nblocks) {
      struct blkbuf *b = t->blocks[i];
      void *data = b->data;
      uint32 flags = first_tag ? 0 : JBD2_FLAG_SAME_UUID;

      // a block that begins with the jbd2 magic would look like a log
      // block to recovery, so it goes out with those four bytes zeroed
      // and the escape flag set. replay puts the magic back.
      if (*(uint32 *)data == jbd2_be32(JBD2_MAGIC_NUMBER)) {
        memmove(j->ebuf, data, BLKSIZE);
        *(uint32 *)j->ebuf = 0;
        data = j->ebuf;
        flags |= JBD2_FLAG_ESCAPE;
      }

      tag = (journal_block_tag3_t *)tagp;
      tag->t_blocknr = jbd2_be32((uint32)b->blkno);
      tag->t_blocknr_high = jbd2_be32((uint32)(b->blkno >> 32));
      tag->t_flags = jbd2_be32(flags);
      tag->t_checksum = jbd2_be32(jbd2_tag_csum(j, tid, data));

      if (data == j->ebuf) {
        // the escaped copy is one buffer reused for every escaped
        // block, so it cannot wait in a batch
        r = jbd2_batch_flush(&bt);
        if (r == 0)
          r = jbd2_jwrite(j, blk, data);
      } else {
        r = jbd2_jwrite_batch(j, &bt, blk, data);
      }
      if (r < 0)
        return r;
      j->nblocks_written++;
      blk = jbd2_log_next(j, blk);
      i++;

      tagp += j->tag_bytes;
      space_left -= j->tag_bytes;
      if (first_tag) {
        memmove(tagp, j->uuid, 16);
        tagp += 16;
        space_left -= 16;
        first_tag = 0;
      }
      if (space_left < j->tag_bytes + 16 + csum_size)
        break;
    }

    tag->t_flags = jbd2_be32(jbd2_be32(tag->t_flags) | JBD2_FLAG_LAST_TAG);
    jbd2_desc_csum_set(j, j->dbuf);
    // the descriptor is built in one buffer that the next group reuses,
    // so the blocks it describes go out first and it goes out alone.
    r = jbd2_batch_flush(&bt);
    if (r == 0)
      r = jbd2_jwrite(j, descblk, j->dbuf);
    if (r < 0)
      return r;
    j->nblocks_written++;
  }

  // the revokes of this transaction, which must be on the log before its
  // commit block for replay to honour them.
  while (k < t->nrevokes) {
    jbd2_journal_revoke_header_t *rh;
    int off;

    memset(j->mbuf, 0, BLKSIZE);
    rh = j->mbuf;
    rh->r_header.h_magic = jbd2_be32(JBD2_MAGIC_NUMBER);
    rh->r_header.h_blocktype = jbd2_be32(JBD2_REVOKE_BLOCK);
    rh->r_header.h_sequence = jbd2_be32(tid);
    off = (int)sizeof(*rh);
    while (k < t->nrevokes && off + 8 <= BLKSIZE - csum_size) {
      *(uint64 *)((uchar *)j->mbuf + off) = jbd2_be64(t->revokes[k++]);
      off += 8;
    }
    rh->r_count = jbd2_be32((uint32)off);
    jbd2_desc_csum_set(j, j->mbuf);
    r = jbd2_jwrite(j, blk, j->mbuf);
    if (r < 0)
      return r;
    j->nblocks_written++;
    blk = jbd2_log_next(j, blk);
  }

  // everything the commit block commits must be on the disk before it is
  //
  r = blk_flush(j->disk);
  if (r < 0)
    return r;

  memset(j->mbuf, 0, BLKSIZE);
  ch = j->mbuf;
  ch->h_magic = jbd2_be32(JBD2_MAGIC_NUMBER);
  ch->h_blocktype = jbd2_be32(JBD2_COMMIT_BLOCK);
  ch->h_sequence = jbd2_be32(tid);
  ns = clock_realtime_ns();
  ch->h_commit_sec = jbd2_be64(ns / 1000000000UL);
  ch->h_commit_nsec = jbd2_be32((uint32)(ns % 1000000000UL));
  jbd2_commit_csum_set(j, j->mbuf);
  r = jbd2_jwrite(j, blk, j->mbuf);
  if (r < 0)
    return r;
  j->nblocks_written++;
  blk = jbd2_log_next(j, blk);

  // and the commit block itself must be on the disk before anything is
  // written to a home location on the strength of it
  r = blk_flush(j->disk);
  if (r < 0)
    return r;

  *head_out = blk;
  return 0;
}

// write every block of the committed transaction to the place it really
// lives. this is checkpointing, and it is what lets the log's start move
// forward. the blocks go straight to the disk rather than through
// blk_sync, because a caller may be holding a buffer's lock while it
// waits for this commit and blk_sync would wait for that lock.
static int
jbd2_checkpoint(struct jbd2 *j)
{
  struct jbd2_txn *t = &j->txn;
  int r;

  // in order of block number, so that the ones that lie next to each
  // other on the disk go in one request. the array's order means
  // nothing after this: the caller only walks it to clear a flag.
  for (int i = 1; i < t->nblocks; i++) {
    struct blkbuf *v = t->blocks[i];
    int k = i - 1;
    while (k >= 0 && t->blocks[k]->blkno > v->blkno) {
      t->blocks[k + 1] = t->blocks[k];
      k--;
    }
    t->blocks[k + 1] = v;
  }

  struct jbd2_batch bt;
  memset(&bt, 0, sizeof(bt));
  bt.disk = j->disk;
  for (int i = 0; i < t->nblocks; i++) {
    struct blkbuf *b = t->blocks[i];
    r = jbd2_batch_add(&bt, b->blkno, b->data);
    if (r < 0)
      return r;
  }
  r = jbd2_batch_flush(&bt);
  if (r < 0)
    return r;
  return blk_flush(j->disk);
}

// the whole of a commit: close the transaction, write it, checkpoint it,
// and only then tell the journal superblock that the log has moved on.
static int
jbd2_do_commit(struct jbd2 *j)
{
  uint32 tid;
  uint64 newhead;
  uint now;
  int r;

  acquire(&j->lock);

  // one commit at a time. a second caller waits for the first, and by
  // the time it wakes its own blocks are on the log too.
  while (j->committing) {
    sleep_prepare(j);
    release(&j->lock);
    sleep();
    acquire(&j->lock);
  }
  if (j->failed) {
    release(&j->lock);
    return -EIO;
  }
  if (j->txn.nblocks == 0 && j->txn.nrevokes == 0) {
    j->commit_request = 0;
    release(&j->lock);
    return 0;
  }

  // close the transaction to new handles and let the open ones finish.
  // waiting is what makes it safe to log the blocks where they lie:
  // nothing may modify a block while it is being written to the log.
  j->committing = 1;
  while (j->nhandles > 0) {
    sleep_prepare(j);
    release(&j->lock);
    sleep();
    acquire(&j->lock);
  }
  tid = j->txn.tid;
  release(&j->lock);

  // data=ordered: the file data goes to its home blocks, and to the
  // disk, before anything of this transaction reaches the log. after a
  // crash the metadata that names a block is never newer than the
  // block: a file that was being extended comes back short, not
  // pointing at whatever those blocks held before.
  //
  // what is written here is every dirty block the journal does not
  // own, not only the ones this transaction's handles touched. that is
  // a superset of what ordering requires, and it is the same work the
  // writeback thread does on the same five-second interval.
  if (j->ordered) {
    r = blk_sync(j->disk);
    if (r < 0) {
      acquire(&j->lock);
      j->committing = 0;
      release(&j->lock);
      wakeup(j);
      return r;
    }
  }

  newhead = j->head;
  r = jbd2_write_txn(j, tid, &newhead);
  if (r == 0)
    r = jbd2_checkpoint(j);
  if (r == 0) {
    // the blocks are home, so the log no longer has to hold them: point
    // the superblock at where the next transaction will start.
    j->head = newhead;
    j->next_tid = tid + 1;
    if (j->next_tid == 0)
      j->next_tid = 1;
    r = jbd2_write_super(j, j->head, j->next_tid);
  }
  now = jbd2_now();

  acquire(&j->lock);
  if (r < 0) {
    // the log is no longer a record of anything, so no further
    // transaction may be started against it.
    j->failed = 1;
    printk("jbd2: commit of transaction %u failed (%d); the journal is stopped\n", tid, r);
  } else {
    j->ntransactions++;
    j->txn.tid = j->next_tid;
  }
  for (int i = 0; i < j->txn.nblocks; i++)
    j->txn.blocks[i]->flags &= ~BLK_JBD2_JOURNALED;
  j->txn.nblocks = 0;
  j->txn.nrevokes = 0;
  j->txn.reserved = 0; // no handle is open
  j->committing = 0;
  j->commit_request = 0;
  j->last_commit_tick = now;
  release(&j->lock);
  wakeup(j);
  return r;
}

int
jbd2_commit(struct jbd2 *j)
{
  if (j == 0)
    return -EINVAL;
  acquire(&j->lock);
  j->commit_request = 1;
  release(&j->lock);
  wakeup(j);
  return jbd2_do_commit(j);
}

// ---- handles ----

// whether a handle asking for `credits` more blocks still fits. every
// transaction fits the log by construction -- max_txn_blocks and
// max_revokes were cut down at open until it did -- so this only has to
// count blocks.
static int
jbd2_fits(struct jbd2 *j, int credits)
{
  return j->txn.nblocks + j->txn.reserved + credits <= j->max_txn_blocks;
}

struct jbd2_handle *
jbd2_start(struct jbd2 *j, int credits)
{
  struct jbd2_handle *h;

  if (j == 0 || credits <= 0)
    return 0;
  if (credits > JBD2_MAX_TXN_BLOCKS) {
    printk("jbd2: a handle asked for %d credits, more than a transaction holds\n", credits);
    return 0;
  }

  acquire(&j->lock);
  // bounded so that a journal that can never satisfy the request gives
  // up instead of looping
  for (int tries = 0; tries < 1000; tries++) {
    if (!j->recovered || j->failed) {
      release(&j->lock);
      return 0;
    }
    if (!j->committing && credits > j->max_txn_blocks) {
      release(&j->lock);
      printk("jbd2: a handle asked for %d credits; this log holds %d\n",
             credits, j->max_txn_blocks);
      return 0;
    }
    if (!j->committing && jbd2_fits(j, credits)) {
      h = 0;
      for (int i = 0; i < JBD2_MAX_HANDLES; i++) {
        if (!j->handles[i].inuse) {
          h = &j->handles[i];
          break;
        }
      }
      if (h) {
        h->j = j;
        h->credits = credits;
        h->used = 0;
        h->inuse = 1;
        j->txn.reserved += credits;
        j->nhandles++;
        release(&j->lock);
        return h;
      }
    } else if (!j->committing) {
      // the running transaction is too full for this handle: put it on
      // the log to make room, then look again.
      release(&j->lock);
      jbd2_do_commit(j);
      acquire(&j->lock);
      continue;
    }
    sleep_prepare(j);
    release(&j->lock);
    sleep();
    acquire(&j->lock);
  }
  release(&j->lock);
  printk("jbd2: gave up waiting for room for a handle\n");
  return 0;
}

int
jbd2_stop(struct jbd2_handle *h)
{
  struct jbd2 *j;

  if (h == 0 || !h->inuse)
    return -EINVAL;
  j = h->j;
  acquire(&j->lock);
  // hand back the credits this handle promised to use and did not
  j->txn.reserved -= h->credits - h->used;
  if (j->txn.reserved < 0)
    j->txn.reserved = 0;
  j->nhandles--;
  h->inuse = 0;
  release(&j->lock);
  wakeup(j);
  return 0;
}

// is this block already part of the running transaction? the caller
// holds j->lock.
static int
jbd2_in_txn(struct jbd2 *j, struct blkbuf *b)
{
  for (int i = 0; i < j->txn.nblocks; i++)
    if (j->txn.blocks[i] == b)
      return 1;
  return 0;
}

int
jbd2_get_write_access(struct jbd2_handle *h, struct blkbuf *b)
{
  struct jbd2 *j;

  if (h == 0 || !h->inuse || b == 0)
    return -EINVAL;
  j = h->j;
  if (b->disk != j->disk)
    return -EINVAL;

  acquire(&j->lock);
  if (j->failed) {
    release(&j->lock);
    return -EIO;
  }
  // a commit cannot be running: it waits for every handle to stop before
  // it writes anything, and no handle may start while it does. so a
  // block in this transaction is a block nothing else is writing out.
  if (jbd2_in_txn(j, b)) {
    release(&j->lock);
    return 0;
  }
  if (h->used >= h->credits) {
    release(&j->lock);
    printk("jbd2: a handle dirtied more than the %d blocks it asked for\n", h->credits);
    return -ENOSPC;
  }
  if (j->txn.nblocks >= j->max_txn_blocks) {
    release(&j->lock);
    return -ENOSPC;
  }
  j->txn.blocks[j->txn.nblocks++] = b;
  j->txn.reserved--;
  h->used++;
  // set before the block is marked dirty, so that a writeback that sees
  // the block dirty also sees that the journal owns it
  b->flags |= BLK_JBD2_JOURNALED;
  release(&j->lock);

  // the cache reclaims only blocks that are clean and unreferenced, so
  // marking it dirty is also what keeps it from being reused under the
  // transaction that is about to log it.
  blk_mark_dirty(b);
  return 0;
}

int
jbd2_dirty_metadata(struct jbd2_handle *h, struct blkbuf *b)
{
  struct jbd2 *j;
  int found;

  if (h == 0 || !h->inuse || b == 0)
    return -EINVAL;
  j = h->j;

  acquire(&j->lock);
  if (j->failed) {
    release(&j->lock);
    return -EIO;
  }
  found = jbd2_in_txn(j, b);
  release(&j->lock);
  if (!found) {
    printk("jbd2: block %lu was dirtied without jbd2_get_write_access\n", b->blkno);
    return -EINVAL;
  }
  blk_mark_dirty(b);
  return 0;
}

int
jbd2_revoke(struct jbd2_handle *h, uint64 blkno)
{
  struct jbd2 *j;

  if (h == 0 || !h->inuse)
    return -EINVAL;
  j = h->j;
  if (blkno >= j->devblocks)
    return -EINVAL;

  acquire(&j->lock);
  if (j->failed) {
    release(&j->lock);
    return -EIO;
  }
  // the block is being freed, so what this transaction was going to log
  // about it is pointless: drop it rather than log it and revoke it in
  // the same breath.
  for (int i = 0; i < j->txn.nblocks; i++) {
    if (j->txn.blocks[i]->blkno == blkno) {
      j->txn.blocks[i]->flags &= ~BLK_JBD2_JOURNALED;
      j->txn.blocks[i] = j->txn.blocks[--j->txn.nblocks];
      break;
    }
  }
  for (int i = 0; i < j->txn.nrevokes; i++) {
    if (j->txn.revokes[i] == blkno) {
      release(&j->lock);
      return 0;
    }
  }
  if (j->txn.nrevokes >= j->max_revokes) {
    release(&j->lock);
    return -ENOSPC;
  }
  j->txn.revokes[j->txn.nrevokes++] = blkno;
  release(&j->lock);
  return 0;
}

// ---- the commit thread ----

// kjournald. it wakes on every timer tick, which is cheap, and commits
// when the interval has passed, when someone asked, or when the running
// transaction has grown past a quarter of the log. waking on the tick
// rather than on a channel of our own is what gives the interval: this
// kernel has no timed sleep.
static void
jbd2_kjournald(void *arg)
{
  struct jbd2 *j = arg;

  for (;;) {
    uint now = jbd2_now();
    int go = 0;

    acquire(&j->lock);
    if (j->stopping) {
      release(&j->lock);
      break;
    }
    if (j->recovered && !j->failed && !j->committing &&
        (j->txn.nblocks > 0 || j->txn.nrevokes > 0)) {
      int loglen = (int)(j->maxlen - j->first);
      int used = jbd2_txn_log_blocks(j, j->txn.nblocks, j->txn.nrevokes);
      if (j->commit_request)
        go = 1;
      else if (now - j->last_commit_tick >= (uint)(JBD2_COMMIT_SECS * tick_hz))
        go = 1;
      else if (used * 4 >= loglen)
        go = 1;
    }
    release(&j->lock);

    if (go) {
      jbd2_do_commit(j);
      continue;
    }
    acquire(&tickslock);
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
  }

  acquire(&j->lock);
  j->thread_running = 0;
  release(&j->lock);
  wakeup(j);
}

// ---- open and close ----

static void
jbd2_free(struct jbd2 *j)
{
  if (j->sbuf)
    kfree(j->sbuf);
  if (j->dbuf)
    kfree(j->dbuf);
  if (j->mbuf)
    kfree(j->mbuf);
  if (j->ebuf)
    kfree(j->ebuf);
  if (j->rbuf)
    kfree(j->rbuf);
  kfree_order(j, JBD2_ORDER);
}

// cut the size of a transaction down until one of them, with its
// descriptors, its revokes and its commit block, fits in this log.
static int
jbd2_size_transactions(struct jbd2 *j)
{
  int loglen = (int)(j->maxlen - j->first);

  j->max_txn_blocks = JBD2_MAX_TXN_BLOCKS;
  j->max_revokes = JBD2_MAX_REVOKES;
  while (jbd2_txn_log_blocks(j, j->max_txn_blocks, j->max_revokes) > loglen) {
    if (j->max_txn_blocks <= 1 && j->max_revokes <= 1)
      return -1;
    if (j->max_txn_blocks > 1)
      j->max_txn_blocks /= 2;
    if (j->max_revokes > 1)
      j->max_revokes /= 2;
  }
  return 0;
}

struct jbd2 *
jbd2_open(struct gendisk *disk, uint64 (*map)(void *priv, uint64 jblk), void *priv,
          uint64 nblocks, const unsigned char uuid[16])
{
  struct jbd2 *j;
  journal_superblock_t *sb;
  uint32 type, known;
  uint64 maxlen, first;

  if (disk == 0 || map == 0) {
    printk("jbd2: no disk or no block mapping\n");
    return 0;
  }
  if (nblocks < JBD2_MIN_JOURNAL_BLOCKS) {
    printk("jbd2: a journal of %lu blocks is too short\n", nblocks);
    return 0;
  }

  j = kalloc_order(JBD2_ORDER);
  if (j == 0)
    return 0;
  memset(j, 0, sizeof(*j));
  initlock(&j->lock, "jbd2");
  j->disk = disk;
  j->map = map;
  j->priv = priv;
  j->devblocks = disk->nsectors / SECTORS_PER_BLOCK;
  j->maxlen = nblocks;
  j->first = 1;

  j->sbuf = kalloc();
  j->dbuf = kalloc();
  j->mbuf = kalloc();
  j->ebuf = kalloc();
  j->rbuf = kalloc();
  if (j->sbuf == 0 || j->dbuf == 0 || j->mbuf == 0 || j->ebuf == 0 || j->rbuf == 0) {
    jbd2_free(j);
    return 0;
  }

  if (jbd2_jread(j, 0, j->sbuf) < 0) {
    jbd2_free(j);
    return 0;
  }
  sb = j->sbuf;

  if (jbd2_be32(sb->s_header.h_magic) != JBD2_MAGIC_NUMBER) {
    printk("jbd2: no journal superblock in journal block 0\n");
    jbd2_free(j);
    return 0;
  }
  type = jbd2_be32(sb->s_header.h_blocktype);
  if (type != JBD2_SUPERBLOCK_V1 && type != JBD2_SUPERBLOCK_V2) {
    printk("jbd2: journal block 0 is of type %u, not a superblock\n", type);
    jbd2_free(j);
    return 0;
  }
  if (jbd2_be32(sb->s_blocksize) != BLKSIZE) {
    printk("jbd2: the journal's block size is %u, not %d\n", jbd2_be32(sb->s_blocksize), BLKSIZE);
    jbd2_free(j);
    return 0;
  }
  maxlen = jbd2_be32(sb->s_maxlen);
  first = jbd2_be32(sb->s_first);
  if (maxlen < JBD2_MIN_JOURNAL_BLOCKS || maxlen > nblocks || first < 1 || first >= maxlen) {
    printk("jbd2: the journal says maxlen %lu first %lu, in a file of %lu blocks\n",
           maxlen, first, nblocks);
    jbd2_free(j);
    return 0;
  }
  j->maxlen = maxlen;
  j->first = first;

  // a version 1 superblock has no feature words at all, so it is treated
  // as a journal with none of them set.
  j->features = (type == JBD2_SUPERBLOCK_V2) ? jbd2_be32(sb->s_feature_incompat) : 0;
  known = JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT |
          JBD2_FEATURE_INCOMPAT_CSUM_V3;
  if (j->features & ~known) {
    // csum_v2, async_commit and fast_commit each change what the blocks
    // on the log mean. replaying one of them by guessing at the layout
    // would put the wrong bytes in the right places, which is worse than
    // refusing, so we refuse and the file system stays read-only.
    printk("jbd2: the journal uses incompat features 0x%x that this driver will not replay\n",
           j->features & ~known);
    jbd2_free(j);
    return 0;
  }

  // the seed is taken from the journal superblock's own uuid, as Linux
  // takes it, so that a journal written elsewhere checks out here.
  j->csum_seed = jbd2_crc32c(0xffffffff, sb->s_uuid, 16);
  memmove(j->uuid, sb->s_uuid, 16);
  if (uuid && memcmp(uuid, sb->s_uuid, 16) != 0)
    printk("jbd2: the journal's uuid is not the file system's; using the journal's\n");

  if ((j->features & JBD2_FEATURE_INCOMPAT_CSUM_V3) &&
      jbd2_be32(sb->s_header.h_blocktype) == JBD2_SUPERBLOCK_V2) {
    if (sb->s_checksum_type != JBD2_CRC32C_CHKSUM) {
      printk("jbd2: the journal asks for checksum type %d, not crc32c\n", sb->s_checksum_type);
      jbd2_free(j);
      return 0;
    }
    if (!jbd2_super_csum_ok(j, sb)) {
      printk("jbd2: the journal superblock's own checksum is wrong\n");
      jbd2_free(j);
      return 0;
    }
  }

  j->tag_bytes = jbd2_tag_bytes(j->features);
  if (jbd2_size_transactions(j) < 0) {
    printk("jbd2: a log of %lu blocks cannot hold a transaction\n", maxlen - first);
    jbd2_free(j);
    return 0;
  }

  j->head = j->first;
  j->next_tid = jbd2_be32(sb->s_sequence);
  if (j->next_tid == 0)
    j->next_tid = 1;
  j->txn.tid = j->next_tid;
  j->last_commit_tick = jbd2_now();

  j->thread_running = 1;
  if (kthread_create(jbd2_kjournald, j, "kjournald") < 0) {
    printk("jbd2: could not start the commit thread\n");
    j->thread_running = 0;
    jbd2_free(j);
    return 0;
  }

  printk("jbd2: %s journal, %lu blocks from %lu, sequence %u, features 0x%x\n",
         disk->name, maxlen, first, j->next_tid, j->features);
  return j;
}

void
jbd2_close(struct jbd2 *j)
{
  if (j == 0)
    return;

  // stop the commit thread first: nothing else may commit while we do
  acquire(&j->lock);
  j->stopping = 1;
  while (j->thread_running) {
    sleep_prepare(j);
    release(&j->lock);
    sleep();
    acquire(&j->lock);
  }
  release(&j->lock);

  // the last transaction, and then a journal superblock with no start,
  // which is how a journal says the file system came down cleanly and
  // there is nothing to replay.
  if (j->recovered && !j->failed) {
    jbd2_do_commit(j);
    if (!j->failed)
      jbd2_write_super(j, 0, j->next_tid);
  }

  acquire(&j->lock);
  for (int i = 0; i < j->txn.nblocks; i++)
    j->txn.blocks[i]->flags &= ~BLK_JBD2_JOURNALED;
  j->txn.nblocks = 0;
  release(&j->lock);

  jbd2_free(j);
}

// ---- what the file system asks ----

// data=ordered on or off. the file system decides; the journal only
// has to know when to write the data out.
void
jbd2_set_ordered(struct jbd2 *j, int on)
{
  if (j == 0)
    return;
  acquire(&j->lock);
  j->ordered = on;
  release(&j->lock);
}

int
jbd2_is_journaled(struct jbd2 *j, struct blkbuf *b)
{
  // no lock: the writeback path calls this while it holds the block
  // cache's lock, and taking the journal's lock here would order the two
  // the opposite way round from every other path. the flag is set before
  // the block is marked dirty and cleared after the block is safe, and
  // the cache's own lock is what publishes the pair.
  if (j == 0 || b == 0 || b->disk != j->disk)
    return 0;
  return (b->flags & BLK_JBD2_JOURNALED) != 0;
}

void
jbd2_stats(struct jbd2 *j, uint64 *transactions, uint64 *blocks_written, int *free_blocks)
{
  if (j == 0)
    return;
  acquire(&j->lock);
  if (transactions)
    *transactions = j->ntransactions;
  if (blocks_written)
    *blocks_written = j->nblocks_written;
  if (free_blocks) {
    // the log is emptied by every commit, so what is not free is what
    // the running transaction will take when it is written.
    int loglen = (int)(j->maxlen - j->first);
    int used = jbd2_txn_log_blocks(j, j->txn.nblocks, j->txn.nrevokes);
    *free_blocks = loglen > used ? loglen - used : 0;
  }
  release(&j->lock);
}
