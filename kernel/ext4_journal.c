//
// ext4's end of the journal.
//
// jbd2.c knows how to write a log and how to replay one; it knows
// nothing about ext4. this file is what connects the two: it finds the
// journal inode, turns a journal block number into a disk block number,
// replays the log before anything else reads the file system, and wraps
// each file system operation in one transaction.
//
// the journal is metadata only. file data is written straight to its
// home block, which is what Linux calls data=writeback: after a crash a
// file that was being extended can hold whatever was in its new blocks
// before. ordering data against the metadata that exposes it needs a
// data list on the transaction, and that is the next thing to add.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "proc.h"
#include "defs.h"
#include "blk.h"
#include "ext4_ondisk.h"
#include "ext4.h"
#include "ext4_internal.h"
#include "jbd2.h"
#include "errno.h"

// the journal inode, read once at mount. jbd2 asks for a mapping while
// it recovers, which is before the file system is usable, so the inode
// cannot be looked up again each time; it is kept here instead.
struct ext4_journal {
  struct ext4_fs *fs;
  struct ext4_inode_info inode;
};

// journal block number to disk block number. zero means the journal has
// no such block, which jbd2 treats as corruption and refuses to use.
static uint64
journal_map(void *priv, uint64 jblk)
{
  struct ext4_journal *jf = priv;
  uint64 diskblk = 0;

  if (ext4_bmap(jf->fs, &jf->inode, jblk, &diskblk) < 0)
    return 0;
  return diskblk;
}

// the RECOVER flag in the superblock's incompat word. it means "the log
// may hold something that is not at its home block yet", and it is what
// tells the next reader -- this kernel, Linux or e2fsck -- to replay the
// journal before it believes anything else on the disk.
//
// Linux sets it for as long as the file system is mounted read-write
// with a journal and clears it on a clean unmount, rather than trying to
// follow whether the log happens to be empty at this instant, and this
// does the same: the window in which a commit block is on the disk and
// its blocks are not yet home is exactly the window a crash can land in,
// and it is not a window anything outside the journal can see.
static int
set_recover_flag(struct ext4_fs *fs, int on)
{
  struct blkbuf *b = blk_get(fs->disk, 0);
  struct ext4_super_block *sb;

  if (b == 0)
    return -EIO;
  sb = (struct ext4_super_block *)((uchar *)b->data + 1024);
  if (on) {
    sb->s_feature_incompat |= (uint32)EXT4_FEATURE_INCOMPAT_RECOVER;
    // and the file system is no longer in the state a clean unmount
    // leaves behind, which is the other half of what e2fsck looks at
    sb->s_state &= ~(uint16)EXT4_VALID_FS;
  } else {
    sb->s_feature_incompat &= ~(uint32)EXT4_FEATURE_INCOMPAT_RECOVER;
    sb->s_state |= (uint16)EXT4_VALID_FS;
  }
  ext4_super_csum(fs, sb);
  blk_mark_dirty(b);
  blk_put(b);
  if (on)
    fs->feature_incompat |= (uint32)EXT4_FEATURE_INCOMPAT_RECOVER;
  else
    fs->feature_incompat &= ~(uint32)EXT4_FEATURE_INCOMPAT_RECOVER;
  // and on the disk before anything the flag protects is written
  return blk_sync(fs->disk);
}

int
ext4_journal_open(struct ext4_fs *fs)
{
  struct ext4_journal *jf;
  uint64 nblocks;
  int r;

  initsleeplock(&fs->lock, "ext4");
  fs->journal = 0;
  fs->handle = 0;

  if (!(fs->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)) {
    if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
      printk("ext4: %s: the superblock says the journal needs replay and there is no journal\n",
             fs->disk->name);
      return -EIO;
    }
    return 0; // nothing to do: an ext2 image, or one with the journal removed
  }
  if (fs->journal_inum == 0) {
    printk("ext4: %s: the file system has a journal but does not say which inode it is\n",
           fs->disk->name);
    return -EIO;
  }

  jf = kalloc();
  if (jf == 0)
    return -ENOMEM;
  jf->fs = fs;
  r = ext4_read_inode(fs, fs->journal_inum, &jf->inode);
  if (r < 0) {
    printk("ext4: %s: the journal inode %u cannot be read\n", fs->disk->name, fs->journal_inum);
    kfree(jf);
    return r;
  }
  nblocks = (jf->inode.size + fs->blocksize - 1) / fs->blocksize;
  if (nblocks < JBD2_MIN_JOURNAL_BLOCKS) {
    printk("ext4: %s: the journal is %lu blocks, too few to hold a transaction\n", fs->disk->name,
           nblocks);
    kfree(jf);
    return -EIO;
  }

  fs->journal = jbd2_open(fs->disk, journal_map, jf, nblocks, fs->uuid);
  if (fs->journal == 0) {
    kfree(jf);
    return -EIO;
  }

  jbd2_set_ordered(fs->journal, fs->data_mode == EXT4_DATA_ORDERED);

  r = jbd2_recover(fs->journal);
  if (r < 0) {
    printk("ext4: %s: the journal could not be replayed\n", fs->disk->name);
    jbd2_close(fs->journal);
    fs->journal = 0;
    kfree(jf);
    return r;
  }
  if (r > 0)
    printk("ext4: %s: the journal replayed %d blocks\n", fs->disk->name, r);

  // the log is empty now. a read-only mount may not say so, and leaves
  // the flag as it found it: the next read-write mount replays the same
  // transactions, which is what Linux does too. a read-write mount says
  // instead that from here on the log is where the truth may be.
  if (!fs->readonly)
    set_recover_flag(fs, 1);
  return 0;
}

void
ext4_journal_close(struct ext4_fs *fs)
{
  if (fs->journal == 0)
    return;
  jbd2_close(fs->journal); // commits what is left and marks the log empty
  fs->journal = 0;
  // everything is at its home block now, so the next reader has no
  // reason to look at the log
  if (!fs->readonly)
    set_recover_flag(fs, 0);
}

int
ext4_journal_commit(struct ext4_fs *fs)
{
  if (fs->journal == 0)
    return 0;
  return jbd2_commit(fs->journal);
}

// ---- one operation, one transaction ----

int
ext4_trans_begin(struct ext4_fs *fs, int credits)
{
  if (fs->readonly)
    return -EROFS;
  acquiresleep(&fs->lock);
  if (fs->journal == 0)
    return 0; // no journal: the lock alone is what an operation gets
  fs->handle = jbd2_start(fs->journal, credits);
  if (fs->handle == 0) {
    releasesleep(&fs->lock);
    return -EIO;
  }
  return 0;
}

void
ext4_trans_end(struct ext4_fs *fs)
{
  if (fs->handle) {
    jbd2_stop(fs->handle);
    fs->handle = 0;
  }
  releasesleep(&fs->lock);
}

// file data. in data=journal it goes on the log like metadata, so a
// write is atomic against a crash; in the other two modes the block is
// simply dirty and the writeback thread (data=writeback) or the commit
// (data=ordered) puts it on the disk.
int
ext4_journal_data_access(struct ext4_fs *fs, struct blkbuf *b)
{
  if (fs->data_mode != EXT4_DATA_JOURNAL)
    return 0;
  return ext4_journal_access(fs, b);
}

void
ext4_journal_data(struct ext4_fs *fs, struct blkbuf *b)
{
  if (fs->data_mode == EXT4_DATA_JOURNAL) {
    ext4_journal_dirty(fs, b);
    return;
  }
  blk_mark_dirty(b);
}

int
ext4_journal_access(struct ext4_fs *fs, struct blkbuf *b)
{
  if (fs->handle == 0)
    return 0;
  return jbd2_get_write_access(fs->handle, b);
}

void
ext4_journal_dirty(struct ext4_fs *fs, struct blkbuf *b)
{
  if (fs->handle == 0) {
    blk_mark_dirty(b);
    return;
  }
  jbd2_dirty_metadata(fs->handle, b);
}

void
ext4_journal_revoke(struct ext4_fs *fs, uint64 blkno)
{
  if (fs->handle == 0)
    return;
  jbd2_revoke(fs->handle, blkno);
}
