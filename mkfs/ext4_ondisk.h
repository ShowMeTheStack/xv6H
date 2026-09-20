#ifndef EXT4_ONDISK_H
#define EXT4_ONDISK_H

// the ext4 and jbd2 on-disk formats, as Linux 6.12 defines them in
// fs/ext4/ext4.h, fs/ext4/ext4_extents.h and include/linux/jbd2.h.
//
// this header is shared by the host tool mkfs/mkext4.c and, later, by the
// kernel's ext4 driver, so it carries types and constants only: no
// functions, no other headers. the host gets its fixed-width types from
// <stdint.h>; the kernel defines EXT4_ONDISK_KERNEL before including this
// and brings its own (kernel/types.h).
//
// every multi-byte ext4 field is little-endian on disk. both riscv64 and
// the build hosts we support (x86-64, arm64) are little-endian, so the
// __le types below are plain integers and nothing is ever byte-swapped;
// they exist to document the layout and to make a big-endian port notice
// every place that would have to change. the jbd2 journal is the
// exception: every field of the journal's own metadata is BIG-endian, so
// those are marked __be and do need swapping on a little-endian machine.

#ifdef EXT4_ONDISK_KERNEL
typedef uint16 __le16;
typedef uint32 __le32;
typedef uint64 __le64;
typedef uint32 __be32;
#else
#include <stdint.h>
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint32_t __be32;
#endif

// ---------------------------------------------------------------------
// superblock
// ---------------------------------------------------------------------

// the superblock always lives at byte offset 1024 of the volume, whatever
// the block size is. with 4 KiB blocks that puts it inside block 0, whose
// first 1024 bytes are left for a boot sector.
#define EXT4_SUPER_OFFSET 1024
#define EXT4_SUPER_MAGIC  0xef53

// block sizes are expressed as a shift from 1024 bytes: s_log_block_size
// 2 means 4096. we only ever write 4096.
#define EXT4_MIN_BLOCK_LOG_SIZE 10

// s_state
#define EXT4_VALID_FS 0x0001 // cleanly unmounted
#define EXT4_ERROR_FS 0x0002 // errors detected

// s_errors: what the kernel should do when it finds corruption
#define EXT4_ERRORS_CONTINUE 1
#define EXT4_ERRORS_RO       2
#define EXT4_ERRORS_PANIC    3

// s_rev_level: revision 1 is the dynamic revision, the one that has
// s_inode_size, s_first_ino and the feature flags at all
#define EXT4_GOOD_OLD_REV 0
#define EXT4_DYNAMIC_REV  1

// s_creator_os
#define EXT4_OS_LINUX 0

// the 128-byte inode of the original ext2 format; anything past that in a
// larger inode is the "extra" area described by i_extra_isize
#define EXT4_GOOD_OLD_INODE_SIZE 128

// s_checksum_type: the only algorithm ever defined
#define EXT4_CRC32C_CHKSUM 1

// s_def_hash_version, the htree hash functions
#define EXT4_HASH_LEGACY            0
#define EXT4_HASH_HALF_MD4          1
#define EXT4_HASH_TEA               2
#define EXT4_HASH_LEGACY_UNSIGNED   3
#define EXT4_HASH_HALF_MD4_UNSIGNED 4
#define EXT4_HASH_TEA_UNSIGNED      5

// s_flags. these record whether the volume's htree hashes were computed
// with a signed or an unsigned char type, because the two disagree on
// bytes >= 0x80 and a directory hashed one way cannot be searched the
// other way. riscv64 and arm64 have unsigned char, x86-64 has signed.
#define EXT4_FLAGS_SIGNED_HASH   0x0001
#define EXT4_FLAGS_UNSIGNED_HASH 0x0002
#define EXT4_FLAGS_TEST_FILESYS  0x0004

// s_jnl_backup_type: s_jnl_blocks holds a copy of the journal inode's
// i_block array, so the journal can be found again if the inode table is
// damaged
#define EXT4_JNL_BACKUP_BLOCKS 1

// s_default_mount_opts
#define EXT4_DEFM_DEBUG      0x0001
#define EXT4_DEFM_BSDGROUPS  0x0002
#define EXT4_DEFM_XATTR_USER 0x0004
#define EXT4_DEFM_ACL        0x0008
#define EXT4_DEFM_UID16      0x0010

struct ext4_super_block {
  __le32 s_inodes_count;        // total inodes, including the reserved ones
  __le32 s_blocks_count_lo;     // total blocks, low 32 bits
  __le32 s_r_blocks_count_lo;   // blocks reserved for root, low 32 bits
  __le32 s_free_blocks_count_lo;// free blocks, low 32 bits
  __le32 s_free_inodes_count;   // free inodes
  __le32 s_first_data_block;    // 0 for 4 KiB blocks, 1 for 1 KiB blocks
  __le32 s_log_block_size;      // block size = 1024 << this
  __le32 s_log_cluster_size;    // cluster size; equals the above without bigalloc
  __le32 s_blocks_per_group;    // blocks covered by one block bitmap
  __le32 s_clusters_per_group;  // clusters per group; equals the above without bigalloc
  __le32 s_inodes_per_group;    // inodes in each group's inode table
  __le32 s_mtime;               // last mount time, seconds since the epoch
  __le32 s_wtime;               // last write time
  __le16 s_mnt_count;           // mounts since the last check
  __le16 s_max_mnt_count;       // mounts allowed before a check is forced; -1 is never
  __le16 s_magic;               // EXT4_SUPER_MAGIC
  __le16 s_state;               // EXT4_VALID_FS when cleanly unmounted
  __le16 s_errors;              // what to do on an error
  __le16 s_minor_rev_level;     // minor revision
  __le32 s_lastcheck;           // time of the last fsck
  __le32 s_checkinterval;       // maximum time between checks; 0 is never
  __le32 s_creator_os;          // EXT4_OS_LINUX
  __le32 s_rev_level;           // EXT4_DYNAMIC_REV
  __le16 s_def_resuid;          // uid allowed to use the reserved blocks
  __le16 s_def_resgid;          // gid allowed to use the reserved blocks
  __le32 s_first_ino;           // first inode a file may use; 11 since ext2
  __le16 s_inode_size;          // bytes per inode table entry; 256 here
  __le16 s_block_group_nr;      // the group this copy of the superblock lives in
  __le32 s_feature_compat;      // features an old driver may safely ignore
  __le32 s_feature_incompat;    // features a driver must understand to mount
  __le32 s_feature_ro_compat;   // features a driver must understand to write
  unsigned char s_uuid[16];     // volume identifier, also the checksum seed
  char s_volume_name[16];       // volume label, not necessarily terminated
  char s_last_mounted[64];      // directory where it was last mounted
  __le32 s_algorithm_usage_bitmap; // compression, never used
  unsigned char s_prealloc_blocks;     // ext3 preallocation hint, unused by ext4
  unsigned char s_prealloc_dir_blocks; // the same for directories
  __le16 s_reserved_gdt_blocks; // blocks kept after the group descriptors for growth
  unsigned char s_journal_uuid[16]; // uuid of the journal, matching s_uuid for an internal one
  __le32 s_journal_inum;        // inode of the internal journal; 8
  __le32 s_journal_dev;         // device of an external journal
  __le32 s_last_orphan;         // head of the list of inodes to delete
  __le32 s_hash_seed[4];        // seed of the htree hash functions
  unsigned char s_def_hash_version; // default htree hash function
  unsigned char s_jnl_backup_type;  // EXT4_JNL_BACKUP_BLOCKS if s_jnl_blocks is valid
  __le16 s_desc_size;           // bytes per group descriptor; 64 with the 64bit feature
  __le32 s_default_mount_opts;  // mount options the kernel applies by default
  __le32 s_first_meta_bg;       // first group described by meta_bg
  __le32 s_mkfs_time;           // when the file system was created
  __le32 s_jnl_blocks[17];      // copy of the journal inode's i_block, i_size_high, i_size
  __le32 s_blocks_count_hi;     // total blocks, high 32 bits
  __le32 s_r_blocks_count_hi;   // reserved blocks, high 32 bits
  __le32 s_free_blocks_count_hi;// free blocks, high 32 bits
  __le16 s_min_extra_isize;     // extra inode bytes every inode is guaranteed to have
  __le16 s_want_extra_isize;    // extra inode bytes new inodes should reserve
  __le32 s_flags;               // signed or unsigned htree hashes, test flag
  __le16 s_raid_stride;         // raid stride in blocks
  __le16 s_mmp_update_interval; // multi-mount protection check interval
  __le64 s_mmp_block;           // block holding the multi-mount protection record
  __le32 s_raid_stripe_width;   // blocks per full raid stripe
  unsigned char s_log_groups_per_flex; // flex_bg size = 1 << this
  unsigned char s_checksum_type;       // EXT4_CRC32C_CHKSUM
  __le16 s_reserved_pad;
  __le64 s_kbytes_written;      // lifetime kilobytes written, a wear statistic
  __le32 s_snapshot_inum;       // inode of the active snapshot
  __le32 s_snapshot_id;         // id of the active snapshot
  __le64 s_snapshot_r_blocks_count; // blocks reserved for the active snapshot
  __le32 s_snapshot_list;       // inode heading the snapshot list
  __le32 s_error_count;         // number of errors seen
  __le32 s_first_error_time;    // when the first error happened
  __le32 s_first_error_ino;     // inode involved in the first error
  __le64 s_first_error_block;   // block involved in the first error
  unsigned char s_first_error_func[32]; // kernel function that reported it
  __le32 s_first_error_line;    // line number that reported it
  __le32 s_last_error_time;     // when the most recent error happened
  __le32 s_last_error_ino;      // inode involved in the most recent error
  __le32 s_last_error_line;     // line number that reported it
  __le64 s_last_error_block;    // block involved in the most recent error
  unsigned char s_last_error_func[32]; // kernel function that reported it
  unsigned char s_mount_opts[64];      // default mount options, as a string
  __le32 s_usr_quota_inum;      // inode holding the user quota file
  __le32 s_grp_quota_inum;      // inode holding the group quota file
  __le32 s_overhead_clusters;   // clusters used by metadata, 0 if not computed
  __le32 s_backup_bgs[2];       // groups holding the backups under sparse_super2
  unsigned char s_encrypt_algos[4];    // encryption algorithms in use
  unsigned char s_encrypt_pw_salt[16]; // salt for the string2key algorithm
  __le32 s_lpf_ino;             // inode of lost+found
  __le32 s_prj_quota_inum;      // inode holding the project quota file
  __le32 s_checksum_seed;       // crc32c(~0, s_uuid), so the uuid can change later
  unsigned char s_wtime_hi;     // high bits of the times above, for dates past 2038
  unsigned char s_mtime_hi;
  unsigned char s_mkfs_time_hi;
  unsigned char s_lastcheck_hi;
  unsigned char s_first_error_time_hi;
  unsigned char s_last_error_time_hi;
  unsigned char s_first_error_errcode;
  unsigned char s_last_error_errcode;
  __le16 s_encoding;            // filename character set for casefolding
  __le16 s_encoding_flags;      // flags for that character set
  __le32 s_orphan_file_inum;    // inode of the orphan file
  __le32 s_reserved[94];        // padding to 1024 bytes
  __le32 s_checksum;            // crc32c(~0, superblock up to this field)
};

// compat features: a driver that does not know them can still mount and
// write the volume.
#define EXT4_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL   0x0004 // an ext3/ext4 journal is present
#define EXT4_FEATURE_COMPAT_EXT_ATTR      0x0008 // extended attributes are used
#define EXT4_FEATURE_COMPAT_RESIZE_INODE  0x0010 // inode 7 maps the reserved gdt blocks
#define EXT4_FEATURE_COMPAT_DIR_INDEX     0x0020 // htree directories may be present
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200
#define EXT4_FEATURE_COMPAT_FAST_COMMIT   0x0400
#define EXT4_FEATURE_COMPAT_STABLE_INODES 0x0800
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE   0x1000

// ro_compat features: a driver that does not know them may mount the
// volume read-only, because writing without understanding them would
// corrupt it.
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001 // superblock backups only in some groups
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002 // files may be larger than 2 GiB
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE     0x0008 // i_blocks may count file blocks, not sectors
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010 // crc16 group descriptors (uninit_bg)
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK     0x0020 // a directory may have more than 65000 links
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   0x0040 // inodes carry the extra fields
#define EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT  0x0080
#define EXT4_FEATURE_RO_COMPAT_QUOTA         0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC      0x0200
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400 // crc32c on every metadata structure
#define EXT4_FEATURE_RO_COMPAT_REPLICA       0x0800
#define EXT4_FEATURE_RO_COMPAT_READONLY      0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT       0x2000
#define EXT4_FEATURE_RO_COMPAT_VERITY        0x8000

// incompat features: a driver that does not know them must refuse the
// volume outright.
#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE    0x0002 // directory entries carry a file type
#define EXT4_FEATURE_INCOMPAT_RECOVER     0x0004 // the journal needs replaying
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008 // the journal lives on another device
#define EXT4_FEATURE_INCOMPAT_META_BG     0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x0040 // inodes map blocks with extent trees
#define EXT4_FEATURE_INCOMPAT_64BIT       0x0080 // 64-bit block numbers, 64-byte descriptors
#define EXT4_FEATURE_INCOMPAT_MMP         0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x0200 // a group's metadata may live in another group
#define EXT4_FEATURE_INCOMPAT_EA_INODE    0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA     0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x2000 // s_checksum_seed is authoritative
#define EXT4_FEATURE_INCOMPAT_LARGEDIR    0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT     0x10000
#define EXT4_FEATURE_INCOMPAT_CASEFOLD    0x20000

// ---------------------------------------------------------------------
// block group descriptors
// ---------------------------------------------------------------------

// with the 64bit feature the descriptor is 64 bytes and each block number
// is split into a _lo and a _hi half; without it only the first 32 bytes
// exist. the group descriptor table follows the superblock, is duplicated
// wherever a superblock backup lives, and is what every lookup starts
// from: inode n lives in group (n-1)/s_inodes_per_group.
struct ext4_group_desc {
  __le32 bg_block_bitmap_lo;      // block holding this group's block bitmap
  __le32 bg_inode_bitmap_lo;      // block holding this group's inode bitmap
  __le32 bg_inode_table_lo;       // first block of this group's inode table
  __le16 bg_free_blocks_count_lo; // free blocks in this group
  __le16 bg_free_inodes_count_lo; // free inodes in this group
  __le16 bg_used_dirs_count_lo;   // directories in this group, for the allocator
  __le16 bg_flags;                // EXT4_BG_* below
  __le32 bg_exclude_bitmap_lo;    // snapshot exclude bitmap
  __le16 bg_block_bitmap_csum_lo; // crc32c of the block bitmap, low 16 bits
  __le16 bg_inode_bitmap_csum_lo; // crc32c of the inode bitmap, low 16 bits
  __le16 bg_itable_unused_lo;     // inodes at the end of the table never yet used
  __le16 bg_checksum;             // crc32c(seed, group number, this descriptor)
  __le32 bg_block_bitmap_hi;      // high 32 bits of bg_block_bitmap
  __le32 bg_inode_bitmap_hi;      // high 32 bits of bg_inode_bitmap
  __le32 bg_inode_table_hi;       // high 32 bits of bg_inode_table
  __le16 bg_free_blocks_count_hi; // high 16 bits of the free block count
  __le16 bg_free_inodes_count_hi; // high 16 bits of the free inode count
  __le16 bg_used_dirs_count_hi;   // high 16 bits of the directory count
  __le16 bg_itable_unused_hi;     // high 16 bits of the unused inode count
  __le32 bg_exclude_bitmap_hi;    // high 32 bits of the exclude bitmap block
  __le16 bg_block_bitmap_csum_hi; // crc32c of the block bitmap, high 16 bits
  __le16 bg_inode_bitmap_csum_hi; // crc32c of the inode bitmap, high 16 bits
  __le32 bg_reserved;
};

// bg_flags
#define EXT4_BG_INODE_UNINIT 0x0001 // the inode bitmap and table were never written
#define EXT4_BG_BLOCK_UNINIT 0x0002 // the block bitmap is exactly what the layout implies
#define EXT4_BG_INODE_ZEROED 0x0004 // the inode table has been zeroed

// bg_checksum covers the descriptor with this field taken out, so the
// checksum function stops here and resumes after it
#define EXT4_BG_CHECKSUM_OFFSET 0x1e

// ---------------------------------------------------------------------
// inodes
// ---------------------------------------------------------------------

// inode numbers below EXT4_FIRST_INO are reserved by the format. only
// root and the journal are really used; the rest must be present and
// zeroed, and fsck insists that they have mode 0 and no blocks.
#define EXT4_BAD_INO         1  // blocks that are known to be bad
#define EXT4_ROOT_INO        2  // the root directory
#define EXT4_USR_QUOTA_INO   3  // user quota file
#define EXT4_GRP_QUOTA_INO   4  // group quota file
#define EXT4_BOOT_LOADER_INO 5  // boot loader scratch inode
#define EXT4_UNDEL_DIR_INO   6  // undelete directory
#define EXT4_RESIZE_INO      7  // maps the reserved gdt blocks when resize_inode is set
#define EXT4_JOURNAL_INO     8  // the internal jbd2 journal
#define EXT4_EXCLUDE_INO     9  // snapshot exclude bitmap
#define EXT4_REPLICA_INO     10 // replica inode
#define EXT4_FIRST_INO       11 // first inode a file may use; mkfs gives it to lost+found

// i_mode: the file type is the top four bits, exactly as in POSIX stat
#define EXT4_S_IFMT   0xf000
#define EXT4_S_IFIFO  0x1000
#define EXT4_S_IFCHR  0x2000
#define EXT4_S_IFDIR  0x4000
#define EXT4_S_IFBLK  0x6000
#define EXT4_S_IFREG  0x8000
#define EXT4_S_IFLNK  0xa000
#define EXT4_S_IFSOCK 0xc000

// i_flags
#define EXT4_SECRM_FL       0x00000001 // secure deletion
#define EXT4_UNRM_FL        0x00000002 // undelete
#define EXT4_SYNC_FL        0x00000008 // synchronous updates
#define EXT4_IMMUTABLE_FL   0x00000010 // immutable file
#define EXT4_APPEND_FL      0x00000020 // append only
#define EXT4_NODUMP_FL      0x00000040 // do not dump
#define EXT4_NOATIME_FL     0x00000080 // do not update the access time
#define EXT4_INDEX_FL       0x00001000 // htree indexed directory
#define EXT4_JOURNAL_DATA_FL 0x00004000 // data goes through the journal
#define EXT4_DIRSYNC_FL     0x00010000 // directory changes are synchronous
#define EXT4_TOPDIR_FL      0x00020000 // top of a directory tree, an allocator hint
#define EXT4_HUGE_FILE_FL   0x00040000 // i_blocks counts file blocks, not 512-byte sectors
#define EXT4_EXTENTS_FL     0x00080000 // i_block holds an extent tree, not block pointers
#define EXT4_EA_INODE_FL    0x00200000 // inode holds one large extended attribute
#define EXT4_INLINE_DATA_FL 0x10000000 // file data lives in i_block and the xattr area
#define EXT4_PROJINHERIT_FL 0x20000000 // children inherit the project id
#define EXT4_CASEFOLD_FL    0x40000000 // case-insensitive lookups

// i_block is 60 bytes: 15 32-bit words in the old format, or an extent
// tree root in the extents format
#define EXT4_N_BLOCKS   15
#define EXT4_NDIR_BLOCKS 12
#define EXT4_IND_BLOCK   12
#define EXT4_DIND_BLOCK  13
#define EXT4_TIND_BLOCK  14

struct ext4_inode {
  __le16 i_mode;        // file type and permission bits
  __le16 i_uid;         // owner, low 16 bits
  __le32 i_size_lo;     // size in bytes, low 32 bits
  __le32 i_atime;       // last access time, seconds since the epoch
  __le32 i_ctime;       // last inode change time
  __le32 i_mtime;       // last data modification time
  __le32 i_dtime;       // deletion time; nonzero means the inode is free
  __le16 i_gid;         // group, low 16 bits
  __le16 i_links_count; // hard links; a directory counts "." and each child's ".."
  __le32 i_blocks_lo;   // blocks charged to the file, in 512-byte units by default
  __le32 i_flags;       // EXT4_*_FL
  __le32 l_i_version;   // osd1 on Linux: inode version, used by nfs
  __le32 i_block[EXT4_N_BLOCKS]; // extent tree root, block pointers, device number or fast symlink
  __le32 i_generation;  // file version, used by nfs and by every inode checksum
  __le32 i_file_acl_lo; // block holding the extended attributes
  __le32 i_size_high;   // size in bytes, high 32 bits (dir_acl in the old format)
  __le32 i_obso_faddr;  // obsolete fragment address
  __le16 l_i_blocks_high;   // osd2 on Linux: high 16 bits of i_blocks
  __le16 l_i_file_acl_high; // high 16 bits of the xattr block
  __le16 l_i_uid_high;      // owner, high 16 bits
  __le16 l_i_gid_high;      // group, high 16 bits
  __le16 l_i_checksum_lo;   // crc32c of this inode, low 16 bits
  __le16 l_i_reserved;
  __le16 i_extra_isize; // bytes of this inode used past the first 128; 32 here
  __le16 i_checksum_hi; // crc32c of this inode, high 16 bits
  __le32 i_ctime_extra; // nanoseconds of i_ctime in the top 30 bits, epoch bits in the low 2
  __le32 i_mtime_extra; // nanoseconds and epoch bits of i_mtime
  __le32 i_atime_extra; // nanoseconds and epoch bits of i_atime
  __le32 i_crtime;      // creation time, seconds since the epoch
  __le32 i_crtime_extra;// nanoseconds and epoch bits of i_crtime
  __le32 i_version_hi;  // high 32 bits of the inode version
  __le32 i_projid;      // project id
};

// the offset of the extra fields inside a large inode, and the smallest
// i_extra_isize that still covers i_checksum_hi; an inode with less than
// that has only the 16-bit half of its checksum
#define EXT4_EXTRA_ISIZE          32
#define EXT4_INODE_CSUM_HI_EXTRA_END 4

// ---------------------------------------------------------------------
// directories
// ---------------------------------------------------------------------

// a directory is a sequence of blocks, each a list of entries; rec_len
// carries the walk from one entry to the next, is always a multiple of 4,
// and the last entry of a block stretches to the end of that block (or,
// with metadata_csum, to the tail below). an entry with inode 0 is free
// space. names are not terminated.
#define EXT4_NAME_LEN 255

struct ext4_dir_entry_2 {
  __le32 inode;             // inode this name refers to; 0 means the slot is free
  __le16 rec_len;           // bytes from here to the next entry
  unsigned char name_len;   // length of the name
  unsigned char file_type;  // EXT4_FT_*, present because of the filetype feature
  char name[EXT4_NAME_LEN]; // the name, not terminated, not padded in any defined way
};

// file types, the high byte of the old name_len field
#define EXT4_FT_UNKNOWN  0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_CHRDEV   3
#define EXT4_FT_BLKDEV   4
#define EXT4_FT_FIFO     5
#define EXT4_FT_SOCK     6
#define EXT4_FT_SYMLINK  7

// with metadata_csum the last 12 bytes of every directory block hold this
// structure disguised as an entry: rec_len 12 and the impossible file
// type 0xde keep an old driver from walking into it, and name_len 0 keeps
// it from matching a name.
struct ext4_dir_entry_tail {
  __le32 det_reserved_zero1;        // must be 0, so it never looks like an inode
  __le16 det_rec_len;               // 12
  unsigned char det_reserved_zero2; // must be 0, the name length
  unsigned char det_reserved_ft;    // EXT4_FT_DIR_CSUM
  __le32 det_checksum;              // crc32c(seed, inode, generation, block up to here)
};

#define EXT4_FT_DIR_CSUM 0xde
#define EXT4_DIR_TAIL_SIZE 12

// an htree directory keeps its first block in this shape: two fake
// entries for "." and "..", then a small header, then a sorted array of
// (hash, block) pairs. mkext4 writes linear directories only, but the
// kernel must recognise this layout to read images Linux wrote.
struct ext4_dx_entry {
  __le32 hash;  // lowest hash value found in that block
  __le32 block; // logical block number inside the directory
};

struct ext4_dx_root {
  __le32 dot_inode;              // the "." entry, as a plain directory entry
  __le16 dot_rec_len;            // 12
  unsigned char dot_name_len;    // 1
  unsigned char dot_file_type;   // EXT4_FT_DIR
  char dot_name[4];              // "."
  __le32 dotdot_inode;           // the ".." entry
  __le16 dotdot_rec_len;         // the rest of the block
  unsigned char dotdot_name_len; // 2
  unsigned char dotdot_file_type;// EXT4_FT_DIR
  char dotdot_name[4];           // ".."
  __le32 reserved_zero;          // must be 0: an old driver reads it as an inode number
  unsigned char hash_version;    // which hash function the entries were built with
  unsigned char info_length;     // 8, the length of this header
  unsigned char indirect_levels; // 0 for one level of index blocks
  unsigned char unused_flags;
  __le16 limit;                  // entries that fit in this block
  __le16 count;                  // entries in use
  __le32 block;                  // logical block holding names below the first hash
  struct ext4_dx_entry entries[0]; // count-1 further (hash, block) pairs
};

// the last 8 bytes of an htree index block, again with metadata_csum
struct ext4_dx_tail {
  __le32 dt_reserved;
  __le32 dt_checksum; // crc32c(seed, inode, generation, header and entries in use)
};

// ---------------------------------------------------------------------
// extents
// ---------------------------------------------------------------------

// an extent tree maps logical block numbers to physical ones in runs. the
// root is the 60 bytes of i_block, which hold a header and four entries;
// deeper trees hang index blocks off it, one block each, with the entries
// sorted by logical block. a leaf entry covers up to 32768 blocks, and
// ee_len above that marks the run as allocated but unwritten.
#define EXT4_EXT_MAGIC       0xf30a
#define EXT4_EXT_MAX_LEN     32768 // the largest ee_len, half of it if unwritten
#define EXT4_EXT_INIT_MAX_LEN 32768
#define EXT4_EXT_UNWRITTEN_MASK 0x8000

struct ext4_extent_header {
  __le16 eh_magic;      // EXT4_EXT_MAGIC
  __le16 eh_entries;    // entries in use after this header
  __le16 eh_max;        // entries that fit; 4 in the inode, 340 in a 4 KiB block
  __le16 eh_depth;      // 0 for a leaf of extents, more for a node of indices
  __le32 eh_generation; // unused by Linux
};

struct ext4_extent_idx {
  __le32 ei_block;   // this child covers logical blocks from here on
  __le32 ei_leaf_lo; // block holding the child node, low 32 bits
  __le16 ei_leaf_hi; // high 16 bits of that block number
  __le16 ei_unused;
};

struct ext4_extent {
  __le32 ee_block;    // first logical block this extent covers
  __le16 ee_len;      // blocks covered; the top bit marks an unwritten extent
  __le16 ee_start_hi; // high 16 bits of the first physical block
  __le32 ee_start_lo; // first physical block, low 32 bits
};

// the last 4 bytes of an extent block outside the inode, again only with
// metadata_csum. it sits after the last entry slot the header allows, not
// after the last one in use.
struct ext4_extent_tail {
  __le32 et_checksum; // crc32c(seed, inode, generation, header and all entry slots)
};

// ---------------------------------------------------------------------
// the jbd2 journal
// ---------------------------------------------------------------------

// every field below is big-endian, including inside the descriptor tags:
// jbd2 was written for a portable journal and never changed. the journal
// is a file (inode 8 here) used as a circular log: block 0 is the journal
// superblock and blocks s_first .. s_maxlen-1 are the log itself.
#define JBD2_MAGIC_NUMBER 0xc03b3998

// h_blocktype
#define JBD2_DESCRIPTOR_BLOCK 1 // a list of tags naming the blocks that follow
#define JBD2_COMMIT_BLOCK     2 // the transaction before it is now durable
#define JBD2_SUPERBLOCK_V1    3 // the original journal superblock
#define JBD2_SUPERBLOCK_V2    4 // adds the feature words; what everyone writes
#define JBD2_REVOKE_BLOCK     5 // blocks that must not be replayed

typedef struct journal_header_s {
  __be32 h_magic;     // JBD2_MAGIC_NUMBER
  __be32 h_blocktype; // one of the block types above
  __be32 h_sequence;  // transaction this block belongs to
} journal_header_t;

// tag flags in a descriptor block
#define JBD2_FLAG_ESCAPE   1 // the data block began with the jbd2 magic and was escaped
#define JBD2_FLAG_SAME_UUID 2 // this tag reuses the previous tag's uuid, which is then absent
#define JBD2_FLAG_DELETED  4
#define JBD2_FLAG_LAST_TAG 8 // last tag in this descriptor block

// the tag shape used when csum_v3 is set: a flat structure with the
// checksum and the full 64-bit block number, rather than the packed
// 8- or 12-byte tag of the older formats.
typedef struct journal_block_tag3_s {
  __be32 t_blocknr;      // file system block this journalled block belongs to, low 32 bits
  __be32 t_flags;        // JBD2_FLAG_*
  __be32 t_blocknr_high; // high 32 bits of that block number
  __be32 t_checksum;     // crc32c(journal uuid seed, sequence, the data block)
} journal_block_tag3_t;

struct journal_superblock_s {
  journal_header_t s_header; // magic, JBD2_SUPERBLOCK_V2, and 0
  __be32 s_blocksize;        // journal block size, matching the file system's
  __be32 s_maxlen;           // total blocks in the journal, including this one
  __be32 s_first;            // first block of the log proper; 1
  __be32 s_sequence;         // first expected transaction sequence
  __be32 s_start;            // block the log starts at; 0 means the journal is empty
  __be32 s_errno;            // error that stopped the journal
  __be32 s_feature_compat;   // JBD2_FEATURE_COMPAT_*
  __be32 s_feature_incompat; // JBD2_FEATURE_INCOMPAT_*
  __be32 s_feature_ro_compat;
  unsigned char s_uuid[16];  // uuid of the file system this journal belongs to
  __be32 s_nr_users;         // file systems sharing this journal; 1
  __be32 s_dynsuper;         // block of the dynamic superblock copy
  __be32 s_max_transaction;  // limit on blocks per transaction
  __be32 s_max_trans_data;   // limit on data blocks per transaction
  unsigned char s_checksum_type; // JBD2_CRC32C_CHKSUM when the csum features are set
  unsigned char s_padding2[3];
  __be32 s_num_fc_blks;      // fast commit blocks at the end of the journal
  __be32 s_head;             // oldest block the log still needs
  __be32 s_padding[40];
  __be32 s_checksum;         // crc32c(~0, this 1024-byte block with the field zeroed)
  unsigned char s_users[16 * 48]; // uuids of the file systems sharing the journal
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

// the kernel refuses a journal smaller than this, so mkfs must not make one
#define JBD2_MIN_JOURNAL_BLOCKS 1024

// ---------------------------------------------------------------------
// checksums
// ---------------------------------------------------------------------

// with metadata_csum every structure is covered by crc32c (Castagnoli,
// reflected, polynomial 0x82f63b78) seeded from the volume. the seed is
// crc32c(~0, s_uuid, 16) unless the csum_seed incompat feature says to
// take s_checksum_seed literally, which lets tune2fs change the uuid of a
// mounted volume. the superblock and the journal superblock are the two
// exceptions: they are seeded with ~0 and cover themselves.
//
// each structure then mixes in what identifies it before its own bytes:
//   group descriptor  seed, group number
//   block bitmap      seed
//   inode bitmap      seed
//   inode             seed, inode number, i_generation
//   extent block      seed, inode number, i_generation
//   directory block   seed, inode number, i_generation
#define EXT4_CSUM_SEED_INIT 0xffffffff
#define EXT4_CRC32C_POLY_LE 0x82f63b78

#endif // EXT4_ONDISK_H
