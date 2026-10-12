#ifndef XV6_JBD2_H
#define XV6_JBD2_H
//
// jbd2: the journal ext4 keeps, in the format Linux 6.12 writes.
//
// the journal is a file whose blocks the file system knows how to find.
// block 0 of that file is the journal superblock; blocks s_first ..
// s_maxlen-1 are a circular log of transactions. one transaction on the
// log is a descriptor block naming the blocks that follow, then those
// blocks, then any revoke blocks, then a commit block. a transaction
// whose commit block is on the disk and verifies is one that must be
// replayed after a crash; anything after the last such commit block is
// torn and is thrown away.
//
// this header carries the on-disk format and the interface. the format
// is here rather than in ext4_ondisk.h because that header describes the
// file system and this one describes the log; the field names are the
// ones include/linux/jbd2.h uses, so the two can be read side by side.
//
// every field on the journal's disk is BIG-endian -- jbd2 was written for
// a portable journal and never changed -- while ext4's own structures are
// little-endian. the structures below therefore store plain uint32 and
// every access goes through jbd2_be32()/jbd2_cpu32() in jbd2.c. nothing
// in this format has implicit padding, so the structures are not packed
// and the _Static_asserts in jbd2.c prove the layout.
//
#include "types.h"

struct gendisk;
struct blkbuf;
struct jbd2;
struct jbd2_handle;

// ---- the on-disk format ----

#define JBD2_MAGIC_NUMBER 0xc03b3998

// h_blocktype
#define JBD2_DESCRIPTOR_BLOCK 1 // a list of tags naming the blocks that follow
#define JBD2_COMMIT_BLOCK     2 // the transaction before it is now durable
#define JBD2_SUPERBLOCK_V1    3 // the original journal superblock
#define JBD2_SUPERBLOCK_V2    4 // adds the feature words; what everyone writes
#define JBD2_REVOKE_BLOCK     5 // blocks that must not be replayed

// the first twelve bytes of every block on the log
typedef struct journal_header_s {
  uint32 h_magic;     // JBD2_MAGIC_NUMBER
  uint32 h_blocktype; // one of the block types above
  uint32 h_sequence;  // transaction this block belongs to
} journal_header_t;

// tag flags in a descriptor block
#define JBD2_FLAG_ESCAPE    1 // the data block began with the jbd2 magic and was escaped
#define JBD2_FLAG_SAME_UUID 2 // this tag reuses the previous tag's uuid, which is then absent
#define JBD2_FLAG_DELETED   4
#define JBD2_FLAG_LAST_TAG  8 // last tag in this descriptor block

// the tag shape used when csum_v3 is set: a flat structure with the
// checksum and the full 64-bit block number, rather than the packed 8- or
// 12-byte tag of the older formats. the two shapes agree on t_blocknr at
// offset 0, on t_blocknr_high at offset 8, and on the low half of t_flags
// at offset 6, which is why Linux can write both through one pointer.
typedef struct journal_block_tag3_s {
  uint32 t_blocknr;      // file system block this journalled block belongs to, low 32 bits
  uint32 t_flags;        // JBD2_FLAG_*
  uint32 t_blocknr_high; // high 32 bits of that block number
  uint32 t_checksum;     // crc32c(journal uuid seed, sequence, the data block as logged)
} journal_block_tag3_t;

// the older tag, which this driver reads but does not write
typedef struct journal_block_tag_s {
  uint32 t_blocknr;
  uint16 t_checksum;     // only with csum_v2
  uint16 t_flags;
  uint32 t_blocknr_high; // only with the 64bit feature
} journal_block_tag_t;

// the last four bytes of a descriptor or revoke block once csum_v2 or
// csum_v3 is set. it covers the whole block with this field zeroed.
struct jbd2_journal_block_tail {
  uint32 t_checksum;
};

// a revoke block: the header, then r_count minus sizeof(header) bytes of
// block numbers, 64-bit ones when the 64bit feature is set.
typedef struct jbd2_journal_revoke_header_s {
  journal_header_t r_header;
  uint32 r_count;        // bytes of this block in use, the header included
} jbd2_journal_revoke_header_t;

#define JBD2_CHECKSUM_BYTES (32 / sizeof(uint32))

// the commit block. its checksum lives in h_chksum[0] rather than in a
// tail, and covers the whole block with h_chksum_type, h_chksum_size and
// h_chksum[0] zeroed.
struct commit_header {
  uint32 h_magic;
  uint32 h_blocktype;
  uint32 h_sequence;
  uchar  h_chksum_type;
  uchar  h_chksum_size;
  uchar  h_padding[2];
  uint32 h_chksum[JBD2_CHECKSUM_BYTES];
  uint64 h_commit_sec;
  uint32 h_commit_nsec;
};

struct journal_superblock_s {
  journal_header_t s_header; // magic, JBD2_SUPERBLOCK_V2, and 0
  uint32 s_blocksize;        // journal block size, matching the file system's
  uint32 s_maxlen;           // total blocks in the journal, including this one
  uint32 s_first;            // first block of the log proper; 1
  uint32 s_sequence;         // sequence of the transaction expected at s_start
  uint32 s_start;            // block the log starts at; 0 means the journal is empty
  uint32 s_errno;            // error that stopped the journal
  uint32 s_feature_compat;   // JBD2_FEATURE_COMPAT_*
  uint32 s_feature_incompat; // JBD2_FEATURE_INCOMPAT_*
  uint32 s_feature_ro_compat;
  uchar  s_uuid[16];         // uuid of the file system this journal belongs to
  uint32 s_nr_users;         // file systems sharing this journal; 1
  uint32 s_dynsuper;         // block of the dynamic superblock copy
  uint32 s_max_transaction;  // limit on blocks per transaction
  uint32 s_max_trans_data;   // limit on data blocks per transaction
  uchar  s_checksum_type;    // JBD2_CRC32C_CHKSUM when the csum features are set
  uchar  s_padding2[3];
  uint32 s_num_fc_blks;      // fast commit blocks at the end of the journal
  uint32 s_head;             // oldest block the log still needs
  uint32 s_padding[40];
  uint32 s_checksum;         // crc32c(~0, this 1024-byte structure with the field zeroed)
  uchar  s_users[16 * 48];   // uuids of the file systems sharing the journal
};

typedef struct journal_superblock_s journal_superblock_t;

#define JBD2_FEATURE_COMPAT_CHECKSUM 0x00000001

#define JBD2_FEATURE_INCOMPAT_REVOKE       0x00000001 // revoke blocks may appear
#define JBD2_FEATURE_INCOMPAT_64BIT        0x00000002 // tags carry 64-bit block numbers
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT 0x00000004
#define JBD2_FEATURE_INCOMPAT_CSUM_V2      0x00000008 // crc32c tags, packed tag layout
#define JBD2_FEATURE_INCOMPAT_CSUM_V3      0x00000010 // crc32c tags, journal_block_tag3_t
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT  0x00000020

// s_checksum_type
#define JBD2_CRC32_CHKSUM  1
#define JBD2_MD5_CHKSUM    2
#define JBD2_SHA1_CHKSUM   3
#define JBD2_CRC32C_CHKSUM 4

// the features this journal writes: revoke blocks, 64-bit block numbers
// in the tags, and the csum_v3 tag shape with crc32c everywhere.
#define JBD2_FEATURES_WRITTEN \
  (JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT | \
   JBD2_FEATURE_INCOMPAT_CSUM_V3)

// ---- limits, which are ours and not the format's ----

#define JBD2_MAX_TXN_BLOCKS 512    // metadata blocks one transaction may hold
#define JBD2_MAX_REVOKES    1024   // blocks one transaction may revoke
#define JBD2_MIN_JOURNAL_BLOCKS 16 // smaller than this cannot hold a transaction
#define JBD2_COMMIT_SECS    5      // the commit thread's interval

// ---- the interface the file system calls ----

// open the journal. `map` turns a journal block number (0 is the journal
// superblock) into a disk block number, because the journal lives in a
// file whose blocks the file system knows how to find. returns null when
// the journal superblock is missing, malformed or in a format this
// driver will not touch, and says on the console which it was.
struct jbd2 *jbd2_open(struct gendisk *disk, uint64 (*map)(void *priv, uint64 jblk), void *priv,
                       uint64 nblocks, const unsigned char uuid[16]);
void jbd2_close(struct jbd2 *j);

// replay whatever a previous run left behind. called at mount, before
// anything else reads the file system. returns the number of blocks
// replayed, or a negative error. no transaction may be started until
// this has returned.
int jbd2_recover(struct jbd2 *j);

// a transaction. `credits` is the most blocks this handle will dirty;
// the journal refuses to start one it cannot fit.
struct jbd2_handle *jbd2_start(struct jbd2 *j, int credits);
int jbd2_stop(struct jbd2_handle *h);

// before a block is modified, and after
int jbd2_get_write_access(struct jbd2_handle *h, struct blkbuf *b);
int jbd2_dirty_metadata(struct jbd2_handle *h, struct blkbuf *b);

// this block is being freed: anything the log says about it must not be
// replayed over what comes next
int jbd2_revoke(struct jbd2_handle *h, uint64 blkno);

// write the current transaction out and wait for it. this is what fsync
// and sync call. it must not be called while the caller holds a handle.
int jbd2_commit(struct jbd2 *j);

// how full the journal is, for the file system's own decisions
void jbd2_stats(struct jbd2 *j, uint64 *transactions, uint64 *blocks_written, int *free_blocks);

// whether this block is in a transaction that is not yet on the log, and
// so must not be written to its home location. the writeback path asks
// this without holding any journal lock.
int jbd2_is_journaled(struct jbd2 *j, struct blkbuf *b);

// whether file data is written to its home blocks before each commit
void jbd2_set_ordered(struct jbd2 *j, int on);

#endif
