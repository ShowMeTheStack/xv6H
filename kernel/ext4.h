#ifndef XV6_EXT4_H
#define XV6_EXT4_H
//
// the ext4 driver's interface to the rest of the kernel.
//
// a mount is one struct ext4_fs, which holds what the superblock said and
// nothing else: no inode cache, no dirty state, no lock. the block cache
// underneath already serializes access to a block, and this driver only
// reads, so there is nothing here for two callers to race over. the write
// side will have to add a lock and a journal handle to this structure, and
// the fields it needs are left with the names Linux gives them so that the
// addition is mechanical.
//
// everything in here is read-only for now. ext4_mount accepts a
// read-write request and reports that it mounted read-only instead.
//
#include "types.h"
#include "ext4_ondisk.h"

struct gendisk;
struct jbd2;
struct jbd2_handle;

// one mounted file system. the on-disk numbers are kept in host form here
// so nothing above has to know where they came from or that the 64-bit
// ones arrive in two halves.
struct ext4_fs {
  struct gendisk *disk;
  int readonly;                  // always 1 in this revision

  uint32 blocksize;              // 4096, which is what the block layer uses
  uint32 blockbits;              // 12
  uint64 blocks_count;
  uint64 free_blocks;
  uint32 inodes_count;
  uint32 free_inodes;
  uint32 inodes_per_group;
  uint32 blocks_per_group;
  uint32 inodes_per_block;
  uint32 inode_size;
  uint32 first_ino;
  uint32 first_data_block;
  uint32 ngroups;
  uint32 desc_size;              // 32 or 64
  uint64 gdt_block;              // the first block of the group descriptors
  uint32 descs_per_block;

  uint32 feature_compat;
  uint32 feature_incompat;
  uint32 feature_ro_compat;

  uint32 hash_version;           // one of the DX_HASH_* values, sign resolved
  int hash_unsigned;             // whether a name byte widens as unsigned
  uint32 hash_seed[4];

  uchar uuid[16];
  uint32 csum_seed;              // for the write side; nothing verifies it yet
  int has_metadata_csum;

  // the journal, and the transaction the operation in progress runs in.
  // fs->handle is set by ext4_trans_begin and cleared by ext4_trans_end,
  // which hold fs->lock across the whole operation, so there is exactly
  // one of them at a time on one mount. a mount with no journal leaves
  // both null and every write goes straight to the block cache.
  // how file data is ordered against the metadata that names it.
  // writeback leaves it to the writeback thread, which is fastest and
  // says nothing about what a crash leaves behind. ordered writes the
  // data home before each commit, so the metadata is never newer than
  // the blocks it points at. journal puts the data on the log as well,
  // which makes a write atomic and costs writing everything twice.
  int data_mode;
  uint32 journal_inum;
  struct jbd2 *journal;
  struct jbd2_handle *handle;
  struct sleeplock lock;
};

// an inode, read into memory
struct ext4_inode_info {
  uint64 ino;
  uint32 mode;          // the mode field, with the format bits
  uint32 uid, gid;
  uint64 size;
  uint32 nlink;
  uint64 blocks;        // 512-byte units, as ext4 counts them
  uint64 atime, mtime, ctime, crtime;   // seconds
  uint32 flags;
  uint32 generation;
  unsigned char raw[160];  // the on-disk inode, for the fields above and the extent root

  // readahead: the file block a sequential reader is expected to ask
  // for next, and how far ahead to read when it does. the window grows
  // while the guess keeps being right and collapses when it is wrong,
  // so a program that seeks pays nothing for one that does not.
  uint64 ra_next;
  uint32 ra_window;
};

// what ext4_bmap returns on success. a hole and an unwritten extent both
// read as zeros, but they are not the same thing on disk: a hole has no
// block at all, an unwritten extent has one that fallocate reserved and
// nobody has written yet. a reader may treat them alike; the write side
// has to tell them apart, so the mapping says which it found.
#define EXT4_MAP_MAPPED     0   // *diskblk is real data
#define EXT4_MAP_HOLE       0   // ... and is 0, meaning nothing is mapped
#define EXT4_MAP_UNWRITTEN  1   // *diskblk is allocated but reads as zeros

struct ext4_fs *ext4_mount(struct gendisk *disk, int readonly);
void ext4_unmount(struct ext4_fs *fs);
uint64 ext4_root_ino(struct ext4_fs *fs);
void ext4_statfs(struct ext4_fs *fs, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
                 uint64 *bsize);

int ext4_read_inode(struct ext4_fs *fs, uint64 ino, struct ext4_inode_info *out);

// the disk block that holds file block `fileblk`, or 0 for a hole
int ext4_bmap(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 fileblk, uint64 *diskblk);
// the same, and how many file blocks from there run contiguously on the
// disk: what an extent knows and a caller walking a file wants.
int ext4_bmap_run(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 fileblk, uint64 *diskblk,
                  uint64 *run);

// read ahead of a sequential reader: called after a read of the file
// blocks [first, last], it works out whether the reader is walking the
// file and, if it is, fetches what comes next.
void ext4_readahead(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 first, uint64 last);

// bytes from a file, as read(2) would give them. `dst` is a user
// address when `user` is set and a kernel one when it is not.
int ext4_read_file(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 off, int user,
                   uint64 dst, uint64 n);

// one name in a directory: 0 when found, -ENOENT when not
int ext4_lookup(struct ext4_fs *fs, struct ext4_inode_info *dir, const char *name, int namelen,
                uint64 *ino_out, unsigned char *type_out);

// walk a directory: *pos starts at 0 and is carried between calls.
// returns 1 when an entry was filled in, 0 at the end, negative on error.
int ext4_readdir(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 *pos, char *name,
                 int namemax, uint64 *ino, unsigned char *type);

// the target of a symbolic link, into `buf`; returns its length
int ext4_readlink(struct ext4_fs *fs, struct ext4_inode_info *in, char *buf, int max);

// ---- the write side (ext4_write.c) ----

// write an inode's fields back, with its checksum
int ext4_write_inode(struct ext4_fs *fs, uint64 ino, struct ext4_inode_info *in);
// stamp the times on an in-memory inode
void ext4_touch(struct ext4_inode_info *in, int mtime);
// the disk block of a file block, allocating it and the tree around it
int ext4_bmap_alloc(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 fileblk,
                    uint64 *out);
// the same, for a caller that knows how many blocks it is about to
// write: the run comes back in one piece, in one extent, with one pass
// over the bitmap. *got is how many blocks of the `want` it got.
int ext4_bmap_alloc_run(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 fileblk,
                        uint32 want, int prefill, uint64 *out, uint32 *got);
// bytes into a file, growing it as needed. `src` is a user address when
// `user` is set. a copy that faults part way leaves the bytes it
// managed to read in the file and reports how many of them there were.
int ext4_write_file(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 off,
                    int user, uint64 src, uint64 n);
// free everything a file holds
int ext4_truncate(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, uint64 newsize);
// directory entries
int ext4_add_entry(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                   const char *name, int namelen, uint64 ino, uchar type);
int ext4_remove_entry(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                      const char *name, int namelen, uint64 *ino_out);
// a new inode, and a new directory with its two entries
// a device node: its number lives where its blocks would
int ext4_set_device(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino, int major,
                    int minor);
void ext4_device_of(struct ext4_inode_info *in, int *major, int *minor);
// a symbolic link's target, in the inode when it is short enough
int ext4_set_symlink(struct ext4_fs *fs, struct ext4_inode_info *in, uint64 ino,
                     const char *target, int len);
int ext4_make_inode(struct ext4_fs *fs, uint64 dirino, uint16 mode, uint64 *ino_out,
                    struct ext4_inode_info *out);
int ext4_make_dir(struct ext4_fs *fs, struct ext4_inode_info *parent, uint64 parentino, uint64 ino,
                  struct ext4_inode_info *in);
int ext4_dir_empty(struct ext4_fs *fs, struct ext4_inode_info *dir);
int ext4_free_inode(struct ext4_fs *fs, uint64 ino, int is_dir);

// the directory hash, exposed because htree readdir in hash order and the
// write side's leaf splitting both need it. `name` need not be terminated.
uint32 ext4_dirhash(struct ext4_fs *fs, const char *name, int len, uint32 *minor_out);

// ---- the journal (ext4_journal.c) ----

// find the journal, replay whatever a previous run left on it, and open
// it for this mount. a file system without a journal, or one mounted
// read-only, is not an error: the journal stays null and every write
// goes straight to the block cache. returns a negative error only when
// there is a journal that cannot be trusted, in which case the caller
// must not mount read-write.
#define EXT4_DATA_WRITEBACK 0
#define EXT4_DATA_ORDERED   1
#define EXT4_DATA_JOURNAL   2

int ext4_journal_open(struct ext4_fs *fs);
void ext4_journal_close(struct ext4_fs *fs);
int ext4_journal_commit(struct ext4_fs *fs);

// one file system operation, as one transaction. `credits` is the most
// metadata blocks it will dirty; ask for the worst case. these nest
// nowhere: an operation begins one and ends it before it returns.
int ext4_trans_begin(struct ext4_fs *fs, int credits);
void ext4_trans_end(struct ext4_fs *fs);

// a block of file data, once it has been written into. what this does
// depends on the mount's data mode: on the log in data=journal, and
// marked dirty for the writeback thread otherwise. in data=journal the
// caller must have called ext4_journal_data_access first.
void ext4_journal_data(struct ext4_fs *fs, struct blkbuf *b);
int ext4_journal_data_access(struct ext4_fs *fs, struct blkbuf *b);

// a metadata block, before it is modified and after. without a journal
// these are nothing and blk_mark_dirty, which is what an unjournalled
// mount did before there was one.
int ext4_journal_access(struct ext4_fs *fs, struct blkbuf *b);
void ext4_journal_dirty(struct ext4_fs *fs, struct blkbuf *b);
// a metadata block that has just been freed must not be replayed over
// whatever is put in its place
void ext4_journal_revoke(struct ext4_fs *fs, uint64 blkno);

// the two helpers the write side shares with the read side. ext4_bread is
// blk_get with the block number checked against the superblock's own count
// first, so that no caller has to remember to; ext4_group_desc copies one
// group descriptor out of the table, widened to the 64-byte form whatever
// the image stores.
struct blkbuf;
struct blkbuf *ext4_bread(struct ext4_fs *fs, uint64 blkno);
int ext4_group_desc(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *out);

#endif
