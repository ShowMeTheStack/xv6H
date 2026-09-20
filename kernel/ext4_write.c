//
// ext4, the write side.
//
// what makes writing ext4 harder than reading it is not the structures
// but the bookkeeping around them: every metadata block carries a
// crc32c that must be recomputed, every allocation must be reflected in
// a bitmap, a group descriptor, the superblock's counters and the
// group's unused-inode hint, and the extent tree must stay a tree.
// getting any of those wrong produces a file system that this kernel
// can still read and that Linux refuses, which is the worst of both,
// so each is done in one place here and nowhere else.
//
// allocation is deliberately simple: the first free block at or after a
// goal, and the first free inode in the directory's own group or the
// next one with room. delayed allocation and the multi-block allocator
// belong to a later step; what matters here is that what is written is
// correct.
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
#include "errno.h"

// how many blocks one allocation may hand out at once. it bounds the
// bitmap scan and the run an extent grows by; the bookkeeping is the
// same whatever it is, so this is only a limit on how much of the disk
// one write can claim before anything else gets a turn.
#define EXT4_ALLOC_RUN_MAX 1024

// ---- checksums ----
//
// the seed of a file system's checksums is the uuid, and the seed of
// anything belonging to one inode is that seeded with the inode number
// and its generation. Linux computes them in exactly this order and a
// checksum is only as good as its agreement with the other reader.

uint32
ext4_inode_seed(struct ext4_fs *fs, uint64 ino, uint32 gen)
{
  uint32 num = (uint32)ino;
  uint32 crc = crc32c(fs->csum_seed, &num, sizeof(num));

  return crc32c(crc, &gen, sizeof(gen));
}

void
ext4_gd_csum(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *gd)
{
  uint32 le_group = group;
  uint32 crc;

  if (!fs->has_metadata_csum)
    return;
  gd->bg_checksum = 0;
  crc = crc32c(fs->csum_seed, &le_group, sizeof(le_group));
  crc = crc32c(crc, gd, fs->desc_size);
  gd->bg_checksum = (uint16)(crc & 0xffff);
}

void
ext4_super_csum(struct ext4_fs *fs, struct ext4_super_block *sb)
{
  if (!fs->has_metadata_csum)
    return;
  sb->s_checksum = crc32c(~0U, sb, 1020); // every byte before the field
}

void
ext4_inode_csum(struct ext4_fs *fs, uint64 ino, struct ext4_inode *di)
{
  uint32 crc;

  if (!fs->has_metadata_csum)
    return;
  uint32 gen = di->i_generation;
  di->l_i_checksum_lo = 0;
  if (fs->inode_size > 128)
    di->i_checksum_hi = 0;
  crc = crc32c(ext4_inode_seed(fs, ino, gen), di, fs->inode_size);
  di->l_i_checksum_lo = (uint16)(crc & 0xffff);
  if (fs->inode_size > 128)
    di->i_checksum_hi = (uint16)(crc >> 16);
}

// the checksum of a directory block hides in the last twelve bytes,
// shaped like an entry with a file type no real entry can have.
void
ext4_dirblock_csum(struct ext4_fs *fs, uint64 ino, uint32 gen, void *block)
{
  uint32 usable = fs->blocksize - EXT4_DIR_TAIL_SIZE;
  struct ext4_dir_entry_tail *t = (struct ext4_dir_entry_tail *)((uchar *)block + usable);

  if (!fs->has_metadata_csum)
    return;
  memset(t, 0, sizeof(*t));
  t->det_rec_len = EXT4_DIR_TAIL_SIZE;
  t->det_reserved_ft = EXT4_FT_DIR_CSUM;
  t->det_checksum = crc32c(ext4_inode_seed(fs, ino, gen), block, usable);
}

// the checksum of an extent block covers every slot the header allows,
// used or not, and sits after all of them.
void
ext4_extentblock_csum(struct ext4_fs *fs, uint64 ino, uint32 gen, void *block)
{
  struct ext4_extent_header *eh = block;
  uint32 len = 12 + (uint32)eh->eh_max * 12;
  struct ext4_extent_tail *tail;

  if (!fs->has_metadata_csum)
    return;
  if (len + 4 > fs->blocksize)
    return;
  tail = (struct ext4_extent_tail *)((uchar *)block + len);
  tail->et_checksum = crc32c(ext4_inode_seed(fs, ino, gen), block, len);
}

// ---- group descriptors and the superblock ----

int
ext4_write_group_desc(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *gd)
{
  struct blkbuf *b;
  uint64 byte, blk;
  uint32 off;

  if (group >= fs->ngroups)
    return -EINVAL;
  ext4_gd_csum(fs, group, gd);
  byte = (uint64)group * fs->desc_size;
  blk = fs->gdt_block + byte / fs->blocksize;
  off = (uint32)(byte % fs->blocksize);
  b = ext4_bread(fs, blk);
  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);
  memmove((uchar *)b->data + off, gd, fs->desc_size);
  ext4_journal_dirty(fs, b);
  blk_put(b);
  return 0;
}

// the superblock, with the counters this driver keeps in memory written
// back into it. it lives at byte 1024, which is inside block 0 for a
// 4096-byte file system.
int
ext4_write_super(struct ext4_fs *fs)
{
  // block 0 is the one block ext4_bread will not hand out, because for
  // every other caller a block number of zero means a hole. the
  // superblock lives in it, so this is the one place that asks the
  // block layer directly.
  struct blkbuf *b = blk_get(fs->disk, 0);
  struct ext4_super_block *sb;

  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);
  sb = (struct ext4_super_block *)((uchar *)b->data + 1024);
  sb->s_free_blocks_count_lo = (uint32)fs->free_blocks;
  sb->s_free_blocks_count_hi = (uint32)(fs->free_blocks >> 32);
  sb->s_free_inodes_count = fs->free_inodes;
  ext4_super_csum(fs, sb);
  ext4_journal_dirty(fs, b);
  blk_put(b);
  return 0;
}

// ---- bitmaps ----

static int
bit_test(const uchar *map, uint32 i)
{
  return (map[i >> 3] >> (i & 7)) & 1;
}

static void
bit_set(uchar *map, uint32 i)
{
  map[i >> 3] |= 1 << (i & 7);
}

static void
bit_clear(uchar *map, uint32 i)
{
  map[i >> 3] &= ~(1 << (i & 7));
}

static uint64
gd_block_bitmap(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint64 v = gd->bg_block_bitmap_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint64)gd->bg_block_bitmap_hi << 32;
  return v;
}

static uint64
gd_inode_bitmap(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint64 v = gd->bg_inode_bitmap_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint64)gd->bg_inode_bitmap_hi << 32;
  return v;
}

static uint64
gd_inode_table(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint64 v = gd->bg_inode_table_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint64)gd->bg_inode_table_hi << 32;
  return v;
}

static uint32
gd_free_blocks(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint32 v = gd->bg_free_blocks_count_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint32)gd->bg_free_blocks_count_hi << 16;
  return v;
}

static void
gd_set_free_blocks(struct ext4_fs *fs, struct ext4_group_desc *gd, uint32 v)
{
  gd->bg_free_blocks_count_lo = (uint16)v;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    gd->bg_free_blocks_count_hi = (uint16)(v >> 16);
}

static uint32
gd_free_inodes(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint32 v = gd->bg_free_inodes_count_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint32)gd->bg_free_inodes_count_hi << 16;
  return v;
}

static void
gd_set_free_inodes(struct ext4_fs *fs, struct ext4_group_desc *gd, uint32 v)
{
  gd->bg_free_inodes_count_lo = (uint16)v;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    gd->bg_free_inodes_count_hi = (uint16)(v >> 16);
}

static uint32
gd_used_dirs(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint32 v = gd->bg_used_dirs_count_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint32)gd->bg_used_dirs_count_hi << 16;
  return v;
}

static void
gd_set_used_dirs(struct ext4_fs *fs, struct ext4_group_desc *gd, uint32 v)
{
  gd->bg_used_dirs_count_lo = (uint16)v;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    gd->bg_used_dirs_count_hi = (uint16)(v >> 16);
}

static uint32
gd_itable_unused(struct ext4_fs *fs, const struct ext4_group_desc *gd)
{
  uint32 v = gd->bg_itable_unused_lo;

  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    v |= (uint32)gd->bg_itable_unused_hi << 16;
  return v;
}

static void
gd_set_itable_unused(struct ext4_fs *fs, struct ext4_group_desc *gd, uint32 v)
{
  gd->bg_itable_unused_lo = (uint16)v;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    gd->bg_itable_unused_hi = (uint16)(v >> 16);
}

// the bitmap checksums, which the group descriptor carries
static void
block_bitmap_csum(struct ext4_fs *fs, struct ext4_group_desc *gd, const void *map)
{
  uint32 crc;

  if (!fs->has_metadata_csum)
    return;
  crc = crc32c(fs->csum_seed, map, fs->blocks_per_group / 8);
  gd->bg_block_bitmap_csum_lo = (uint16)(crc & 0xffff);
  if (fs->desc_size >= 64)
    gd->bg_block_bitmap_csum_hi = (uint16)(crc >> 16);
}

static void
inode_bitmap_csum(struct ext4_fs *fs, struct ext4_group_desc *gd, const void *map)
{
  uint32 crc;

  if (!fs->has_metadata_csum)
    return;
  crc = crc32c(fs->csum_seed, map, fs->inodes_per_group / 8);
  gd->bg_inode_bitmap_csum_lo = (uint16)(crc & 0xffff);
  if (fs->desc_size >= 64)
    gd->bg_inode_bitmap_csum_hi = (uint16)(crc >> 16);
}

// a group that was never used has its bitmaps and inode table marked
// uninitialized; writing into it means those flags must go, because
// what they promise is no longer true.
static void
clear_uninit(struct ext4_group_desc *gd, uint16 flag)
{
  gd->bg_flags &= ~flag;
}

// ---- allocating and freeing blocks ----

// a run of up to `want` free blocks at or after `goal`, all in one
// group and next to one another on the disk. one pass over the bitmap,
// one group descriptor, one superblock: the bookkeeping a single block
// costs, for as many as the caller asked for. *got says how many it
// got, which is at least one.
//
// a block handed to a file must not show what the last file to hold it
// wrote, so the run is zeroed -- unless `prefill` says the caller is
// about to write every byte of every block of it, in which case the
// zeros would be overwritten before anything could read them and
// writing them is pure cost.
int
ext4_alloc_blocks(struct ext4_fs *fs, uint64 goal, uint32 want, int prefill, uint64 *out,
                  uint32 *got)
{
  uint32 start_group;

  if (fs->readonly)
    return -EROFS;
  if (fs->free_blocks == 0)
    return -ENOSPC;
  if (want < 1)
    want = 1;
  if (want > EXT4_ALLOC_RUN_MAX)
    want = EXT4_ALLOC_RUN_MAX;
  if (goal >= fs->blocks_count)
    goal = 0;
  start_group = goal ? (uint32)((goal - fs->first_data_block) / fs->blocks_per_group) : 0;

  for (uint32 n = 0; n < fs->ngroups; n++) {
    uint32 g = (start_group + n) % fs->ngroups;
    struct ext4_group_desc gd;

    if (ext4_group_desc(fs, g, &gd) < 0)
      continue;
    if (gd_free_blocks(fs, &gd) == 0)
      continue;

    uint64 bmapblk = gd_block_bitmap(fs, &gd);
    struct blkbuf *bm = ext4_bread(fs, bmapblk);
    if (bm == 0)
      continue;

    uint64 group_first = fs->first_data_block + (uint64)g * fs->blocks_per_group;
    uint32 nblocks = fs->blocks_per_group;
    if (group_first + nblocks > fs->blocks_count)
      nblocks = (uint32)(fs->blocks_count - group_first);

    uint32 begin = 0;
    if (n == 0 && goal >= group_first && goal < group_first + nblocks)
      begin = (uint32)(goal - group_first);

    for (uint32 pass = 0; pass < 2; pass++) {
      uint32 from = pass == 0 ? begin : 0;
      uint32 to = pass == 0 ? nblocks : begin;
      for (uint32 i = from; i < to; i++) {
        if (bit_test(bm->data, i))
          continue;

        // how far the run of free bits goes, up to what was asked for
        uint32 n = 1;
        while (n < want && i + n < to && !bit_test(bm->data, i + n))
          n++;

        ext4_journal_access(fs, bm);
        for (uint32 k = 0; k < n; k++)
          bit_set(bm->data, i + k);
        gd_set_free_blocks(fs, &gd, gd_free_blocks(fs, &gd) - n);
        clear_uninit(&gd, EXT4_BG_BLOCK_UNINIT);
        block_bitmap_csum(fs, &gd, bm->data);
        ext4_journal_dirty(fs, bm);
        blk_put(bm);
        fs->free_blocks -= n;
        ext4_write_group_desc(fs, g, &gd);
        ext4_write_super(fs);
        *out = group_first + i;
        *got = n;
        // a freshly allocated block must not carry what was there
        // before it: a reader of a file would see someone else's data.
        // not journalled: what these blocks are for is not known yet,
        // and until the bitmap change above is committed they are still
        // free, so zeros in them are zeros in blocks nothing points at.
        if (!prefill) {
          for (uint32 k = 0; k < n; k++) {
            struct blkbuf *nb = blk_get_zero(fs->disk, *out + k);
            if (nb) {
              blk_mark_dirty(nb);
              blk_put(nb);
            }
          }
        }
        return 0;
      }
    }
    blk_put(bm);
  }
  return -ENOSPC;
}

int
ext4_alloc_block(struct ext4_fs *fs, uint64 goal, uint64 *out)
{
  uint32 got = 0;

  return ext4_alloc_blocks(fs, goal, 1, 0, out, &got);
}

int
ext4_free_block(struct ext4_fs *fs, uint64 blkno)
{
  struct ext4_group_desc gd;
  uint32 g, index;

  if (fs->readonly)
    return -EROFS;
  if (blkno == 0 || blkno >= fs->blocks_count)
    return -EINVAL;
  g = (uint32)((blkno - fs->first_data_block) / fs->blocks_per_group);
  index = (uint32)((blkno - fs->first_data_block) % fs->blocks_per_group);
  if (ext4_group_desc(fs, g, &gd) < 0)
    return -EIO;

  struct blkbuf *bm = ext4_bread(fs, gd_block_bitmap(fs, &gd));
  if (bm == 0)
    return -EIO;
  if (!bit_test(bm->data, index)) {
    blk_put(bm);
    return 0; // freeing a block that is already free is not worth a fuss
  }
  // whatever this block held may be on the log; a revoke is what stops
  // recovery from writing it back over what comes next
  ext4_journal_revoke(fs, blkno);
  ext4_journal_access(fs, bm);
  bit_clear(bm->data, index);
  block_bitmap_csum(fs, &gd, bm->data);
  ext4_journal_dirty(fs, bm);
  blk_put(bm);

  gd_set_free_blocks(fs, &gd, gd_free_blocks(fs, &gd) + 1);
  fs->free_blocks++;
  ext4_write_group_desc(fs, g, &gd);
  ext4_write_super(fs);
  return 0;
}

// ---- allocating and freeing inodes ----

int
ext4_alloc_inode(struct ext4_fs *fs, uint64 dirino, int is_dir, uint64 *out)
{
  uint32 start_group = 0;

  if (fs->readonly)
    return -EROFS;
  if (fs->free_inodes == 0)
    return -ENOSPC;
  if (dirino > 0)
    start_group = (uint32)((dirino - 1) / fs->inodes_per_group);

  for (uint32 n = 0; n < fs->ngroups; n++) {
    uint32 g = (start_group + n) % fs->ngroups;
    struct ext4_group_desc gd;

    if (ext4_group_desc(fs, g, &gd) < 0)
      continue;
    if (gd_free_inodes(fs, &gd) == 0)
      continue;

    struct blkbuf *bm = ext4_bread(fs, gd_inode_bitmap(fs, &gd));
    if (bm == 0)
      continue;

    for (uint32 i = 0; i < fs->inodes_per_group; i++) {
      uint64 ino = (uint64)g * fs->inodes_per_group + i + 1;
      if (ino < fs->first_ino && ino != EXT4_ROOT_INO)
        continue; // the reserved inodes belong to the format
      if (bit_test(bm->data, i))
        continue;

      ext4_journal_access(fs, bm);
      bit_set(bm->data, i);
      inode_bitmap_csum(fs, &gd, bm->data);
      ext4_journal_dirty(fs, bm);
      blk_put(bm);

      gd_set_free_inodes(fs, &gd, gd_free_inodes(fs, &gd) - 1);
      if (is_dir)
        gd_set_used_dirs(fs, &gd, gd_used_dirs(fs, &gd) + 1);
      clear_uninit(&gd, EXT4_BG_INODE_UNINIT);
      // the hint says how many inodes at the end of the table were
      // never used; handing one out from below it shortens it.
      uint32 unused = gd_itable_unused(fs, &gd);
      if (unused > fs->inodes_per_group - (i + 1))
        gd_set_itable_unused(fs, &gd, fs->inodes_per_group - (i + 1));
      fs->free_inodes--;
      ext4_write_group_desc(fs, g, &gd);
      ext4_write_super(fs);

      // the inode itself starts empty
      uint64 byte = (uint64)i * fs->inode_size;
      uint64 blk = gd_inode_table(fs, &gd) + byte / fs->blocksize;
      struct blkbuf *ib = ext4_bread(fs, blk);
      if (ib) {
        ext4_journal_access(fs, ib);
        memset((uchar *)ib->data + byte % fs->blocksize, 0, fs->inode_size);
        ext4_journal_dirty(fs, ib);
        blk_put(ib);
      }
      *out = ino;
      return 0;
    }
    blk_put(bm);
  }
  return -ENOSPC;
}

static void inode_mark_deleted(struct ext4_fs *fs, uint64 ino);
static uint64 now_seconds(void);

int
ext4_free_inode(struct ext4_fs *fs, uint64 ino, int is_dir)
{
  struct ext4_group_desc gd;
  uint32 g, index;

  if (fs->readonly)
    return -EROFS;
  if (ino < 1 || ino > fs->inodes_count)
    return -EINVAL;
  g = (uint32)((ino - 1) / fs->inodes_per_group);
  index = (uint32)((ino - 1) % fs->inodes_per_group);
  if (ext4_group_desc(fs, g, &gd) < 0)
    return -EIO;

  struct blkbuf *bm = ext4_bread(fs, gd_inode_bitmap(fs, &gd));
  if (bm == 0)
    return -EIO;
  if (bit_test(bm->data, index)) {
    inode_mark_deleted(fs, ino);
    ext4_journal_access(fs, bm);
    bit_clear(bm->data, index);
    inode_bitmap_csum(fs, &gd, bm->data);
    ext4_journal_dirty(fs, bm);
    gd_set_free_inodes(fs, &gd, gd_free_inodes(fs, &gd) + 1);
    if (is_dir && gd_used_dirs(fs, &gd) > 0)
      gd_set_used_dirs(fs, &gd, gd_used_dirs(fs, &gd) - 1);
    fs->free_inodes++;
    ext4_write_group_desc(fs, g, &gd);
    ext4_write_super(fs);
  }
  blk_put(bm);
  return 0;
}

// ---- writing an inode back ----

// where one inode lives on the disk
static int
inode_place(struct ext4_fs *fs, uint64 ino, uint64 *blk, uint32 *off)
{
  struct ext4_group_desc gd;
  uint32 group, index;
  uint64 itable, byte;

  if (ino < 1 || ino > fs->inodes_count)
    return -EINVAL;
  group = (uint32)((ino - 1) / fs->inodes_per_group);
  index = (uint32)((ino - 1) % fs->inodes_per_group);
  if (ext4_group_desc(fs, group, &gd) < 0)
    return -EIO;
  itable = gd_inode_table(fs, &gd);
  byte = (uint64)index * fs->inode_size;
  *blk = itable + byte / fs->blocksize;
  *off = (uint32)(byte % fs->blocksize);
  if (*blk >= fs->blocks_count || *off + fs->inode_size > fs->blocksize)
    return -EIO;
  return 0;
}

// an inode that has just been freed. Linux records the moment a link
// count reached zero in i_dtime, and e2fsck treats an unallocated inode
// without one as damage, so the last writer of an inode has to say so.
static void
inode_mark_deleted(struct ext4_fs *fs, uint64 ino)
{
  struct blkbuf *b;
  uint64 blk;
  uint32 off;

  if (inode_place(fs, ino, &blk, &off) < 0)
    return;
  b = ext4_bread(fs, blk);
  if (b == 0)
    return;
  ext4_journal_access(fs, b);

  struct ext4_inode *di = (struct ext4_inode *)((uchar *)b->data + off);
  di->i_links_count = 0;
  di->i_dtime = (uint32)now_seconds();
  ext4_inode_csum(fs, ino, di);
  ext4_journal_dirty(fs, b);
  blk_put(b);
}

// write the raw inode of an in-memory inode back, checksum and all
int
ext4_write_inode(struct ext4_fs *fs, uint64 ino, struct ext4_inode_info *in)
{
  struct blkbuf *b;
  uint64 blk;
  uint32 off;
  int r;

  if (fs->readonly)
    return -EROFS;
  r = inode_place(fs, ino, &blk, &off);
  if (r < 0)
    return r;
  b = ext4_bread(fs, blk);
  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);

  struct ext4_inode *di = (struct ext4_inode *)((uchar *)b->data + off);
  uint32 copy = fs->inode_size;
  if (copy > EXT4_INODE_RAW_SIZE)
    copy = EXT4_INODE_RAW_SIZE;
  memmove(di, in->raw, copy);

  // the fields the caller may have changed live in raw already, except
  // the ones the in-memory inode keeps of its own
  di->i_size_lo = (uint32)in->size;
  di->i_size_high = (uint32)(in->size >> 32);
  di->i_links_count = (uint16)in->nlink;
  di->i_mode = (uint16)in->mode;
  di->i_uid = (uint16)in->uid;
  di->i_gid = (uint16)in->gid;
  di->i_blocks_lo = (uint32)in->blocks;
  di->i_atime = (uint32)in->atime;
  di->i_mtime = (uint32)in->mtime;
  di->i_ctime = (uint32)in->ctime;
  di->i_flags = in->flags;

  ext4_inode_csum(fs, ino, di);
  // keep the caller's copy in step, so a later write does not undo this
  memmove(in->raw, di, copy);
  ext4_journal_dirty(fs, b);
  blk_put(b);
  return 0;
}

// the seconds a file system timestamp holds
static uint64
now_seconds(void)
{
  uint64 ns = clock_realtime_ns();

  return ns / 1000000000UL;
}

void
ext4_touch(struct ext4_inode_info *in, int mtime)
{
  uint64 t = now_seconds();

  in->atime = t;
  in->ctime = t;
  if (mtime)
    in->mtime = t;
}

// ---- the extent tree ----
//
// only what writing needs: find the block that holds file block n,
// allocating it and the tree around it when it is not there yet. the
// tree grows at the right-hand edge, which is where a file that is
// being written grows.

struct extent_path {
  struct blkbuf *buf;   // the block this level lives in, or zero for the inode
  struct ext4_extent_header *eh;
  void *entries;
};

static struct ext4_extent_header *
inode_extent_root(struct ext4_inode_info *in)
{
  struct ext4_inode *di = (struct ext4_inode *)in->raw;

  return (struct ext4_extent_header *)di->i_block;
}

// start a file off with an empty extent tree in its inode
static void
extent_init(struct ext4_inode_info *in)
{
  struct ext4_inode *di = (struct ext4_inode *)in->raw;
  struct ext4_extent_header *eh = (struct ext4_extent_header *)di->i_block;

  memset(di->i_block, 0, sizeof(di->i_block));
  eh->eh_magic = EXT4_EXT_MAGIC;
  eh->eh_entries = 0;
  eh->eh_max = 4;    // 60 bytes: a 12-byte header and four 12-byte entries
  eh->eh_depth = 0;
  eh->eh_generation = 0;
  in->flags |= EXT4_EXTENTS_FL;
  di->i_flags = in->flags;
}

// the last extent of a leaf, or zero when it is empty
static struct ext4_extent *
last_extent(struct ext4_extent_header *eh)
{
  struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);

  if (eh->eh_entries == 0)
    return 0;
  return &ex[eh->eh_entries - 1];
}

static uint32
extent_len(const struct ext4_extent *ex)
{
  // a length above 32768 means the extent is unwritten, and its real
  // length is what is left after taking that away
  return ex->ee_len > 32768 ? ex->ee_len - 32768 : ex->ee_len;
}

static uint64
extent_start(const struct ext4_extent *ex)
{
  return ex->ee_start_lo | ((uint64)ex->ee_start_hi << 32);
}

static void
extent_set(struct ext4_extent *ex, uint32 fileblk, uint64 diskblk, uint32 len)
{
  ex->ee_block = fileblk;
  ex->ee_len = (uint16)len;
  ex->ee_start_lo = (uint32)diskblk;
  ex->ee_start_hi = (uint16)(diskblk >> 32);
}

// grow a depth-0 tree whose root is full into a depth-1 one: the four
// extents move into a block of their own and the root points at it.
static int
extent_grow(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino)
{
  struct ext4_extent_header *root = inode_extent_root(in);
  uint64 blkno;
  int r;

  if (root->eh_depth >= 2)
    return -ENOSPC; // two levels is as deep as this driver builds
  r = ext4_alloc_block(fs, 0, &blkno);
  if (r < 0)
    return r;

  struct blkbuf *b = blk_get_zero(fs->disk, blkno);
  if (b == 0) {
    ext4_free_block(fs, blkno);
    return -EIO;
  }
  ext4_journal_access(fs, b);
  struct ext4_extent_header *neh = b->data;
  uint32 entry_size = 12;
  uint32 max = (fs->blocksize - 12 - 4) / entry_size; // room for the tail

  neh->eh_magic = EXT4_EXT_MAGIC;
  neh->eh_entries = root->eh_entries;
  neh->eh_max = (uint16)max;
  neh->eh_depth = root->eh_depth;
  neh->eh_generation = 0;
  memmove(neh + 1, root + 1, (uint32)root->eh_entries * entry_size);
  ext4_extentblock_csum(fs, ino, in->generation, b->data);
  ext4_journal_dirty(fs, b);
  blk_put(b);

  // the root now holds one index entry pointing at that block
  struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(root + 1);
  uint32 first_block = 0;
  if (root->eh_depth == 0) {
    struct ext4_extent *ex = (struct ext4_extent *)(root + 1);
    first_block = ex[0].ee_block;
  } else {
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(root + 1);
    first_block = ix[0].ei_block;
  }
  memset(root + 1, 0, 4 * entry_size);
  idx[0].ei_block = first_block;
  idx[0].ei_leaf_lo = (uint32)blkno;
  idx[0].ei_leaf_hi = (uint16)(blkno >> 32);
  idx[0].ei_unused = 0;
  root->eh_entries = 1;
  root->eh_depth++;
  in->blocks += fs->blocksize / 512;
  return 0;
}

// the rightmost leaf of the tree, read into `out`. `path` records the
// index entry above it so the caller can add another leaf.
struct leaf_ref {
  struct blkbuf *buf;                 // zero when the leaf is the inode itself
  struct ext4_extent_header *eh;
  struct ext4_extent_idx *parent_idx; // the entry that points at it
  struct blkbuf *parent_buf;
  struct ext4_extent_header *parent_eh;
};

static int
rightmost_leaf(struct ext4_fs *fs, struct ext4_inode_info *in, struct leaf_ref *out)
{
  struct ext4_extent_header *eh = inode_extent_root(in);

  memset(out, 0, sizeof(*out));
  if (eh->eh_magic != EXT4_EXT_MAGIC)
    return -EIO;
  if (eh->eh_depth == 0) {
    out->eh = eh;
    return 0;
  }

  struct ext4_extent_header *parent_eh = eh;
  struct blkbuf *parent_buf = 0;
  for (int depth = eh->eh_depth; depth > 0; depth--) {
    if (parent_eh->eh_entries == 0)
      return -EIO;
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(parent_eh + 1);
    struct ext4_extent_idx *last = &ix[parent_eh->eh_entries - 1];
    uint64 child = last->ei_leaf_lo | ((uint64)last->ei_leaf_hi << 32);
    struct blkbuf *b = ext4_bread(fs, child);
    if (b == 0) {
      if (parent_buf)
        blk_put(parent_buf);
      return -EIO;
    }
    struct ext4_extent_header *ceh = b->data;
    if (ceh->eh_magic != EXT4_EXT_MAGIC) {
      blk_put(b);
      if (parent_buf)
        blk_put(parent_buf);
      return -EIO;
    }
    // this walk exists to change the tree, so write access is taken on
    // the way down rather than after the change has already been made
    ext4_journal_access(fs, b);
    if (depth == 1) {
      out->buf = b;
      out->eh = ceh;
      out->parent_idx = last;
      out->parent_buf = parent_buf;
      out->parent_eh = parent_eh;
      return 0;
    }
    if (parent_buf)
      blk_put(parent_buf);
    parent_buf = b;
    parent_eh = ceh;
  }
  return -EIO;
}

static void
leaf_done(struct ext4_fs *fs, struct leaf_ref *lf, uint64 ino, uint32 gen, int dirty)
{
  if (lf->buf) {
    if (dirty) {
      ext4_extentblock_csum(fs, ino, gen, lf->buf->data);
      ext4_journal_dirty(fs, lf->buf);
    }
    blk_put(lf->buf);
  }
  if (lf->parent_buf) {
    if (dirty) {
      ext4_extentblock_csum(fs, ino, gen, lf->parent_buf->data);
      ext4_journal_dirty(fs, lf->parent_buf);
    }
    blk_put(lf->parent_buf);
  }
}

// give a whole run back, for the paths where the tree could not be
// made to hold what was just allocated
static void
free_run(struct ext4_fs *fs, uint64 blkno, uint32 n)
{
  for (uint32 i = 0; i < n; i++)
    ext4_free_block(fs, blkno + i);
}

// the disk block of file block `fileblk`, allocating it if it is not
// there. the file is assumed to be written in order, which is what a
// program that writes a file does, so a new block joins the last extent
// when it follows it on the disk and starts a new one when it does not.
int
ext4_bmap_alloc_run(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 fileblk,
                    uint32 want, int prefill, uint64 *out, uint32 *got)
{
  uint64 existing = 0;
  int r;

  *got = 1;
  if (fs->readonly)
    return -EROFS;
  if (fileblk > 0xffffffffUL)
    return -EFBIG;

  // already there?
  r = ext4_bmap(fs, in, fileblk, &existing);
  if (r >= 0 && existing != 0) {
    *out = existing;
    return 0;
  }

  struct ext4_extent_header *root = inode_extent_root(in);
  if ((in->flags & EXT4_EXTENTS_FL) == 0 || root->eh_magic != EXT4_EXT_MAGIC) {
    if (in->size != 0 || in->blocks != 0)
      return -EOPNOTSUPP; // an old block-mapped file: read only
    extent_init(in);
    root = inode_extent_root(in);
  }

  struct leaf_ref lf;
  r = rightmost_leaf(fs, in, &lf);
  if (r < 0)
    return r;

  // where should the new block go? next to the last one, if it can
  uint64 goal = 0;
  struct ext4_extent *last = last_extent(lf.eh);
  if (last)
    goal = extent_start(last) + extent_len(last);

  // an extent's length is sixteen bits, so a run that would take it
  // past that is asked for shorter rather than allocated and wasted
  int can_merge = last && last->ee_block + extent_len(last) == fileblk && last->ee_len <= 32768;
  if (can_merge) {
    uint32 room = 32767 - extent_len(last);
    if (want > room)
      want = room;
    if (want == 0) {
      can_merge = 0;
      want = 1;
    }
  }
  if (want > 32767)
    want = 32767; // a new extent has the same limit

  uint64 blkno;
  uint32 n = 1;
  r = ext4_alloc_blocks(fs, goal, want, prefill, &blkno, &n);
  if (r < 0) {
    leaf_done(fs, &lf, ino, in->generation, 0);
    return r;
  }

  // the run continues the last extent when it follows it in the file
  // and on the disk, and the extent has room to grow
  if (can_merge && extent_start(last) + extent_len(last) == blkno) {
    last->ee_len = (uint16)(extent_len(last) + n);
    in->blocks += (uint64)n * (fs->blocksize / 512);
    leaf_done(fs, &lf, ino, in->generation, 1);
    *out = blkno;
    *got = n;
    return 0;
  }

  // otherwise it needs an entry of its own
  if (lf.eh->eh_entries >= lf.eh->eh_max) {
    leaf_done(fs, &lf, ino, in->generation, 1);
    if (lf.buf == 0) {
      // the root is full: give the tree a level
      r = extent_grow(fs, in, ino);
      if (r < 0) {
        free_run(fs, blkno, n);
        return r;
      }
    } else {
      // the leaf is full: add another one beside it, if the level above
      // has room
      if (lf.parent_eh == 0 || lf.parent_eh->eh_entries >= lf.parent_eh->eh_max) {
        free_run(fs, blkno, n);
        return -ENOSPC;
      }
      uint64 leafblk;
      r = ext4_alloc_block(fs, blkno, &leafblk);
      if (r < 0) {
        free_run(fs, blkno, n);
        return r;
      }
      struct blkbuf *nb = blk_get_zero(fs->disk, leafblk);
      if (nb == 0) {
        ext4_free_block(fs, leafblk);
        free_run(fs, blkno, n);
        return -EIO;
      }
      ext4_journal_access(fs, nb);
      struct ext4_extent_header *neh = nb->data;
      neh->eh_magic = EXT4_EXT_MAGIC;
      neh->eh_entries = 1;
      neh->eh_max = (uint16)((fs->blocksize - 12 - 4) / 12);
      neh->eh_depth = 0;
      extent_set((struct ext4_extent *)(neh + 1), (uint32)fileblk, blkno, n);
      ext4_extentblock_csum(fs, ino, in->generation, nb->data);
      ext4_journal_dirty(fs, nb);
      blk_put(nb);

      // and the index entry that points at it, in the level above
      struct leaf_ref up;
      r = rightmost_leaf(fs, in, &up);
      if (r < 0) {
        ext4_free_block(fs, leafblk);
        free_run(fs, blkno, n);
        return r;
      }
      struct ext4_extent_header *peh = up.parent_eh ? up.parent_eh : inode_extent_root(in);
      struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(peh + 1);
      ix[peh->eh_entries].ei_block = (uint32)fileblk;
      ix[peh->eh_entries].ei_leaf_lo = (uint32)leafblk;
      ix[peh->eh_entries].ei_leaf_hi = (uint16)(leafblk >> 32);
      ix[peh->eh_entries].ei_unused = 0;
      peh->eh_entries++;
      if (up.parent_buf) {
        ext4_extentblock_csum(fs, ino, in->generation, up.parent_buf->data);
        ext4_journal_dirty(fs, up.parent_buf);
      }
      leaf_done(fs, &up, ino, in->generation, 1);
      in->blocks += (uint64)(n + 1) * (fs->blocksize / 512); // the run and its leaf
      *out = blkno;
      *got = n;
      return 0;
    }

    // the tree changed shape: find the leaf again and put the block in
    r = rightmost_leaf(fs, in, &lf);
    if (r < 0) {
      free_run(fs, blkno, n);
      return r;
    }
  }

  struct ext4_extent *ex = (struct ext4_extent *)(lf.eh + 1);
  extent_set(&ex[lf.eh->eh_entries], (uint32)fileblk, blkno, n);
  lf.eh->eh_entries++;
  in->blocks += (uint64)n * (fs->blocksize / 512);
  leaf_done(fs, &lf, ino, in->generation, 1);
  *out = blkno;
  *got = n;
  return 0;
}

// the single-block form, which is what everything but a data write
// wants: a directory block, an extent block, one block of a file.
int
ext4_bmap_alloc(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 fileblk,
                uint64 *out)
{
  uint32 got = 0;

  return ext4_bmap_alloc_run(fs, in, ino, fileblk, 1, 0, out, &got);
}


// ---- writing file data ----

int
ext4_write_file(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 off, int user,
                uint64 src, uint64 n)
{
  uint64 done = 0;
  int faulted = 0;

  if (fs->readonly)
    return -EROFS;
  while (done < n) {
    uint64 fileblk = (off + done) / fs->blocksize;
    uint32 within = (uint32)((off + done) % fs->blocksize);
    uint32 chunk = fs->blocksize - within;
    if (chunk > n - done)
      chunk = (uint32)(n - done);

    // how many whole blocks this write still has to put on the disk
    // after this one. asking for them together is what lets the
    // allocator hand back a run: one pass over the bitmap, one group
    // descriptor, one superblock and one extent for all of them,
    // instead of that work per block.
    uint32 want = 1;
    int prefill = 0;
    if (within == 0) {
      uint64 rest = n - done;
      want = (uint32)(rest / fs->blocksize);
      if (want == 0)
        want = 1;
      else
        prefill = 1; // every block of this run is written in full below
    }

    uint64 diskblk;
    uint32 got = 1;
    int r = ext4_bmap_alloc_run(fs, in, ino, fileblk, want, prefill, &diskblk, &got);
    if (r < 0)
      return done > 0 ? (int)done : r;

    // the run is contiguous both in the file and on the disk, so the
    // bytes that fall inside it are written without mapping again.
    for (uint32 k = 0; k < got && done < n; k++) {
      within = (uint32)((off + done) % fs->blocksize);
      chunk = fs->blocksize - within;
      if (chunk > n - done)
        chunk = (uint32)(n - done);

      struct blkbuf *b;
      if (within == 0 && chunk == fs->blocksize)
        b = blk_get_fill(fs->disk, diskblk + k); // every byte is about to be written
      else
        b = ext4_bread(fs, diskblk + k);
      if (b == 0)
        return done > 0 ? (int)done : -EIO;
      // in data=journal the block has to be the journal's before it is
      // modified, the same rule metadata follows
      ext4_journal_data_access(fs, b);
      // straight into the cached block: no bounce buffer, and a copy
      // that faults part way leaves what it managed to read, which is
      // what a write straddling the end of an address space must do.
      int e = either_copyin((uchar *)b->data + within, user, src + done, chunk);
      ext4_journal_data(fs, b);
      blk_put(b);
      if (e < 0) {
        faulted = 1;
        break;
      }
      done += chunk;
    }
    if (faulted)
      break;
  }

  if (off + done > in->size)
    in->size = off + done;
  ext4_touch(in, 1);
  ext4_write_inode(fs, ino, in);
  if (done == 0 && faulted)
    return -EFAULT;
  return (int)done;
}

// ---- truncating ----

// free every block of a file and empty its extent tree. partial
// truncation to a non-zero length comes with the rest of the write
// path; a file is either made and written or removed.
int
ext4_truncate(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 newsize)
{
  struct ext4_extent_header *root = inode_extent_root(in);

  if (fs->readonly)
    return -EROFS;
  if (newsize != 0)
    return -EOPNOTSUPP;
  if ((in->flags & EXT4_EXTENTS_FL) == 0 || root->eh_magic != EXT4_EXT_MAGIC) {
    in->size = 0;
    ext4_write_inode(fs, ino, in);
    return 0;
  }

  // walk the tree, freeing the blocks of every leaf and then the index
  // blocks themselves
  if (root->eh_depth == 0) {
    struct ext4_extent *ex = (struct ext4_extent *)(root + 1);
    for (int i = 0; i < root->eh_entries; i++) {
      uint64 start = extent_start(&ex[i]);
      for (uint32 k = 0; k < extent_len(&ex[i]); k++)
        ext4_free_block(fs, start + k);
    }
  } else {
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(root + 1);
    for (int i = 0; i < root->eh_entries; i++) {
      uint64 child = ix[i].ei_leaf_lo | ((uint64)ix[i].ei_leaf_hi << 32);
      struct blkbuf *b = ext4_bread(fs, child);
      if (b == 0)
        continue;
      struct ext4_extent_header *eh = b->data;
      if (eh->eh_magic == EXT4_EXT_MAGIC && eh->eh_depth == 0) {
        struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
        for (int k = 0; k < eh->eh_entries; k++) {
          uint64 start = extent_start(&ex[k]);
          for (uint32 j = 0; j < extent_len(&ex[k]); j++)
            ext4_free_block(fs, start + j);
        }
      }
      blk_put(b);
      ext4_free_block(fs, child);
    }
  }

  extent_init(in);
  in->size = 0;
  in->blocks = 0;
  ext4_touch(in, 1);
  ext4_write_inode(fs, ino, in);
  return 0;
}

// ---- directories ----

static uint32
dirent_needed(int namelen)
{
  return (uint32)(8 + namelen + 3) & ~3U;
}

// add one entry to a directory. the directories this driver writes are
// linear; a hashed one from Linux is read but not changed, because
// splitting a hash leaf is a different piece of work.
int
ext4_add_entry(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, const char *name,
               int namelen, uint64 ino, uchar type)
{
  uint64 existing = 0;

  // a name may not be added twice. the caller usually knows already,
  // but link(2) hands the same name to both ends and something has to
  // say no.
  if (ext4_lookup(fs, dir, name, namelen, &existing, 0) == 0)
    return -EEXIST;

  uint32 need = dirent_needed(namelen);
  uint64 nblocks = dir->size / fs->blocksize;

  if (fs->readonly)
    return -EROFS;
  if (namelen <= 0 || namelen > 255)
    return -ENAMETOOLONG;
  if (dir->flags & EXT4_INDEX_FL)
    return ext4_htree_add(fs, dir, dirino, name, namelen, ino, type);

  uint32 usable = fs->blocksize - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);

  for (uint64 bi = 0; bi < nblocks; bi++) {
    uint64 diskblk;
    if (ext4_bmap(fs, dir, bi, &diskblk) < 0 || diskblk == 0)
      continue;
    struct blkbuf *b = ext4_bread(fs, diskblk);
    if (b == 0)
      continue;

    uint32 off = 0;
    while (off < usable) {
      struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)((uchar *)b->data + off);
      if (de->rec_len < 8 || off + de->rec_len > usable)
        break;
      uint32 used = de->inode ? dirent_needed(de->name_len) : 0;
      if (de->rec_len - used >= need) {
        struct ext4_dir_entry_2 *nd;
        ext4_journal_access(fs, b);
        if (used == 0) {
          nd = de;
          nd->rec_len = de->rec_len;
        } else {
          uint32 rest = de->rec_len - used;
          de->rec_len = (uint16)used;
          nd = (struct ext4_dir_entry_2 *)((uchar *)de + used);
          nd->rec_len = (uint16)rest;
        }
        nd->inode = (uint32)ino;
        nd->name_len = (uchar)namelen;
        nd->file_type = type;
        memmove(nd->name, name, namelen);
        ext4_dirblock_csum(fs, dirino, dir->generation, b->data);
        ext4_journal_dirty(fs, b);
        blk_put(b);
        ext4_touch(dir, 1);
        ext4_write_inode(fs, dirino, dir);
        return 0;
      }
      off += de->rec_len;
    }
    blk_put(b);
  }

  // no room. a directory that is still one block becomes indexed
  // instead of growing into a list: a list costs a whole read of the
  // directory to decide that a name is not in it, and making N files
  // in one costs N^2 blocks read.
  if (nblocks == 1) {
    int cr = ext4_dir_make_indexed(fs, dir, dirino);
    if (cr == 0)
      return ext4_htree_add(fs, dir, dirino, name, namelen, ino, type);
    if (cr != -EOPNOTSUPP)
      return cr;
    // the directory is not one this can index; it grows as before.
  }

  uint64 diskblk;
  int r = ext4_bmap_alloc(fs, dir, dirino, nblocks, &diskblk);
  if (r < 0)
    return r;
  struct blkbuf *b = blk_get_zero(fs->disk, diskblk);
  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);
  struct ext4_dir_entry_2 *de = b->data;
  de->inode = (uint32)ino;
  de->rec_len = (uint16)usable;
  de->name_len = (uchar)namelen;
  de->file_type = type;
  memmove(de->name, name, namelen);
  ext4_dirblock_csum(fs, dirino, dir->generation, b->data);
  ext4_journal_dirty(fs, b);
  blk_put(b);

  dir->size = (nblocks + 1) * fs->blocksize;
  ext4_touch(dir, 1);
  ext4_write_inode(fs, dirino, dir);
  return 0;
}

// take one entry out by joining it to the one before it, which is how
// ext4 frees a slot without moving anything
int
ext4_remove_entry(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, const char *name,
                  int namelen, uint64 *ino_out)
{
  uint64 nblocks = dir->size / fs->blocksize;
  uint32 usable = fs->blocksize - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);

  if (fs->readonly)
    return -EROFS;
  if (dir->flags & EXT4_INDEX_FL)
    return ext4_htree_remove(fs, dir, dirino, name, namelen, ino_out);

  for (uint64 bi = 0; bi < nblocks; bi++) {
    uint64 diskblk;
    if (ext4_bmap(fs, dir, bi, &diskblk) < 0 || diskblk == 0)
      continue;
    struct blkbuf *b = ext4_bread(fs, diskblk);
    if (b == 0)
      continue;

    uint32 off = 0;
    struct ext4_dir_entry_2 *prev = 0;
    while (off < usable) {
      struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)((uchar *)b->data + off);
      if (de->rec_len < 8 || off + de->rec_len > usable)
        break;
      if (de->inode && de->name_len == namelen && memcmp(de->name, name, namelen) == 0) {
        if (ino_out)
          *ino_out = de->inode;
        ext4_journal_access(fs, b);
        if (prev)
          prev->rec_len = (uint16)(prev->rec_len + de->rec_len);
        else
          de->inode = 0;
        ext4_dirblock_csum(fs, dirino, dir->generation, b->data);
        ext4_journal_dirty(fs, b);
        blk_put(b);
        ext4_touch(dir, 1);
        ext4_write_inode(fs, dirino, dir);
        return 0;
      }
      prev = de;
      off += de->rec_len;
    }
    blk_put(b);
  }
  return -ENOENT;
}

// ---- making and removing files ----

// a device node keeps its device number where its blocks would be, and
// has no blocks at all: i_block[0] holds (major << 8) | minor for the
// numbers that fit in a byte each, which is the form Linux has read
// since ext2 and the only one this kernel's device numbers need. the
// extent flag has to go with them, because an inode cannot both be a
// device and have an extent tree.
int
ext4_set_device(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, int major, int minor)
{
  struct ext4_inode *di = (struct ext4_inode *)in->raw;

  in->flags &= ~(uint32)EXT4_EXTENTS_FL;
  di->i_flags = in->flags;
  memset(di->i_block, 0, sizeof(di->i_block));
  di->i_block[0] = (uint32)(((major & 0xff) << 8) | (minor & 0xff));
  in->size = 0;
  in->blocks = 0;
  return ext4_write_inode(fs, ino, in);
}

// the device number an inode carries, in the same two forms Linux writes
void
ext4_device_of(struct ext4_inode_info *in, int *major, int *minor)
{
  const struct ext4_inode *di = (const struct ext4_inode *)in->raw;

  if (di->i_block[0]) {
    *major = (int)((di->i_block[0] >> 8) & 0xff);
    *minor = (int)(di->i_block[0] & 0xff);
  } else {
    // the wide form, which carries 12 bits of major and 20 of minor
    uint32 v = di->i_block[1];
    *major = (int)((v >> 8) & 0xfff);
    *minor = (int)((v & 0xff) | ((v >> 12) & 0xfff00));
  }
}

int
ext4_make_inode(struct ext4_fs *fs, uint64 dirino, uint16 mode, uint64 *ino_out,
                struct ext4_inode_info *out)
{
  uint64 ino;
  int is_dir = (mode & 0xf000) == 0x4000;
  int r = ext4_alloc_inode(fs, dirino, is_dir, &ino);

  if (r < 0)
    return r;
  memset(out, 0, sizeof(*out));
  out->ino = ino;
  out->mode = mode;
  out->nlink = 1;
  out->size = 0;
  out->blocks = 0;
  ext4_touch(out, 1);
  out->crtime = out->mtime;

  struct ext4_inode *di = (struct ext4_inode *)out->raw;
  di->i_extra_isize = 32; // what the format asks of a 256-byte inode
  extent_init(out);

  r = ext4_write_inode(fs, ino, out);
  if (r < 0) {
    ext4_free_inode(fs, ino, is_dir);
    return r;
  }
  *ino_out = ino;
  return 0;
}

// a new directory starts with the two entries every directory has
int
ext4_make_dir(struct ext4_fs *fs, struct ext4_inode_info *parent, uint64 parentino, uint64 ino,
              struct ext4_inode_info *in)
{
  uint64 diskblk;
  int r = ext4_bmap_alloc(fs, in, ino, 0, &diskblk);

  if (r < 0)
    return r;
  struct blkbuf *b = blk_get_zero(fs->disk, diskblk);
  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);

  uint32 usable = fs->blocksize - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
  struct ext4_dir_entry_2 *dot = b->data;
  dot->inode = (uint32)ino;
  dot->rec_len = 12;
  dot->name_len = 1;
  dot->file_type = EXT4_FT_DIR;
  dot->name[0] = '.';

  struct ext4_dir_entry_2 *dotdot = (struct ext4_dir_entry_2 *)((uchar *)b->data + 12);
  dotdot->inode = (uint32)parentino;
  dotdot->rec_len = (uint16)(usable - 12);
  dotdot->name_len = 2;
  dotdot->file_type = EXT4_FT_DIR;
  dotdot->name[0] = '.';
  dotdot->name[1] = '.';

  ext4_dirblock_csum(fs, ino, in->generation, b->data);
  ext4_journal_dirty(fs, b);
  blk_put(b);

  in->size = fs->blocksize;
  in->nlink = 2; // itself and its entry in the parent
  ext4_write_inode(fs, ino, in);

  parent->nlink++;
  ext4_write_inode(fs, parentino, parent);
  return 0;
}

// is a directory empty apart from . and ..?
int
ext4_dir_empty(struct ext4_fs *fs, struct ext4_inode_info *dir)
{
  uint64 pos = 0;
  char name[256];
  uint64 ino;
  uchar type;

  for (;;) {
    int r = ext4_readdir(fs, dir, &pos, name, sizeof(name), &ino, &type);
    if (r < 0)
      return r;
    if (r == 0)
      return 1;
    if (strncmp(name, ".", 2) == 0 || strncmp(name, "..", 3) == 0)
      continue;
    return 0;
  }
}
