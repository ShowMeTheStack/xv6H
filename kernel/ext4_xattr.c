//
// extended attributes: a name and a value hung on an inode, outside
// its contents. ext4 keeps them in two places. the first is the room
// left over inside a 256-byte inode, past the fields the format
// defines, which is where a short attribute goes and costs nothing.
// the second is one block of its own, pointed at by i_file_acl, which
// is where the rest go.
//
// both places have the same shape: a header, then entries growing up
// from the front, then values growing down from the back, with the
// free space in the middle. rather than shuffle that in place, this
// file reads a region into a list, changes the list, and writes the
// region out again. a region is at most a block, the list is small,
// and a rewrite cannot leave a half-moved value behind.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "blk.h"
#include "errno.h"
#include "defs.h"
#include "ext4_ondisk.h"
#include "ext4.h"
#include "ext4_internal.h"
#include "xattr.h"

#define EXT4_XATTR_MAGIC 0xEA020000
#define EXT4_XATTR_PAD   4
#define EXT4_XATTR_ROUND (EXT4_XATTR_PAD - 1)
#define EXT4_XATTR_NAME_MAX 255

// one entry: the fixed part, then the name, the whole thing padded to
// four bytes. the value is elsewhere in the region, at e_value_offs.
struct ext4_xattr_entry {
  uint8 e_name_len;
  uint8 e_name_index;
  uint16 e_value_offs;
  uint32 e_value_inum; // an attribute whose value is its own inode; we write 0
  uint32 e_value_size;
  uint32 e_hash;
  char e_name[];
};

// the header a block carries; the one inside an inode is the magic alone
struct ext4_xattr_header {
  uint32 h_magic;
  uint32 h_refcount;
  uint32 h_blocks;
  uint32 h_hash;
  uint32 h_checksum;
  uint32 h_reserved[3];
};

_Static_assert(sizeof(struct ext4_xattr_entry) == 16, "xattr entry is not 16 bytes");
_Static_assert(sizeof(struct ext4_xattr_header) == 32, "xattr header is not 32 bytes");

#define XATTR_ENTRY_LEN(namelen) (((namelen) + EXT4_XATTR_ROUND + 16) & ~EXT4_XATTR_ROUND)
#define XATTR_VALUE_LEN(size) (((size) + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND)

// the last entry is four zero bytes, not an entry at all
static int
is_last(const struct ext4_xattr_entry *e)
{
  return *(const uint32 *)e == 0;
}

static struct ext4_xattr_entry *
next_entry(struct ext4_xattr_entry *e)
{
  return (struct ext4_xattr_entry *)((uchar *)e + XATTR_ENTRY_LEN(e->e_name_len));
}

// ---- names ----
//
// on the disk a name is a small number for its prefix and the rest of
// the name as text, so that "user." is one byte rather than five.
static const struct {
  int index;
  const char *prefix;
} xattr_prefixes[] = {
  {1, "user."},
  {2, "system.posix_acl_access"},
  {3, "system.posix_acl_default"},
  {4, "trusted."},
  {6, "security."},
  {7, "system."},
};

// split a full name into its prefix number and the rest
static int
name_split(const char *name, int *index, const char **rest)
{
  for (uint i = 0; i < sizeof(xattr_prefixes) / sizeof(xattr_prefixes[0]); i++) {
    const char *p = xattr_prefixes[i].prefix;
    int n = strlen(p);
    if (strncmp(name, p, n) == 0) {
      // the two whole-name forms have nothing after the prefix
      if (xattr_prefixes[i].index == 2 || xattr_prefixes[i].index == 3) {
        if (name[n] != 0)
          continue;
      }
      *index = xattr_prefixes[i].index;
      *rest = name + n;
      return 0;
    }
  }
  return -EOPNOTSUPP; // a name with no prefix ext4 knows is not one it stores
}

// put a name back together, into buf, and say how long it is
static int
name_join(int index, const char *name, int namelen, char *buf, int max)
{
  const char *prefix = 0;

  for (uint i = 0; i < sizeof(xattr_prefixes) / sizeof(xattr_prefixes[0]); i++)
    if (xattr_prefixes[i].index == index) {
      prefix = xattr_prefixes[i].prefix;
      break;
    }
  if (prefix == 0)
    return -1;
  int plen = strlen(prefix);
  if (plen + namelen + 1 > max)
    return -1;
  memmove(buf, prefix, plen);
  memmove(buf + plen, name, namelen);
  buf[plen + namelen] = 0;
  return plen + namelen;
}

// ---- the hashes and the checksum ----
//
// every entry in a block carries a hash of its name and value, and the
// block carries a hash of those, so that Linux can find a block worth
// sharing between two inodes. we never share one, but the numbers have
// to be the ones it would have written or e2fsck reports them.
#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16
#define BLOCK_HASH_SHIFT 16

static void
entry_hash(uchar *base, struct ext4_xattr_entry *e)
{
  uint32 hash = 0;
  const char *name = e->e_name;

  for (int n = 0; n < e->e_name_len; n++)
    hash = (hash << NAME_HASH_SHIFT) ^ (hash >> (32 - NAME_HASH_SHIFT)) ^ *name++;

  if (e->e_value_inum == 0 && e->e_value_size != 0) {
    uint32 *value = (uint32 *)(base + e->e_value_offs);
    for (int n = (e->e_value_size + EXT4_XATTR_ROUND) >> 2; n; n--)
      hash = (hash << VALUE_HASH_SHIFT) ^ (hash >> (32 - VALUE_HASH_SHIFT)) ^ *value++;
  }
  e->e_hash = hash;
}

static void
block_hash(struct ext4_xattr_header *h)
{
  uint32 hash = 0;
  struct ext4_xattr_entry *e;

  for (e = (struct ext4_xattr_entry *)(h + 1); !is_last(e); e = next_entry(e)) {
    if (e->e_hash == 0) {
      hash = 0;
      break;
    }
    hash = (hash << BLOCK_HASH_SHIFT) ^ (hash >> (32 - BLOCK_HASH_SHIFT)) ^ e->e_hash;
  }
  h->h_hash = hash;
}

// the block's checksum is seeded from the file system's uuid and the
// block number, not from the inode: the block may be shared.
static void
block_csum(struct ext4_fs *fs, uint64 blkno, void *block)
{
  struct ext4_xattr_header *h = (struct ext4_xattr_header *)block;
  uint32 zero = 0, csum;
  uint64 le_blkno = blkno;
  uint32 off = (uint32)__builtin_offsetof(struct ext4_xattr_header, h_checksum);

  if (!(fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM))
    return;
  csum = crc32c(fs->csum_seed, &le_blkno, sizeof(le_blkno));
  csum = crc32c(csum, h, off);
  csum = crc32c(csum, &zero, sizeof(zero));
  csum = crc32c(csum, (uchar *)block + off + sizeof(uint32), fs->blocksize - off - sizeof(uint32));
  h->h_checksum = csum;
}

// ---- a region, read into a list and written back out ----

#define XATTR_MAX_ITEMS 64

struct xa_item {
  uint8 index;
  uint8 namelen;
  uint32 vlen;
  const char *name;  // into the scratch copy
  const uchar *val;
};

struct xa_region {
  uchar *base;   // what e_value_offs counts from
  uchar *first;  // the first entry
  uchar *end;    // one past the last usable byte
};

// read a region's entries into the list. the caller's scratch holds a
// copy of the region, so that the list can point into it while the
// region itself is rewritten.
static int
region_decode(struct xa_region *r, uchar *copy, struct xa_item *items, int max)
{
  struct ext4_xattr_entry *e;
  uint64 firstoff = r->first - r->base;
  uint64 size = r->end - r->base;
  int n = 0;

  memmove(copy, r->base, size);
  for (e = (struct ext4_xattr_entry *)(copy + firstoff); !is_last(e); e = next_entry(e)) {
    // every entry has to lie inside the region, and so does its value
    if ((uchar *)e + 16 > copy + size)
      return -EUCLEAN;
    if (e->e_name_len == 0 || (uchar *)e + XATTR_ENTRY_LEN(e->e_name_len) > copy + size)
      return -EUCLEAN;
    if (e->e_value_inum != 0)
      return -EOPNOTSUPP; // a value in an inode of its own; we do not write them
    if ((uint64)e->e_value_offs + e->e_value_size > size)
      return -EUCLEAN;
    if (n >= max)
      return -ENOSPC;
    items[n].index = e->e_name_index;
    items[n].namelen = e->e_name_len;
    items[n].name = e->e_name;
    items[n].vlen = e->e_value_size;
    items[n].val = copy + e->e_value_offs;
    n++;
  }
  return n;
}

// the order Linux keeps entries in, and looks them up by
static int
item_cmp(uint8 ai, const char *an, int alen, uint8 bi, const char *bn, int blen)
{
  if (ai != bi)
    return (int)ai - (int)bi;
  if (alen != blen)
    return alen - blen;
  return strncmp(an, bn, alen);
}

// write the list back into the region, sorted, entries from the front
// and values from the back. -ENOSPC when it will not fit, and nothing
// is written in that case.
static int
region_encode(struct xa_region *r, struct xa_item *items, int n, int hash_entries)
{
  uint64 size = r->end - r->base;
  uint64 firstoff = r->first - r->base;
  uint64 voff = size;
  uchar *out;
  uchar *ep;
  int order[XATTR_MAX_ITEMS];

  // the space is checked against a scratch region and only then copied
  // over the real one, so a failure leaves what was there
  if ((out = kalloc()) == 0)
    return -ENOMEM;
  memset(out, 0, PGSIZE);

  for (int i = 0; i < n; i++)
    order[i] = i;
  for (int i = 1; i < n; i++) {
    int k = order[i], j = i - 1;
    while (j >= 0 &&
           item_cmp(items[order[j]].index, items[order[j]].name, items[order[j]].namelen,
                    items[k].index, items[k].name, items[k].namelen) > 0) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }

  ep = out + firstoff;
  for (int i = 0; i < n; i++) {
    struct xa_item *it = &items[order[i]];
    uint32 elen = XATTR_ENTRY_LEN(it->namelen);
    uint32 vlen = XATTR_VALUE_LEN(it->vlen);

    if (voff < vlen || (uint64)(ep - out) + elen + 4 > voff - vlen) {
      kfree(out);
      return -ENOSPC;
    }
    voff -= vlen;

    struct ext4_xattr_entry *e = (struct ext4_xattr_entry *)ep;
    e->e_name_len = it->namelen;
    e->e_name_index = it->index;
    e->e_value_offs = (uint16)voff;
    e->e_value_inum = 0;
    e->e_value_size = it->vlen;
    e->e_hash = 0;
    memmove(e->e_name, it->name, it->namelen);
    memmove(out + voff, it->val, it->vlen);
    if (hash_entries)
      entry_hash(out, e);
    ep += elen;
  }
  // the four zero bytes that end the entries are already there

  memmove(r->base + firstoff, out + firstoff, size - firstoff);
  kfree(out);
  return 0;
}

// ---- the two regions of one inode ----

// where the room inside the inode is, or 0 when there is none. the
// header is the magic word, and the entries start after it.
static int
ibody_region(struct ext4_fs *fs, struct ext4_inode *di, struct xa_region *r)
{
  uint32 extra;

  if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE)
    return -ENOSPC;
  extra = di->i_extra_isize;
  if (extra < 4 || extra > fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE)
    return -ENOSPC;
  uchar *hdr = (uchar *)di + EXT4_GOOD_OLD_INODE_SIZE + extra;
  uchar *end = (uchar *)di + fs->inode_size;
  if (hdr + 8 > end) // the magic and the terminator, at least
    return -ENOSPC;
  r->base = r->first = hdr + 4;
  r->end = end;
  return 0;
}

static int
ibody_present(struct ext4_inode *di, struct ext4_fs *fs)
{
  struct xa_region r;

  if (ibody_region(fs, di, &r) < 0)
    return 0;
  return *(uint32 *)(r.first - 4) == EXT4_XATTR_MAGIC;
}

static uint64
inode_acl_block(struct ext4_inode *di)
{
  return (uint64)di->i_file_acl_lo | ((uint64)di->l_i_file_acl_high << 32);
}

static void
set_acl_block(struct ext4_inode *di, uint64 blk)
{
  di->i_file_acl_lo = (uint32)blk;
  di->l_i_file_acl_high = (uint16)(blk >> 32);
}

// the compat bit that says this file system has attributes on it
static void
mark_ext_attr(struct ext4_fs *fs)
{
  struct blkbuf *b;
  struct ext4_super_block *sb;

  if (fs->feature_compat & EXT4_FEATURE_COMPAT_EXT_ATTR)
    return;
  if ((b = blk_get(fs->disk, 0)) == 0)
    return;
  ext4_journal_access(fs, b);
  sb = (struct ext4_super_block *)((uchar *)b->data + 1024);
  sb->s_feature_compat |= EXT4_FEATURE_COMPAT_EXT_ATTR;
  ext4_super_csum(fs, sb);
  ext4_journal_dirty(fs, b);
  blk_put(b);
  fs->feature_compat |= EXT4_FEATURE_COMPAT_EXT_ATTR;
}

// ---- reading ----

// look through one region for a name, copying the value out
static int
region_get(struct xa_region *r, int index, const char *name, int namelen, char *buf, int size)
{
  struct ext4_xattr_entry *e;
  uint64 rsize = r->end - r->base;

  for (e = (struct ext4_xattr_entry *)r->first; !is_last(e); e = next_entry(e)) {
    if ((uchar *)e + 16 > r->end || e->e_name_len == 0 ||
        (uchar *)e + XATTR_ENTRY_LEN(e->e_name_len) > r->end)
      return -EUCLEAN;
    if (e->e_name_index == index && e->e_name_len == namelen &&
        strncmp(e->e_name, name, namelen) == 0) {
      if (e->e_value_inum != 0)
        return -EOPNOTSUPP;
      if ((uint64)e->e_value_offs + e->e_value_size > rsize)
        return -EUCLEAN;
      if (size == 0)
        return (int)e->e_value_size; // asking how big it is
      if ((uint32)size < e->e_value_size)
        return -ERANGE;
      memmove(buf, r->base + e->e_value_offs, e->e_value_size);
      return (int)e->e_value_size;
    }
  }
  return -ENODATA;
}

static int
region_list(struct xa_region *r, char *buf, int size, int *used)
{
  struct ext4_xattr_entry *e;
  char full[EXT4_XATTR_NAME_MAX + 32];

  for (e = (struct ext4_xattr_entry *)r->first; !is_last(e); e = next_entry(e)) {
    if ((uchar *)e + 16 > r->end || e->e_name_len == 0 ||
        (uchar *)e + XATTR_ENTRY_LEN(e->e_name_len) > r->end)
      return -EUCLEAN;
    int n = name_join(e->e_name_index, e->e_name, e->e_name_len, full, sizeof(full));
    if (n < 0)
      continue; // a prefix we do not know: not ours to report
    if (size > 0) {
      if (*used + n + 1 > size)
        return -ERANGE;
      memmove(buf + *used, full, n + 1);
    }
    *used += n + 1;
  }
  return 0;
}

// the block an inode's attributes live in, checked before it is used
static struct blkbuf *
xattr_bread(struct ext4_fs *fs, uint64 blkno)
{
  struct blkbuf *b = ext4_bread(fs, blkno);
  struct ext4_xattr_header *h;

  if (b == 0)
    return 0;
  h = (struct ext4_xattr_header *)b->data;
  if (h->h_magic != EXT4_XATTR_MAGIC) {
    blk_put(b);
    return 0;
  }
  return b;
}

int
ext4_xattr_get(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, const char *fullname,
               char *buf, int size)
{
  struct ext4_inode *raw;
  struct blkbuf *ib, *bb;
  struct xa_region r;
  uint64 blk, acl;
  uint32 off;
  const char *name;
  int index, namelen, res;

  if ((res = name_split(fullname, &index, &name)) < 0)
    return res;
  namelen = strlen(name);
  if (namelen == 0 || namelen > EXT4_XATTR_NAME_MAX)
    return -ERANGE;

  if ((res = ext4_inode_place(fs, ino, &blk, &off)) < 0)
    return res;
  if ((ib = ext4_bread(fs, blk)) == 0)
    return -EIO;
  raw = (struct ext4_inode *)((uchar *)ib->data + off);

  res = -ENODATA;
  if (ibody_present(raw, fs) && ibody_region(fs, raw, &r) == 0)
    res = region_get(&r, index, name, namelen, buf, size);
  acl = inode_acl_block(raw);
  blk_put(ib);

  if (res == -ENODATA && acl != 0) {
    if ((bb = xattr_bread(fs, acl)) == 0)
      return -EUCLEAN;
    r.base = (uchar *)bb->data;
    r.first = (uchar *)bb->data + sizeof(struct ext4_xattr_header);
    r.end = (uchar *)bb->data + fs->blocksize;
    res = region_get(&r, index, name, namelen, buf, size);
    blk_put(bb);
  }
  (void)in;
  return res;
}

int
ext4_xattr_list(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, char *buf, int size)
{
  struct ext4_inode *raw;
  struct blkbuf *ib, *bb;
  struct xa_region r;
  uint64 blk, acl;
  uint32 off;
  int used = 0, res;

  if ((res = ext4_inode_place(fs, ino, &blk, &off)) < 0)
    return res;
  if ((ib = ext4_bread(fs, blk)) == 0)
    return -EIO;
  raw = (struct ext4_inode *)((uchar *)ib->data + off);
  res = 0;
  if (ibody_present(raw, fs) && ibody_region(fs, raw, &r) == 0)
    res = region_list(&r, buf, size, &used);
  acl = inode_acl_block(raw);
  blk_put(ib);
  if (res < 0)
    return res;

  if (acl != 0) {
    if ((bb = xattr_bread(fs, acl)) == 0)
      return -EUCLEAN;
    r.base = (uchar *)bb->data;
    r.first = (uchar *)bb->data + sizeof(struct ext4_xattr_header);
    r.end = (uchar *)bb->data + fs->blocksize;
    res = region_list(&r, buf, size, &used);
    blk_put(bb);
    if (res < 0)
      return res;
  }
  (void)in;
  return used;
}

// ---- writing ----

// apply one change to a decoded list. returns the new count, or a
// negative error. value == 0 removes the name.
static int
list_apply(struct xa_item *items, int n, int max, int index, const char *name, int namelen,
           const uchar *value, int vlen, int *found)
{
  *found = 0;
  for (int i = 0; i < n; i++)
    if (items[i].index == index && items[i].namelen == namelen &&
        strncmp(items[i].name, name, namelen) == 0) {
      *found = 1;
      if (value == 0) {
        for (int j = i; j + 1 < n; j++)
          items[j] = items[j + 1];
        return n - 1;
      }
      items[i].val = value;
      items[i].vlen = vlen;
      return n;
    }
  if (value == 0)
    return n; // not there; the caller decides whether that is an error
  if (n >= max)
    return -ENOSPC;
  items[n].index = index;
  items[n].namelen = namelen;
  items[n].name = name;
  items[n].val = value;
  items[n].vlen = vlen;
  return n + 1;
}

// write a region that has been emptied: inside the inode that means
// clearing the magic, so Linux does not look for entries that are gone.
static void
ibody_clear(struct xa_region *r)
{
  memset(r->first - 4, 0, (r->end - r->first) + 4);
}

// the block for an inode's attributes: the one it has, or a new one.
// returns the block number, with *b holding it, journalled.
static int
block_for(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 acl, uint64 *out,
          struct blkbuf **bout)
{
  struct blkbuf *b;
  struct ext4_xattr_header *h;
  uint64 blk;
  int r;

  if (acl != 0) {
    if ((b = xattr_bread(fs, acl)) == 0)
      return -EUCLEAN;
    ext4_journal_access(fs, b);
    *out = acl;
    *bout = b;
    return 0;
  }
  if ((r = ext4_alloc_block(fs, 0, &blk)) < 0)
    return r;
  if ((b = blk_get_fill(fs->disk, blk)) == 0) {
    ext4_free_block(fs, blk);
    return -EIO;
  }
  ext4_journal_access(fs, b);
  memset(b->data, 0, fs->blocksize);
  h = (struct ext4_xattr_header *)b->data;
  h->h_magic = EXT4_XATTR_MAGIC;
  h->h_refcount = 1;
  h->h_blocks = 1;
  in->blocks += fs->blocksize / 512;
  *out = blk;
  *bout = b;
  return 0;
}

// set, replace or remove one attribute. flags are XATTR_CREATE and
// XATTR_REPLACE, which say what must already be true of the name.
int
ext4_xattr_set(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, const char *fullname,
               const char *value, int vlen, int flags)
{
  struct ext4_inode *raw;
  struct blkbuf *ib = 0, *bb = 0;
  struct xa_region ir, br;
  struct xa_item items[XATTR_MAX_ITEMS];
  uchar *copy = 0;
  uint64 blk, acl, newacl;
  uint32 off;
  const char *name;
  int index, namelen, res, n, found_i = 0, found_b = 0, have_ibody;

  if (fs->readonly)
    return -EROFS;
  if ((res = name_split(fullname, &index, &name)) < 0)
    return res;
  namelen = strlen(name);
  if (namelen == 0 || namelen > EXT4_XATTR_NAME_MAX)
    return -ERANGE;
  if (value != 0 && (vlen < 0 || (uint32)vlen > fs->blocksize - 64))
    return -E2BIG;

  if ((res = ext4_inode_place(fs, ino, &blk, &off)) < 0)
    return res;
  if ((copy = kalloc()) == 0)
    return -ENOMEM;
  if ((ib = ext4_bread(fs, blk)) == 0) {
    kfree(copy);
    return -EIO;
  }
  raw = (struct ext4_inode *)((uchar *)ib->data + off);
  acl = inode_acl_block(raw);
  have_ibody = ibody_region(fs, raw, &ir) == 0;

  // is the name already there, and where? the answer decides what the
  // flags allow and which region has to be rewritten.
  {
    char probe[1];
    if (have_ibody && ibody_present(raw, fs))
      found_i = region_get(&ir, index, name, namelen, probe, 0) >= 0;
  }
  if (!found_i && acl != 0) {
    if ((bb = xattr_bread(fs, acl)) == 0) {
      res = -EUCLEAN;
      goto out;
    }
    br.base = (uchar *)bb->data;
    br.first = (uchar *)bb->data + sizeof(struct ext4_xattr_header);
    br.end = (uchar *)bb->data + fs->blocksize;
    char probe[1];
    found_b = region_get(&br, index, name, namelen, probe, 0) >= 0;
    blk_put(bb);
    bb = 0;
  }

  if ((flags & XATTR_CREATE) && (found_i || found_b)) {
    res = -EEXIST;
    goto out;
  }
  if ((flags & XATTR_REPLACE) && !(found_i || found_b)) {
    res = -ENODATA;
    goto out;
  }
  if (value == 0 && !(found_i || found_b)) {
    res = -ENODATA;
    goto out;
  }

  // the room inside the inode comes first: it costs no block and no
  // second read. a value that does not fit there goes to a block.
  if (have_ibody && (found_i || value != 0)) {
    ext4_journal_access(fs, ib);
    if (!ibody_present(raw, fs)) {
      n = 0;
    } else if ((n = region_decode(&ir, copy, items, XATTR_MAX_ITEMS)) < 0) {
      res = n;
      goto out;
    }
    int had;
    n = list_apply(items, n, XATTR_MAX_ITEMS, index, name, namelen, (const uchar *)value, vlen,
                   &had);
    if (n >= 0) {
      if (n == 0) {
        ibody_clear(&ir);
        res = 0;
      } else {
        *(uint32 *)(ir.first - 4) = EXT4_XATTR_MAGIC;
        res = region_encode(&ir, items, n, 0);
        if (res < 0)
          *(uint32 *)(ir.first - 4) = ibody_present(raw, fs) ? EXT4_XATTR_MAGIC : 0;
      }
    } else {
      res = n;
    }
    if (res == 0) {
      ext4_journal_dirty(fs, ib);
      // it went inside the inode, so any older copy in the block goes
      if (found_b)
        goto drop_from_block;
      goto done;
    }
    if (res != -ENOSPC)
      goto out;
    // it does not fit inside the inode. if the old value was in there,
    // it has to come out before the new one goes in the block.
    if (found_i) {
      if ((n = region_decode(&ir, copy, items, XATTR_MAX_ITEMS)) < 0) {
        res = n;
        goto out;
      }
      int had;
      n = list_apply(items, n, XATTR_MAX_ITEMS, index, name, namelen, 0, 0, &had);
      if (n == 0)
        ibody_clear(&ir);
      else if ((res = region_encode(&ir, items, n, 0)) < 0)
        goto out;
      ext4_journal_dirty(fs, ib);
      found_i = 0;
    }
  }

  if (value == 0) {
  drop_from_block:
    // removing: the block loses the name, and goes away when empty
    if (acl == 0) {
      res = 0;
      goto done;
    }
    if ((bb = xattr_bread(fs, acl)) == 0) {
      res = -EUCLEAN;
      goto out;
    }
    ext4_journal_access(fs, bb);
    br.base = (uchar *)bb->data;
    br.first = (uchar *)bb->data + sizeof(struct ext4_xattr_header);
    br.end = (uchar *)bb->data + fs->blocksize;
    if ((n = region_decode(&br, copy, items, XATTR_MAX_ITEMS)) < 0) {
      res = n;
      goto out;
    }
    int had;
    n = list_apply(items, n, XATTR_MAX_ITEMS, index, name, namelen, 0, 0, &had);
    if (n == 0) {
      blk_put(bb);
      bb = 0;
      ext4_free_block(fs, acl);
      if (in->blocks >= fs->blocksize / 512)
        in->blocks -= fs->blocksize / 512;
      set_acl_block((struct ext4_inode *)in->raw, 0);
      res = 0;
      goto done;
    }
    if ((res = region_encode(&br, items, n, 1)) < 0)
      goto out;
    block_hash((struct ext4_xattr_header *)bb->data);
    block_csum(fs, acl, bb->data);
    ext4_journal_dirty(fs, bb);
    res = 0;
    goto done;
  }

  // the value goes in the block: the one there or a new one
  if ((res = block_for(fs, in, acl, &newacl, &bb)) < 0)
    goto out;
  br.base = (uchar *)bb->data;
  br.first = (uchar *)bb->data + sizeof(struct ext4_xattr_header);
  br.end = (uchar *)bb->data + fs->blocksize;
  if ((n = region_decode(&br, copy, items, XATTR_MAX_ITEMS)) < 0) {
    res = n;
    goto out;
  }
  {
    int had;
    n = list_apply(items, n, XATTR_MAX_ITEMS, index, name, namelen, (const uchar *)value, vlen,
                   &had);
  }
  if (n < 0) {
    res = n;
    goto out;
  }
  if ((res = region_encode(&br, items, n, 1)) < 0) {
    if (acl == 0) { // a block allocated for a value that will not fit
      blk_put(bb);
      bb = 0;
      ext4_free_block(fs, newacl);
      in->blocks -= fs->blocksize / 512;
    }
    goto out;
  }
  block_hash((struct ext4_xattr_header *)bb->data);
  block_csum(fs, newacl, bb->data);
  ext4_journal_dirty(fs, bb);
  set_acl_block((struct ext4_inode *)in->raw, newacl);
  mark_ext_attr(fs);
  res = 0;

done:
  // the inode itself: i_file_acl may have changed, and the times have
  ext4_touch(in, 0);
  if (ib) {
    blk_put(ib);
    ib = 0;
  }
  if (bb) {
    blk_put(bb);
    bb = 0;
  }
  if (copy)
    kfree(copy);
  ext4_write_inode(fs, ino, in);
  return 0;

out:
  if (ib)
    blk_put(ib);
  if (bb)
    blk_put(bb);
  if (copy)
    kfree(copy);
  return res;
}

// the inode is gone: its attribute block goes with it. this reads the
// inode from the disk rather than taking a copy, because the only
// caller is the one that frees an inode nobody holds any more.
void
ext4_xattr_drop(struct ext4_fs *fs, uint64 ino)
{
  struct ext4_inode *raw;
  struct blkbuf *ib;
  uint64 blk, acl;
  uint32 off;

  if (fs->readonly)
    return;
  if (ext4_inode_place(fs, ino, &blk, &off) < 0)
    return;
  if ((ib = ext4_bread(fs, blk)) == 0)
    return;
  raw = (struct ext4_inode *)((uchar *)ib->data + off);
  acl = inode_acl_block(raw);
  if (acl == 0) {
    blk_put(ib);
    return;
  }
  ext4_journal_access(fs, ib);
  set_acl_block(raw, 0);
  ext4_inode_csum(fs, ino, raw);
  ext4_journal_dirty(fs, ib);
  blk_put(ib);
  ext4_free_block(fs, acl);
}
