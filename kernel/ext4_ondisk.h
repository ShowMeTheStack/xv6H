#ifndef XV6_EXT4_ONDISK_H
#define XV6_EXT4_ONDISK_H
//
// the ext4 on-disk format: exactly the bytes Linux 6.12 writes, with the
// field names fs/ext4/ext4.h uses so that this header and the host tool
// mkfs/mkext4.c can be read side by side.
//
// every field is little-endian. riscv is little-endian too, so a field is
// read by loading it, not by swapping it; a port to a big-endian machine
// would have to put byte swaps in here and nowhere else.
//
// the structures are deliberately not packed. the ext4 layout has no
// implicit padding anywhere, so a plain struct has the same layout as the
// disk, and leaving it unpacked lets the compiler use aligned loads. the
// _Static_asserts at the bottom of this file prove that field by field, and
// the driver only ever overlays one of these structures on a base address
// that is at least as aligned as the structure needs:
//
//   superblock      byte 1024 of block 0          1024-aligned
//   group desc      g * s_desc_size (32 or 64)    32-aligned
//   inode           k * s_inode_size (>= 128)     128-aligned
//   extent header   byte 40 of an inode, or 0 of a block
//   directory entry a 4-aligned offset, which the driver checks first
//
#include "types.h"

#define EXT4_SUPER_MAGIC      0xEF53
#define EXT4_SUPER_OFFSET     1024      // the superblock starts here, always
#define EXT4_MIN_BLOCK_SIZE   1024      // what s_log_block_size is a shift of
#define EXT4_ROOT_INO         2

// s_state. VALID_FS is "this was unmounted cleanly"; a kernel clears it
// while the file system is mounted read-write and sets it again on the
// way out, which is how e2fsck decides whether a check is due.
#define EXT4_VALID_FS         0x0001
#define EXT4_ERROR_FS         0x0002
#define EXT4_ORPHAN_FS        0x0004
#define EXT4_GOOD_OLD_INODE_SIZE 128    // an inode before the extra fields
#define EXT4_N_BLOCKS         15        // i_block[] entries
#define EXT4_NDIR_BLOCKS      12        // of which these are direct
#define EXT4_IND_BLOCK        12
#define EXT4_DIND_BLOCK       13
#define EXT4_TIND_BLOCK       14
#define EXT4_NAME_LEN         255

// ---- feature flags ----
//
// the three words split by what a reader must do about a bit it does not
// know: a compat bit may be ignored, an ro_compat bit means the image may
// still be read, an incompat bit means the image must not be touched.

#define EXT4_FEATURE_COMPAT_DIR_PREALLOC    0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES   0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL     0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR        0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE    0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX       0x0020
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2   0x0200
#define EXT4_FEATURE_COMPAT_FAST_COMMIT     0x0400
#define EXT4_FEATURE_COMPAT_STABLE_INODES   0x0800
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE     0x1000

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR    0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE    0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM     0x0010   // uninit_bg
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK    0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE  0x0040
#define EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT 0x0080
#define EXT4_FEATURE_RO_COMPAT_QUOTA        0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC     0x0200
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#define EXT4_FEATURE_RO_COMPAT_REPLICA      0x0800
#define EXT4_FEATURE_RO_COMPAT_READONLY     0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT      0x2000
#define EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS 0x4000
#define EXT4_FEATURE_RO_COMPAT_VERITY       0x8000

#define EXT4_FEATURE_INCOMPAT_COMPRESSION   0x00001
#define EXT4_FEATURE_INCOMPAT_FILETYPE      0x00002
#define EXT4_FEATURE_INCOMPAT_RECOVER       0x00004   // the journal needs replay
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV   0x00008
#define EXT4_FEATURE_INCOMPAT_META_BG       0x00010
#define EXT4_FEATURE_INCOMPAT_EXTENTS       0x00040
#define EXT4_FEATURE_INCOMPAT_64BIT         0x00080
#define EXT4_FEATURE_INCOMPAT_MMP           0x00100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG       0x00200
#define EXT4_FEATURE_INCOMPAT_EA_INODE      0x00400
#define EXT4_FEATURE_INCOMPAT_DIRDATA       0x01000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED     0x02000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR      0x04000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA   0x08000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT       0x10000
#define EXT4_FEATURE_INCOMPAT_CASEFOLD      0x20000

// s_flags: which way the hash functions treat a char. an image made on a
// machine where char is signed and one made where it is unsigned hash the
// same name to different values, so the superblock records the choice.
#define EXT2_FLAGS_SIGNED_HASH    0x0001
#define EXT2_FLAGS_UNSIGNED_HASH  0x0002
#define EXT2_FLAGS_TEST_FILESYS   0x0004

// i_flags
#define EXT4_SECRM_FL         0x00000001
#define EXT4_UNRM_FL          0x00000002
#define EXT4_COMPR_FL         0x00000004
#define EXT4_SYNC_FL          0x00000008
#define EXT4_IMMUTABLE_FL     0x00000010
#define EXT4_APPEND_FL        0x00000020
#define EXT4_NODUMP_FL        0x00000040
#define EXT4_NOATIME_FL       0x00000080
#define EXT4_INDEX_FL         0x00001000   // this directory is hash-indexed
#define EXT4_IMAGIC_FL        0x00002000
#define EXT4_JOURNAL_DATA_FL  0x00004000
#define EXT4_NOTAIL_FL        0x00008000
#define EXT4_DIRSYNC_FL       0x00010000
#define EXT4_TOPDIR_FL        0x00020000
#define EXT4_HUGE_FILE_FL     0x00040000   // i_blocks counts fs blocks, not sectors
#define EXT4_EXTENTS_FL       0x00080000   // i_block holds an extent tree
#define EXT4_VERITY_FL        0x00100000
#define EXT4_EA_INODE_FL      0x00200000
#define EXT4_INLINE_DATA_FL   0x10000000
#define EXT4_PROJINHERIT_FL   0x20000000
#define EXT4_CASEFOLD_FL      0x40000000

// group descriptor flags
#define EXT4_BG_INODE_UNINIT  0x0001   // the inode table was never written
#define EXT4_BG_BLOCK_UNINIT  0x0002
#define EXT4_BG_INODE_ZEROED  0x0004

// the mode field's format bits, which are the POSIX ones
#define EXT4_S_IFMT           0170000
#define EXT4_S_IFSOCK         0140000
#define EXT4_S_IFLNK          0120000
#define EXT4_S_IFREG          0100000
#define EXT4_S_IFBLK          0060000
#define EXT4_S_IFDIR          0040000
#define EXT4_S_IFCHR          0020000
#define EXT4_S_IFIFO          0010000

// directory entry file types, the values the filetype feature stores
#define EXT4_FT_UNKNOWN       0
#define EXT4_FT_REG_FILE      1
#define EXT4_FT_DIR           2
#define EXT4_FT_CHRDEV        3
#define EXT4_FT_BLKDEV        4
#define EXT4_FT_FIFO          5
#define EXT4_FT_SOCK          6
#define EXT4_FT_SYMLINK       7
#define EXT4_FT_MAX           8

// directory hash functions, as s_def_hash_version names them. the three
// "unsigned" versions are the same three functions with char treated as
// unsigned; see the comment on EXT2_FLAGS_UNSIGNED_HASH.
#define DX_HASH_LEGACY            0
#define DX_HASH_HALF_MD4          1
#define DX_HASH_TEA               2
#define DX_HASH_LEGACY_UNSIGNED   3
#define DX_HASH_HALF_MD4_UNSIGNED 4
#define DX_HASH_TEA_UNSIGNED      5

// the value a hash may not take, because htree readdir uses it to mean
// "past the last entry"
#define EXT4_HTREE_EOF_32BIT  ((1UL << (32 - 1)) - 1)

// ---- the superblock ----

struct ext4_super_block {
  uint32 s_inodes_count;
  uint32 s_blocks_count_lo;
  uint32 s_r_blocks_count_lo;
  uint32 s_free_blocks_count_lo;
  uint32 s_free_inodes_count;
  uint32 s_first_data_block;     // 0 when the block size is over 1024
  uint32 s_log_block_size;       // block size is 1024 << this
  uint32 s_log_cluster_size;
  uint32 s_blocks_per_group;
  uint32 s_clusters_per_group;
  uint32 s_inodes_per_group;
  uint32 s_mtime;
  uint32 s_wtime;
  uint16 s_mnt_count;
  uint16 s_max_mnt_count;
  uint16 s_magic;                // 0xEF53
  uint16 s_state;
  uint16 s_errors;
  uint16 s_minor_rev_level;
  uint32 s_lastcheck;
  uint32 s_checkinterval;
  uint32 s_creator_os;
  uint32 s_rev_level;
  uint16 s_def_resuid;
  uint16 s_def_resgid;
  // the dynamic revision fields start here
  uint32 s_first_ino;            // the first inode a file may use
  uint16 s_inode_size;
  uint16 s_block_group_nr;
  uint32 s_feature_compat;
  uint32 s_feature_incompat;
  uint32 s_feature_ro_compat;
  uchar  s_uuid[16];
  char   s_volume_name[16];
  char   s_last_mounted[64];
  uint32 s_algorithm_usage_bitmap;
  uchar  s_prealloc_blocks;
  uchar  s_prealloc_dir_blocks;
  uint16 s_reserved_gdt_blocks;
  // the journal fields
  uchar  s_journal_uuid[16];
  uint32 s_journal_inum;
  uint32 s_journal_dev;
  uint32 s_last_orphan;
  uint32 s_hash_seed[4];         // the per-file-system seed of the dir hash
  uchar  s_def_hash_version;
  uchar  s_jnl_backup_type;
  uint16 s_desc_size;            // group descriptor size, when 64bit is set
  uint32 s_default_mount_opts;
  uint32 s_first_meta_bg;
  uint32 s_mkfs_time;
  uint32 s_jnl_blocks[17];
  // the 64-bit fields
  uint32 s_blocks_count_hi;
  uint32 s_r_blocks_count_hi;
  uint32 s_free_blocks_count_hi;
  uint16 s_min_extra_isize;
  uint16 s_want_extra_isize;
  uint32 s_flags;
  uint16 s_raid_stride;
  uint16 s_mmp_update_interval;
  uint64 s_mmp_block;
  uint32 s_raid_stripe_width;
  uchar  s_log_groups_per_flex;
  uchar  s_checksum_type;
  uchar  s_encryption_level;
  uchar  s_reserved_pad;
  uint64 s_kbytes_written;
  uint32 s_snapshot_inum;
  uint32 s_snapshot_id;
  uint64 s_snapshot_r_blocks_count;
  uint32 s_snapshot_list;
  uint32 s_error_count;
  uint32 s_first_error_time;
  uint32 s_first_error_ino;
  uint64 s_first_error_block;
  uchar  s_first_error_func[32];
  uint32 s_first_error_line;
  uint32 s_last_error_time;
  uint32 s_last_error_ino;
  uint32 s_last_error_line;
  uint64 s_last_error_block;
  uchar  s_last_error_func[32];
  uchar  s_mount_opts[64];
  uint32 s_usr_quota_inum;
  uint32 s_grp_quota_inum;
  uint32 s_overhead_clusters;
  uint32 s_backup_bgs[2];
  uchar  s_encrypt_algos[4];
  uchar  s_encrypt_pw_salt[16];
  uint32 s_lpf_ino;
  uint32 s_prj_quota_inum;
  uint32 s_checksum_seed;
  uchar  s_wtime_hi;
  uchar  s_mtime_hi;
  uchar  s_mkfs_time_hi;
  uchar  s_lastcheck_hi;
  uchar  s_first_error_time_hi;
  uchar  s_last_error_time_hi;
  uchar  s_first_error_errcode;
  uchar  s_last_error_errcode;
  uint16 s_encoding;
  uint16 s_encoding_flags;
  uint32 s_orphan_file_inum;
  uint32 s_reserved[94];
  uint32 s_checksum;
};

// ---- the group descriptor ----
//
// 32 bytes without the 64bit feature, 64 with it; s_desc_size says which,
// and the _hi halves exist only in the 64-byte form.

struct ext4_group_desc {
  uint32 bg_block_bitmap_lo;
  uint32 bg_inode_bitmap_lo;
  uint32 bg_inode_table_lo;
  uint16 bg_free_blocks_count_lo;
  uint16 bg_free_inodes_count_lo;
  uint16 bg_used_dirs_count_lo;
  uint16 bg_flags;
  uint32 bg_exclude_bitmap_lo;
  uint16 bg_block_bitmap_csum_lo;
  uint16 bg_inode_bitmap_csum_lo;
  uint16 bg_itable_unused_lo;
  uint16 bg_checksum;
  uint32 bg_block_bitmap_hi;
  uint32 bg_inode_bitmap_hi;
  uint32 bg_inode_table_hi;
  uint16 bg_free_blocks_count_hi;
  uint16 bg_free_inodes_count_hi;
  uint16 bg_used_dirs_count_hi;
  uint16 bg_itable_unused_hi;
  uint32 bg_exclude_bitmap_hi;
  uint16 bg_block_bitmap_csum_hi;
  uint16 bg_inode_bitmap_csum_hi;
  uint32 bg_reserved;
};

#define EXT4_MIN_DESC_SIZE    32
#define EXT4_MIN_DESC_SIZE_64BIT 64

// ---- the inode ----
//
// the first 128 bytes are the ext2 inode. anything past that exists only
// when s_inode_size says so and i_extra_isize covers it; the driver checks
// both before it reads a field beyond i_projid's start.

struct ext4_inode {
  uint16 i_mode;
  uint16 i_uid;                  // the low 16 bits; l_i_uid_high has the rest
  uint32 i_size_lo;
  uint32 i_atime;
  uint32 i_ctime;
  uint32 i_mtime;
  uint32 i_dtime;
  uint16 i_gid;
  uint16 i_links_count;
  uint32 i_blocks_lo;            // 512-byte units unless EXT4_HUGE_FILE_FL
  uint32 i_flags;
  uint32 l_i_version;            // osd1, the linux form
  uint32 i_block[EXT4_N_BLOCKS]; // the extent root, or the old block map
  uint32 i_generation;
  uint32 i_file_acl_lo;
  uint32 i_size_high;
  uint32 i_obso_faddr;
  // osd2, the linux form
  uint16 l_i_blocks_high;
  uint16 l_i_file_acl_high;
  uint16 l_i_uid_high;
  uint16 l_i_gid_high;
  uint16 l_i_checksum_lo;
  uint16 l_i_reserved;
  // the extra fields; i_extra_isize says how many of them are present
  uint16 i_extra_isize;
  uint16 i_checksum_hi;
  uint32 i_ctime_extra;          // low 2 bits extend the seconds past 2038
  uint32 i_mtime_extra;
  uint32 i_atime_extra;
  uint32 i_crtime;
  uint32 i_crtime_extra;
  uint32 i_version_hi;
  uint32 i_projid;
};

#define EXT4_INODE_RAW_SIZE   160   // through i_projid: what the driver keeps
#define EXT4_I_BLOCK_OFF      40    // where i_block[] starts inside an inode
#define EXT4_I_BLOCK_SIZE     60    // and how many bytes it is

// a field past the ext2 inode exists only when the inode is long enough to
// hold it and i_extra_isize claims it; both have to be true, because the
// inode size is the file system's and i_extra_isize is this inode's own.
// this is Linux's EXT4_FITS_IN_INODE.
#define EXT4_FIELD_END(field) \
  ((int)__builtin_offsetof(struct ext4_inode, field) + \
   (int)sizeof(((struct ext4_inode *)0)->field))
#define EXT4_FITS_IN_INODE(isize, extra, field) \
  ((int)(isize) >= EXT4_FIELD_END(field) && \
   (int)(extra) >= EXT4_FIELD_END(field) - EXT4_GOOD_OLD_INODE_SIZE)

// the two low bits of an *_extra field are the high bits of the seconds, so
// a 34-bit second count reaches to 2446 instead of stopping in 2038.
#define EXT4_EPOCH_BITS 2
#define EXT4_EPOCH_MASK ((1u << EXT4_EPOCH_BITS) - 1)

// ---- extents ----
//
// i_block[] holds a 60-byte extent tree root when EXT4_EXTENTS_FL is set:
// a header, then eh_entries of either ext4_extent (depth 0) or
// ext4_extent_idx (depth above 0). every node on disk starts with the same
// header and carries the same magic, which is what makes a corrupt tree
// detectable instead of followed.

#define EXT4_EXT_MAGIC        0xF30A
#define EXT4_MAX_EXT_DEPTH    5      // what Linux allows; a bound on the walk
#define EXT4_INIT_MAX_LEN     32768  // an extent longer than this is unwritten
#define EXT4_MAX_EXT_LEN      32767  // the longest an unwritten extent may be

struct ext4_extent_header {
  uint16 eh_magic;               // 0xF30A
  uint16 eh_entries;             // entries in use
  uint16 eh_max;                 // entries this node has room for
  uint16 eh_depth;               // 0 for a leaf of real extents
  uint32 eh_generation;
};

struct ext4_extent_idx {
  uint32 ei_block;               // the first file block this child covers
  uint32 ei_leaf_lo;             // the disk block of the child node
  uint16 ei_leaf_hi;
  uint16 ei_unused;
};

struct ext4_extent {
  uint32 ee_block;               // the first file block
  uint16 ee_len;                 // length; over 32768 means unwritten
  uint16 ee_start_hi;
  uint32 ee_start_lo;
};

// ---- directories ----
//
// a linear directory is a sequence of these, each rec_len bytes long,
// filling a block exactly; an entry with inode 0 is free space, and the
// metadata_csum tail is such an entry with rec_len 12.

struct ext4_dir_entry_2 {
  uint32 inode;
  uint16 rec_len;
  uchar  name_len;
  uchar  file_type;              // only meaningful with the filetype feature
  char   name[EXT4_NAME_LEN];    // name_len bytes of it, not terminated
};

#define EXT4_DIR_ENTRY_HEADER_LEN 8
#define EXT4_DIR_REC_LEN(namelen) (((namelen) + 8 + 3) & ~3)

// ---- hash-indexed directories ----
//
// block 0 of an htree directory looks like a linear block holding just "."
// and "..", where ".." has a rec_len covering the rest of the block; the
// index lives inside that record. an interior node hides the same way
// behind a single free entry spanning the whole block. that is why walking
// the blocks linearly sees only the leaves.

struct ext4_dx_root_info {
  uint32 reserved_zero;
  uchar  hash_version;
  uchar  info_length;            // 8
  uchar  indirect_levels;        // 0, 1, or 2 with largedir
  uchar  unused_flags;
};

// one child pointer: the lowest hash it covers and the *file* block, not
// the disk block, of the node or leaf that covers it.
struct ext4_dx_entry {
  uint32 hash;
  uint32 block;
};

// the first dx_entry of a node is not an entry: its hash word holds the
// count and the limit instead, and only its block word is real.
struct ext4_dx_countlimit {
  uint16 limit;
  uint16 count;
};

// with metadata_csum an index block ends with this, in the space of one
// entry that the limit therefore does not count. it is not the tail a
// leaf block carries: a leaf is an ordinary directory block and has the
// twelve-byte fake entry, an index block has these eight bytes.
struct ext4_dx_tail {
  uint32 dt_reserved;
  uint32 dt_checksum;
};

#define EXT4_DX_ROOT_INFO_OFF   24   // "." (12) + ".." (12)
#define EXT4_DX_ROOT_ENTRIES_OFF 32  // plus the 8-byte dx_root_info
#define EXT4_DX_NODE_ENTRIES_OFF 8   // past the one fake entry

// ---- proof that the structures above are the format ----
//
// ext4 has no implicit padding, so if the compiler inserted any, these
// fail. the offsets are the ones fs/ext4/ext4.h produces.

#define EXT4_OFF(t, f) __builtin_offsetof(struct t, f)

_Static_assert(EXT4_OFF(ext4_super_block, s_magic) == 56, "s_magic moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_first_ino) == 84, "s_first_ino moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_inode_size) == 88, "s_inode_size moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_feature_compat) == 92, "s_feature_compat moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_feature_incompat) == 96, "s_feature_incompat moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_feature_ro_compat) == 100, "s_feature_ro_compat moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_uuid) == 104, "s_uuid moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_hash_seed) == 236, "s_hash_seed moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_def_hash_version) == 252, "s_def_hash_version moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_desc_size) == 254, "s_desc_size moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_mkfs_time) == 264, "s_mkfs_time moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_blocks_count_hi) == 336, "s_blocks_count_hi moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_flags) == 352, "s_flags moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_mmp_block) == 360, "s_mmp_block moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_kbytes_written) == 376, "s_kbytes_written moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_mount_opts) == 512, "s_mount_opts moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_checksum_seed) == 624, "s_checksum_seed moved");
_Static_assert(EXT4_OFF(ext4_super_block, s_checksum) == 1020, "s_checksum moved");
_Static_assert(sizeof(struct ext4_super_block) == 1024, "superblock is not 1024 bytes");

_Static_assert(EXT4_OFF(ext4_group_desc, bg_flags) == 18, "bg_flags moved");
_Static_assert(EXT4_OFF(ext4_group_desc, bg_checksum) == 30, "bg_checksum moved");
_Static_assert(EXT4_OFF(ext4_group_desc, bg_inode_table_hi) == 40, "bg_inode_table_hi moved");
_Static_assert(sizeof(struct ext4_group_desc) == 64, "group descriptor is not 64 bytes");

_Static_assert(EXT4_OFF(ext4_inode, i_blocks_lo) == 28, "i_blocks_lo moved");
_Static_assert(EXT4_OFF(ext4_inode, i_block) == EXT4_I_BLOCK_OFF, "i_block moved");
_Static_assert(EXT4_OFF(ext4_inode, i_generation) == 100, "i_generation moved");
_Static_assert(EXT4_OFF(ext4_inode, i_size_high) == 108, "i_size_high moved");
_Static_assert(EXT4_OFF(ext4_inode, l_i_blocks_high) == 116, "l_i_blocks_high moved");
_Static_assert(EXT4_OFF(ext4_inode, i_extra_isize) == 128, "i_extra_isize moved");
_Static_assert(EXT4_OFF(ext4_inode, i_crtime) == 144, "i_crtime moved");
_Static_assert(sizeof(struct ext4_inode) == EXT4_INODE_RAW_SIZE, "inode is not 160 bytes");

_Static_assert(sizeof(struct ext4_extent_header) == 12, "extent header is not 12 bytes");
_Static_assert(sizeof(struct ext4_extent_idx) == 12, "extent index is not 12 bytes");
_Static_assert(sizeof(struct ext4_extent) == 12, "extent is not 12 bytes");
_Static_assert(EXT4_OFF(ext4_dir_entry_2, name) == 8, "dirent name moved");
_Static_assert(sizeof(struct ext4_dx_root_info) == 8, "dx_root_info is not 8 bytes");
_Static_assert(sizeof(struct ext4_dx_entry) == 8, "dx_entry is not 8 bytes");
_Static_assert(sizeof(struct ext4_dx_tail) == 8, "dx_tail is not 8 bytes");


// the checksum a directory block carries, in the last twelve bytes,
// shaped like an entry whose file type no real entry can have so that a
// reader which does not know about it walks straight past the end.
struct ext4_dir_entry_tail {
  uint32 det_reserved_zero1;        // must be 0, so it never looks like an inode
  uint16 det_rec_len;               // 12
  uchar det_reserved_zero2;         // must be 0, the name length
  uchar det_reserved_ft;            // EXT4_FT_DIR_CSUM
  uint32 det_checksum;              // crc32c(seed, inode, generation, block up to here)
};

#define EXT4_FT_DIR_CSUM 0xde
#define EXT4_DIR_TAIL_SIZE 12

// the checksum of an extent block, after every entry slot the header
// allows rather than after the ones in use
struct ext4_extent_tail {
  uint32 et_checksum;
};

#endif
