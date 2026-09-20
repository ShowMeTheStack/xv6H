//
// the read side of ext4: mount, inodes, extents, directories.
//
// the shape of this file follows the shape of the format. a mount reads
// the superblock and decides whether it understands the image at all; an
// inode is found from the group descriptors; the bytes of a file are found
// by mapping a file block to a disk block, either through the extent tree
// that ext4 uses or through the indirect blocks that ext2 used and that a
// Linux-made image may still hold for an old inode; a directory is a file
// whose bytes are entries, laid out either as a plain list or behind a hash
// index.
//
// two rules run through all of it.
//
// the first is about locks. a block is reached only through blk_get, which
// returns it locked, and this file never holds two blocks at once. where a
// walk has to move from one block to the next it copies out the few bytes
// it needs -- a child block number, an index entry -- releases, and only
// then reads the next. the block cache makes the re-reads cheap, and the
// code needs no lock order because it never holds a second lock to order.
//
// the second is about trust. these images come from elsewhere and may be
// corrupt or built to attack us, so nothing read off the disk is used
// before it is checked against a number that was itself checked: block
// numbers against s_blocks_count, inode numbers against s_inodes_count,
// a record length against the end of its block, a tree depth against a
// fixed bound. every loop that follows a disk pointer has a bound that
// does not come from the disk. a structure that fails a check makes the
// operation return -EIO; it never makes the driver read outside a buffer
// and it never makes it spin.
//
// checksums: this driver neither verifies nor computes them. it mounts an
// image with metadata_csum and reads it, relying on the structural checks
// above rather than on crc32c. that is safe for reading -- a bad checksum
// on a structure that passes every bound check can only give wrong data,
// not a wrong access -- but it is not safe for writing, so the write side
// must add crc32c before it touches a block, and until it does, mounting
// is read-only whatever the caller asks for.
//
// errors: -EIO means the image is malformed or unreadable, since the
// errno list here has no EUCLEAN to tell the two apart.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "blk.h"
#include "errno.h"
#include "ext4_ondisk.h"
#include "ext4.h"
#include "ext4_internal.h"

// the driver overlays struct ext4_inode on this field, so it has to be at
// least as aligned as that structure needs.
_Static_assert(__builtin_offsetof(struct ext4_inode_info, raw) % 8 == 0,
               "the raw inode must be aligned");
_Static_assert(sizeof(((struct ext4_inode_info *)0)->raw) >= EXT4_INODE_RAW_SIZE,
               "the raw inode must hold a whole inode");

// the features this driver knows how to read. anything outside this mask
// in s_feature_incompat means the image uses a layout we would misread, so
// the mount is refused rather than guessed at.
//
//   filetype   the type byte in a directory entry
//   recover    the journal has records to replay. we do not replay them,
//              so the image is mounted read-only and stale; the caller is
//              told, because that is a real difference in what it sees.
//   extents    the extent tree in i_block
//   64bit      64-bit block numbers and 64-byte group descriptors
//   flex_bg    the group metadata is not where the group is, which only
//              matters to an allocator; a reader follows bg_inode_table
//   csum_seed  s_checksum_seed holds the seed instead of the uuid, which
//              matters only to code that computes checksums
#define EXT4_INCOMPAT_SUPPORTED \
  (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_RECOVER | \
   EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT | \
   EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED)

// the ro_compat features a reader is entitled to ignore, and does. the two
// that are not merely informational are large_file and huge_file, which
// change how i_size and i_blocks are read, and both are handled.
#define EXT4_RO_COMPAT_SUPPORTED \
  (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE | \
   EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM | \
   EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | \
   EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)

// features that are refused outright, whichever word they live in, because
// the design says an image using one is not an image this kernel serves.
// they are named separately from the "unknown feature" case so the message
// can say which one it was.
#define EXT4_INCOMPAT_REFUSED \
  (EXT4_FEATURE_INCOMPAT_INLINE_DATA | EXT4_FEATURE_INCOMPAT_ENCRYPT | \
   EXT4_FEATURE_INCOMPAT_META_BG | EXT4_FEATURE_INCOMPAT_MMP | \
   EXT4_FEATURE_INCOMPAT_CASEFOLD | EXT4_FEATURE_INCOMPAT_COMPRESSION | \
   EXT4_FEATURE_INCOMPAT_JOURNAL_DEV)
#define EXT4_RO_COMPAT_REFUSED \
  (EXT4_FEATURE_RO_COMPAT_VERITY | EXT4_FEATURE_RO_COMPAT_BIGALLOC | \
   EXT4_FEATURE_RO_COMPAT_QUOTA | EXT4_FEATURE_RO_COMPAT_PROJECT)


#define ext4_min(a, b) ((a) < (b) ? (a) : (b))

// ---- the block cache, with the file system's numbers checked first ----

// read one file system block. the block number is checked against the
// superblock's own block count here, once, so that no caller has to
// remember to; a block past the end of the file system is corruption,
// not a short read.
struct blkbuf *
ext4_bread(struct ext4_fs *fs, uint64 blkno)
{
  if (blkno == 0 || blkno >= fs->blocks_count)
    return 0;
  return blk_get(fs->disk, blkno);
}

// whether this disk holds an ext4 file system, asked before anything is
// mounted and without a word on the console: at boot the root device is
// tried as ext4 first and as the old format second, and the one that is
// not there must not look like a failure.
int
ext4_probe(struct gendisk *disk)
{
  struct blkbuf *b = blk_get(disk, 0);
  struct ext4_super_block *sb;
  int ok;

  if (b == 0)
    return 0;
  sb = (struct ext4_super_block *)((uchar *)b->data + 1024);
  ok = sb->s_magic == EXT4_SUPER_MAGIC;
  blk_put(b);
  return ok;
}

// ---- mount ----

// the group descriptor for one group, copied out of its block. the caller
// gets a whole 64-byte descriptor even on a file system with 32-byte ones;
// the high halves are then zero, which is exactly what they mean without
// the 64bit feature.
int
ext4_group_desc(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *out)
{
  struct blkbuf *b;
  uint64 byte, blk;
  uint32 off;

  if (group >= fs->ngroups)
    return -EINVAL;
  byte = (uint64)group * fs->desc_size;
  blk = fs->gdt_block + byte / fs->blocksize;
  off = (uint32)(byte % fs->blocksize);

  memset(out, 0, sizeof(*out));
  b = ext4_bread(fs, blk);
  if (b == 0)
    return -EIO;
  memmove(out, (uchar *)b->data + off, ext4_min(fs->desc_size, (uint32)sizeof(*out)));
  blk_put(b);
  return 0;
}

// a power of two, and nothing else, is a legal size for an inode or a
// group descriptor; the format says so and the arithmetic below relies on
// it to keep a structure from straddling two blocks.
static int
ext4_is_pow2(uint32 v)
{
  return v != 0 && (v & (v - 1)) == 0;
}

// which way the directory hashes treat a char. an image records the answer
// in s_flags because a name hashes differently on the two kinds of machine,
// and an image that says nothing is assumed to have come from a machine
// like the one we are on.
static int
ext4_hash_is_unsigned(uint32 sb_flags)
{
  if (sb_flags & EXT2_FLAGS_UNSIGNED_HASH)
    return 1;
  if (sb_flags & EXT2_FLAGS_SIGNED_HASH)
    return 0;
  return ((char)-1) > 0;
}

struct ext4_fs *
ext4_mount(struct gendisk *disk, int readonly)
{
  struct ext4_fs *fs;
  struct blkbuf *b;
  const struct ext4_super_block *sb;
  uint32 bad, logbs, i;
  uint64 gblocks;

  if (disk == 0)
    return 0;

  fs = kalloc();
  if (fs == 0)
    return 0;
  memset(fs, 0, sizeof(*fs));
  fs->disk = disk;

  // the superblock is at byte 1024 of the file system, which is inside the
  // first 4096-byte block whatever block size the image uses. that is why
  // it can be read before the block size is known.
  b = blk_get(disk, EXT4_SUPER_OFFSET / BLKSIZE);
  if (b == 0) {
    printk("ext4: %s: cannot read the superblock\n", disk->name);
    kfree(fs);
    return 0;
  }
  sb = (const struct ext4_super_block *)((uchar *)b->data + EXT4_SUPER_OFFSET % BLKSIZE);

  if (sb->s_magic != EXT4_SUPER_MAGIC) {
    printk("ext4: %s: not an ext4 file system (magic 0x%x)\n", disk->name, (uint32)sb->s_magic);
    goto fail;
  }

  // the block layer hands out 4096-byte blocks and this driver hands them
  // straight to the file system, so a file system block and a cache block
  // are the same thing and have the same number. an image with any other
  // block size would need a second layer of mapping that does not exist.
  logbs = sb->s_log_block_size;
  if (logbs != 2) {
    printk("ext4: %s: block size %u is not 4096, which is all this kernel maps\n", disk->name,
           logbs > 20 ? 0 : (uint32)EXT4_MIN_BLOCK_SIZE << logbs);
    goto fail;
  }
  fs->blocksize = BLKSIZE;
  fs->blockbits = BLKSHIFT;

  fs->feature_compat = sb->s_feature_compat;
  fs->feature_incompat = sb->s_feature_incompat;
  fs->feature_ro_compat = sb->s_feature_ro_compat;

  // a refused feature gets its own message, because "unsupported incompat
  // 0x8000" is a worse thing to read at boot than "inline_data".
  bad = fs->feature_incompat & EXT4_INCOMPAT_REFUSED;
  if (bad) {
    printk("ext4: %s: refusing an image with %s\n", disk->name,
           (bad & EXT4_FEATURE_INCOMPAT_INLINE_DATA) ? "inline_data" :
           (bad & EXT4_FEATURE_INCOMPAT_ENCRYPT) ? "encrypt" :
           (bad & EXT4_FEATURE_INCOMPAT_META_BG) ? "meta_bg" :
           (bad & EXT4_FEATURE_INCOMPAT_MMP) ? "mmp" :
           (bad & EXT4_FEATURE_INCOMPAT_CASEFOLD) ? "casefold" :
           (bad & EXT4_FEATURE_INCOMPAT_COMPRESSION) ? "compression" : "journal_dev");
    goto fail;
  }
  bad = fs->feature_ro_compat & EXT4_RO_COMPAT_REFUSED;
  if (bad) {
    printk("ext4: %s: refusing an image with %s\n", disk->name,
           (bad & EXT4_FEATURE_RO_COMPAT_VERITY) ? "verity" :
           (bad & EXT4_FEATURE_RO_COMPAT_BIGALLOC) ? "bigalloc" :
           (bad & EXT4_FEATURE_RO_COMPAT_QUOTA) ? "quota" : "project");
    goto fail;
  }

  // an incompat bit we do not know is a layout we would misread, so there
  // is nothing to do but refuse. this is the whole point of the word.
  bad = fs->feature_incompat & ~(uint32)EXT4_INCOMPAT_SUPPORTED;
  if (bad) {
    printk("ext4: %s: unsupported incompat features 0x%x\n", disk->name, bad);
    goto fail;
  }

  // the block size settles what a group descriptor and an inode may be.
  fs->desc_size = EXT4_MIN_DESC_SIZE;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
    fs->desc_size = sb->s_desc_size;
    if (fs->desc_size < EXT4_MIN_DESC_SIZE_64BIT || fs->desc_size > fs->blocksize ||
        !ext4_is_pow2(fs->desc_size)) {
      printk("ext4: %s: group descriptor size %u is not legal\n", disk->name, fs->desc_size);
      goto fail;
    }
  }
  fs->descs_per_block = fs->blocksize / fs->desc_size;

  fs->inode_size = EXT4_GOOD_OLD_INODE_SIZE;
  if (sb->s_rev_level > 0)
    fs->inode_size = sb->s_inode_size;
  if (fs->inode_size < EXT4_GOOD_OLD_INODE_SIZE || fs->inode_size > fs->blocksize ||
      !ext4_is_pow2(fs->inode_size)) {
    printk("ext4: %s: inode size %u is not legal\n", disk->name, fs->inode_size);
    goto fail;
  }
  fs->inodes_per_block = fs->blocksize / fs->inode_size;

  fs->blocks_count = sb->s_blocks_count_lo;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    fs->blocks_count |= (uint64)sb->s_blocks_count_hi << 32;
  fs->free_blocks = sb->s_free_blocks_count_lo;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    fs->free_blocks |= (uint64)sb->s_free_blocks_count_hi << 32;
  fs->inodes_count = sb->s_inodes_count;
  fs->free_inodes = sb->s_free_inodes_count;
  fs->blocks_per_group = sb->s_blocks_per_group;
  fs->inodes_per_group = sb->s_inodes_per_group;
  fs->first_data_block = sb->s_first_data_block;
  fs->first_ino = sb->s_first_ino;
  if (fs->first_ino == 0)
    fs->first_ino = 11;

  // a block group's bitmaps are one block each, so neither count may need
  // more bits than a block holds; an inode table has to fit in the group;
  // and the root inode has to exist at all.
  if (fs->blocks_per_group == 0 || fs->blocks_per_group > fs->blocksize * 8 ||
      fs->inodes_per_group == 0 || fs->inodes_per_group > fs->blocksize * 8 ||
      fs->inodes_count < EXT4_ROOT_INO || fs->blocks_count <= fs->first_data_block ||
      fs->first_data_block >= fs->blocks_count) {
    printk("ext4: %s: the superblock's geometry is not self-consistent\n", disk->name);
    goto fail;
  }

  gblocks = fs->blocks_count - fs->first_data_block;
  fs->ngroups = (uint32)((gblocks + fs->blocks_per_group - 1) / fs->blocks_per_group);
  if (fs->ngroups == 0 || (uint64)fs->ngroups * fs->inodes_per_group < fs->inodes_count) {
    printk("ext4: %s: %u groups cannot hold %u inodes\n", disk->name, fs->ngroups,
           fs->inodes_count);
    goto fail;
  }

  // with a 4096-byte block the superblock shares block 0 with nothing, so
  // the group descriptors begin in the next block. (that is the general
  // rule s_first_data_block + 1, which for this block size is block 1.)
  fs->gdt_block = (uint64)fs->first_data_block + 1;
  if (fs->gdt_block + ((uint64)fs->ngroups * fs->desc_size + fs->blocksize - 1) / fs->blocksize >
      fs->blocks_count) {
    printk("ext4: %s: the group descriptor table runs past the end of the image\n", disk->name);
    goto fail;
  }

  fs->has_metadata_csum = (fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
  // the seed of every metadata checksum. the superblock carries one of
  // its own only when the csum_seed feature says so, which is what lets
  // tune2fs change a uuid without rewriting the whole file system;
  // without it every reader derives the same seed from the uuid, and a
  // driver that trusts the stored field instead gets zero and disagrees
  // with Linux about every checksum it writes.
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
    fs->csum_seed = sb->s_checksum_seed;
  else
    fs->csum_seed = crc32c(~0U, sb->s_uuid, sizeof(sb->s_uuid));
  memmove(fs->uuid, sb->s_uuid, sizeof(fs->uuid));
  fs->journal_inum = sb->s_journal_inum;

  // the directory hash: the seed is per file system, the function is named
  // by the superblock but may be overridden per directory by the htree
  // root, and the sign of a char is recorded separately from both.
  for (i = 0; i < 4; i++)
    fs->hash_seed[i] = sb->s_hash_seed[i];
  fs->hash_unsigned = ext4_hash_is_unsigned(sb->s_flags);
  fs->hash_version = sb->s_def_hash_version;
  if (fs->hash_version <= DX_HASH_TEA && fs->hash_unsigned)
    fs->hash_version += 3;

  // a read-write mount needs every ro_compat feature to be one this
  // driver knows, because those are exactly the features whose meaning
  // a writer must respect.
  fs->readonly = readonly;
  if (fs->feature_ro_compat & ~(uint32)EXT4_RO_COMPAT_SUPPORTED)
    fs->readonly = 1;
  blk_put(b);
  b = 0;

  printk("ext4: %s: %lu blocks of %u bytes, %u inodes in %u groups, inode size %u\n", disk->name,
         fs->blocks_count, fs->blocksize, fs->inodes_count, fs->ngroups, fs->inode_size);
  printk("ext4: %s: features compat 0x%x incompat 0x%x ro_compat 0x%x, %u-byte descriptors\n",
         disk->name, fs->feature_compat, fs->feature_incompat, fs->feature_ro_compat,
         fs->desc_size);
  if (fs->has_metadata_csum)
    printk("ext4: %s: metadata_csum is set; checksums are not verified\n", disk->name);
  bad = fs->feature_ro_compat & ~(uint32)EXT4_RO_COMPAT_SUPPORTED;
  if (bad)
    printk("ext4: %s: unknown ro_compat features 0x%x; a read-write mount must refuse them\n",
           disk->name, bad);
  if (!readonly && fs->readonly)
    printk("ext4: %s: asked for read-write, mounted read-only instead\n", disk->name);
  printk("ext4: %s: mounted %s\n", disk->name, fs->readonly ? "read-only" : "read-write");
  return fs;

fail:
  if (b)
    blk_put(b);
  kfree(fs);
  return 0;
}

void
ext4_unmount(struct ext4_fs *fs)
{
  if (fs == 0)
    return;
  // nothing of this mount lives anywhere but in the block cache, and the
  // cache belongs to the disk, not to the mount, so there is nothing to
  // flush. the write side will have to commit the journal here.
  kfree(fs);
}

uint64
ext4_root_ino(struct ext4_fs *fs)
{
  (void)fs;
  return EXT4_ROOT_INO;
}

void
ext4_statfs(struct ext4_fs *fs, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
            uint64 *bsize)
{
  if (fs == 0)
    return;
  if (blocks)
    *blocks = fs->blocks_count;
  if (bfree)
    *bfree = fs->free_blocks;
  if (files)
    *files = fs->inodes_count;
  if (ffree)
    *ffree = fs->free_inodes;
  if (bsize)
    *bsize = fs->blocksize;
}

// ---- inodes ----

// seconds since 1970 out of an ext4 timestamp. the on-disk word is a
// signed 32-bit count, which stops in 2038; the format buys another two
// bits from the low end of the matching *_extra word, and those two bits
// are added above the 32, so a time that had gone negative comes back as
// the large positive number it was meant to be.
static uint64
ext4_time(uint32 secs, uint32 extra, int have_extra)
{
  int64 t = (int)secs;

  if (have_extra)
    t += (int64)(uint64)(extra & EXT4_EPOCH_MASK) << 32;
  return (uint64)t;
}

int
ext4_read_inode(struct ext4_fs *fs, uint64 ino, struct ext4_inode_info *out)
{
  struct ext4_group_desc gd;
  const struct ext4_inode *di;
  struct blkbuf *b;
  uint64 itable, byte, blk;
  uint32 group, index, off, copy, extra;
  int r;

  if (fs == 0 || out == 0)
    return -EINVAL;
  // inode 0 does not exist: the format uses it in a directory entry to
  // mean "this record is free", so it can never name a file.
  if (ino < 1 || ino > fs->inodes_count)
    return -EINVAL;

  group = (uint32)((ino - 1) / fs->inodes_per_group);
  index = (uint32)((ino - 1) % fs->inodes_per_group);
  r = ext4_group_desc(fs, group, &gd);
  if (r < 0)
    return r;

  itable = gd.bg_inode_table_lo;
  if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
    itable |= (uint64)gd.bg_inode_table_hi << 32;

  byte = (uint64)index * fs->inode_size;
  blk = itable + byte / fs->blocksize;
  off = (uint32)(byte % fs->blocksize);
  // the inode size is a power of two no larger than a block, so an inode
  // never straddles two blocks; check it anyway, since the value that
  // makes it true came off the disk.
  if (off + fs->inode_size > fs->blocksize)
    return -EIO;
  // the whole inode table must lie inside the file system
  if (itable == 0 || itable >= fs->blocks_count || blk >= fs->blocks_count)
    return -EIO;

  b = ext4_bread(fs, blk);
  if (b == 0)
    return -EIO;
  memset(out, 0, sizeof(*out));
  copy = ext4_min(fs->inode_size, (uint32)EXT4_INODE_RAW_SIZE);
  memmove(out->raw, (uchar *)b->data + off, copy);
  blk_put(b);

  di = (const struct ext4_inode *)out->raw;
  out->ino = ino;
  out->mode = di->i_mode;
  out->uid = (uint32)di->i_uid | ((uint32)di->l_i_uid_high << 16);
  out->gid = (uint32)di->i_gid | ((uint32)di->l_i_gid_high << 16);
  out->nlink = di->i_links_count;
  out->flags = di->i_flags;
  out->generation = di->i_generation;

  // i_extra_isize says how much of the inode past the ext2 part this
  // particular inode filled in; a shorter inode simply has no extra
  // fields, and reading them would be reading another inode's bytes.
  extra = 0;
  if (fs->inode_size >= EXT4_GOOD_OLD_INODE_SIZE + (uint32)sizeof(di->i_extra_isize))
    extra = di->i_extra_isize;
  if (extra > fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE)
    extra = 0;

  // i_size_high is the high half of the size only for a regular file. for
  // a directory the same word was ext2's i_dir_acl, and ext4 keeps it that
  // way unless the largedir feature is on, which this driver refuses.
  out->size = di->i_size_lo;
  if ((out->mode & EXT4_S_IFMT) == EXT4_S_IFREG)
    out->size |= (uint64)di->i_size_high << 32;

  // i_blocks counts 512-byte sectors, with a high half that exists only
  // with huge_file; with huge_file and the per-inode flag it counts file
  // system blocks instead, which is how a file larger than 2^48 sectors
  // is described. the caller was promised sectors, so convert.
  out->blocks = di->i_blocks_lo;
  if (fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) {
    out->blocks |= (uint64)di->l_i_blocks_high << 32;
    if (out->flags & EXT4_HUGE_FILE_FL)
      out->blocks <<= fs->blockbits - 9;
  }

  out->atime = ext4_time(di->i_atime, di->i_atime_extra,
                         EXT4_FITS_IN_INODE(fs->inode_size, extra, i_atime_extra));
  out->mtime = ext4_time(di->i_mtime, di->i_mtime_extra,
                         EXT4_FITS_IN_INODE(fs->inode_size, extra, i_mtime_extra));
  out->ctime = ext4_time(di->i_ctime, di->i_ctime_extra,
                         EXT4_FITS_IN_INODE(fs->inode_size, extra, i_ctime_extra));
  // there is no creation time at all in an inode too short to hold one
  out->crtime = 0;
  if (EXT4_FITS_IN_INODE(fs->inode_size, extra, i_crtime))
    out->crtime = ext4_time(di->i_crtime, di->i_crtime_extra,
                            EXT4_FITS_IN_INODE(fs->inode_size, extra, i_crtime_extra));
  return 0;
}

// ---- extents ----

// one node of an extent tree, wherever it lives: 60 bytes in the inode for
// the root, a whole block for everything below. the checks are the ones
// the format guarantees, and a node that fails any of them ends the walk.
// `space` is how many bytes the node may occupy, and `depth_want` is the
// depth its parent said it would have (-1 for the root, which answers for
// itself).
static int
ext4_ext_check(const struct ext4_extent_header *eh, uint32 space, int depth_want)
{
  uint32 fit;

  if (eh->eh_magic != EXT4_EXT_MAGIC)
    return -EIO;
  if (eh->eh_depth > EXT4_MAX_EXT_DEPTH)
    return -EIO;
  if (depth_want >= 0 && eh->eh_depth != (uint32)depth_want)
    return -EIO;
  fit = (space - (uint32)sizeof(*eh)) / (uint32)sizeof(struct ext4_extent);
  if (eh->eh_max > fit || eh->eh_entries > eh->eh_max)
    return -EIO;
  return 0;
}

// map a file block through the extent tree. the tree is searched from the
// root in the inode downwards, one node held at a time; a file block that
// no extent covers is a hole, which is not an error.
static int
ext4_ext_lookup(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 fileblk, uint64 *diskblk,
                int *unwritten)
{
  const struct ext4_extent_header *eh;
  const struct ext4_extent_idx *ix;
  const struct ext4_extent *ex;
  struct blkbuf *b = 0;
  const uchar *node = in->raw + EXT4_I_BLOCK_OFF;
  uint32 space = EXT4_I_BLOCK_SIZE;
  uint64 child;
  int level, depth_want = -1, ret = -EIO;
  uint32 i, nent, prev, first, len;

  // an extent's ee_block is 32 bits wide, so nothing past block 2^32 can
  // be mapped by an extent tree and everything there is a hole.
  if (fileblk > 0xffffffffu)
    return 0;

  for (level = 0; level <= EXT4_MAX_EXT_DEPTH; level++) {
    eh = (const struct ext4_extent_header *)node;
    if (ext4_ext_check(eh, space, depth_want) < 0)
      goto out;
    nent = eh->eh_entries;

    if (eh->eh_depth == 0) {
      // a leaf: the extents are sorted by first block, so the last one
      // that starts at or before the target is the only candidate.
      ex = (const struct ext4_extent *)(node + sizeof(*eh));
      prev = 0;
      ret = 0;
      for (i = 0; i < nent; i++) {
        first = ex[i].ee_block;
        if (i > 0 && first < prev) {
          ret = -EIO;              // out of order: the tree is not a tree
          goto out;
        }
        prev = first;
        if (fileblk < first)
          break;
        len = ex[i].ee_len;
        // the high bit of the length is not length: it says the extent is
        // allocated but was never written, so it reads as zeros. the
        // caller is told, because the write side must not treat the two
        // alike even though a reader may.
        if (len > EXT4_INIT_MAX_LEN) {
          len -= EXT4_INIT_MAX_LEN;
          if (fileblk < (uint64)first + len)
            *unwritten = 1;
        }
        if (len == 0 || len > EXT4_INIT_MAX_LEN) {
          ret = -EIO;
          goto out;
        }
        if (fileblk >= (uint64)first + len)
          continue;                // a hole between this extent and the next
        child = ex[i].ee_start_lo | ((uint64)ex[i].ee_start_hi << 32);
        if (child == 0 || child + len > fs->blocks_count) {
          ret = -EIO;
          goto out;
        }
        *diskblk = child + (fileblk - first);
        break;
      }
      goto out;                    // found it, or it is a hole: either way done
    }

    // an interior node: find the child that covers the target. entry 0
    // covers everything below the first key, so an entry before the
    // target must exist unless the tree is empty.
    if (nent == 0) {
      ret = 0;                     // an empty index is an empty file
      goto out;
    }
    ix = (const struct ext4_extent_idx *)(node + sizeof(*eh));
    prev = 0;
    child = 0;
    for (i = 0; i < nent; i++) {
      first = ix[i].ei_block;
      if (i > 0 && first < prev)
        goto out;                  // out of order
      prev = first;
      if (i > 0 && fileblk < first)
        break;
      child = ix[i].ei_leaf_lo | ((uint64)ix[i].ei_leaf_hi << 32);
    }
    if (child == 0 || child >= fs->blocks_count)
      goto out;
    depth_want = (int)eh->eh_depth - 1;

    // drop the node we are leaving before reading the one below it, so
    // that only one block is ever held.
    if (b)
      blk_put(b);
    b = ext4_bread(fs, child);
    if (b == 0)
      goto out;
    node = (const uchar *)b->data;
    space = fs->blocksize;
  }
  // the loop ran out of levels without reaching a leaf, which only a
  // malformed tree can do, since the depth is checked to fall each step.
  ret = -EIO;

out:
  if (b)
    blk_put(b);
  return ret;
}

// map a file block through the old block map: twelve direct entries, then
// one, two and three levels of indirection. a Linux image may hold such an
// inode long after the file system grew extents, so a reader needs both.
static int
ext4_ind_lookup(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 fileblk, uint64 *diskblk)
{
  const struct ext4_inode *di = (const struct ext4_inode *)in->raw;
  struct blkbuf *b;
  uint64 perblk = fs->blocksize / sizeof(uint32);
  uint64 idx[3], n = fileblk, blk;
  int levels, i;

  if (fileblk < EXT4_NDIR_BLOCKS) {
    blk = di->i_block[fileblk];
    levels = 0;
  } else if ((n -= EXT4_NDIR_BLOCKS) < perblk) {
    blk = di->i_block[EXT4_IND_BLOCK];
    idx[0] = n;
    levels = 1;
  } else if ((n -= perblk) < perblk * perblk) {
    blk = di->i_block[EXT4_DIND_BLOCK];
    idx[0] = n / perblk;
    idx[1] = n % perblk;
    levels = 2;
  } else if ((n -= perblk * perblk) < perblk * perblk * perblk) {
    blk = di->i_block[EXT4_TIND_BLOCK];
    idx[0] = n / (perblk * perblk);
    idx[1] = (n / perblk) % perblk;
    idx[2] = n % perblk;
    levels = 3;
  } else {
    // past the last block a triple indirect can name: a hole by
    // construction, since no such block can be allocated.
    return 0;
  }

  // a zero at any level is a hole, and stops the walk without an error.
  for (i = 0; i < levels && blk != 0; i++) {
    b = ext4_bread(fs, blk);
    if (b == 0)
      return -EIO;
    blk = ((const uint32 *)b->data)[idx[i]];
    blk_put(b);
  }
  if (blk >= fs->blocks_count)
    return -EIO;
  *diskblk = blk;
  return 0;
}

int
ext4_bmap(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 fileblk, uint64 *diskblk)
{
  int unwritten = 0, r;

  if (fs == 0 || in == 0 || diskblk == 0)
    return -EINVAL;
  *diskblk = 0;
  // inline_data is refused at mount, but a single inode can still carry
  // the flag; its data is in the inode, not in any block, so there is no
  // mapping to give.
  if (in->flags & EXT4_INLINE_DATA_FL)
    return -EOPNOTSUPP;

  if (in->flags & EXT4_EXTENTS_FL)
    r = ext4_ext_lookup(fs, in, fileblk, diskblk, &unwritten);
  else
    r = ext4_ind_lookup(fs, in, fileblk, diskblk);
  if (r < 0)
    return r;
  return unwritten ? EXT4_MAP_UNWRITTEN : EXT4_MAP_MAPPED;
}

// ---- file data ----

// bytes out of a file and into `dst`, which is a user address when
// `user` is set and a kernel one when it is not. the copy goes straight
// out of the cached block: the bounce buffer this used to need was a
// whole extra copy of every byte read, and one buffer shared by every
// reader in the system.
int
ext4_read_file(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 off, int user, uint64 dst,
               uint64 n)
{
  uint64 done = 0, fileblk, diskblk;
  uint32 boff, chunk;
  struct blkbuf *b;
  int r;

  if (fs == 0 || in == 0 || dst == 0)
    return -EINVAL;
  if (off >= in->size)
    return 0;
  if (n > in->size - off)
    n = in->size - off;
  // the result is an int, so no single call may claim more than that
  if (n > 0x7fffffffu)
    n = 0x7fffffffu;

  while (done < n) {
    fileblk = (off + done) >> fs->blockbits;
    boff = (uint32)((off + done) & (fs->blocksize - 1));
    chunk = fs->blocksize - boff;
    if (chunk > n - done)
      chunk = (uint32)(n - done);

    r = ext4_bmap(fs, in, fileblk, &diskblk);
    if (r < 0)
      return done > 0 ? (int)done : r;
    // a hole and an unwritten extent both read as zeros; that is what the
    // format promises and what a caller comparing against Linux expects.
    if (diskblk == 0 || r == EXT4_MAP_UNWRITTEN) {
      // a hole and an unwritten extent both read as zeros
      if (either_memset(user, dst + done, 0, chunk) < 0)
        return done > 0 ? (int)done : -EFAULT;
    } else {
      b = ext4_bread(fs, diskblk);
      if (b == 0)
        return done > 0 ? (int)done : -EIO;
      int e = either_copyout(user, dst + done, (uchar *)b->data + boff, chunk);
      blk_put(b);
      if (e < 0)
        return done > 0 ? (int)done : -EFAULT;
    }
    done += chunk;
  }
  return (int)done;
}

int
ext4_readlink(struct ext4_fs *fs, struct ext4_inode_info *in, char *buf, int max)
{
  const struct ext4_inode *di;
  uint64 len, eablocks;

  if (fs == 0 || in == 0 || buf == 0 || max <= 0)
    return -EINVAL;
  if ((in->mode & EXT4_S_IFMT) != EXT4_S_IFLNK)
    return -EINVAL;
  di = (const struct ext4_inode *)in->raw;

  len = in->size;
  if (len > (uint64)max)
    len = (uint64)max;

  // a short target is kept in i_block itself instead of in a block, and
  // the way to tell is that the inode owns no blocks. an extended
  // attribute block is the one thing that can be charged to an inode with
  // no data, so it is discounted before the test -- the same test Linux
  // makes in ext4_inode_is_fast_symlink.
  eablocks = di->i_file_acl_lo ? (uint64)(fs->blocksize >> 9) : 0;
  if (in->blocks == eablocks && in->size < EXT4_I_BLOCK_SIZE) {
    memmove(buf, (const char *)(in->raw + EXT4_I_BLOCK_OFF), (uint)len);
    return (int)len;
  }
  return ext4_read_file(fs, in, 0, 0, (uint64)buf, len);
}

// ---- the directory hashes ----
//
// these are the four functions of fs/ext4/hash.c, byte for byte: a name
// has to hash here to the number Linux put in the index or the index is
// useless. each comes in a signed and an unsigned variant, which differ
// only in how a byte above 127 is widened, and which one an image wants is
// recorded in its superblock.

#define DX_DELTA 0x9E3779B9u
#define DX_MD4_K2 0x5A827999u
#define DX_MD4_K3 0x6ED9EBA1u

static uint32
rol32(uint32 x, int n)
{
  return (x << n) | (x >> (32 - n));
}

// widen one name byte the way this image's hash wants it
static uint32
dx_byte(const char *p, int i, int sgn)
{
  if (sgn)
    return (uint32)(int)((const signed char *)p)[i];
  return (uint32)((const unsigned char *)p)[i];
}

// the original ext2 directory hash, kept because images made with it are
// still mounted. it is not a good hash; it does not have to be, it only
// has to be the same one that built the index.
static uint32
dx_hack_hash(const char *name, int len, int sgn)
{
  uint32 hash = 0, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
  int i;

  for (i = 0; i < len; i++) {
    hash = hash1 + (hash0 ^ (dx_byte(name, i, sgn) * 7152373u));
    if (hash & 0x80000000u)
      hash -= 0x7fffffffu;
    hash1 = hash0;
    hash0 = hash;
  }
  return hash0 << 1;
}

// pack up to num words of the name into buf, padding the tail with the
// name's length repeated in every byte so that two names differing only in
// trailing bytes cannot collide.
static void
dx_str2hashbuf(const char *msg, int len, uint32 *buf, int num, int sgn)
{
  uint32 pad, val;
  int i;

  pad = (uint32)len | ((uint32)len << 8);
  pad |= pad << 16;

  val = pad;
  if (len > num * 4)
    len = num * 4;
  for (i = 0; i < len; i++) {
    val = dx_byte(msg, i, sgn) + (val << 8);
    if ((i % 4) == 3) {
      *buf++ = val;
      val = pad;
      num--;
    }
  }
  if (--num >= 0)
    *buf++ = val;
  while (--num >= 0)
    *buf++ = pad;
}

static void
dx_tea_transform(uint32 buf[4], const uint32 in[4])
{
  uint32 sum = 0;
  uint32 b0 = buf[0], b1 = buf[1];
  uint32 a = in[0], b = in[1], c = in[2], d = in[3];
  int n = 16;

  do {
    sum += DX_DELTA;
    b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
    b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
  } while (--n);

  buf[0] += b0;
  buf[1] += b1;
}

#define MD4_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD4_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD4_ROUND(f, a, b, c, d, x, s) ((a) += f(b, c, d) + (x), (a) = rol32(a, s))

// the first two rounds and a bit of MD4, which is all ext4 uses
static void
dx_half_md4_transform(uint32 buf[4], const uint32 in[8])
{
  uint32 a = buf[0], b = buf[1], c = buf[2], d = buf[3];

  MD4_ROUND(MD4_F, a, b, c, d, in[0], 3);
  MD4_ROUND(MD4_F, d, a, b, c, in[1], 7);
  MD4_ROUND(MD4_F, c, d, a, b, in[2], 11);
  MD4_ROUND(MD4_F, b, c, d, a, in[3], 19);
  MD4_ROUND(MD4_F, a, b, c, d, in[4], 3);
  MD4_ROUND(MD4_F, d, a, b, c, in[5], 7);
  MD4_ROUND(MD4_F, c, d, a, b, in[6], 11);
  MD4_ROUND(MD4_F, b, c, d, a, in[7], 19);

  MD4_ROUND(MD4_G, a, b, c, d, in[1] + DX_MD4_K2, 3);
  MD4_ROUND(MD4_G, d, a, b, c, in[3] + DX_MD4_K2, 5);
  MD4_ROUND(MD4_G, c, d, a, b, in[5] + DX_MD4_K2, 9);
  MD4_ROUND(MD4_G, b, c, d, a, in[7] + DX_MD4_K2, 13);
  MD4_ROUND(MD4_G, a, b, c, d, in[0] + DX_MD4_K2, 3);
  MD4_ROUND(MD4_G, d, a, b, c, in[2] + DX_MD4_K2, 5);
  MD4_ROUND(MD4_G, c, d, a, b, in[4] + DX_MD4_K2, 9);
  MD4_ROUND(MD4_G, b, c, d, a, in[6] + DX_MD4_K2, 13);

  MD4_ROUND(MD4_H, a, b, c, d, in[3] + DX_MD4_K3, 3);
  MD4_ROUND(MD4_H, d, a, b, c, in[7] + DX_MD4_K3, 9);
  MD4_ROUND(MD4_H, c, d, a, b, in[2] + DX_MD4_K3, 11);
  MD4_ROUND(MD4_H, b, c, d, a, in[6] + DX_MD4_K3, 15);
  MD4_ROUND(MD4_H, a, b, c, d, in[1] + DX_MD4_K3, 3);
  MD4_ROUND(MD4_H, d, a, b, c, in[5] + DX_MD4_K3, 9);
  MD4_ROUND(MD4_H, c, d, a, b, in[0] + DX_MD4_K3, 11);
  MD4_ROUND(MD4_H, b, c, d, a, in[4] + DX_MD4_K3, 15);

  buf[0] += a;
  buf[1] += b;
  buf[2] += c;
  buf[3] += d;
}

// hash one name with one of the six variants. returns -EINVAL for a
// version this driver does not know, which is not fatal: the caller falls
// back to reading every entry.
static int
dx_hash(struct ext4_fs *fs, uint32 version, const char *name, int len, uint32 *major,
        uint32 *minor)
{
  uint32 buf[4], in[8], hash = 0, mhash = 0;
  const char *p;
  int left, sgn, i;

  // the seed is the file system's, unless it is all zero, in which case
  // the hash starts from MD4's own constants.
  buf[0] = 0x67452301;
  buf[1] = 0xefcdab89;
  buf[2] = 0x98badcfe;
  buf[3] = 0x10325476;
  for (i = 0; i < 4; i++) {
    if (fs->hash_seed[i] != 0) {
      for (i = 0; i < 4; i++)
        buf[i] = fs->hash_seed[i];
      break;
    }
  }

  sgn = (version == DX_HASH_LEGACY || version == DX_HASH_HALF_MD4 || version == DX_HASH_TEA);

  switch (version) {
  case DX_HASH_LEGACY:
  case DX_HASH_LEGACY_UNSIGNED:
    hash = dx_hack_hash(name, len, sgn);
    break;

  case DX_HASH_HALF_MD4:
  case DX_HASH_HALF_MD4_UNSIGNED:
    p = name;
    left = len;
    while (left > 0) {
      dx_str2hashbuf(p, left, in, 8, sgn);
      dx_half_md4_transform(buf, in);
      left -= 32;
      p += 32;
    }
    hash = buf[1];
    mhash = buf[2];
    break;

  case DX_HASH_TEA:
  case DX_HASH_TEA_UNSIGNED:
    p = name;
    left = len;
    while (left > 0) {
      dx_str2hashbuf(p, left, in, 4, sgn);
      dx_tea_transform(buf, in);
      left -= 16;
      p += 16;
    }
    hash = buf[0];
    mhash = buf[1];
    break;

  default:
    *major = 0;
    *minor = 0;
    return -EINVAL;
  }

  // the low bit is not part of the hash: an index entry uses it to say
  // that a run of equal hashes continues into the next leaf. and one
  // value is reserved to mean end-of-directory, so it is nudged aside.
  hash &= ~1u;
  if (hash == (uint32)(EXT4_HTREE_EOF_32BIT << 1))
    hash = (uint32)((EXT4_HTREE_EOF_32BIT - 1) << 1);
  *major = hash;
  *minor = mhash;
  return 0;
}

// the hash an htree directory was built with, which the root block
// names and which need not be the superblock's default.
uint32
ext4_dirhash_version(struct ext4_fs *fs, uint32 version, const char *name, int len,
                     uint32 *minor_out)
{
  uint32 major = 0, minor = 0;

  if (fs != 0 && name != 0 && len > 0)
    dx_hash(fs, version, name, len, &major, &minor);
  if (minor_out)
    *minor_out = minor;
  return major;
}

uint32
ext4_dirhash(struct ext4_fs *fs, const char *name, int len, uint32 *minor_out)
{
  uint32 major = 0, minor = 0;

  if (fs != 0 && name != 0 && len > 0)
    dx_hash(fs, fs->hash_version, name, len, &major, &minor);
  if (minor_out)
    *minor_out = minor;
  return major;
}

// ---- directories ----

// one block of a directory, by its block number inside the file. returns 0
// for a hole or for a block past the end, both of which are simply empty;
// *err says whether that 0 was a failure instead.
struct blkbuf *
ext4_dir_bread(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 fileblk, int *err)
{
  struct blkbuf *b;
  uint64 diskblk = 0;
  int r;

  *err = 0;
  if (fileblk >= (dir->size >> fs->blockbits))
    return 0;
  r = ext4_bmap(fs, dir, fileblk, &diskblk);
  if (r < 0) {
    *err = r;
    return 0;
  }
  if (diskblk == 0 || r == EXT4_MAP_UNWRITTEN)
    return 0;
  b = ext4_bread(fs, diskblk);
  if (b == 0)
    *err = -EIO;
  return b;
}

// check the entry at `off` and say how long it is. the format demands that
// a record length be at least a header, a multiple of four, big enough for
// the name it claims, and inside the block. an entry that breaks any of
// those is how a corrupt directory would walk a reader off the end of a
// block, so it ends the walk instead.
int
ext4_dirent_check(const uchar *data, uint32 bsize, uint32 off, const struct ext4_dir_entry_2 **de)
{
  const struct ext4_dir_entry_2 *d;
  uint32 rec;

  if (off > bsize || bsize - off < EXT4_DIR_ENTRY_HEADER_LEN)
    return -EIO;
  d = (const struct ext4_dir_entry_2 *)(data + off);
  rec = d->rec_len;
  if (rec < EXT4_DIR_ENTRY_HEADER_LEN || (rec & 3) != 0)
    return -EIO;
  if (rec > bsize - off)
    return -EIO;
  if ((uint32)d->name_len + EXT4_DIR_ENTRY_HEADER_LEN > rec)
    return -EIO;
  *de = d;
  return (int)rec;
}

// look for one name in one block of a directory. returns 1 when it is
// there, 0 when it is not, and negative when the block is malformed.
static int
ext4_dir_block_find(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 fileblk,
                    const char *name, int namelen, uint64 *ino_out, uchar *type_out)
{
  const struct ext4_dir_entry_2 *d;
  struct blkbuf *b;
  const uchar *data;
  uint32 off = 0;
  int r, err, found = 0;

  b = ext4_dir_bread(fs, dir, fileblk, &err);
  if (b == 0)
    return err;
  data = (const uchar *)b->data;

  while (off < fs->blocksize) {
    r = ext4_dirent_check(data, fs->blocksize, off, &d);
    if (r < 0) {
      blk_put(b);
      return r;
    }
    // inode 0 marks free space, which is also how the checksum tail and
    // the hidden index blocks of an htree directory look from here.
    if (d->inode != 0 && d->name_len == namelen && memcmp(d->name, name, namelen) == 0) {
      if (d->inode > fs->inodes_count) {
        blk_put(b);
        return -EIO;
      }
      if (ino_out)
        *ino_out = d->inode;
      if (type_out)
        *type_out = d->file_type;
      found = 1;
      break;
    }
    off += (uint32)r;
  }
  blk_put(b);
  return found;
}

// the number of blocks a directory has, bounded so that a corrupt size
// cannot turn a linear walk into an unbounded one.
uint64
ext4_dir_nblocks(struct ext4_fs *fs, struct ext4_inode_info *dir)
{
  uint64 n = dir->size >> fs->blockbits;

  if (n > EXT4_MAX_DIR_BLOCKS)
    n = EXT4_MAX_DIR_BLOCKS;
  return n;
}

// check that a block in hand really is an index node, and say where its
// entries begin and how many there are.
//
// the index hides inside ordinary-looking directory records: in the root
// behind a ".." whose length covers the rest of the block, in an interior
// node behind one free record covering the whole block. that disguise is
// what lets a linear walk skip the index without knowing about it, and
// checking it here is what stops a plain data block being read as a node.
int
ext4_dx_node(struct ext4_fs *fs, const uchar *data, int is_root, uint32 *eoff, uint32 *count)
{
  const struct ext4_dx_countlimit *cl;
  const struct ext4_dir_entry_2 *d;
  uint32 off, fit, limit, n;

  if (is_root) {
    const struct ext4_dx_root_info *info;
    d = (const struct ext4_dir_entry_2 *)data;
    if (d->rec_len != 12 || d->name_len != 1 || d->name[0] != '.')
      return -EIO;
    d = (const struct ext4_dir_entry_2 *)(data + 12);
    if (d->rec_len != fs->blocksize - 12 || d->name_len != 2 || d->name[0] != '.' ||
        d->name[1] != '.')
      return -EIO;
    info = (const struct ext4_dx_root_info *)(data + EXT4_DX_ROOT_INFO_OFF);
    if (info->reserved_zero != 0 || info->info_length != sizeof(*info))
      return -EOPNOTSUPP;          // an index laid out some other way
    off = EXT4_DX_ROOT_ENTRIES_OFF;
  } else {
    d = (const struct ext4_dir_entry_2 *)data;
    if (d->inode != 0 || d->rec_len != fs->blocksize)
      return -EIO;
    off = EXT4_DX_NODE_ENTRIES_OFF;
  }

  cl = (const struct ext4_dx_countlimit *)(data + off);
  limit = cl->limit;
  n = cl->count;
  // the limit is how many entries fit; with metadata_csum the last eight
  // bytes are a checksum tail, so the limit is one less than the space
  // allows. either way it may not claim more space than the block has.
  fit = (fs->blocksize - off) / (uint32)sizeof(struct ext4_dx_entry);
  if (limit > fit || n == 0 || n > limit)
    return -EIO;
  *eoff = off;
  *count = n;
  return 0;
}

// read one entry out of an index node. `nodeblk` is a block of the
// directory file, not of the disk. entry 0 of any node is not an entry:
// its first word holds the node's count and limit, so only its block
// number is real and its hash is taken to be zero, which is right, since
// entry 0 covers every hash below the first key.
static int
ext4_dx_entry(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 nodeblk, int is_root,
              uint32 at, uint32 *count, uint32 *hash, uint32 *block)
{
  const struct ext4_dx_entry *e;
  struct blkbuf *b;
  uint32 eoff = 0, n = 0;
  int err, ret;

  b = ext4_dir_bread(fs, dir, nodeblk, &err);
  if (b == 0)
    return err ? err : -EIO;
  ret = ext4_dx_node(fs, (const uchar *)b->data, is_root, &eoff, &n);
  if (ret == 0 && at >= n)
    ret = -EIO;
  if (ret == 0) {
    e = (const struct ext4_dx_entry *)((const uchar *)b->data + eoff);
    *count = n;
    *hash = at == 0 ? 0 : e[at].hash;
    *block = e[at].block;
  }
  blk_put(b);
  return ret;
}

// find the child of one index node that covers `hash`. the entries are
// sorted by hash, so this is the last one whose key is not above it. the
// search runs while the node is in hand and copies out only its answer,
// which keeps a lookup to one block read per level of the tree.
static int
ext4_dx_search(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 nodeblk, int is_root,
               uint32 hash, uint32 *at_out, uint32 *count_out, uint32 *block_out)
{
  const struct ext4_dx_entry *e;
  struct blkbuf *b;
  uint32 eoff = 0, n = 0, lo, hi, mid, at;
  int err, ret;

  b = ext4_dir_bread(fs, dir, nodeblk, &err);
  if (b == 0)
    return err ? err : -EIO;
  ret = ext4_dx_node(fs, (const uchar *)b->data, is_root, &eoff, &n);
  if (ret == 0) {
    e = (const struct ext4_dx_entry *)((const uchar *)b->data + eoff);
    // entry 0 carries no key of its own, since it covers every hash below
    // the first one, so the search runs over 1..count-1 and falls back on
    // it. a node whose keys are not sorted gives a wrong child rather than
    // an out-of-range one, and the child is bounds-checked by the caller.
    lo = 1;
    hi = n;
    while (lo < hi) {
      mid = lo + (hi - lo) / 2;
      if ((e[mid].hash & ~1u) > hash)
        hi = mid;
      else
        lo = mid + 1;
    }
    at = lo - 1;
    *at_out = at;
    *count_out = n;
    *block_out = e[at].block;
  }
  blk_put(b);
  return ret;
}

// one step of the descent's bookkeeping: which node, which entry in it,
// how many entries it has.
struct ext4_dx_frame {
  uint64 nodeblk;
  uint32 at;
  uint32 count;
};

// move to the leaf after the current one, but only while the index says a
// run of equal hashes continues into it. two names can hash alike, and
// when they do the format spreads them over neighbouring leaves and marks
// the continuation with the low bit of the key; without this a lookup
// would miss the second of two colliding names.
static int
ext4_dx_next(struct ext4_fs *fs, struct ext4_inode_info *dir, uint32 hash,
             struct ext4_dx_frame *frame, int nframes, uint64 *leaf)
{
  uint32 count, h, blk;
  int p = nframes - 1, q, r;

  while (1) {
    frame[p].at++;
    if (frame[p].at < frame[p].count)
      break;
    if (p == 0)
      return 0;                    // the last leaf of the directory
    p--;
  }

  r = ext4_dx_entry(fs, dir, frame[p].nodeblk, p == 0, frame[p].at, &count, &h, &blk);
  if (r < 0)
    return r;
  if ((h & ~1u) != hash)
    return 0;                      // the next key is a different hash: stop

  // walk back down to a leaf, taking the first child at every level below
  // the one that moved.
  for (q = p + 1; q < nframes; q++) {
    frame[q].nodeblk = blk;
    frame[q].at = 0;
    r = ext4_dx_entry(fs, dir, blk, 0, 0, &frame[q].count, &h, &blk);
    if (r < 0)
      return r;
  }
  *leaf = blk;
  return 1;
}

// look one name up through the hash index. returns 0 when found, -ENOENT
// when the index says it is not there, -EOPNOTSUPP when the index is not
// one we can read (the caller then walks the whole directory), and -EIO
// when it is malformed.
static int
ext4_dx_lookup(struct ext4_fs *fs, struct ext4_inode_info *dir, const char *name, int namelen,
               uint64 *ino_out, uchar *type_out)
{
  struct ext4_dx_frame frame[EXT4_MAX_DX_LEVELS];
  const struct ext4_dx_root_info *info;
  struct blkbuf *b;
  uint32 hash = 0, minor = 0, version, levels, at = 0, count = 0, block = 0;
  uint64 nodeblk, leaf, nblocks;
  int i, r, err, tries;

  // the root block names the hash this directory was built with, which
  // need not be the superblock's default; the sign convention is still
  // the file system's.
  b = ext4_dir_bread(fs, dir, 0, &err);
  if (b == 0)
    return err ? err : -EIO;
  info = (const struct ext4_dx_root_info *)((const uchar *)b->data + EXT4_DX_ROOT_INFO_OFF);
  version = info->hash_version;
  levels = info->indirect_levels;
  if (info->reserved_zero != 0 || info->info_length != sizeof(*info)) {
    blk_put(b);
    return -EOPNOTSUPP;
  }
  blk_put(b);

  if (version <= DX_HASH_TEA && fs->hash_unsigned)
    version += 3;
  if (levels + 1 > EXT4_MAX_DX_LEVELS)
    return -EIO;
  if (dx_hash(fs, version, name, namelen, &hash, &minor) < 0)
    return -EOPNOTSUPP;

  nblocks = ext4_dir_nblocks(fs, dir);
  nodeblk = 0;
  for (i = 0; i <= (int)levels; i++) {
    r = ext4_dx_search(fs, dir, nodeblk, i == 0, hash, &at, &count, &block);
    if (r < 0)
      return r;
    if (block == 0 || (uint64)block >= nblocks)
      return -EIO;                 // a child outside the directory's own blocks
    frame[i].nodeblk = nodeblk;
    frame[i].at = at;
    frame[i].count = count;
    nodeblk = block;
  }
  leaf = nodeblk;

  for (tries = 0; tries < EXT4_MAX_DX_RETRIES; tries++) {
    r = ext4_dir_block_find(fs, dir, leaf, name, namelen, ino_out, type_out);
    if (r < 0)
      return r;
    if (r == 1)
      return 0;
    r = ext4_dx_next(fs, dir, hash, frame, (int)levels + 1, &leaf);
    if (r < 0)
      return r;
    if (r == 0)
      break;
    if (leaf == 0 || leaf >= nblocks)
      return -EIO;
  }
  return -ENOENT;
}

int
ext4_lookup(struct ext4_fs *fs, struct ext4_inode_info *dir, const char *name, int namelen,
            uint64 *ino_out, unsigned char *type_out)
{
  uint64 blk, nblocks;
  int r;

  if (fs == 0 || dir == 0 || name == 0)
    return -EINVAL;
  if ((dir->mode & EXT4_S_IFMT) != EXT4_S_IFDIR)
    return -ENOTDIR;
  if (namelen <= 0 || namelen > EXT4_NAME_LEN)
    return -ENAMETOOLONG;

  nblocks = ext4_dir_nblocks(fs, dir);

  if (dir->flags & EXT4_INDEX_FL) {
    // "." and ".." live in the root block of an htree directory, not in
    // any leaf, so the hash descent cannot reach them; they are found
    // where they are instead.
    if (namelen <= 2 && name[0] == '.' && (namelen == 1 || name[1] == '.')) {
      r = ext4_dir_block_find(fs, dir, 0, name, namelen, ino_out, type_out);
      if (r < 0)
        return r;
      return r == 1 ? 0 : -ENOENT;
    }
    r = ext4_dx_lookup(fs, dir, name, namelen, ino_out, type_out);
    // a malformed index is an error; an index we merely cannot read is
    // not, because every entry is still reachable by walking the leaves.
    if (r != -EOPNOTSUPP)
      return r;
  }

  for (blk = 0; blk < nblocks; blk++) {
    r = ext4_dir_block_find(fs, dir, blk, name, namelen, ino_out, type_out);
    if (r < 0)
      return r;
    if (r == 1)
      return 0;
  }
  return -ENOENT;
}

int
ext4_readdir(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 *pos, char *name,
             int namemax, uint64 *ino, unsigned char *type)
{
  const struct ext4_dir_entry_2 *d;
  struct blkbuf *b;
  const uchar *data;
  uint64 fileblk, nblocks, guard;
  uint32 off;
  int r, err;

  if (fs == 0 || dir == 0 || pos == 0 || name == 0 || namemax <= 0)
    return -EINVAL;
  if ((dir->mode & EXT4_S_IFMT) != EXT4_S_IFDIR)
    return -ENOTDIR;

  nblocks = ext4_dir_nblocks(fs, dir);

  // an htree directory is walked block by block like any other, which
  // returns its entries in the order they sit on disk rather than in hash
  // order. that is allowed -- readdir promises no order -- and it is the
  // only walk that needs no index at all: the root and the interior nodes
  // look like free space from here, because the format deliberately
  // disguised them as one free record each, so they are skipped for free.
  for (guard = 0; guard < EXT4_MAX_DIR_BLOCKS; guard++) {
    fileblk = *pos >> fs->blockbits;
    off = (uint32)(*pos & (fs->blocksize - 1));
    if (fileblk >= nblocks)
      return 0;
    // every record starts on a four-byte boundary, so a position that
    // does not is one we never produced
    if (off & 3)
      return -EIO;

    b = ext4_dir_bread(fs, dir, fileblk, &err);
    if (b == 0) {
      if (err)
        return err;
      *pos = (fileblk + 1) << fs->blockbits;   // a hole holds no entries
      continue;
    }
    data = (const uchar *)b->data;

    while (off < fs->blocksize) {
      r = ext4_dirent_check(data, fs->blocksize, off, &d);
      if (r < 0) {
        blk_put(b);
        return r;
      }
      off += (uint32)r;
      if (d->inode == 0 || d->name_len == 0)
        continue;                  // free space, a checksum tail, or an index
      if (d->inode > fs->inodes_count) {
        blk_put(b);
        return -EIO;
      }
      if (d->name_len >= namemax) {
        blk_put(b);
        return -ENAMETOOLONG;
      }
      memmove(name, d->name, d->name_len);
      name[d->name_len] = 0;
      if (ino)
        *ino = d->inode;
      if (type)
        *type = d->file_type;
      *pos = (fileblk << fs->blockbits) + off;
      blk_put(b);
      return 1;
    }
    blk_put(b);
    *pos = (fileblk + 1) << fs->blockbits;
  }
  // the guard ran out, which takes a directory claiming more blocks than
  // EXT4_MAX_DIR_BLOCKS; treat it as the corruption it is.
  return -EIO;
}

// ---- readahead ----
//
// a reader that is walking a file forwards will want the next blocks,
// and the request that fetches the block it is waiting for can carry
// them at almost no extra cost. the window starts small, doubles while
// the guess keeps being right, and goes back to nothing the moment the
// reader jumps somewhere else: a program that seeks pays nothing for a
// program that does not.
#define EXT4_RA_MIN    16   // 64 KB, the first guess
#define EXT4_RA_MAX   256   // 1 MB, as far ahead as this will ever read

void
ext4_readahead(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 first, uint64 last)
{
  uint64 nblocks = (in->size + fs->blocksize - 1) / fs->blocksize;
  uint64 start, blk, prev = 0;
  int run = 0;

  if (fs == 0 || in == 0 || nblocks == 0)
    return;
  if (first != in->ra_next) {
    // not where the reader was expected: start again, and read nothing
    // ahead until it has shown that it is going forwards.
    in->ra_next = last + 1;
    in->ra_window = 0;
    return;
  }
  in->ra_next = last + 1;
  in->ra_window = in->ra_window ? in->ra_window * 2 : EXT4_RA_MIN;
  if (in->ra_window > EXT4_RA_MAX)
    in->ra_window = EXT4_RA_MAX;

  // the blocks that come after what was just read, as far as they stay
  // next to one another on the disk: a run is one request.
  start = last + 1;
  if (start < nblocks) {
    // if the next block is already here the reader is not waiting for
    // the disk, and mapping a window of the file to discover that the
    // rest is here too costs more than it saves.
    uint64 first_blk = 0;
    if (ext4_bmap(fs, in, start, &first_blk) < 0 || first_blk == 0 ||
        blk_cached(fs->disk, first_blk))
      return;
  }
  for (uint64 i = 0; i < in->ra_window && start + i < nblocks; i++) {
    if (ext4_bmap(fs, in, start + i, &blk) < 0 || blk == 0)
      break;
    if (run > 0 && blk != prev + 1) {
      blk_readahead(fs->disk, prev - run + 1, run);
      run = 0;
    }
    prev = blk;
    if (++run == BIO_MAXPAGES) {
      blk_readahead(fs->disk, prev - run + 1, run);
      run = 0;
    }
  }
  if (run > 0)
    blk_readahead(fs->disk, prev - run + 1, run);
}
