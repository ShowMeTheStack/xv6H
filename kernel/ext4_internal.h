#ifndef XV6_EXT4_INTERNAL_H
#define XV6_EXT4_INTERNAL_H
//
// what the parts of the ext4 driver share with each other and with
// nothing else. the reading side is in ext4_read.c, the writing side in
// ext4_write.c, and these are the few things both need.
//
#include "ext4.h"

struct blkbuf;

// one file system block, with its number checked against the superblock
struct blkbuf *ext4_bread(struct ext4_fs *fs, uint64 blkno);

// a copy of one group descriptor, widened to 64 bytes whatever the
// file system stores
int ext4_group_desc(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *out);

// bounds that do not come from the disk, so that a corrupt number can
// make a walk stop rather than run.
#define EXT4_MAX_DIR_BLOCKS   (1u << 20)   // 4 GiB of directory
#define EXT4_MAX_DX_LEVELS    3            // root plus two interior levels
#define EXT4_MAX_DX_RETRIES   64           // leaves visited for one collision run

// ---- what the read side lends the write side ----
//
// the htree code writes the same structures the read side walks, so it
// uses the same checks: a block of a directory, an entry that has to be
// inside it, an index node that has to look like one.
struct blkbuf *ext4_dir_bread(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 fileblk,
                              int *err);
int ext4_dirent_check(const uchar *data, uint32 bsize, uint32 off,
                      const struct ext4_dir_entry_2 **de);
uint64 ext4_dir_nblocks(struct ext4_fs *fs, struct ext4_inode_info *dir);
int ext4_dx_node(struct ext4_fs *fs, const uchar *data, int is_root, uint32 *eoff, uint32 *count);
uint32 ext4_dirhash_version(struct ext4_fs *fs, uint32 version, const char *name, int len,
                            uint32 *minor_out);

// ---- the write side ----

// the checksums, which every metadata block carries when the file
// system has metadata_csum. writing a block without its checksum makes
// the file system unreadable to Linux, so these are not optional.
// the seed every checksum of one inode's blocks starts from
uint32 ext4_inode_seed(struct ext4_fs *fs, uint64 ino, uint32 gen);
void ext4_gd_csum(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *gd);
void ext4_super_csum(struct ext4_fs *fs, struct ext4_super_block *sb);
void ext4_inode_csum(struct ext4_fs *fs, uint64 ino, struct ext4_inode *di);
void ext4_dirblock_csum(struct ext4_fs *fs, uint64 ino, uint32 gen, void *block);
void ext4_extentblock_csum(struct ext4_fs *fs, uint64 ino, uint32 gen, void *block);

int ext4_write_group_desc(struct ext4_fs *fs, uint32 group, struct ext4_group_desc *gd);
int ext4_write_super(struct ext4_fs *fs);

// blocks and inodes
int ext4_alloc_block(struct ext4_fs *fs, uint64 goal, uint64 *out);
int ext4_alloc_blocks(struct ext4_fs *fs, uint64 goal, uint32 want, int prefill, uint64 *out,
                      uint32 *got);
int ext4_free_block(struct ext4_fs *fs, uint64 blkno);
int ext4_alloc_inode(struct ext4_fs *fs, uint64 dirino, int is_dir, uint64 *out);
int ext4_free_inode(struct ext4_fs *fs, uint64 ino, int is_dir);

// ---- hash-indexed directories (ext4_htree.c) ----
//
// a name in an indexed directory, and the conversion that makes a
// directory indexed once one block is no longer enough for it.
int ext4_htree_add(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                   const char *name, int namelen, uint64 ino, uchar type);
int ext4_htree_remove(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                      const char *name, int namelen, uint64 *ino_out);
int ext4_dir_make_indexed(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino);
// the checksum an index block carries, which is not the one a leaf has
void ext4_dxblock_csum(struct ext4_fs *fs, uint64 ino, uint32 gen, void *block, int is_root);
// one name into one block of a directory; -ENOSPC when it will not fit
int ext4_dir_block_insert(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                          uint64 fileblk, const char *name, int namelen, uint64 ino, uchar type);
// another block at the end of the directory file, zeroed and in hand
int ext4_dir_grow(struct ext4_fs *fs, struct ext4_inode_info *dir, uint64 dirino,
                  uint64 *fileblk, struct blkbuf **out);

#endif
