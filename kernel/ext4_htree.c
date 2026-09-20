//
// hash-indexed directories, the writing of them.
//
// a big directory kept as a list of blocks costs a whole read of it to
// decide that a name is not there, and creating N files in one costs
// N^2 blocks read. ext4's answer is a tree of hashes hidden inside the
// directory's own blocks: block 0 holds "." and ".." and, behind a
// ".." whose length covers the rest of the block, an array of (hash,
// block) pairs sorted by hash. A lookup hashes the name, descends the
// array, and reads one leaf.
//
// the disguise matters: a reader that does not know about the index
// walks the directory block by block and sees only leaves, because the
// index blocks look like free space. that is why a file system with
// dir_index can still be read by something that has never heard of it,
// and why every write here has to keep the disguise intact.
//
// what this file adds to the read side in ext4_read.c: making a
// directory indexed when one block is no longer enough, splitting a
// leaf when it fills, growing the index a level when the root fills,
// and removing a name from a leaf.
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

#define DX_ENTRY_SIZE ((uint32)sizeof(struct ext4_dx_entry))

// one step of a descent: the node it was taken from, and which of its
// entries. a split has to put a new key next to the one it followed,
// which is why the whole path is kept and not only the leaf.
struct dx_frame {
  uint64 blk;    // the node's block in the directory file
  int is_root;
  uint32 at;     // the entry that covered the hash
  uint32 count;
};

// ---- checksums ----

// how many entries an index block has room for, and where its tail is.
// with metadata_csum the last eight bytes are the checksum, in the
// space of one entry, so the limit is one less than the block allows.
static uint32
dx_limit(struct ext4_fs *fs, uint32 eoff)
{
  uint32 fit = (fs->blocksize - eoff) / DX_ENTRY_SIZE;

  if (fs->has_metadata_csum && fit > 0)
    fit--;
  return fit;
}

void
ext4_dxblock_csum(struct ext4_fs *fs, uint64 ino, uint32 gen, void *block, int is_root)
{
  uint32 eoff = is_root ? EXT4_DX_ROOT_ENTRIES_OFF : EXT4_DX_NODE_ENTRIES_OFF;
  struct ext4_dx_countlimit *cl;
  struct ext4_dx_tail *t;
  uint32 count, size, crc;

  if (!fs->has_metadata_csum)
    return;
  cl = (struct ext4_dx_countlimit *)((uchar *)block + eoff);
  count = cl->count;
  size = eoff + count * DX_ENTRY_SIZE;
  if (size + sizeof(*t) > fs->blocksize)
    return;
  // the tail sits where the entries stop growing, which is after the
  // limit and not after the count: Linux puts it at limit entries.
  t = (struct ext4_dx_tail *)((uchar *)block + eoff + (uint32)cl->limit * DX_ENTRY_SIZE);
  t->dt_reserved = 0;
  t->dt_checksum = 0;
  crc = ext4_inode_seed(fs, ino, gen);
  crc = crc32c(crc, block, (int)size);
  crc = crc32c(crc, t, (int)sizeof(*t));
  t->dt_checksum = crc;
}

// ---- reading a directory block for writing ----

// the block of a directory, by its number in the file, with the write
// access the journal wants taken before anything changes it.
static struct blkbuf *
dir_bwrite(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 fileblk, int *err)
{
  struct blkbuf *b = ext4_dir_bread(fs, dir, fileblk, err);

  if (b == 0)
    return 0;
  ext4_journal_access(fs, b);
  return b;
}

// the usable bytes of a directory block: the checksum tail of a leaf
// takes twelve of them.
static uint32
leaf_usable(struct ext4_fs *fs)
{
  return fs->blocksize - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
}

static uint32
dirent_needed(int namelen)
{
  return (uint32)(8 + namelen + 3) & ~3U;
}

// ---- one block ----

// put a name in this block if there is room for it. the entries of a
// directory block are a list of records, each covering the space of
// the next until something is put there, so making room means finding
// a record with slack and splitting it.
int
ext4_dir_block_insert(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                      uint64 fileblk, const char *name, int namelen, uint64 ino, uchar type)
{
  uint32 need = dirent_needed(namelen);
  uint32 usable = leaf_usable(fs);
  uint32 off = 0;
  int err = 0;
  struct blkbuf *b = dir_bwrite(fs, dir, fileblk, &err);

  if (b == 0)
    return err ? err : -EIO;

  while (off < usable) {
    struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)((uchar *)b->data + off);
    if (de->rec_len < 8 || off + de->rec_len > usable)
      break;
    uint32 used = de->inode ? dirent_needed(de->name_len) : 0;
    if (de->rec_len - used >= need) {
      struct ext4_dir_entry_2 *nd;
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
      return 0;
    }
    off += de->rec_len;
  }
  blk_put(b);
  return -ENOSPC;
}

// another block at the end of the directory file, zeroed, in hand, and
// already counted in the directory's size.
int
ext4_dir_grow(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, uint64 *fileblk,
              struct blkbuf **out)
{
  uint64 nblocks = dir->size >> fs->blockbits;
  uint64 diskblk;
  struct blkbuf *b;
  int r = ext4_bmap_alloc(fs, dir, dirino, nblocks, &diskblk);

  if (r < 0)
    return r;
  b = blk_get_zero(fs->disk, diskblk);
  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);
  dir->size = (nblocks + 1) * fs->blocksize;
  // the size goes on the disk now rather than when the operation that
  // asked for the block finishes: the extent tree already points at it,
  // and an inode that still claimed the old size would leave a block
  // past the end of the file for e2fsck to find.
  r = ext4_write_inode(fs, dirino, dir);
  if (r < 0) {
    blk_put(b);
    return r;
  }
  *fileblk = nblocks;
  *out = b;
  return 0;
}

// ---- the index ----

// the hash this directory was built with, and how many levels of index
// it has. -EOPNOTSUPP for an index laid out in a way this cannot read,
// which is what tells the caller to leave the directory alone.
static int
dx_root_info(struct ext4_fs *fs, struct ext4_inode_info *dir, uint32 *version, uint32 *levels)
{
  const struct ext4_dx_root_info *info;
  struct blkbuf *b;
  int err = 0;

  b = ext4_dir_bread(fs, dir, 0, &err);
  if (b == 0)
    return err ? err : -EIO;
  info = (const struct ext4_dx_root_info *)((const uchar *)b->data + EXT4_DX_ROOT_INFO_OFF);
  if (info->reserved_zero != 0 || info->info_length != sizeof(*info)) {
    blk_put(b);
    return -EOPNOTSUPP;
  }
  *version = info->hash_version;
  *levels = info->indirect_levels;
  blk_put(b);
  if (*version <= DX_HASH_TEA && fs->hash_unsigned)
    *version += 3;
  if (*levels + 1 > EXT4_MAX_DX_LEVELS)
    return -EIO;
  return 0;
}

// walk down to the leaf that covers `hash`, keeping the path. entry 0
// of a node carries no key of its own: it covers every hash below the
// first one, which is why the search runs over 1..count-1.
static int
dx_probe(struct ext4_fs *fs, struct ext4_inode_info *dir, uint32 levels, uint32 hash,
         struct dx_frame *path, uint64 *leaf)
{
  uint64 nodeblk = 0, nblocks = ext4_dir_nblocks(fs, dir);

  for (uint32 i = 0; i <= levels; i++) {
    struct blkbuf *b;
    uint32 eoff = 0, count = 0, lo, hi, mid, at, block;
    int err = 0, r;

    b = ext4_dir_bread(fs, dir, nodeblk, &err);
    if (b == 0)
      return err ? err : -EIO;
    r = ext4_dx_node(fs, (const uchar *)b->data, i == 0, &eoff, &count);
    if (r < 0) {
      blk_put(b);
      return r;
    }
    const struct ext4_dx_entry *e = (const struct ext4_dx_entry *)((const uchar *)b->data + eoff);
    lo = 1;
    hi = count;
    while (lo < hi) {
      mid = lo + (hi - lo) / 2;
      if ((e[mid].hash & ~1u) > hash)
        hi = mid;
      else
        lo = mid + 1;
    }
    at = lo - 1;
    block = e[at].block;
    blk_put(b);

    if (block == 0 || (uint64)block >= nblocks)
      return -EIO;
    path[i].blk = nodeblk;
    path[i].is_root = (i == 0);
    path[i].at = at;
    path[i].count = count;
    nodeblk = block;
  }
  *leaf = nodeblk;
  return 0;
}

// put (hash, block) into a node, after the entry the descent took.
// -ENOSPC when the node is full, which is what makes the caller grow
// the tree or give up.
static int
dx_insert(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, struct dx_frame *f,
          uint32 hash, uint32 block)
{
  struct ext4_dx_countlimit *cl;
  struct ext4_dx_entry *e;
  uint32 eoff = 0, count = 0;
  int err = 0, r;
  struct blkbuf *b = dir_bwrite(fs, dir, f->blk, &err);

  if (b == 0)
    return err ? err : -EIO;
  r = ext4_dx_node(fs, (const uchar *)b->data, f->is_root, &eoff, &count);
  if (r < 0) {
    blk_put(b);
    return r;
  }
  cl = (struct ext4_dx_countlimit *)((uchar *)b->data + eoff);
  if (count >= cl->limit) {
    blk_put(b);
    return -ENOSPC;
  }
  e = (struct ext4_dx_entry *)((uchar *)b->data + eoff);
  // the entries are sorted by hash, so everything above the one the
  // descent took moves up by one to make room
  for (uint32 i = count; i > f->at + 1; i--)
    e[i] = e[i - 1];
  e[f->at + 1].hash = hash;
  e[f->at + 1].block = block;
  cl->count = (uint16)(count + 1);
  ext4_dxblock_csum(fs, dirino, dir->generation, b->data, f->is_root);
  ext4_journal_dirty(fs, b);
  blk_put(b);
  return 0;
}

// ---- splitting ----

// every entry of one leaf, with its hash, so that the half of them with
// the larger hashes can be moved somewhere else. a 4 KB block holds at
// most 4096/12 of them.
#define DX_MAX_LEAF_ENTRIES (4096 / 12)

struct dx_map {
  uint32 hash;
  uint16 off;
  uint16 size;
};

static void
dx_map_sort(struct dx_map *m, int n)
{
  // insertion sort: n is a few hundred at most, and the array is
  // already nearly sorted when the names came in hash order.
  for (int i = 1; i < n; i++) {
    struct dx_map v = m[i];
    int j = i - 1;
    while (j >= 0 && m[j].hash > v.hash) {
      m[j + 1] = m[j];
      j--;
    }
    m[j + 1] = v;
  }
}

// split one leaf in two: the entries with the larger hashes move to a
// new block, and the hash that divides them becomes the new key in the
// level above. returns the new block and that hash.
static int
dx_split_leaf(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, uint32 version,
              uint64 leafblk, uint64 *newblk, uint32 *split_hash)
{
  struct dx_map map[DX_MAX_LEAF_ENTRIES];
  uint32 usable = leaf_usable(fs);
  uint32 off = 0;
  int n = 0, err = 0, r;
  struct blkbuf *lb, *nb;
  uchar *old;

  lb = dir_bwrite(fs, dir, leafblk, &err);
  if (lb == 0)
    return err ? err : -EIO;

  // every name in the block, and where it is
  while (off < usable && n < DX_MAX_LEAF_ENTRIES) {
    const struct ext4_dir_entry_2 *de;
    r = ext4_dirent_check((const uchar *)lb->data, usable, off, &de);
    if (r < 0)
      break;
    if (de->inode != 0) {
      map[n].hash = ext4_dirhash_version(fs, version, de->name, de->name_len, 0);
      map[n].off = (uint16)off;
      map[n].size = (uint16)dirent_needed(de->name_len);
      n++;
    }
    off += (uint32)r;
  }
  if (n < 2) {
    blk_put(lb);
    return -ENOSPC; // nothing to move: one enormous name, or an empty block
  }
  dx_map_sort(map, n);

  // the halfway point, moved off any run of equal hashes: two names
  // with the same hash must stay in the same leaf, or a lookup that
  // stops at the first leaf would miss one.
  int half = n / 2;
  while (half < n && map[half].hash == map[half - 1].hash)
    half++;
  if (half >= n) {
    blk_put(lb);
    return -ENOSPC; // every name in the block hashes the same
  }

  r = ext4_dir_grow(fs, dir, dirino, newblk, &nb);
  if (r < 0) {
    blk_put(lb);
    return r;
  }

  // the new block starts as one free record covering all of it, and
  // the moved names are written into it one after another
  old = (uchar *)lb->data;
  struct ext4_dir_entry_2 *nd = (struct ext4_dir_entry_2 *)nb->data;
  nd->inode = 0;
  nd->rec_len = (uint16)usable;
  nd->name_len = 0;
  nd->file_type = 0;

  for (int i = half; i < n; i++) {
    const struct ext4_dir_entry_2 *sd = (const struct ext4_dir_entry_2 *)(old + map[i].off);
    r = 0;
    {
      // the same insertion the linear case does, straight into the
      // block in hand rather than through another read of it
      uint32 o = 0;
      int placed = 0;
      while (o < usable) {
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)((uchar *)nb->data + o);
        if (de->rec_len < 8 || o + de->rec_len > usable)
          break;
        uint32 used = de->inode ? dirent_needed(de->name_len) : 0;
        if (de->rec_len - used >= map[i].size) {
          struct ext4_dir_entry_2 *t;
          if (used == 0) {
            t = de;
            t->rec_len = de->rec_len;
          } else {
            uint32 rest = de->rec_len - used;
            de->rec_len = (uint16)used;
            t = (struct ext4_dir_entry_2 *)((uchar *)de + used);
            t->rec_len = (uint16)rest;
          }
          t->inode = sd->inode;
          t->name_len = sd->name_len;
          t->file_type = sd->file_type;
          memmove(t->name, sd->name, sd->name_len);
          placed = 1;
          break;
        }
        o += de->rec_len;
      }
      if (!placed)
        r = -EIO;
    }
    if (r < 0) {
      blk_put(nb);
      blk_put(lb);
      return r;
    }
  }

  // rebuild the old block with the names that stayed
  {
    uchar *copy = kalloc();
    if (copy == 0) {
      blk_put(nb);
      blk_put(lb);
      return -ENOMEM;
    }
    memmove(copy, old, fs->blocksize);
    memset(old, 0, fs->blocksize);
    struct ext4_dir_entry_2 *first = (struct ext4_dir_entry_2 *)old;
    first->inode = 0;
    first->rec_len = (uint16)usable;
    first->name_len = 0;
    first->file_type = 0;
    for (int i = 0; i < half; i++) {
      const struct ext4_dir_entry_2 *sd = (const struct ext4_dir_entry_2 *)(copy + map[i].off);
      uint32 o = 0;
      int placed = 0;
      while (o < usable) {
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(old + o);
        uint32 used = de->inode ? dirent_needed(de->name_len) : 0;
        if (de->rec_len - used >= map[i].size) {
          struct ext4_dir_entry_2 *t;
          if (used == 0) {
            t = de;
          } else {
            uint32 rest = de->rec_len - used;
            de->rec_len = (uint16)used;
            t = (struct ext4_dir_entry_2 *)((uchar *)de + used);
            t->rec_len = (uint16)rest;
          }
          t->inode = sd->inode;
          t->name_len = sd->name_len;
          t->file_type = sd->file_type;
          memmove(t->name, sd->name, sd->name_len);
          placed = 1;
          break;
        }
        o += de->rec_len;
      }
      if (!placed) {
        // the names that stay came out of this very block, so this
        // cannot happen; if it ever does, losing a name silently is
        // the one outcome that must not be allowed.
        kfree(copy);
        blk_put(lb);
        return -EIO;
      }
    }
    kfree(copy);
  }

  *split_hash = map[half].hash;
  ext4_dirblock_csum(fs, dirino, dir->generation, nb->data);
  ext4_journal_dirty(fs, nb);
  blk_put(nb);
  ext4_dirblock_csum(fs, dirino, dir->generation, lb->data);
  ext4_journal_dirty(fs, lb);
  blk_put(lb);
  return 0;
}

// an interior node is full: give half its entries to a new node beside
// it, and hand back the hash that divides them so that the level above
// can point at it. this is the same idea as splitting a leaf, with
// index entries instead of names, and it is what lets a tree of one
// level hold more than one node's worth of leaves.
static int
dx_split_node(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, uint64 nodeblk,
              uint64 *newblk, uint32 *split_hash)
{
  struct blkbuf *ob, *nb;
  struct ext4_dx_countlimit *ocl, *ncl;
  struct ext4_dx_entry *oe, *ne;
  uint32 eoff = 0, count = 0, half;
  int err = 0, r;

  ob = dir_bwrite(fs, dir, nodeblk, &err);
  if (ob == 0)
    return err ? err : -EIO;
  r = ext4_dx_node(fs, (const uchar *)ob->data, 0, &eoff, &count);
  if (r < 0) {
    blk_put(ob);
    return r;
  }
  if (count < 2) {
    blk_put(ob);
    return -ENOSPC;
  }

  r = ext4_dir_grow(fs, dir, dirino, newblk, &nb);
  if (r < 0) {
    blk_put(ob);
    return r;
  }
  if (*newblk > 0xffffffffu) {
    blk_put(nb);
    blk_put(ob);
    return -EFBIG;
  }

  // the new node hides behind one free record covering the whole block
  struct ext4_dir_entry_2 *fake = (struct ext4_dir_entry_2 *)nb->data;
  fake->inode = 0;
  fake->rec_len = (uint16)fs->blocksize;
  fake->name_len = 0;
  fake->file_type = 0;

  half = count / 2;
  oe = (struct ext4_dx_entry *)((uchar *)ob->data + eoff);
  ne = (struct ext4_dx_entry *)((uchar *)nb->data + EXT4_DX_NODE_ENTRIES_OFF);
  for (uint32 i = 0; i < count - half; i++)
    ne[i] = oe[half + i];
  // the key the level above will hold for the new node is the lowest
  // hash it covers, and that entry becomes its entry 0, which carries
  // no key of its own.
  *split_hash = oe[half].hash;
  ncl = (struct ext4_dx_countlimit *)((uchar *)nb->data + EXT4_DX_NODE_ENTRIES_OFF);
  ncl->count = (uint16)(count - half);
  ncl->limit = (uint16)dx_limit(fs, EXT4_DX_NODE_ENTRIES_OFF);

  ocl = (struct ext4_dx_countlimit *)((uchar *)ob->data + eoff);
  ocl->count = (uint16)half;

  ext4_dxblock_csum(fs, dirino, dir->generation, nb->data, 0);
  ext4_journal_dirty(fs, nb);
  blk_put(nb);
  ext4_dxblock_csum(fs, dirino, dir->generation, ob->data, 0);
  ext4_journal_dirty(fs, ob);
  blk_put(ob);
  return 0;
}

// the root is full and the tree has one level: give it another. the
// root's entries move into a new node, and the root keeps one entry
// pointing at it.
static int
dx_grow_levels(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino)
{
  struct blkbuf *rb, *nb;
  struct ext4_dx_root_info *info;
  struct ext4_dx_countlimit *rcl, *ncl;
  struct ext4_dx_entry *re, *ne;
  uint32 eoff = 0, count = 0;
  uint64 newblk;
  int err = 0, r;

  rb = dir_bwrite(fs, dir, 0, &err);
  if (rb == 0)
    return err ? err : -EIO;
  r = ext4_dx_node(fs, (const uchar *)rb->data, 1, &eoff, &count);
  if (r < 0) {
    blk_put(rb);
    return r;
  }
  info = (struct ext4_dx_root_info *)((uchar *)rb->data + EXT4_DX_ROOT_INFO_OFF);
  if (info->indirect_levels != 0) {
    blk_put(rb);
    return -ENOSPC; // two levels is as deep as this builds
  }

  r = ext4_dir_grow(fs, dir, dirino, &newblk, &nb);
  if (r < 0) {
    blk_put(rb);
    return r;
  }
  if (newblk > 0xffffffffu) {
    blk_put(nb);
    blk_put(rb);
    return -EFBIG;
  }

  // the new node hides behind one free record covering the whole block
  struct ext4_dir_entry_2 *fake = (struct ext4_dir_entry_2 *)nb->data;
  fake->inode = 0;
  fake->rec_len = (uint16)fs->blocksize;
  fake->name_len = 0;
  fake->file_type = 0;

  re = (struct ext4_dx_entry *)((uchar *)rb->data + eoff);
  ne = (struct ext4_dx_entry *)((uchar *)nb->data + EXT4_DX_NODE_ENTRIES_OFF);
  for (uint32 i = 0; i < count; i++)
    ne[i] = re[i];
  ncl = (struct ext4_dx_countlimit *)((uchar *)nb->data + EXT4_DX_NODE_ENTRIES_OFF);
  ncl->count = (uint16)count;
  ncl->limit = (uint16)dx_limit(fs, EXT4_DX_NODE_ENTRIES_OFF);

  // entry 0's hash word is the count and the limit, so it is written
  // through the countlimit and never as a hash: writing a zero there
  // would say the node has room for nothing and hold nothing.
  re[0].block = (uint32)newblk;
  rcl = (struct ext4_dx_countlimit *)((uchar *)rb->data + eoff);
  rcl->count = 1;
  rcl->limit = (uint16)dx_limit(fs, eoff);
  info->indirect_levels = 1;

  ext4_dxblock_csum(fs, dirino, dir->generation, nb->data, 0);
  ext4_journal_dirty(fs, nb);
  blk_put(nb);
  ext4_dxblock_csum(fs, dirino, dir->generation, rb->data, 1);
  ext4_journal_dirty(fs, rb);
  blk_put(rb);
  return 0;
}

// ---- making a directory indexed ----

// the superblock has to admit that this file system has indexed
// directories before one of them is written, or a reader that trusts
// the feature word would walk an index as though it were free space.
static int
set_dir_index_feature(struct ext4_fs *fs)
{
  struct blkbuf *b;
  struct ext4_super_block *sb;

  if (fs->feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX)
    return 0;
  b = blk_get(fs->disk, 0);
  if (b == 0)
    return -EIO;
  ext4_journal_access(fs, b);
  sb = (struct ext4_super_block *)((uchar *)b->data + 1024);
  sb->s_feature_compat |= EXT4_FEATURE_COMPAT_DIR_INDEX;
  ext4_super_csum(fs, sb);
  ext4_journal_dirty(fs, b);
  blk_put(b);
  fs->feature_compat |= EXT4_FEATURE_COMPAT_DIR_INDEX;
  return 0;
}

// turn a one-block linear directory into an indexed one: everything but
// "." and ".." moves to a block of its own, and the space that is left
// in block 0 becomes the root of the index.
int
ext4_dir_make_indexed(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino)
{
  struct blkbuf *rb, *nb;
  struct ext4_dx_root_info *info;
  struct ext4_dx_countlimit *cl;
  struct ext4_dx_entry *e;
  uint32 usable = leaf_usable(fs);
  uint64 newblk;
  uchar *copy;
  int err = 0, r;

  if (dir->flags & EXT4_INDEX_FL)
    return 0;
  if (dir->size != fs->blocksize)
    return -EOPNOTSUPP; // only a directory that is still one block
  r = set_dir_index_feature(fs);
  if (r < 0)
    return r;

  rb = dir_bwrite(fs, dir, 0, &err);
  if (rb == 0)
    return err ? err : -EIO;

  // block 0 must start with "." and ".." for the index to hide behind
  struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)rb->data;
  struct ext4_dir_entry_2 *dotdot = (struct ext4_dir_entry_2 *)((uchar *)rb->data + 12);
  if (dot->rec_len != 12 || dot->name_len != 1 || dot->name[0] != '.' || dotdot->name_len != 2) {
    blk_put(rb);
    return -EOPNOTSUPP;
  }

  copy = kalloc();
  if (copy == 0) {
    blk_put(rb);
    return -ENOMEM;
  }
  memmove(copy, rb->data, fs->blocksize);

  r = ext4_dir_grow(fs, dir, dirino, &newblk, &nb);
  if (r < 0) {
    kfree(copy);
    blk_put(rb);
    return r;
  }

  // every name but the two that stay goes into the new block
  struct ext4_dir_entry_2 *nd = (struct ext4_dir_entry_2 *)nb->data;
  nd->inode = 0;
  nd->rec_len = (uint16)usable;
  nd->name_len = 0;
  nd->file_type = 0;
  {
    uint32 off = 24; // past "." and ".."
    uint32 put = 0;
    while (off < usable) {
      const struct ext4_dir_entry_2 *sd = (const struct ext4_dir_entry_2 *)(copy + off);
      if (sd->rec_len < 8 || off + sd->rec_len > usable)
        break;
      if (sd->inode != 0) {
        struct ext4_dir_entry_2 *t = (struct ext4_dir_entry_2 *)((uchar *)nb->data + put);
        uint32 need = dirent_needed(sd->name_len);
        if (put + need > usable) {
          kfree(copy);
          blk_put(nb);
          blk_put(rb);
          return -ENOSPC;
        }
        t->inode = sd->inode;
        t->name_len = sd->name_len;
        t->file_type = sd->file_type;
        memmove(t->name, sd->name, sd->name_len);
        t->rec_len = (uint16)need;
        put += need;
      }
      off += sd->rec_len;
    }
    // the last record covers the rest of the block
    if (put == 0) {
      nd->rec_len = (uint16)usable;
    } else {
      struct ext4_dir_entry_2 *last = 0;
      uint32 o = 0;
      while (o < put) {
        last = (struct ext4_dir_entry_2 *)((uchar *)nb->data + o);
        o += last->rec_len;
      }
      last->rec_len = (uint16)(usable - (o - last->rec_len));
    }
  }
  kfree(copy);

  // block 0 keeps "." and "..", and ".." grows to cover the rest so
  // that a reader without dir_index sees no entries beyond them
  dotdot->rec_len = (uint16)(fs->blocksize - 12);
  memset((uchar *)rb->data + 24, 0, fs->blocksize - 24);

  info = (struct ext4_dx_root_info *)((uchar *)rb->data + EXT4_DX_ROOT_INFO_OFF);
  info->reserved_zero = 0;
  // the version as the superblock stores it: mount adds three to it
  // when a char is unsigned here, and the root block records what was
  // on the disk rather than what this kernel made of it.
  info->hash_version = (uchar)(fs->hash_unsigned && fs->hash_version > DX_HASH_TEA
                                   ? fs->hash_version - 3
                                   : fs->hash_version);
  info->info_length = sizeof(*info);
  info->indirect_levels = 0;
  info->unused_flags = 0;

  // entry 0 has no key: its hash word is where the count and the limit
  // live, which is why they are written after the block number and the
  // hash is not written at all.
  e = (struct ext4_dx_entry *)((uchar *)rb->data + EXT4_DX_ROOT_ENTRIES_OFF);
  e[0].block = (uint32)newblk;
  cl = (struct ext4_dx_countlimit *)((uchar *)rb->data + EXT4_DX_ROOT_ENTRIES_OFF);
  cl->limit = (uint16)dx_limit(fs, EXT4_DX_ROOT_ENTRIES_OFF);
  cl->count = 1;

  ext4_dirblock_csum(fs, dirino, dir->generation, nb->data);
  ext4_journal_dirty(fs, nb);
  blk_put(nb);
  ext4_dxblock_csum(fs, dirino, dir->generation, rb->data, 1);
  ext4_journal_dirty(fs, rb);
  blk_put(rb);

  dir->flags |= EXT4_INDEX_FL;
  ext4_touch(dir, 1);
  return ext4_write_inode(fs, dirino, dir);
}

// ---- the two operations ----

int
ext4_htree_add(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino, const char *name,
               int namelen, uint64 ino, uchar type)
{
  struct dx_frame path[EXT4_MAX_DX_LEVELS];
  uint32 version = 0, levels = 0, hash, split = 0;
  uint64 leaf = 0, newblk = 0;
  int r;

  r = dx_root_info(fs, dir, &version, &levels);
  if (r < 0)
    return r;
  hash = ext4_dirhash_version(fs, version, name, namelen, 0);

  r = dx_probe(fs, dir, levels, hash, path, &leaf);
  if (r < 0)
    return r;
  r = ext4_dir_block_insert(fs, dir, dirino, leaf, name, namelen, ino, type);
  if (r != -ENOSPC)
    return r;

  // the leaf is full. split it, put the new key in the level above, and
  // then the name goes in whichever half covers its hash.
  r = dx_split_leaf(fs, dir, dirino, version, leaf, &newblk, &split);
  if (r < 0)
    return r;
  if (newblk > 0xffffffffu)
    return -EFBIG;

  r = dx_insert(fs, dir, dirino, &path[levels], split, (uint32)newblk);
  if (r == -ENOSPC) {
    // the node that should hold the new key is full too. if it is an
    // interior node it splits the same way a leaf does; if it is the
    // root, the tree gains a level and the root's entries move into a
    // node of their own. either way the shape has changed, so the
    // place for the key is looked for again from the top.
    if (levels > 0) {
      uint64 nodeblk = 0;
      uint32 nodesplit = 0;

      r = dx_split_node(fs, dir, dirino, path[levels].blk, &nodeblk, &nodesplit);
      if (r == 0)
        r = dx_insert(fs, dir, dirino, &path[levels - 1], nodesplit, (uint32)nodeblk);
    } else {
      r = dx_grow_levels(fs, dir, dirino);
    }
    if (r < 0)
      return r;
    r = dx_root_info(fs, dir, &version, &levels);
    if (r < 0)
      return r;
    r = dx_probe(fs, dir, levels, split, path, &leaf);
    if (r < 0)
      return r;
    r = dx_insert(fs, dir, dirino, &path[levels], split, (uint32)newblk);
  }
  if (r < 0)
    return r;

  r = dx_probe(fs, dir, levels, hash, path, &leaf);
  if (r < 0)
    return r;
  r = ext4_dir_block_insert(fs, dir, dirino, leaf, name, namelen, ino, type);
  if (r < 0)
    return r;
  ext4_touch(dir, 1);
  return ext4_write_inode(fs, dirino, dir);
}

int
ext4_htree_remove(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                  const char *name, int namelen, uint64 *ino_out)
{
  struct dx_frame path[EXT4_MAX_DX_LEVELS];
  uint32 version = 0, levels = 0, hash;
  uint32 usable = leaf_usable(fs);
  uint64 leaf = 0;
  int r, err = 0;

  r = dx_root_info(fs, dir, &version, &levels);
  if (r < 0)
    return r;
  hash = ext4_dirhash_version(fs, version, name, namelen, 0);

  // a name may be in the leaf the hash points at or in one of the
  // leaves after it, when several share a hash; the index marks a
  // continued run with the low bit of the key, and the read side walks
  // it. removing only ever has to look where a lookup would find it.
  for (int tries = 0; tries < EXT4_MAX_DX_RETRIES; tries++) {
    r = dx_probe(fs, dir, levels, hash, path, &leaf);
    if (r < 0)
      return r;

    struct blkbuf *b = dir_bwrite(fs, dir, leaf, &err);
    if (b == 0)
      return err ? err : -EIO;
    uint32 off = 0;
    struct ext4_dir_entry_2 *prev = 0;
    while (off < usable) {
      struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)((uchar *)b->data + off);
      if (de->rec_len < 8 || off + de->rec_len > usable)
        break;
      if (de->inode && de->name_len == namelen && memcmp(de->name, name, namelen) == 0) {
        if (ino_out)
          *ino_out = de->inode;
        if (prev)
          prev->rec_len = (uint16)(prev->rec_len + de->rec_len);
        else
          de->inode = 0;
        ext4_dirblock_csum(fs, dirino, dir->generation, b->data);
        ext4_journal_dirty(fs, b);
        blk_put(b);
        ext4_touch(dir, 1);
        return ext4_write_inode(fs, dirino, dir);
      }
      prev = de;
      off += de->rec_len;
    }
    blk_put(b);
    // not in this leaf: if the key that led here was marked as
    // continued, the next leaf may have it.
    if (!(hash & 1))
      break;
    hash++;
  }
  return -ENOENT;
}
