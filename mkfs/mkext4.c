// mkext4 -- write an ext4 file system image on the build host, and read
// one back.
//
// the format is the one the kernel's ext4 driver expects:
// 4096-byte blocks, 256-byte inodes with the extra fields, 64-byte group
// descriptors, extent-mapped files, an internal jbd2 journal at inode 8,
// and crc32c checksums on every metadata structure, so that Linux's
// e2fsck accepts the image without a word and the kernel driver can be
// written against images a real implementation would produce.
//
// the layout of a default 64 MiB image (16384 blocks, so a single block
// group, since one block bitmap covers 4096*8 = 32768 blocks):
//
//   block 0        the boot area, with the superblock at byte 1024; the
//                  superblock is always at that offset whatever the block
//                  size, which is why block 0 is not entirely superblock
//   block 1        the group descriptor table, one 64-byte descriptor per
//                  group rounded up to a block
//   blocks 2-8     reserved group descriptor blocks: empty blocks held
//                  back so that a later resize can extend the table
//                  without moving the data that follows it
//   block 9        block bitmap of group 0
//   block 10       inode bitmap of group 0
//   blocks 11-266  inode table of group 0 (4096 inodes of 256 bytes)
//   block 267 on   the journal, the root directory, lost+found, and the
//                  files, in that order
//
// with more than one group, flex_bg lets a group's bitmaps and inode
// table live outside that group, so the bitmaps and inode tables of each
// run of 16 groups are laid out together at the front of the first group
// of the run; that is what mke2fs does and what makes a mkfs image look
// like a real one. a superblock and a copy of the descriptor table are
// repeated at the start of the groups sparse_super names: 0, 1, and the
// powers of 3, 5 and 7.
//
// every metadata structure carries a crc32c seeded from the volume uuid.
// getting these right is the whole difficulty of writing an image by
// hand: e2fsck checks the superblock, each group descriptor, each block
// and inode bitmap, each inode, each extent block outside an inode, and
// the tail entry of each directory block, and a single wrong word makes
// it report corruption. checksum() below and the csum_* helpers are the
// only places that know how each one is seeded.
//
// what this tool deliberately does not write: htree directories (the
// dir_index feature is left off, so directories are linear lists of
// entries and a reader never has to hash a name), inline data, bigalloc,
// the resize inode (the reserved descriptor blocks are held back but
// inode 7 stays empty, so growing an image needs the table rewritten),
// extended attributes, and sparse files (a hole would be perfectly legal,
// but nothing here needs one).

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mkfs/ext4_ondisk.h"

// the geometry we write. the block size is not a variable: 4096 is what
// the kernel driver will support, and it is the only size for which the
// numbers below (one bitmap block per group, 16 inodes per block) hold.
#define BLOCK_SIZE       4096
#define BLOCK_LOG        12
#define INODE_SIZE       256
#define DESC_SIZE        64
#define BLOCKS_PER_GROUP (BLOCK_SIZE * 8)
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define DESC_PER_BLOCK   (BLOCK_SIZE / DESC_SIZE)
#define LOG_FLEX_BG      4
#define FLEX_BG_SIZE     (1 << LOG_FLEX_BG)

// the usable part of a directory block: metadata_csum steals the last 12
// bytes of every block for the fake entry that holds the checksum
#define DIR_USABLE (BLOCK_SIZE - EXT4_DIR_TAIL_SIZE)

// entries in an extent block outside the inode. the header takes 12
// bytes and the checksum tail takes 4, which leaves exactly 340 entries
// and no slack at all.
#define EXT_PER_BLOCK ((BLOCK_SIZE - 12 - 4) / 12)

// defaults, overridable from the command line
#define DEFAULT_BLOCKS          16384 // 64 MiB
#define DEFAULT_BYTES_PER_INODE 16384
#define DEFAULT_JOURNAL_BLOCKS  4096

// every offset in the format depends on these being exactly right, so
// make the compiler prove them rather than finding out from e2fsck.
_Static_assert(sizeof(struct ext4_super_block) == 1024, "superblock must be 1024 bytes");
_Static_assert(sizeof(struct ext4_group_desc) == 64, "group descriptor must be 64 bytes");
_Static_assert(sizeof(struct ext4_inode) == 160, "inode must be 160 bytes of the 256 we write");
_Static_assert(sizeof(struct ext4_extent) == 12, "extent must be 12 bytes");
_Static_assert(sizeof(struct ext4_extent_idx) == 12, "extent index must be 12 bytes");
_Static_assert(sizeof(struct ext4_extent_header) == 12, "extent header must be 12 bytes");
_Static_assert(sizeof(journal_superblock_t) == 1024, "journal superblock must be 1024 bytes");
_Static_assert(offsetof(struct ext4_super_block, s_checksum) == 1020, "s_checksum at 0x3fc");
_Static_assert(offsetof(struct ext4_super_block, s_uuid) == 0x68, "s_uuid at 0x68");
_Static_assert(offsetof(struct ext4_super_block, s_checksum_seed) == 0x270, "s_checksum_seed at 0x270");
_Static_assert(offsetof(struct ext4_group_desc, bg_checksum) == EXT4_BG_CHECKSUM_OFFSET,
               "bg_checksum at 0x1e");
_Static_assert(offsetof(struct ext4_inode, i_extra_isize) == 128, "extra fields start at 128");

// the kinds of thing a spec on the command line can ask for
#define NODE_DIR  1
#define NODE_FILE 2
#define NODE_SYM  3
#define NODE_CHR  4
#define NODE_BLK  5

// one name in the tree we are about to write. the whole tree is built in
// memory first, because a directory's blocks cannot be laid out until
// every one of its entries is known, and an inode's link count cannot be
// known until every child has been seen.
struct node {
  char *name;          // the single path component, not the whole path
  int type;            // NODE_*
  uint32_t mode;       // permission bits only; the type comes from ->type
  uint32_t ino;        // inode number, assigned as the node is created
  char *source;        // host file to copy, for NODE_FILE
  char *link;          // target text, for NODE_SYM
  uint32_t major;      // device number, for NODE_CHR and NODE_BLK
  uint32_t minor;
  int written;         // set once the node's inode and blocks are on disk
  struct node *parent;
  struct node *child;  // first child, for a directory
  struct node *next;   // next sibling
};

// one run of consecutive blocks belonging to a file
struct extent {
  uint32_t logical;  // first logical block of the run
  uint64_t physical; // first physical block of the run
  uint32_t len;      // blocks in the run, at most EXT4_EXT_MAX_LEN
};

// everything the writer needs to know about the image being built. the
// bitmaps and the inode table are held in memory and written out at the
// end, because the group descriptors that describe them can only be
// filled in once every allocation has happened.
struct image {
  int fd;
  const char *path;
  uint64_t blocks;          // total blocks in the volume
  uint32_t groups;          // block groups
  uint32_t ipg;             // inodes per group
  uint32_t inodes;          // total inodes, groups * ipg
  uint32_t desc_blocks;     // blocks holding the group descriptor table
  uint32_t reserved_gdt;    // empty blocks held back after the table
  uint32_t itb;             // inode table blocks per group
  uint32_t journal_blocks;  // blocks in the jbd2 journal
  uint32_t mtime;           // the one timestamp every inode gets
  unsigned char uuid[16];
  char label[16];
  uint32_t hash_seed[4];
  uint32_t csum_seed;       // crc32c(~0, uuid), the seed of everything else
  unsigned char *bmap;      // block in use, one bit per block of the volume
  unsigned char *imap;      // inode in use, one bit per inode of the volume
  unsigned char *itab;      // the inode table of the whole volume
  uint64_t *bbitmap_blk;    // per group: where its block bitmap landed
  uint64_t *ibitmap_blk;    // per group: where its inode bitmap landed
  uint64_t *itable_blk;     // per group: where its inode table landed
  uint32_t *used_dirs;      // per group: directories in it
  uint64_t alloc_hint;      // where the next allocation starts looking
  uint32_t next_ino;        // next inode number to hand out
  uint64_t data_blocks;     // blocks handed to files, for the summary
};

static const char *progname = "mkext4";

static void
die(const char *s)
{
  perror(s);
  exit(1);
}

// complain about something that is our own fault or the user's, rather
// than the operating system's, and give up
static void
fail(const char *fmt, ...)
{
  va_list ap;

  fprintf(stderr, "%s: ", progname);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

static void *
xmalloc(size_t n)
{
  void *p = calloc(1, n);

  if (p == 0)
    fail("out of memory asking for %zu bytes", n);
  return p;
}

static char *
xstrdup(const char *s)
{
  char *p = strdup(s);

  if (p == 0)
    fail("out of memory");
  return p;
}

// ---------------------------------------------------------------------
// crc32c
// ---------------------------------------------------------------------

// ext4's metadata_csum and jbd2's checksum v3 both use crc32c, the
// Castagnoli polynomial in its reflected form, with no final inversion:
// the caller supplies the seed and the result is used as it comes out.
// this is the bit-at-a-time definition turned into a table, and it must
// agree with ext2fs_crc32c_le() in e2fsprogs to the last bit.
static uint32_t crc32c_table[256];

static void
crc32c_init(void)
{
  uint32_t i, j, c;

  for (i = 0; i < 256; i++) {
    c = i;
    for (j = 0; j < 8; j++)
      c = (c & 1) ? (c >> 1) ^ EXT4_CRC32C_POLY_LE : c >> 1;
    crc32c_table[i] = c;
  }
}

static uint32_t
crc32c(uint32_t crc, const void *buf, size_t len)
{
  const unsigned char *p = buf;

  while (len--)
    crc = (crc >> 8) ^ crc32c_table[(crc ^ *p++) & 0xff];
  return crc;
}

// the journal keeps every one of its own fields big-endian, so these two
// are the only byte swaps in the tool
static uint32_t
cpu_to_be32(uint32_t x)
{
  return ((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | ((x >> 24) & 0xff);
}

// ---------------------------------------------------------------------
// raw image access
// ---------------------------------------------------------------------

static void
write_block(struct image *img, uint64_t blk, const void *buf)
{
  off_t off = (off_t)blk * BLOCK_SIZE;

  if (blk >= img->blocks)
    fail("write past the end of the image at block %llu", (unsigned long long)blk);
  if (pwrite(img->fd, buf, BLOCK_SIZE, off) != BLOCK_SIZE)
    die("write");
}

static void
write_bytes(struct image *img, off_t off, const void *buf, size_t n)
{
  if (pwrite(img->fd, buf, n, off) != (ssize_t)n)
    die("write");
}

// ---------------------------------------------------------------------
// bitmaps and allocation
// ---------------------------------------------------------------------

static int
bit_test(const unsigned char *map, uint64_t i)
{
  return (map[i >> 3] >> (i & 7)) & 1;
}

static void
bit_set(unsigned char *map, uint64_t i)
{
  map[i >> 3] |= 1 << (i & 7);
}

static void
mark_block(struct image *img, uint64_t blk)
{
  if (blk >= img->blocks)
    fail("block %llu is past the end of the image", (unsigned long long)blk);
  if (bit_test(img->bmap, blk))
    fail("block %llu allocated twice", (unsigned long long)blk);
  bit_set(img->bmap, blk);
}

static void
mark_blocks(struct image *img, uint64_t blk, uint64_t n)
{
  uint64_t i;

  for (i = 0; i < n; i++)
    mark_block(img, blk + i);
}

// take the longest run of free blocks, up to want, starting at or after
// hint. files are laid out with this, so a file that fits in a hole gets
// one extent and the extent tree stays in its inode.
static uint64_t
alloc_run(struct image *img, uint64_t hint, uint32_t want, uint32_t *got)
{
  uint64_t b, start;
  uint32_t n;

  for (b = hint; b < img->blocks; b++) {
    if (bit_test(img->bmap, b))
      continue;
    start = b;
    for (n = 0; n < want && start + n < img->blocks && !bit_test(img->bmap, start + n); n++)
      ;
    mark_blocks(img, start, n);
    *got = n;
    return start;
  }
  fail("out of blocks: the image is too small for what it is being asked to hold");
  return 0;
}

// cut a file of nblk blocks into extents. an extent can cover at most
// 32768 blocks, and a run that the allocator had to break gives another
// extent, so a badly fragmented image produces a deeper tree rather than
// a failure.
static int
alloc_file_blocks(struct image *img, uint64_t nblk, struct extent *ex, int maxex)
{
  uint64_t done = 0;
  int n = 0;
  uint32_t want, got;
  uint64_t blk;

  while (done < nblk) {
    want = (nblk - done > EXT4_EXT_MAX_LEN) ? EXT4_EXT_MAX_LEN : (uint32_t)(nblk - done);
    blk = alloc_run(img, img->alloc_hint, want, &got);
    img->alloc_hint = blk + got;
    if (n > 0 && ex[n - 1].physical + ex[n - 1].len == blk &&
        ex[n - 1].len + got <= EXT4_EXT_MAX_LEN) {
      ex[n - 1].len += got;
    } else {
      if (n >= maxex)
        fail("file needs more than %d extents", maxex);
      ex[n].logical = (uint32_t)done;
      ex[n].physical = blk;
      ex[n].len = got;
      n++;
    }
    done += got;
  }
  img->data_blocks += nblk;
  return n;
}

// ---------------------------------------------------------------------
// the inode table in memory
// ---------------------------------------------------------------------

// inodes are numbered from 1, and the table we keep is the whole volume's
// in one array so that a group's slice can be written out later without
// any further bookkeeping
static struct ext4_inode *
get_inode(struct image *img, uint32_t ino)
{
  if (ino < 1 || ino > img->inodes)
    fail("inode %u is out of range", ino);
  return (struct ext4_inode *)(img->itab + (size_t)(ino - 1) * INODE_SIZE);
}

// hand out the next inode and mark it in use. inodes are handed out in
// order, the first EXT4_FIRST_INO-1 of them to the format itself, so the
// bitmap can never be marked twice and a group's used inodes are always
// a prefix of its table.
static uint32_t
alloc_inode(struct image *img)
{
  uint32_t ino = img->next_ino++;

  if (ino > img->inodes)
    fail("out of inodes: %u inodes is not enough, use -i to ask for more", img->inodes);
  bit_set(img->imap, ino - 1);
  return ino;
}

// fill in the fields every inode has. the times are one timestamp for the
// whole image: reproducible builds want that, and nothing in the image is
// older or newer than anything else in any meaningful way.
static void
init_inode(struct image *img, uint32_t ino, uint16_t mode, uint32_t flags)
{
  struct ext4_inode *in = get_inode(img, ino);

  memset(in, 0, INODE_SIZE);
  in->i_mode = mode;
  in->i_links_count = 1;
  in->i_flags = flags;
  in->i_atime = img->mtime;
  in->i_ctime = img->mtime;
  in->i_mtime = img->mtime;
  in->i_crtime = img->mtime;
  // the extra area is what makes nanosecond and creation times possible;
  // every inode must reserve at least s_min_extra_isize of it
  in->i_extra_isize = EXT4_EXTRA_ISIZE;
}

// i_blocks counts 512-byte units, and it counts every block charged to
// the file: its data and the blocks of its extent tree
static void
add_iblocks(struct image *img, uint32_t ino, uint64_t blocks)
{
  struct ext4_inode *in = get_inode(img, ino);
  uint64_t sectors = (uint64_t)in->i_blocks_lo + ((uint64_t)in->l_i_blocks_high << 32);

  sectors += blocks * (BLOCK_SIZE / 512);
  in->i_blocks_lo = (uint32_t)sectors;
  in->l_i_blocks_high = (uint16_t)(sectors >> 32);
}

static void
set_isize(struct image *img, uint32_t ino, uint64_t size)
{
  struct ext4_inode *in = get_inode(img, ino);

  in->i_size_lo = (uint32_t)size;
  in->i_size_high = (uint32_t)(size >> 32);
}

// ---------------------------------------------------------------------
// extent trees
// ---------------------------------------------------------------------

// the checksum of an extent block covers the header and every entry slot
// the header allows, used or not, which is why the tail sits at a fixed
// offset rather than after the last entry in use
static void
extent_block_csum(struct image *img, uint32_t ino, unsigned char *block)
{
  struct ext4_extent_header *eh = (struct ext4_extent_header *)block;
  struct ext4_extent_tail *tail;
  uint32_t gen = get_inode(img, ino)->i_generation;
  uint32_t crc;
  size_t len = 12 + (size_t)eh->eh_max * 12;

  crc = crc32c(img->csum_seed, &ino, sizeof(ino));
  crc = crc32c(crc, &gen, sizeof(gen));
  crc = crc32c(crc, block, len);
  tail = (struct ext4_extent_tail *)(block + len);
  tail->et_checksum = crc;
}

// write one node of the extent tree and return the index entry that the
// level above needs to point at it. entries are 12 bytes whether they are
// extents or indices, so one routine packs both.
static void
write_extent_node(struct image *img, uint32_t ino, const unsigned char *entries, int nent,
                  int depth, uint32_t first_logical, struct ext4_extent_idx *idx)
{
  unsigned char block[BLOCK_SIZE];
  struct ext4_extent_header *eh = (struct ext4_extent_header *)block;
  uint64_t blk;
  uint32_t got;

  blk = alloc_run(img, img->alloc_hint, 1, &got);
  img->alloc_hint = blk + 1;
  add_iblocks(img, ino, 1);

  memset(block, 0, sizeof(block));
  eh->eh_magic = EXT4_EXT_MAGIC;
  eh->eh_entries = nent;
  eh->eh_max = EXT_PER_BLOCK;
  eh->eh_depth = depth;
  memcpy(block + 12, entries, (size_t)nent * 12);
  extent_block_csum(img, ino, block);
  write_block(img, blk, block);

  memset(idx, 0, sizeof(*idx));
  idx->ei_block = first_logical;
  idx->ei_leaf_lo = (uint32_t)blk;
  idx->ei_leaf_hi = (uint16_t)(blk >> 32);
}

// give an inode an extent tree covering the runs in ex. up to four
// extents fit in the 60 bytes of i_block and need no blocks of their own;
// past that the tree grows a level at a time, 340 entries to a block,
// until the root fits in the inode again.
static void
set_extent_tree(struct image *img, uint32_t ino, const struct extent *ex, int nex)
{
  struct ext4_inode *in = get_inode(img, ino);
  struct ext4_extent_header *eh = (struct ext4_extent_header *)in->i_block;
  unsigned char *level, *next;
  int nlevel, nnext, depth, i, j, per, take;
  uint32_t first;

  in->i_flags |= EXT4_EXTENTS_FL;
  memset(in->i_block, 0, sizeof(in->i_block));
  eh->eh_magic = EXT4_EXT_MAGIC;
  eh->eh_max = 4;
  eh->eh_depth = 0;
  eh->eh_entries = 0;
  if (nex == 0)
    return;

  // the leaves, still in memory, in the 12-byte on-disk shape
  level = xmalloc((size_t)nex * 12);
  for (i = 0; i < nex; i++) {
    struct ext4_extent e;
    memset(&e, 0, sizeof(e));
    e.ee_block = ex[i].logical;
    e.ee_len = (uint16_t)ex[i].len;
    e.ee_start_lo = (uint32_t)ex[i].physical;
    e.ee_start_hi = (uint16_t)(ex[i].physical >> 32);
    memcpy(level + (size_t)i * 12, &e, 12);
  }
  nlevel = nex;
  depth = 0;

  // grow upwards until the root fits in the inode
  while (nlevel > 4) {
    per = EXT_PER_BLOCK;
    nnext = (nlevel + per - 1) / per;
    next = xmalloc((size_t)nnext * 12);
    for (i = 0, j = 0; i < nlevel; i += take, j++) {
      take = (nlevel - i > per) ? per : nlevel - i;
      memcpy(&first, level + (size_t)i * 12, 4);
      write_extent_node(img, ino, level + (size_t)i * 12, take, depth, first,
                        (struct ext4_extent_idx *)(next + (size_t)j * 12));
    }
    free(level);
    level = next;
    nlevel = nnext;
    depth++;
  }

  eh->eh_entries = nlevel;
  eh->eh_depth = depth;
  memcpy((unsigned char *)in->i_block + 12, level, (size_t)nlevel * 12);
  free(level);
}

// ---------------------------------------------------------------------
// files
// ---------------------------------------------------------------------

// copy a host file into the image and map it with extents
static void
write_file(struct image *img, uint32_t ino, const char *source, uint16_t mode)
{
  struct stat st;
  struct extent *ex;
  unsigned char buf[BLOCK_SIZE];
  uint64_t nblk;
  int fd, i, nex;
  ssize_t n;

  if ((fd = open(source, O_RDONLY)) < 0)
    die(source);
  if (fstat(fd, &st) < 0)
    die(source);

  init_inode(img, ino, EXT4_S_IFREG | mode, 0);
  set_isize(img, ino, (uint64_t)st.st_size);

  nblk = ((uint64_t)st.st_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
  ex = xmalloc((size_t)(nblk + 1) * sizeof(*ex));
  nex = alloc_file_blocks(img, nblk, ex, (int)nblk + 1);

  // the data goes down in extent order, which is also file order
  for (i = 0; i < nex; i++) {
    uint32_t k;
    for (k = 0; k < ex[i].len; k++) {
      memset(buf, 0, sizeof(buf));
      n = read(fd, buf, BLOCK_SIZE);
      if (n < 0)
        die(source);
      write_block(img, ex[i].physical + k, buf);
    }
  }
  close(fd);

  add_iblocks(img, ino, nblk);
  set_extent_tree(img, ino, ex, nex);
  free(ex);
}

// a symbolic link whose target fits in the 60 bytes of i_block is a fast
// symlink: the text lives in the inode, the file has no blocks at all,
// and the extents flag must stay clear or a reader would try to parse the
// target as an extent header
static void
write_symlink(struct image *img, uint32_t ino, const char *target)
{
  struct ext4_inode *in;
  size_t len = strlen(target);
  struct extent ex;
  unsigned char buf[BLOCK_SIZE];
  uint32_t got;

  init_inode(img, ino, EXT4_S_IFLNK | 0777, 0);
  in = get_inode(img, ino);
  set_isize(img, ino, len);
  if (len < sizeof(in->i_block)) {
    memcpy(in->i_block, target, len);
    return;
  }
  if (len > BLOCK_SIZE - 1)
    fail("symlink target is longer than a block: %s", target);
  memset(buf, 0, sizeof(buf));
  memcpy(buf, target, len);
  ex.logical = 0;
  ex.physical = alloc_run(img, img->alloc_hint, 1, &got);
  ex.len = 1;
  img->alloc_hint = ex.physical + 1;
  img->data_blocks++;
  write_block(img, ex.physical, buf);
  add_iblocks(img, ino, 1);
  set_extent_tree(img, ino, &ex, 1);
}

// a device node has no data: the device number lives in i_block[0] in the
// old encoding when it fits, which is what every ext4 reader looks at
// first, and in i_block[1] in the wider encoding otherwise
static void
write_device(struct image *img, uint32_t ino, int type, uint32_t major, uint32_t minor)
{
  struct ext4_inode *in;

  init_inode(img, ino, (type == NODE_CHR ? EXT4_S_IFCHR : EXT4_S_IFBLK) | 0600, 0);
  in = get_inode(img, ino);
  if (major < 256 && minor < 256) {
    in->i_block[0] = (major << 8) | minor;
  } else {
    in->i_block[1] = (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
  }
}

// ---------------------------------------------------------------------
// directories
// ---------------------------------------------------------------------

// the checksum of a directory block lives in the last 12 bytes, disguised
// as an entry with an impossible file type so that a driver which does
// not know about it walks past the end of the real entries and stops
static void
dir_block_csum(struct image *img, uint32_t ino, unsigned char *block)
{
  struct ext4_dir_entry_tail *t = (struct ext4_dir_entry_tail *)(block + DIR_USABLE);
  uint32_t gen = get_inode(img, ino)->i_generation;
  uint32_t crc;

  memset(t, 0, sizeof(*t));
  t->det_rec_len = EXT4_DIR_TAIL_SIZE;
  t->det_reserved_ft = EXT4_FT_DIR_CSUM;
  crc = crc32c(img->csum_seed, &ino, sizeof(ino));
  crc = crc32c(crc, &gen, sizeof(gen));
  crc = crc32c(crc, block, DIR_USABLE);
  t->det_checksum = crc;
}

static uint8_t
file_type_of(const struct node *np)
{
  switch (np->type) {
  case NODE_DIR:
    return EXT4_FT_DIR;
  case NODE_FILE:
    return EXT4_FT_REG_FILE;
  case NODE_SYM:
    return EXT4_FT_SYMLINK;
  case NODE_CHR:
    return EXT4_FT_CHRDEV;
  case NODE_BLK:
    return EXT4_FT_BLKDEV;
  }
  fail("unknown node type %d", np->type);
  return 0;
}

// the space an entry takes: the 8-byte header and the name, rounded up to
// four bytes, because rec_len must keep every entry aligned
static uint32_t
dirent_size(const char *name)
{
  return (8 + (uint32_t)strlen(name) + 3) & ~3u;
}

// lay one entry into a block being built, and remember where it started
// so that it can be stretched to the end of the block when the block is
// closed. the last entry of a block always reaches the tail: that is how
// a reader knows it has seen everything.
struct dirbuild {
  unsigned char *blocks; // the directory's data, one block at a time
  uint32_t nblocks;
  uint32_t off;          // offset inside the last block
  uint32_t last;         // offset of the last entry written
};

static void
dir_new_block(struct dirbuild *db)
{
  db->blocks = realloc(db->blocks, (size_t)(db->nblocks + 1) * BLOCK_SIZE);
  if (db->blocks == 0)
    fail("out of memory building a directory");
  memset(db->blocks + (size_t)db->nblocks * BLOCK_SIZE, 0, BLOCK_SIZE);
  db->nblocks++;
  db->off = 0;
  db->last = 0;
}

// close the current block: stretch its last entry over the free space up
// to the checksum tail, or, if the block has no entries at all, put an
// empty entry there so the block is still a legal directory block
static void
dir_close_block(struct dirbuild *db)
{
  unsigned char *b = db->blocks + (size_t)(db->nblocks - 1) * BLOCK_SIZE;
  struct ext4_dir_entry_2 *de;

  if (db->off == 0) {
    de = (struct ext4_dir_entry_2 *)b;
    de->inode = 0;
    de->rec_len = DIR_USABLE;
    de->name_len = 0;
    de->file_type = 0;
    return;
  }
  de = (struct ext4_dir_entry_2 *)(b + db->last);
  de->rec_len = DIR_USABLE - db->last;
}

static void
dir_add(struct dirbuild *db, uint32_t ino, uint8_t type, const char *name)
{
  uint32_t need = dirent_size(name);
  unsigned char *b;
  struct ext4_dir_entry_2 *de;

  if (strlen(name) > EXT4_NAME_LEN)
    fail("name is too long for ext4: %s", name);
  if (db->nblocks == 0 || db->off + need > DIR_USABLE) {
    if (db->nblocks > 0)
      dir_close_block(db);
    dir_new_block(db);
  }
  b = db->blocks + (size_t)(db->nblocks - 1) * BLOCK_SIZE;
  de = (struct ext4_dir_entry_2 *)(b + db->off);
  de->inode = ino;
  de->rec_len = (uint16_t)need;
  de->name_len = (unsigned char)strlen(name);
  de->file_type = type;
  memcpy(de->name, name, strlen(name));
  db->last = db->off;
  db->off += need;
}

// write a directory out: "." and ".." first, as every ext4 reader and
// fsck expects, then the children in the order they were given on the
// command line. the link count is two (itself and its entry in its
// parent) plus one for each subdirectory's "..".
static void
write_dir(struct image *img, struct node *dir, uint32_t min_blocks)
{
  struct dirbuild db;
  struct node *np;
  struct extent *ex;
  struct ext4_inode *in;
  uint32_t i, links;
  int nex;

  memset(&db, 0, sizeof(db));
  dir_add(&db, dir->ino, EXT4_FT_DIR, ".");
  dir_add(&db, dir->parent ? dir->parent->ino : dir->ino, EXT4_FT_DIR, "..");
  links = 2;
  for (np = dir->child; np; np = np->next) {
    dir_add(&db, np->ino, file_type_of(np), np->name);
    if (np->type == NODE_DIR)
      links++;
  }
  dir_close_block(&db);

  // lost+found is given blocks it does not need yet, so that fsck can
  // always link something into it even on a full file system
  while (db.nblocks < min_blocks) {
    dir_new_block(&db);
    dir_close_block(&db);
  }

  in = get_inode(img, dir->ino);
  in->i_links_count = (uint16_t)links;
  set_isize(img, dir->ino, (uint64_t)db.nblocks * BLOCK_SIZE);

  ex = xmalloc((size_t)(db.nblocks + 1) * sizeof(*ex));
  nex = alloc_file_blocks(img, db.nblocks, ex, (int)db.nblocks + 1);
  for (i = 0; i < db.nblocks; i++)
    dir_block_csum(img, dir->ino, db.blocks + (size_t)i * BLOCK_SIZE);
  {
    uint32_t written = 0;
    int k;
    for (k = 0; k < nex; k++) {
      uint32_t j;
      for (j = 0; j < ex[k].len; j++)
        write_block(img, ex[k].physical + j, db.blocks + (size_t)(written++) * BLOCK_SIZE);
    }
  }
  add_iblocks(img, dir->ino, db.nblocks);
  set_extent_tree(img, dir->ino, ex, nex);
  free(ex);
  free(db.blocks);
}

// ---------------------------------------------------------------------
// the tree of names to write
// ---------------------------------------------------------------------

static struct node *
new_node(struct image *img, struct node *parent, const char *name, int type)
{
  struct node *np = xmalloc(sizeof(*np));
  struct node **pp;

  np->name = xstrdup(name);
  np->type = type;
  np->parent = parent;
  np->ino = alloc_inode(img);
  if (parent) {
    for (pp = &parent->child; *pp; pp = &(*pp)->next)
      ;
    *pp = np;
  }
  return np;
}

static struct node *
lookup(struct node *dir, const char *name)
{
  struct node *np;

  for (np = dir->child; np; np = np->next)
    if (strcmp(np->name, name) == 0)
      return np;
  return 0;
}

// walk a path, making the directories on the way, and return the
// directory the last component belongs in. the last component itself is
// left in *leaf.
static struct node *
walk_path(struct image *img, struct node *root, char *path, char **leaf)
{
  struct node *dir = root, *np;
  char *p = path, *slash;

  while (*p == '/')
    p++;
  if (*p == 0)
    fail("empty path in a spec");
  for (;;) {
    slash = strchr(p, '/');
    if (slash == 0)
      break;
    *slash = 0;
    if (*p != 0 && strcmp(p, ".") != 0) {
      np = lookup(dir, p);
      if (np == 0) {
        np = new_node(img, dir, p, NODE_DIR);
        np->mode = 0755;
      }
      if (np->type != NODE_DIR)
        fail("%s is not a directory", p);
      dir = np;
    }
    *slash = '/';
    p = slash + 1;
  }
  *leaf = p;
  return dir;
}

// one SPEC from the command line. the forms are
//   SOURCE                    put the file at /BASENAME, without the
//                             leading underscore the build uses to keep
//                             the host from running a riscv binary
//   TARGET:SOURCE             put SOURCE at TARGET
//   TARGET::mode:MODE:SOURCE  put SOURCE at TARGET with that mode, in
//                             octal: the host file system cannot carry
//                             a set-user-id bit through a build, and a
//                             program like su needs one
//   TARGET::dir               an empty directory
//   TARGET::dir:MODE          ... with that mode
//   TARGET::sym:LINKTARGET    a symbolic link
//   TARGET::dev:c:MAJOR:MINOR a character device, or b for a block device
static void
add_spec(struct image *img, struct node *root, char *spec)
{
  char *colon = strchr(spec, ':');
  char *target, *leaf, *rest;
  struct node *dir, *np;
  struct stat st;

  if (colon == 0 || colon[1] != ':') {
    // a plain source file, or TARGET:SOURCE
    char *source;
    if (colon) {
      *colon = 0;
      target = spec;
      source = colon + 1;
    } else {
      source = spec;
      target = strrchr(source, '/');
      target = target ? target + 1 : source;
      if (target[0] == '_')
        target++;
    }
    if (stat(source, &st) < 0)
      die(source);
    dir = walk_path(img, root, target, &leaf);
    if (*leaf == 0)
      fail("no file name in the spec %s", target);
    if (lookup(dir, leaf))
      fail("%s is already in the image", leaf);
    np = new_node(img, dir, leaf, NODE_FILE);
    np->source = xstrdup(source);
    np->mode = st.st_mode & 0777;
    return;
  }

  // one of the TARGET::KIND forms
  *colon = 0;
  target = spec;
  rest = colon + 2;
  dir = walk_path(img, root, target, &leaf);
  if (*leaf == 0)
    fail("no name in the spec %s", target);

  if (strcmp(rest, "dir") == 0 || strncmp(rest, "dir:", 4) == 0) {
    int mode = rest[3] == ':' ? (int)strtol(rest + 4, NULL, 8) : 0755;
    np = lookup(dir, leaf);
    if (np) {
      if (np->type != NODE_DIR)
        fail("%s is already in the image and is not a directory", leaf);
      return;
    }
    np = new_node(img, dir, leaf, NODE_DIR);
    np->mode = mode;
  } else if (strncmp(rest, "mode:", 5) == 0) {
    char *p = rest + 5;
    char *sep = strchr(p, ':');
    if (sep == NULL)
      fail("the spec %s has no source file", spec);
    *sep = 0;
    int mode = (int)strtol(p, NULL, 8);
    const char *source = sep + 1;
    if (stat(source, &st) < 0)
      die(source);
    if (lookup(dir, leaf))
      fail("%s is already in the image", leaf);
    np = new_node(img, dir, leaf, NODE_FILE);
    np->source = xstrdup(source);
    np->mode = mode;
  } else if (strncmp(rest, "sym:", 4) == 0) {
    if (lookup(dir, leaf))
      fail("%s is already in the image", leaf);
    np = new_node(img, dir, leaf, NODE_SYM);
    np->link = xstrdup(rest + 4);
    if (np->link[0] == 0)
      fail("empty symlink target in %s", spec);
  } else if (strncmp(rest, "dev:", 4) == 0) {
    char kind;
    unsigned major, minor;
    if (sscanf(rest + 4, "%c:%u:%u", &kind, &major, &minor) != 3)
      fail("bad device spec: %s", rest);
    if (kind != 'c' && kind != 'b')
      fail("device kind must be c or b: %s", rest);
    if (lookup(dir, leaf))
      fail("%s is already in the image", leaf);
    np = new_node(img, dir, leaf, kind == 'c' ? NODE_CHR : NODE_BLK);
    np->major = major;
    np->minor = minor;
  } else {
    fail("unknown spec kind: %s", rest);
  }
}

// write every node of the tree, directories first so that a directory's
// blocks come before its children's and a reader walking the image in
// block order sees the tree in a sensible order
static void
write_tree(struct image *img, struct node *dir)
{
  struct node *np;

  for (np = dir->child; np; np = np->next) {
    if (np->written)
      continue;
    np->written = 1;
    switch (np->type) {
    case NODE_DIR:
      init_inode(img, np->ino, EXT4_S_IFDIR | np->mode, 0);
      write_dir(img, np, 0);
      break;
    case NODE_FILE:
      write_file(img, np->ino, np->source, (uint16_t)np->mode);
      break;
    case NODE_SYM:
      write_symlink(img, np->ino, np->link);
      break;
    case NODE_CHR:
    case NODE_BLK:
      write_device(img, np->ino, np->type, np->major, np->minor);
      break;
    }
  }
  for (np = dir->child; np; np = np->next)
    if (np->type == NODE_DIR)
      write_tree(img, np);
}

// ---------------------------------------------------------------------
// the journal
// ---------------------------------------------------------------------

// the journal is an ordinary file, inode 8, whose first block holds the
// jbd2 superblock and whose remaining blocks are the circular log. an
// empty journal is one with s_start 0 and s_sequence 1: recovery sees
// nothing to replay and the kernel can start writing at block s_first.
static void
write_journal(struct image *img)
{
  unsigned char block[BLOCK_SIZE];
  journal_superblock_t *js = (journal_superblock_t *)block;
  struct extent *ex;
  uint32_t n = img->journal_blocks;
  uint32_t crc;
  int nex;

  init_inode(img, EXT4_JOURNAL_INO, EXT4_S_IFREG | 0600, 0);
  set_isize(img, EXT4_JOURNAL_INO, (uint64_t)n * BLOCK_SIZE);

  ex = xmalloc((size_t)(n + 1) * sizeof(*ex));
  nex = alloc_file_blocks(img, n, ex, (int)n + 1);
  add_iblocks(img, EXT4_JOURNAL_INO, n);
  set_extent_tree(img, EXT4_JOURNAL_INO, ex, nex);

  memset(block, 0, sizeof(block));
  js->s_header.h_magic = cpu_to_be32(JBD2_MAGIC_NUMBER);
  js->s_header.h_blocktype = cpu_to_be32(JBD2_SUPERBLOCK_V2);
  js->s_header.h_sequence = 0;
  js->s_blocksize = cpu_to_be32(BLOCK_SIZE);
  js->s_maxlen = cpu_to_be32(n);
  js->s_first = cpu_to_be32(1);
  js->s_sequence = cpu_to_be32(1);
  js->s_start = cpu_to_be32(0);
  js->s_nr_users = cpu_to_be32(1);
  memcpy(js->s_uuid, img->uuid, sizeof(js->s_uuid));
  // checksum v3 is the tag format ext4 uses on a checksummed volume; the
  // journal superblock then checksums itself, seeded with ~0 like the
  // file system superblock and unlike everything else
  js->s_feature_incompat = cpu_to_be32(JBD2_FEATURE_INCOMPAT_CSUM_V3);
  js->s_checksum_type = JBD2_CRC32C_CHKSUM;
  crc = crc32c(EXT4_CSUM_SEED_INIT, js, sizeof(*js));
  js->s_checksum = cpu_to_be32(crc);
  write_block(img, ex[0].physical, block);
  free(ex);
}

// ---------------------------------------------------------------------
// superblock, group descriptors, bitmaps
// ---------------------------------------------------------------------

static int
is_power_of(uint32_t n, uint32_t b)
{
  while (n > 1) {
    if (n % b != 0)
      return 0;
    n /= b;
  }
  return 1;
}

// sparse_super keeps superblock backups only in group 0, group 1 and the
// groups whose number is a power of 3, 5 or 7; without it every group
// would carry one, which on a large volume is a lot of wasted space
static int
group_has_super(uint32_t g)
{
  if (g == 0 || g == 1)
    return 1;
  return is_power_of(g, 3) || is_power_of(g, 5) || is_power_of(g, 7);
}

// the group descriptor checksum is the low half of a crc32c over the
// group number and then the whole descriptor with the checksum field
// itself zeroed. the older crc16 form of this checksum instead skipped
// the two bytes of the field, which is not the same thing and is why a
// descriptor checksum is the first thing to get wrong.
static void
group_desc_csum(struct image *img, uint32_t g, struct ext4_group_desc *gd)
{
  uint32_t le_group = g;
  uint32_t crc;

  gd->bg_checksum = 0;
  crc = crc32c(img->csum_seed, &le_group, sizeof(le_group));
  crc = crc32c(crc, gd, DESC_SIZE);
  gd->bg_checksum = (uint16_t)(crc & 0xffff);
}

static void
super_csum(struct ext4_super_block *sb)
{
  sb->s_checksum = crc32c(EXT4_CSUM_SEED_INIT, sb, offsetof(struct ext4_super_block, s_checksum));
}

// the resize inode, inode 7, is what makes the reserved descriptor
// blocks usable: it claims them so that nothing else allocates them, and
// records where each one's backup copies live. it is the one inode in the
// image that still uses the old indirect block scheme, because resize2fs
// and fsck read it through i_block directly: i_block[EXT4_DIND_BLOCK]
// points at a block whose entry (desc_blocks + k) is the k-th reserved
// descriptor block, and that block in turn lists the same block in every
// group that carries a superblock backup. fsck checks exactly this shape,
// and refuses a volume that reserves descriptor blocks without it.
static void
write_resize_inode(struct image *img)
{
  uint32_t apb = BLOCK_SIZE / 4;
  uint32_t buf[BLOCK_SIZE / 4];
  uint32_t dind[BLOCK_SIZE / 4];
  struct ext4_inode *in;
  uint64_t dind_blk, charged;
  uint32_t k, g, n, got, gdt_blk;

  if (img->reserved_gdt == 0)
    return;

  init_inode(img, EXT4_RESIZE_INO, EXT4_S_IFREG | 0600, 0);
  dind_blk = alloc_run(img, img->alloc_hint, 1, &got);
  img->alloc_hint = dind_blk + 1;
  memset(dind, 0, sizeof(dind));
  charged = 1;

  for (k = 0; k < img->reserved_gdt; k++) {
    gdt_blk = 1 + img->desc_blocks + k;
    dind[(img->desc_blocks + k) % apb] = gdt_blk;
    memset(buf, 0, sizeof(buf));
    n = 0;
    for (g = 1; g < img->groups; g++) {
      uint64_t backup;
      if (!group_has_super(g))
        continue;
      backup = gdt_blk + (uint64_t)g * BLOCKS_PER_GROUP;
      // the resize inode is the one place the old 32-bit block pointers
      // survive, so a volume whose backups sit past 2^32 blocks (16 TiB)
      // cannot describe them; mke2fs has the same limit
      if (n >= apb || backup > 0xffffffffu)
        fail("the volume is too large for the resize inode to describe");
      buf[n++] = (uint32_t)backup;
    }
    write_block(img, gdt_blk, buf);
    // the reserved block itself, and the backup of it in every group that
    // carries one, are all charged to this inode
    charged += 1 + n;
  }
  write_block(img, dind_blk, dind);

  in = get_inode(img, EXT4_RESIZE_INO);
  in->i_block[EXT4_DIND_BLOCK] = (uint32_t)dind_blk;
  add_iblocks(img, EXT4_RESIZE_INO, charged);
  // the size mke2fs gives it: everything a double indirect block could
  // ever reach, so that no later growth makes the inode look truncated
  set_isize(img, EXT4_RESIZE_INO, ((uint64_t)apb * apb + apb + EXT4_NDIR_BLOCKS) * BLOCK_SIZE);
}

// reserve the blocks every group needs before anything else is allocated:
// the superblock backup and the descriptor table where sparse_super puts
// them. this happens first so that the flex_bg placement below can simply
// take the next free blocks it finds.
static void
reserve_group_metadata(struct image *img)
{
  uint32_t g;
  uint64_t start;

  for (g = 0; g < img->groups; g++) {
    if (!group_has_super(g))
      continue;
    start = (uint64_t)g * BLOCKS_PER_GROUP;
    mark_block(img, start);
    mark_blocks(img, start + 1, img->desc_blocks + img->reserved_gdt);
  }
}

// place the bitmaps and inode tables. with flex_bg the metadata of a run
// of 16 groups is packed together at the front of the run, which keeps
// the seeks of an allocation-heavy workload in one place and leaves long
// runs of free blocks for files.
static void
place_group_metadata(struct image *img)
{
  uint32_t f, g, first, last, got;
  uint64_t hint;

  for (f = 0; f * FLEX_BG_SIZE < img->groups; f++) {
    first = f * FLEX_BG_SIZE;
    last = first + FLEX_BG_SIZE;
    if (last > img->groups)
      last = img->groups;
    hint = (uint64_t)first * BLOCKS_PER_GROUP;
    for (g = first; g < last; g++) {
      img->bbitmap_blk[g] = alloc_run(img, hint, 1, &got);
      hint = img->bbitmap_blk[g] + 1;
    }
    for (g = first; g < last; g++) {
      img->ibitmap_blk[g] = alloc_run(img, hint, 1, &got);
      hint = img->ibitmap_blk[g] + 1;
    }
    for (g = first; g < last; g++) {
      // an inode table has to be one run of blocks: the table is indexed
      // arithmetically, not through any map
      img->itable_blk[g] = alloc_run(img, hint, img->itb, &got);
      if (got != img->itb)
        fail("no room for the inode table of group %u", g);
      hint = img->itable_blk[g] + img->itb;
    }
  }
}

// write the bitmaps, the inode table, the group descriptors and the
// superblock, with every count and checksum they carry
static void
write_metadata(struct image *img)
{
  struct ext4_super_block sb;
  struct ext4_group_desc *gds;
  unsigned char block[BLOCK_SIZE];
  unsigned char *gdt;
  uint32_t g, i, free_inodes = 0, crc;
  uint64_t free_blocks = 0, b;
  uint32_t jnl_backup[17];
  struct ext4_inode *jin;

  gds = xmalloc((size_t)img->groups * DESC_SIZE);
  gdt = xmalloc((size_t)img->desc_blocks * BLOCK_SIZE);

  // the inode checksums come last, because they cover the finished inode
  for (i = 1; i <= img->inodes; i++) {
    struct ext4_inode *in = get_inode(img, i);
    uint32_t ino = i, gen = in->i_generation;
    int all_zero = 1;
    unsigned char *p = (unsigned char *)in;
    size_t k;

    for (k = 0; k < INODE_SIZE; k++)
      if (p[k]) {
        all_zero = 0;
        break;
      }
    // an inode that was never formatted stays all zeroes; fsck accepts a
    // zero checksum there, and writing one would only make it different
    // from what mke2fs leaves behind
    if (all_zero)
      continue;
    in->l_i_checksum_lo = 0;
    in->i_checksum_hi = 0;
    crc = crc32c(img->csum_seed, &ino, sizeof(ino));
    crc = crc32c(crc, &gen, sizeof(gen));
    crc = crc32c(crc, p, INODE_SIZE);
    in->l_i_checksum_lo = (uint16_t)(crc & 0xffff);
    in->i_checksum_hi = (uint16_t)(crc >> 16);
  }

  for (g = 0; g < img->groups; g++) {
    struct ext4_group_desc *gd = &gds[g];
    uint64_t gstart = (uint64_t)g * BLOCKS_PER_GROUP;
    uint32_t gfree = 0, ifree = 0, unused;
    uint32_t last_used = 0;

    // the block bitmap covers a whole group whether or not the volume
    // reaches that far; the bits past the end of the volume are set, and
    // fsck checks that padding as carefully as it checks the rest
    memset(block, 0, sizeof(block));
    for (i = 0; i < BLOCKS_PER_GROUP; i++) {
      b = gstart + i;
      if (b >= img->blocks || bit_test(img->bmap, b))
        bit_set(block, i);
      else
        gfree++;
    }
    write_block(img, img->bbitmap_blk[g], block);
    crc = crc32c(img->csum_seed, block, BLOCKS_PER_GROUP / 8);
    gd->bg_block_bitmap_csum_lo = (uint16_t)(crc & 0xffff);
    gd->bg_block_bitmap_csum_hi = (uint16_t)(crc >> 16);

    // the inode bitmap has one bit per inode of the group and the rest of
    // the block is padding, again all ones. its checksum covers only the
    // bits, not the padding.
    memset(block, 0, sizeof(block));
    for (i = 0; i < img->ipg; i++) {
      uint32_t ino = g * img->ipg + i + 1;
      if (ino <= img->inodes && bit_test(img->imap, ino - 1)) {
        bit_set(block, i);
        last_used = i + 1;
      } else {
        ifree++;
      }
    }
    for (i = img->ipg; i < BLOCK_SIZE * 8; i++)
      bit_set(block, i);
    write_block(img, img->ibitmap_blk[g], block);
    crc = crc32c(img->csum_seed, block, img->ipg / 8);
    gd->bg_inode_bitmap_csum_lo = (uint16_t)(crc & 0xffff);
    gd->bg_inode_bitmap_csum_hi = (uint16_t)(crc >> 16);

    // the inode table of this group, straight out of the array we built
    for (i = 0; i < img->itb; i++)
      write_block(img, img->itable_blk[g] + i,
                  img->itab + ((size_t)g * img->ipg + (size_t)i * INODES_PER_BLOCK) * INODE_SIZE);

    // bg_itable_unused says how many inodes at the end of the table have
    // never been used, so that fsck and the kernel can skip reading them
    unused = img->ipg - last_used;

    gd->bg_block_bitmap_lo = (uint32_t)img->bbitmap_blk[g];
    gd->bg_block_bitmap_hi = (uint32_t)(img->bbitmap_blk[g] >> 32);
    gd->bg_inode_bitmap_lo = (uint32_t)img->ibitmap_blk[g];
    gd->bg_inode_bitmap_hi = (uint32_t)(img->ibitmap_blk[g] >> 32);
    gd->bg_inode_table_lo = (uint32_t)img->itable_blk[g];
    gd->bg_inode_table_hi = (uint32_t)(img->itable_blk[g] >> 32);
    gd->bg_free_blocks_count_lo = (uint16_t)gfree;
    gd->bg_free_blocks_count_hi = (uint16_t)(gfree >> 16);
    gd->bg_free_inodes_count_lo = (uint16_t)ifree;
    gd->bg_free_inodes_count_hi = (uint16_t)(ifree >> 16);
    gd->bg_used_dirs_count_lo = (uint16_t)img->used_dirs[g];
    gd->bg_used_dirs_count_hi = (uint16_t)(img->used_dirs[g] >> 16);
    gd->bg_itable_unused_lo = (uint16_t)unused;
    gd->bg_itable_unused_hi = (uint16_t)(unused >> 16);
    gd->bg_flags = 0;
    free_blocks += gfree;
    free_inodes += ifree;
    group_desc_csum(img, g, gd);
  }

  // the journal inode's block map, kept in the superblock as a backup
  jin = get_inode(img, EXT4_JOURNAL_INO);
  memcpy(jnl_backup, jin->i_block, sizeof(jin->i_block));
  jnl_backup[15] = jin->i_size_high;
  jnl_backup[16] = jin->i_size_lo;

  memset(&sb, 0, sizeof(sb));
  sb.s_inodes_count = img->inodes;
  sb.s_blocks_count_lo = (uint32_t)img->blocks;
  sb.s_blocks_count_hi = (uint32_t)(img->blocks >> 32);
  // five percent of the volume is kept for root, so that a full file
  // system can still be repaired and logged into
  sb.s_r_blocks_count_lo = (uint32_t)(img->blocks / 20);
  sb.s_r_blocks_count_hi = (uint32_t)((img->blocks / 20) >> 32);
  sb.s_free_blocks_count_lo = (uint32_t)free_blocks;
  sb.s_free_blocks_count_hi = (uint32_t)(free_blocks >> 32);
  sb.s_free_inodes_count = free_inodes;
  sb.s_first_data_block = 0; // block 0 holds the superblock at offset 1024
  sb.s_log_block_size = BLOCK_LOG - EXT4_MIN_BLOCK_LOG_SIZE;
  sb.s_log_cluster_size = BLOCK_LOG - EXT4_MIN_BLOCK_LOG_SIZE;
  sb.s_blocks_per_group = BLOCKS_PER_GROUP;
  sb.s_clusters_per_group = BLOCKS_PER_GROUP;
  sb.s_inodes_per_group = img->ipg;
  sb.s_mtime = 0;
  sb.s_wtime = img->mtime;
  sb.s_mnt_count = 0;
  sb.s_max_mnt_count = 0xffff; // -1: never force a check on mount count
  sb.s_magic = EXT4_SUPER_MAGIC;
  sb.s_state = EXT4_VALID_FS;
  sb.s_errors = EXT4_ERRORS_RO;
  sb.s_lastcheck = img->mtime;
  sb.s_checkinterval = 0;
  sb.s_creator_os = EXT4_OS_LINUX;
  sb.s_rev_level = EXT4_DYNAMIC_REV;
  sb.s_first_ino = EXT4_FIRST_INO;
  sb.s_inode_size = INODE_SIZE;
  sb.s_block_group_nr = 0;
  sb.s_feature_compat = EXT4_FEATURE_COMPAT_HAS_JOURNAL | EXT4_FEATURE_COMPAT_RESIZE_INODE;
  sb.s_feature_incompat = EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |
                          EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG;
  sb.s_feature_ro_compat = EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
                           EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
                           EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
                           EXT4_FEATURE_RO_COMPAT_DIR_NLINK |
                           EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
                           EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
  memcpy(sb.s_uuid, img->uuid, sizeof(sb.s_uuid));
  memcpy(sb.s_volume_name, img->label, sizeof(sb.s_volume_name));
  sb.s_reserved_gdt_blocks = (uint16_t)img->reserved_gdt;
  sb.s_journal_inum = EXT4_JOURNAL_INO;
  memcpy(sb.s_hash_seed, img->hash_seed, sizeof(sb.s_hash_seed));
  sb.s_def_hash_version = EXT4_HASH_HALF_MD4;
  sb.s_jnl_backup_type = EXT4_JNL_BACKUP_BLOCKS;
  memcpy(sb.s_jnl_blocks, jnl_backup, sizeof(sb.s_jnl_blocks));
  sb.s_desc_size = DESC_SIZE;
  sb.s_default_mount_opts = EXT4_DEFM_XATTR_USER | EXT4_DEFM_ACL;
  sb.s_mkfs_time = img->mtime;
  sb.s_min_extra_isize = EXT4_EXTRA_ISIZE;
  sb.s_want_extra_isize = EXT4_EXTRA_ISIZE;
  // riscv64, like arm64, has an unsigned char, and the htree hash of a
  // name depends on the sign: record which one this volume was made with
  // so that a kernel using the other convention knows to compensate
  sb.s_flags = EXT4_FLAGS_UNSIGNED_HASH;
  sb.s_log_groups_per_flex = LOG_FLEX_BG;
  sb.s_checksum_type = EXT4_CRC32C_CHKSUM;
  // the seed every other checksum starts from. with the csum_seed feature
  // off this is only a cache of crc32c(~0, uuid), which is what a reader
  // computes for itself; writing it costs nothing and lets a reader check
  // that it derived the same value.
  sb.s_checksum_seed = img->csum_seed;

  // the group descriptor table, padded out to whole blocks
  memset(gdt, 0, (size_t)img->desc_blocks * BLOCK_SIZE);
  memcpy(gdt, gds, (size_t)img->groups * DESC_SIZE);

  // the primary superblock sits at byte 1024, inside block 0, and the
  // table follows in the next blocks; every backup group repeats both,
  // with s_block_group_nr naming the group so a rescue knows which copy
  // it is looking at
  for (g = 0; g < img->groups; g++) {
    uint64_t gstart = (uint64_t)g * BLOCKS_PER_GROUP;

    if (!group_has_super(g))
      continue;
    sb.s_block_group_nr = (uint16_t)g;
    super_csum(&sb);
    if (g == 0)
      write_bytes(img, EXT4_SUPER_OFFSET, &sb, sizeof(sb));
    else
      write_bytes(img, (off_t)gstart * BLOCK_SIZE, &sb, sizeof(sb));
    for (i = 0; i < img->desc_blocks; i++)
      write_block(img, gstart + 1 + i, gdt + (size_t)i * BLOCK_SIZE);
  }

  printf("%s: %llu blocks of %d bytes, %u inodes, %u group%s, %llu free blocks, %u free inodes\n",
         img->path, (unsigned long long)img->blocks, BLOCK_SIZE, img->inodes, img->groups,
         img->groups == 1 ? "" : "s", (unsigned long long)free_blocks, free_inodes);

  free(gds);
  free(gdt);
}

// ---------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------

// how many descriptor blocks to hold back for growth. mke2fs reserves
// enough for the volume to grow a thousandfold, capped at what one
// indirect block could ever address.
static uint32_t
calc_reserved_gdt(uint64_t blocks, uint32_t desc_blocks)
{
  uint64_t max_resize = blocks * 1024;
  uint64_t rsv_groups = (max_resize + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP;
  uint64_t rsv_gdb = (rsv_groups + DESC_PER_BLOCK - 1) / DESC_PER_BLOCK;

  if (rsv_gdb < desc_blocks)
    return 0;
  rsv_gdb -= desc_blocks;
  if (rsv_gdb > BLOCK_SIZE / 4)
    rsv_gdb = BLOCK_SIZE / 4;
  return (uint32_t)rsv_gdb;
}

static void
init_geometry(struct image *img, uint64_t blocks, uint32_t bytes_per_inode)
{
  uint64_t inodes;

  img->blocks = blocks;
  img->groups = (uint32_t)((blocks + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP);
  img->desc_blocks = (img->groups + DESC_PER_BLOCK - 1) / DESC_PER_BLOCK;
  img->reserved_gdt = calc_reserved_gdt(blocks, img->desc_blocks);

  inodes = (blocks * BLOCK_SIZE) / bytes_per_inode;
  if (inodes < EXT4_FIRST_INO + 1)
    inodes = EXT4_FIRST_INO + 1;
  // every group holds the same number of inodes: a multiple of the 16
  // that fit in a block, so that the inode table is whole blocks and the
  // bitmap ends on a byte, and no more than one bitmap block describes
  img->ipg = (uint32_t)((inodes + img->groups - 1) / img->groups);
  img->ipg = (img->ipg + INODES_PER_BLOCK - 1) & ~(uint32_t)(INODES_PER_BLOCK - 1);
  if (img->ipg > BLOCK_SIZE * 8)
    img->ipg = BLOCK_SIZE * 8;
  if (img->ipg > BLOCKS_PER_GROUP)
    img->ipg = BLOCKS_PER_GROUP;
  img->itb = (img->ipg * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
  img->inodes = img->ipg * img->groups;

  img->bmap = xmalloc((size_t)(blocks + 7) / 8);
  img->imap = xmalloc((size_t)(img->inodes + 7) / 8);
  img->itab = xmalloc((size_t)img->inodes * INODE_SIZE);
  img->bbitmap_blk = xmalloc((size_t)img->groups * sizeof(uint64_t));
  img->ibitmap_blk = xmalloc((size_t)img->groups * sizeof(uint64_t));
  img->itable_blk = xmalloc((size_t)img->groups * sizeof(uint64_t));
  img->used_dirs = xmalloc((size_t)img->groups * sizeof(uint32_t));
}

// ---------------------------------------------------------------------
// reading an image back
// ---------------------------------------------------------------------

// the reader walks the real structures, the way the kernel driver will
// have to: superblock, group descriptor, inode table, extent tree,
// directory blocks. it shares nothing with the writer above except the
// header, so it is an independent check on what was written.
struct reader {
  int fd;
  struct ext4_super_block sb;
  uint32_t bs;
  uint32_t ipg;
  uint32_t inode_size;
  uint32_t groups;
  uint32_t desc_size;
  uint64_t blocks;
  uint64_t first_data_block;
};

static void
read_at(struct reader *r, off_t off, void *buf, size_t n)
{
  if (pread(r->fd, buf, n, off) != (ssize_t)n)
    die("read");
}

static void
read_block(struct reader *r, uint64_t blk, void *buf)
{
  read_at(r, (off_t)blk * r->bs, buf, r->bs);
}

static void
open_reader(struct reader *r, const char *path)
{
  if ((r->fd = open(path, O_RDONLY)) < 0)
    die(path);
  read_at(r, EXT4_SUPER_OFFSET, &r->sb, sizeof(r->sb));
  if (r->sb.s_magic != EXT4_SUPER_MAGIC)
    fail("%s is not an ext4 image: magic is 0x%x", path, r->sb.s_magic);
  r->bs = 1024u << r->sb.s_log_block_size;
  if (r->bs != BLOCK_SIZE)
    fail("block size %u is not supported", r->bs);
  r->ipg = r->sb.s_inodes_per_group;
  r->inode_size = r->sb.s_inode_size;
  r->desc_size = r->sb.s_desc_size ? r->sb.s_desc_size : 32;
  r->blocks = (uint64_t)r->sb.s_blocks_count_lo | ((uint64_t)r->sb.s_blocks_count_hi << 32);
  r->first_data_block = r->sb.s_first_data_block;
  r->groups = (uint32_t)((r->blocks - r->first_data_block + r->sb.s_blocks_per_group - 1) /
                         r->sb.s_blocks_per_group);
}

// the descriptor table starts in the block after the superblock's
static void
read_group_desc(struct reader *r, uint32_t g, struct ext4_group_desc *gd)
{
  off_t off = (off_t)(r->first_data_block + 1) * r->bs + (off_t)g * r->desc_size;

  memset(gd, 0, sizeof(*gd));
  read_at(r, off, gd, r->desc_size < sizeof(*gd) ? r->desc_size : sizeof(*gd));
}

// inode n lives at index (n-1) mod inodes_per_group of the table of group
// (n-1) / inodes_per_group: the whole of the inode lookup
static void
read_inode(struct reader *r, uint32_t ino, struct ext4_inode *in)
{
  struct ext4_group_desc gd;
  uint32_t g = (ino - 1) / r->ipg;
  uint32_t idx = (ino - 1) % r->ipg;
  uint64_t table;

  read_group_desc(r, g, &gd);
  table = (uint64_t)gd.bg_inode_table_lo | ((uint64_t)gd.bg_inode_table_hi << 32);
  memset(in, 0, sizeof(*in));
  read_at(r, (off_t)table * r->bs + (off_t)idx * r->inode_size, in,
          r->inode_size < sizeof(*in) ? r->inode_size : sizeof(*in));
}

// find the physical block holding logical block lblk of a file, walking
// down the extent tree from the root in the inode
static uint64_t
extent_lookup(struct reader *r, const struct ext4_inode *in, uint32_t lblk)
{
  unsigned char node[BLOCK_SIZE];
  const unsigned char *p = (const unsigned char *)in->i_block;
  struct ext4_extent_header eh;
  int i;

  for (;;) {
    memcpy(&eh, p, sizeof(eh));
    if (eh.eh_magic != EXT4_EXT_MAGIC)
      fail("bad extent header 0x%x", eh.eh_magic);
    if (eh.eh_depth == 0) {
      for (i = 0; i < eh.eh_entries; i++) {
        struct ext4_extent e;
        uint32_t len;
        memcpy(&e, p + 12 + (size_t)i * 12, sizeof(e));
        // an ee_len above 32768 means an unwritten extent of ee_len -
        // 32768 blocks, allocated by fallocate but never written, which
        // reads as zeroes; 32768 itself is a full written extent, so the
        // top bit alone cannot be used to tell the two apart
        len = e.ee_len;
        if (len > EXT4_EXT_MAX_LEN)
          len -= EXT4_EXT_MAX_LEN;
        if (lblk >= e.ee_block && lblk < e.ee_block + len) {
          if (e.ee_len > EXT4_EXT_MAX_LEN)
            return 0;
          return ((uint64_t)e.ee_start_hi << 32 | e.ee_start_lo) + (lblk - e.ee_block);
        }
      }
      return 0; // a hole, or past the end
    }
    {
      uint64_t child = 0;
      for (i = 0; i < eh.eh_entries; i++) {
        struct ext4_extent_idx x;
        memcpy(&x, p + 12 + (size_t)i * 12, sizeof(x));
        if (x.ei_block <= lblk)
          child = (uint64_t)x.ei_leaf_hi << 32 | x.ei_leaf_lo;
      }
      if (child == 0)
        return 0;
      read_block(r, child, node);
      p = node;
    }
  }
}

static uint64_t
inode_size_of(const struct ext4_inode *in)
{
  return (uint64_t)in->i_size_lo | ((uint64_t)in->i_size_high << 32);
}

// read one block of a file into buf, zeroing it if the block is a hole
static void
read_file_block(struct reader *r, const struct ext4_inode *in, uint32_t lblk, unsigned char *buf)
{
  uint64_t blk = extent_lookup(r, in, lblk);

  if (blk == 0)
    memset(buf, 0, r->bs);
  else
    read_block(r, blk, buf);
}

// call fn for every entry of a directory, skipping the free slots and the
// checksum tail, which is recognisable by its impossible file type
static void
for_each_dirent(struct reader *r, const struct ext4_inode *in,
                void (*fn)(struct reader *, uint32_t, uint8_t, const char *, void *), void *arg)
{
  unsigned char buf[BLOCK_SIZE];
  uint64_t size = inode_size_of(in);
  uint32_t lblk, off;
  char name[EXT4_NAME_LEN + 1];

  for (lblk = 0; (uint64_t)lblk * r->bs < size; lblk++) {
    read_file_block(r, in, lblk, buf);
    off = 0;
    while (off + 8 <= r->bs) {
      struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(buf + off);
      if (de->rec_len < 8 || (de->rec_len & 3) || off + de->rec_len > r->bs)
        break;
      if (de->inode != 0 && de->file_type != EXT4_FT_DIR_CSUM && de->name_len > 0) {
        memcpy(name, de->name, de->name_len);
        name[de->name_len] = 0;
        fn(r, de->inode, de->file_type, name, arg);
      }
      off += de->rec_len;
    }
  }
}

struct listarg {
  char path[4096];
  size_t len;
};

static void list_entry(struct reader *r, uint32_t ino, uint8_t type, const char *name, void *arg);

static void
list_inode(struct reader *r, uint32_t ino, const char *path, uint8_t type)
{
  struct ext4_inode in;
  char target[BLOCK_SIZE];
  uint64_t size;

  read_inode(r, ino, &in);
  size = inode_size_of(&in);
  switch (type) {
  case EXT4_FT_DIR:
    printf("%-44s dir   %10llu  ino %u\n", path, (unsigned long long)size, ino);
    break;
  case EXT4_FT_SYMLINK:
    if (size < sizeof(in.i_block) && (in.i_flags & EXT4_EXTENTS_FL) == 0) {
      memcpy(target, in.i_block, (size_t)size);
    } else {
      read_file_block(r, &in, 0, (unsigned char *)target);
    }
    target[size] = 0;
    printf("%-44s sym   %10llu  ino %u -> %s\n", path, (unsigned long long)size, ino, target);
    break;
  case EXT4_FT_CHRDEV:
  case EXT4_FT_BLKDEV: {
    uint32_t dev = in.i_block[0];
    printf("%-44s %s   %5u:%-4u  ino %u\n", path, type == EXT4_FT_CHRDEV ? "chr" : "blk",
           dev >> 8, dev & 0xff, ino);
    break;
  }
  default:
    printf("%-44s file  %10llu  ino %u  mode %04o\n", path, (unsigned long long)size, ino,
           in.i_mode & 07777);
    break;
  }
  if (type == EXT4_FT_DIR) {
    struct listarg la;
    la.len = strlen(path);
    if (la.len >= sizeof(la.path) - EXT4_NAME_LEN - 2)
      fail("path is too deep to list");
    memcpy(la.path, path, la.len + 1);
    for_each_dirent(r, &in, list_entry, &la);
  }
}

static void
list_entry(struct reader *r, uint32_t ino, uint8_t type, const char *name, void *arg)
{
  struct listarg *la = arg;
  char path[4096];
  size_t n = la->len;

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return;
  if (n + strlen(name) + 2 > sizeof(path))
    fail("path is too long to list");
  memcpy(path, la->path, n);
  // the root's path is already a slash; every other one needs one added
  if (n != 1)
    path[n++] = '/';
  memcpy(path + n, name, strlen(name) + 1);
  list_inode(r, ino, path, type);
}

static void
list_image(const char *imgpath)
{
  struct reader r;

  memset(&r, 0, sizeof(r));
  open_reader(&r, imgpath);
  printf("%s: %llu blocks, %u inodes, %u groups, uuid ", imgpath,
         (unsigned long long)r.blocks, r.sb.s_inodes_count, r.groups);
  {
    int i;
    for (i = 0; i < 16; i++)
      printf("%02x%s", r.sb.s_uuid[i], (i == 3 || i == 5 || i == 7 || i == 9) ? "-" : "");
  }
  printf("\n");
  list_inode(&r, EXT4_ROOT_INO, "/", EXT4_FT_DIR);
  close(r.fd);
}

struct findarg {
  const char *want;
  uint32_t ino;
  uint8_t type;
};

static void
find_entry(struct reader *r, uint32_t ino, uint8_t type, const char *name, void *arg)
{
  struct findarg *fa = arg;

  (void)r;
  if (fa->ino == 0 && strcmp(name, fa->want) == 0) {
    fa->ino = ino;
    fa->type = type;
  }
}

// resolve a path the way a kernel would: one component at a time, from
// the root inode, reading each directory as it goes
static uint32_t
resolve(struct reader *r, const char *path, uint8_t *type)
{
  char buf[4096];
  char *p, *slash;
  uint32_t ino = EXT4_ROOT_INO;
  struct ext4_inode in;
  struct findarg fa;

  *type = EXT4_FT_DIR;
  if (strlen(path) + 1 > sizeof(buf))
    fail("path is too long");
  memcpy(buf, path, strlen(path) + 1);
  p = buf;
  while (*p == '/')
    p++;
  while (*p) {
    slash = strchr(p, '/');
    if (slash)
      *slash = 0;
    read_inode(r, ino, &in);
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR)
      fail("%s: not a directory", path);
    fa.want = p;
    fa.ino = 0;
    fa.type = 0;
    for_each_dirent(r, &in, find_entry, &fa);
    if (fa.ino == 0)
      fail("%s: no such file or directory", path);
    ino = fa.ino;
    *type = fa.type;
    if (slash == 0)
      break;
    p = slash + 1;
    while (*p == '/')
      p++;
  }
  return ino;
}

static void
extract_file(const char *imgpath, const char *path)
{
  struct reader r;
  struct ext4_inode in;
  unsigned char buf[BLOCK_SIZE];
  uint64_t size, off;
  uint32_t lblk, n;
  uint8_t type;
  uint32_t ino;

  memset(&r, 0, sizeof(r));
  open_reader(&r, imgpath);
  ino = resolve(&r, path, &type);
  read_inode(&r, ino, &in);
  if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG)
    fail("%s: not a regular file", path);
  size = inode_size_of(&in);
  for (off = 0, lblk = 0; off < size; off += n, lblk++) {
    n = (size - off > BLOCK_SIZE) ? BLOCK_SIZE : (uint32_t)(size - off);
    read_file_block(&r, &in, lblk, buf);
    if (fwrite(buf, 1, n, stdout) != n)
      die("write");
  }
  close(r.fd);
}

// ---------------------------------------------------------------------
// the program
// ---------------------------------------------------------------------

static void
usage(void)
{
  fprintf(stderr,
          "usage: mkext4 [-b BLOCKS] [-i BYTES_PER_INODE] [-L LABEL] [-U UUID]\n"
          "              [-j JOURNAL_BLOCKS] IMAGE [SPEC...]\n"
          "       mkext4 -l IMAGE\n"
          "       mkext4 -x IMAGE PATH\n"
          "\n"
          "each SPEC is one of\n"
          "  SOURCE                     the file at /BASENAME, without a leading underscore\n"
          "  TARGET:SOURCE              the file at TARGET, making the directories on the way\n"
          "  TARGET::dir                an empty directory\n"
          "  TARGET::sym:LINKTARGET     a symbolic link\n"
          "  TARGET::dev:c:MAJOR:MINOR  a device node, c for character or b for block\n");
  exit(1);
}

// a uuid the volume is identified by and, more importantly, the seed of
// every checksum in it. random unless the caller pinned it with -U, which
// is what a reproducible build wants.
static void
make_uuid(unsigned char *uuid)
{
  int fd = open("/dev/urandom", O_RDONLY);

  if (fd < 0 || read(fd, uuid, 16) != 16)
    die("/dev/urandom");
  close(fd);
  // version 4, variant 1, as RFC 4122 asks
  uuid[6] = (uuid[6] & 0x0f) | 0x40;
  uuid[8] = (uuid[8] & 0x3f) | 0x80;
}

static void
parse_uuid(const char *s, unsigned char *uuid)
{
  int i = 0;
  unsigned v;

  while (*s && i < 16) {
    if (*s == '-') {
      s++;
      continue;
    }
    if (sscanf(s, "%2x", &v) != 1)
      fail("bad uuid: %s", s);
    uuid[i++] = (unsigned char)v;
    s += 2;
  }
  if (i != 16 || *s != 0)
    fail("a uuid is 16 hex bytes");
}

int
main(int argc, char *argv[])
{
  struct image img;
  struct node *root, *lpf;
  uint64_t blocks = DEFAULT_BLOCKS;
  uint32_t bytes_per_inode = DEFAULT_BYTES_PER_INODE;
  uint32_t journal_blocks = 0;   // 0: chosen from the size, below
  const char *label = "";
  const char *uuidstr = 0;
  const char *epoch;
  int i, g;

  crc32c_init();
  memset(&img, 0, sizeof(img));

  if (argc >= 3 && strcmp(argv[1], "-l") == 0) {
    list_image(argv[2]);
    exit(0);
  }
  if (argc >= 4 && strcmp(argv[1], "-x") == 0) {
    extract_file(argv[2], argv[3]);
    exit(0);
  }

  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    if (i + 1 >= argc)
      usage();
    switch (argv[i][1]) {
    case 'b':
      blocks = strtoull(argv[++i], 0, 0);
      break;
    case 'i':
      bytes_per_inode = (uint32_t)strtoul(argv[++i], 0, 0);
      break;
    case 'L':
      label = argv[++i];
      break;
    case 'U':
      uuidstr = argv[++i];
      break;
    case 'j':
      journal_blocks = (uint32_t)strtoul(argv[++i], 0, 0);
      break;
    default:
      usage();
    }
  }
  if (i >= argc)
    usage();
  img.path = argv[i++];

  if (blocks < 4096)
    fail("an image of %llu blocks is too small to hold a file system",
         (unsigned long long)blocks);
  if (bytes_per_inode < BLOCK_SIZE)
    fail("bytes per inode must be at least the block size");
  // with no -j, the journal is sized from the image, as mke2fs sizes
  // it: a fixed 4096 blocks is more than a small image has to give.
  if (journal_blocks == 0) {
    if (blocks < 2048)
      journal_blocks = JBD2_MIN_JOURNAL_BLOCKS;
    else if (blocks < 32768)
      journal_blocks = 1024;
    else if (blocks < 262144)
      journal_blocks = 4096;
    else
      journal_blocks = 8192;
  }
  if (journal_blocks < JBD2_MIN_JOURNAL_BLOCKS)
    fail("a journal of fewer than %d blocks is refused by the kernel", JBD2_MIN_JOURNAL_BLOCKS);
  if (strlen(label) > 16)
    fail("a volume label is at most 16 characters");

  // one timestamp for the whole image, from the environment if the build
  // asked for a reproducible one
  epoch = getenv("SOURCE_DATE_EPOCH");
  img.mtime = epoch ? (uint32_t)strtoul(epoch, 0, 10) : (uint32_t)time(0);
  memcpy(img.label, label, strlen(label));
  if (uuidstr)
    parse_uuid(uuidstr, img.uuid);
  else
    make_uuid(img.uuid);
  // with the csum_seed feature off, this is how every reader derives the
  // seed, so it must be computed exactly this way
  img.csum_seed = crc32c(EXT4_CSUM_SEED_INIT, img.uuid, 16);
  for (g = 0; g < 4; g++)
    img.hash_seed[g] = crc32c(img.csum_seed, &g, sizeof(g));

  img.journal_blocks = journal_blocks;
  init_geometry(&img, blocks, bytes_per_inode);

  if ((img.fd = open(img.path, O_RDWR | O_CREAT | O_TRUNC, 0666)) < 0)
    die(img.path);
  // the image is a file of exactly the right length; the holes read back
  // as the zeroes that an unused block is supposed to be
  if (ftruncate(img.fd, (off_t)img.blocks * BLOCK_SIZE) < 0)
    die(img.path);

  reserve_group_metadata(&img);
  place_group_metadata(&img);
  img.alloc_hint = 0;

  // the reserved inodes come first and stay empty, except for the root
  // directory, the journal and lost+found
  img.next_ino = 1;
  for (g = 1; g < EXT4_FIRST_INO; g++)
    alloc_inode(&img);

  root = xmalloc(sizeof(*root));
  root->name = xstrdup("");
  root->type = NODE_DIR;
  root->mode = 0755;
  root->ino = EXT4_ROOT_INO;
  lpf = new_node(&img, root, "lost+found", NODE_DIR);
  lpf->mode = 0700;
  if (lpf->ino != EXT4_FIRST_INO)
    fail("lost+found must be inode %d, not %u", EXT4_FIRST_INO, lpf->ino);

  for (; i < argc; i++)
    add_spec(&img, root, argv[i]);

  // the resize inode and the journal go down first, so that they land in
  // one run near the front of the volume, as mke2fs places them
  write_resize_inode(&img);
  write_journal(&img);

  init_inode(&img, root->ino, EXT4_S_IFDIR | root->mode, 0);
  write_dir(&img, root, 0);
  init_inode(&img, lpf->ino, EXT4_S_IFDIR | lpf->mode, 0);
  // fsck needs somewhere to put what it finds even when the volume is
  // full, so lost+found is given its blocks up front
  write_dir(&img, lpf, 12);
  lpf->written = 1;
  write_tree(&img, root);

  // the allocator counts directories per group for the group descriptors
  for (g = 0; g < (int)img.inodes; g++) {
    struct ext4_inode *in = get_inode(&img, g + 1);
    if ((in->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR)
      img.used_dirs[g / (int)img.ipg]++;
  }

  write_metadata(&img);
  if (close(img.fd) < 0)
    die(img.path);
  exit(0);
}
