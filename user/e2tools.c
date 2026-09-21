// dumpe2fs, tune2fs, fsck.ext4 and resize2fs: what an ext4 file
// system says about itself, and the few things that can be changed
// without rewriting it.
//
//   dumpe2fs [-h] [-b] DEVICE        the superblock, and the groups
//   tune2fs -l DEVICE                the superblock alone
//   tune2fs [-L LABEL] [-U UUID] [-c N] [-i N] [-m PCT] [-e BEHAVIOUR] DEVICE
//   fsck.ext4 [-n] [-f] [-p] [-y] DEVICE   look it over
//   resize2fs DEVICE [BLOCKS]        say what it would take
//
// this reads and writes the superblock and reads the group
// descriptors; it does not rebuild anything. what it checks is what
// can be checked by reading: the magic and the sizes, that the
// counts in the superblock agree with the sum of the groups, that
// the bitmaps and inode tables of each group lie inside the file
// system, and that the checksums are right. a file system that needs
// more than that is one to take to Linux's e2fsck, and this says so
// rather than pretending.
//
// mkfs.ext4 is the same program that writes the images this system
// boots from (mkfs/mkext4.c), built for here as well as for the build
// host, so an image made on the machine is the same shape as one made
// beside it.
#include "user/user.h"
#include "mkfs/ext4_ondisk.h"

static int verbose = 1, header_only, force, no_changes = 1, preen;
static const char *new_label, *new_uuid, *new_errors;
static long new_max_mounts = -1, new_interval = -1, new_reserved = -1;
static int status;

static const char *me = "dumpe2fs";

// ---- crc32c, the polynomial ext4 checks everything with ----

static uint32 crc_table[256];
static int crc_ready;

static void
crc_init(void)
{
  for (uint32 i = 0; i < 256; i++) {
    uint32 c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0x82f63b78u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
  crc_ready = 1;
}

// ext4 passes the seed in raw and takes the result raw: there is no
// inversion at either end, which is what makes the seed the volume's
// own value rather than a constant
static uint32
crc32c(uint32 crc, const void *data, int len)
{
  const unsigned char *p = data;

  if (!crc_ready)
    crc_init();
  for (int i = 0; i < len; i++)
    crc = (crc >> 8) ^ crc_table[(crc ^ p[i]) & 0xff];
  return crc;
}

// ---- reading ----

static int
read_at(int fd, void *buf, int n, long long off)
{
  if (lseek(fd, off, SEEK_SET) < 0)
    return -1;
  int done = 0;
  while (done < n) {
    int r = read(fd, (char *)buf + done, n - done);
    if (r <= 0)
      return -1;
    done += r;
  }
  return 0;
}

static int
write_at(int fd, const void *buf, int n, long long off)
{
  if (lseek(fd, off, SEEK_SET) < 0)
    return -1;
  int done = 0;
  while (done < n) {
    int r = write(fd, (const char *)buf + done, n - done);
    if (r <= 0)
      return -1;
    done += r;
  }
  return 0;
}

static void
uuid_string(const unsigned char *u, char *out)
{
  static const char *hex = "0123456789abcdef";
  int k = 0;

  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[k++] = '-';
    out[k++] = hex[u[i] >> 4];
    out[k++] = hex[u[i] & 15];
  }
  out[k] = 0;
}

static int
parse_uuid(const char *s, unsigned char *out)
{
  int k = 0;

  for (const char *p = s; *p && k < 16; p++) {
    if (*p == '-')
      continue;
    int hi = -1, lo = -1;
    const char *q = p;
    for (int half = 0; half < 2; half++) {
      int c = *q;
      int v = (c >= '0' && c <= '9')   ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                       : -1;
      if (v < 0)
        return -1;
      if (half == 0)
        hi = v;
      else
        lo = v;
      q++;
    }
    out[k++] = (unsigned char)((hi << 4) | lo);
    p++;
  }
  return k == 16 ? 0 : -1;
}

static const char *
time_string(uint32 t, char *buf, int size)
{
  time_t tt = (time_t)t;
  struct tm tm;

  if (t == 0) {
    snprintf(buf, size, "n/a");
    return buf;
  }
  gmtime_r(&tt, &tm);
  strftime(buf, size, "%a %b %e %H:%M:%S %Y", &tm);
  return buf;
}

struct fs {
  int fd;
  struct ext4_super_block sb;
  uint32 blocksize;
  uint32 groups;
  uint32 desc_size;
  long long size;
};

static int
open_fs(const char *path, struct fs *fs, int writable)
{
  memset(fs, 0, sizeof(*fs));
  fs->fd = open(path, writable ? O_RDWR : O_RDONLY);
  if (fs->fd < 0) {
    fprintf(stderr, "%s: %s: %s\n", me, path, strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fs->fd, &st) == 0)
    fs->size = st.size;

  if (read_at(fs->fd, &fs->sb, sizeof(fs->sb), 1024) < 0) {
    fprintf(stderr, "%s: %s: cannot read the superblock\n", me, path);
    close(fs->fd);
    return -1;
  }
  if (fs->sb.s_magic != EXT4_SUPER_MAGIC) {
    fprintf(stderr, "%s: %s: bad magic number -- it is not ext4\n", me, path);
    close(fs->fd);
    return -1;
  }
  fs->blocksize = 1024u << fs->sb.s_log_block_size;
  uint64 total = fs->sb.s_blocks_count_lo |
                 ((uint64)fs->sb.s_blocks_count_hi << 32);
  uint32 per = fs->sb.s_blocks_per_group;
  fs->groups = per ? (uint32)((total + per - 1) / per) : 0;
  fs->desc_size = (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                      ? fs->sb.s_desc_size : 32;
  if (fs->desc_size == 0)
    fs->desc_size = 32;
  return 0;
}

// the superblock's own checksum covers everything before it
static uint32
super_csum(const struct ext4_super_block *sb)
{
  return crc32c(EXT4_CSUM_SEED_INIT, sb,
                (int)((const char *)&sb->s_checksum - (const char *)sb));
}

static void
print_features(const struct ext4_super_block *sb)
{
  static const struct {
    uint32 bit;
    const char *name;
  } compat[] = {
    {EXT4_FEATURE_COMPAT_DIR_PREALLOC, "dir_prealloc"},
    {EXT4_FEATURE_COMPAT_HAS_JOURNAL, "has_journal"},
    {EXT4_FEATURE_COMPAT_EXT_ATTR, "ext_attr"},
    {EXT4_FEATURE_COMPAT_RESIZE_INODE, "resize_inode"},
    {EXT4_FEATURE_COMPAT_DIR_INDEX, "dir_index"},
    {0, 0}
  }, incompat[] = {
    {EXT4_FEATURE_INCOMPAT_COMPRESSION, "compression"},
    {EXT4_FEATURE_INCOMPAT_FILETYPE, "filetype"},
    {EXT4_FEATURE_INCOMPAT_RECOVER, "recover"},
    {EXT4_FEATURE_INCOMPAT_JOURNAL_DEV, "journal_dev"},
    {EXT4_FEATURE_INCOMPAT_META_BG, "meta_bg"},
    {EXT4_FEATURE_INCOMPAT_EXTENTS, "extent"},
    {EXT4_FEATURE_INCOMPAT_64BIT, "64bit"},
    {EXT4_FEATURE_INCOMPAT_MMP, "mmp"},
    {EXT4_FEATURE_INCOMPAT_FLEX_BG, "flex_bg"},
    {EXT4_FEATURE_INCOMPAT_EA_INODE, "ea_inode"},
    {EXT4_FEATURE_INCOMPAT_CSUM_SEED, "csum_seed"},
    {EXT4_FEATURE_INCOMPAT_LARGEDIR, "largedir"},
    {EXT4_FEATURE_INCOMPAT_INLINE_DATA, "inline_data"},
    {EXT4_FEATURE_INCOMPAT_ENCRYPT, "encrypt"},
    {0, 0}
  }, ro_compat[] = {
    {EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER, "sparse_super"},
    {EXT4_FEATURE_RO_COMPAT_LARGE_FILE, "large_file"},
    {EXT4_FEATURE_RO_COMPAT_BTREE_DIR, "btree_dir"},
    {EXT4_FEATURE_RO_COMPAT_HUGE_FILE, "huge_file"},
    {EXT4_FEATURE_RO_COMPAT_GDT_CSUM, "uninit_bg"},
    {EXT4_FEATURE_RO_COMPAT_DIR_NLINK, "dir_nlink"},
    {EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE, "extra_isize"},
    {EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT, "snapshot"},
    {EXT4_FEATURE_RO_COMPAT_QUOTA, "quota"},
    {EXT4_FEATURE_RO_COMPAT_BIGALLOC, "bigalloc"},
    {EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, "metadata_csum"},
    {EXT4_FEATURE_RO_COMPAT_REPLICA, "replica"},
    {EXT4_FEATURE_RO_COMPAT_READONLY, "read-only"},
    {EXT4_FEATURE_RO_COMPAT_PROJECT, "project"},
    {EXT4_FEATURE_RO_COMPAT_VERITY, "verity"},
    {0, 0}
  };

  printf("Filesystem features:     ");
  for (int i = 0; compat[i].name; i++)
    if (sb->s_feature_compat & compat[i].bit)
      printf("%s ", compat[i].name);
  for (int i = 0; incompat[i].name; i++)
    if (sb->s_feature_incompat & incompat[i].bit)
      printf("%s ", incompat[i].name);
  for (int i = 0; ro_compat[i].name; i++)
    if (sb->s_feature_ro_compat & ro_compat[i].bit)
      printf("%s ", ro_compat[i].name);
  printf("\n");
}

static void
print_super(const struct fs *fs)
{
  const struct ext4_super_block *sb = &fs->sb;
  char buf[64], uuid[40];
  uint64 blocks = sb->s_blocks_count_lo | ((uint64)sb->s_blocks_count_hi << 32);
  uint64 freeb = sb->s_free_blocks_count_lo |
                 ((uint64)sb->s_free_blocks_count_hi << 32);
  uint64 resv = sb->s_r_blocks_count_lo |
                ((uint64)sb->s_r_blocks_count_hi << 32);

  uuid_string(sb->s_uuid, uuid);
  printf("Filesystem volume name:   %s\n",
         sb->s_volume_name[0] ? sb->s_volume_name : "<none>");
  printf("Last mounted on:          %s\n",
         sb->s_last_mounted[0] ? sb->s_last_mounted : "<not available>");
  printf("Filesystem UUID:          %s\n", uuid);
  printf("Filesystem magic number:  0x%04x\n", sb->s_magic);
  printf("Filesystem revision #:    %u\n", sb->s_rev_level);
  print_features(sb);
  printf("Filesystem state:         %s\n",
         (sb->s_state & 1) ? "clean" : "not clean");
  printf("Errors behavior:          %s\n",
         sb->s_errors == 2 ? "Remount read-only"
         : sb->s_errors == 3 ? "Panic" : "Continue");
  printf("Filesystem OS type:       %s\n", sb->s_creator_os == 0 ? "Linux" : "other");
  printf("Inode count:              %u\n", sb->s_inodes_count);
  printf("Block count:              %llu\n", (unsigned long long)blocks);
  printf("Reserved block count:     %llu\n", (unsigned long long)resv);
  printf("Free blocks:              %llu\n", (unsigned long long)freeb);
  printf("Free inodes:              %u\n", sb->s_free_inodes_count);
  printf("First block:              %u\n", sb->s_first_data_block);
  printf("Block size:               %u\n", fs->blocksize);
  printf("Blocks per group:         %u\n", sb->s_blocks_per_group);
  printf("Inodes per group:         %u\n", sb->s_inodes_per_group);
  printf("Inode blocks per group:   %u\n",
         sb->s_inodes_per_group * sb->s_inode_size / fs->blocksize);
  printf("Flex block group size:    %u\n", 1u << sb->s_log_groups_per_flex);
  printf("Mount count:              %u\n", sb->s_mnt_count);
  printf("Maximum mount count:      %d\n", (short)sb->s_max_mnt_count);
  printf("Last write time:          %s\n", time_string(sb->s_wtime, buf, sizeof(buf)));
  printf("Last mount time:          %s\n", time_string(sb->s_mtime, buf, sizeof(buf)));
  printf("Last checked:             %s\n", time_string(sb->s_lastcheck, buf, sizeof(buf)));
  printf("Check interval:           %u\n", sb->s_checkinterval);
  printf("First inode:              %u\n", sb->s_first_ino);
  printf("Inode size:               %u\n", sb->s_inode_size);
  printf("Required extra isize:     %u\n", sb->s_min_extra_isize);
  printf("Desired extra isize:      %u\n", sb->s_want_extra_isize);
  if (sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)
    printf("Journal inode:            %u\n", sb->s_journal_inum);
  printf("Default mount options:    %s\n",
         (sb->s_default_mount_opts & 0x0008) ? "user_xattr acl" : "(none)");
  printf("Checksum type:            %s\n",
         sb->s_checksum_type == 1 ? "crc32c" : "none");
  printf("Checksum:                 0x%08x\n", sb->s_checksum);
  printf("Checksum verified:        %s\n",
         (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) == 0
             ? "n/a"
         : (super_csum(sb) == sb->s_checksum ? "yes" : "NO"));
}

// one group descriptor, read from the table after the superblock
static int
read_group(const struct fs *fs, uint32 g, struct ext4_group_desc *gd)
{
  long long table = (long long)(fs->blocksize == 1024 ? 2 : 1) * fs->blocksize;
  long long off = table + (long long)g * fs->desc_size;

  memset(gd, 0, sizeof(*gd));
  return read_at(fs->fd, gd, fs->desc_size > (uint32)sizeof(*gd)
                                 ? (int)sizeof(*gd) : (int)fs->desc_size, off);
}

static void
print_groups(const struct fs *fs)
{
  const struct ext4_super_block *sb = &fs->sb;

  printf("\n");
  for (uint32 g = 0; g < fs->groups; g++) {
    struct ext4_group_desc gd;
    if (read_group(fs, g, &gd) < 0) {
      printf("Group %u: (cannot be read)\n", g);
      continue;
    }
    uint64 first = sb->s_first_data_block + (uint64)g * sb->s_blocks_per_group;
    uint64 last = first + sb->s_blocks_per_group - 1;
    uint64 blocks = sb->s_blocks_count_lo | ((uint64)sb->s_blocks_count_hi << 32);
    if (last >= blocks)
      last = blocks - 1;
    uint64 bbitmap = gd.bg_block_bitmap_lo |
                     ((uint64)gd.bg_block_bitmap_hi << 32);
    uint64 ibitmap = gd.bg_inode_bitmap_lo |
                     ((uint64)gd.bg_inode_bitmap_hi << 32);
    uint64 itable = gd.bg_inode_table_lo |
                    ((uint64)gd.bg_inode_table_hi << 32);
    uint32 freeb = gd.bg_free_blocks_count_lo |
                   ((uint32)gd.bg_free_blocks_count_hi << 16);
    uint32 freei = gd.bg_free_inodes_count_lo |
                   ((uint32)gd.bg_free_inodes_count_hi << 16);
    uint32 dirs = gd.bg_used_dirs_count_lo |
                  ((uint32)gd.bg_used_dirs_count_hi << 16);

    printf("Group %u: (Blocks %llu-%llu)\n", g, (unsigned long long)first,
           (unsigned long long)last);
    printf("  Block bitmap at %llu, Inode bitmap at %llu\n",
           (unsigned long long)bbitmap, (unsigned long long)ibitmap);
    printf("  Inode table at %llu\n", (unsigned long long)itable);
    printf("  %u free blocks, %u free inodes, %u directories\n", freeb, freei,
           dirs);
  }
}

// ---- looking it over ----

static void
problem(const char *fmt, long long a, long long b)
{
  printf("  ");
  printf(fmt, a, b);
  printf("\n");
  status |= 4;
}

static int
check_fs(struct fs *fs, const char *path)
{
  const struct ext4_super_block *sb = &fs->sb;
  uint64 blocks = sb->s_blocks_count_lo | ((uint64)sb->s_blocks_count_hi << 32);
  uint64 freeb = sb->s_free_blocks_count_lo |
                 ((uint64)sb->s_free_blocks_count_hi << 32);
  int problems = 0;

  if (!preen)
    printf("%s: checking %s\n", me, path);

  // the sizes have to agree with each other and with the file
  if (fs->blocksize < 1024 || fs->blocksize > 65536) {
    problem("block size %lld makes no sense%s", fs->blocksize, 0);
    problems++;
  }
  if (sb->s_inode_size < 128 || (sb->s_inode_size & (sb->s_inode_size - 1))) {
    problem("inode size %lld makes no sense%s", sb->s_inode_size, 0);
    problems++;
  }
  if (sb->s_blocks_per_group == 0 || sb->s_inodes_per_group == 0) {
    problem("a group holds no blocks or no inodes%s%s", 0, 0);
    problems++;
  }
  if (fs->size > 0 && (long long)(blocks * fs->blocksize) > fs->size) {
    printf("  the superblock claims %llu blocks of %u bytes, which is more "
           "than the %lld bytes there are\n",
           (unsigned long long)blocks, fs->blocksize, fs->size);
    status |= 4;
    problems++;
  }
  if (sb->s_inodes_count != sb->s_inodes_per_group * fs->groups) {
    printf("  inode count %u is not %u groups of %u\n", sb->s_inodes_count,
           fs->groups, sb->s_inodes_per_group);
    status |= 4;
    problems++;
  }
  if ((sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) &&
      super_csum(sb) != sb->s_checksum) {
    printf("  the superblock checksum is wrong\n");
    status |= 4;
    problems++;
  }

  // the counts in the groups have to add up to the ones in the
  // superblock, and every structure has to lie inside the file system
  uint64 sum_free_blocks = 0;
  uint32 sum_free_inodes = 0;
  for (uint32 g = 0; g < fs->groups; g++) {
    struct ext4_group_desc gd;
    if (read_group(fs, g, &gd) < 0) {
      printf("  group %u cannot be read\n", g);
      status |= 4;
      problems++;
      continue;
    }
    uint64 bb = gd.bg_block_bitmap_lo | ((uint64)gd.bg_block_bitmap_hi << 32);
    uint64 ib = gd.bg_inode_bitmap_lo | ((uint64)gd.bg_inode_bitmap_hi << 32);
    uint64 it = gd.bg_inode_table_lo | ((uint64)gd.bg_inode_table_hi << 32);
    if (bb >= blocks || ib >= blocks || it >= blocks) {
      printf("  group %u points outside the file system\n", g);
      status |= 4;
      problems++;
    }
    sum_free_blocks += gd.bg_free_blocks_count_lo |
                       ((uint32)gd.bg_free_blocks_count_hi << 16);
    sum_free_inodes += gd.bg_free_inodes_count_lo |
                       ((uint32)gd.bg_free_inodes_count_hi << 16);
  }
  if (sum_free_blocks != freeb) {
    printf("  free blocks: the superblock says %llu, the groups add to %llu\n",
           (unsigned long long)freeb, (unsigned long long)sum_free_blocks);
    status |= 4;
    problems++;
  }
  if (sum_free_inodes != sb->s_free_inodes_count) {
    printf("  free inodes: the superblock says %u, the groups add to %u\n",
           sb->s_free_inodes_count, sum_free_inodes);
    status |= 4;
    problems++;
  }

  if (!(sb->s_state & 1))
    printf("  the file system was not cleanly unmounted\n");
  if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER)
    printf("  the journal has entries that have not been replayed\n");

  if (problems == 0) {
    if (!preen)
      printf("%s: %s: clean, %u/%u files, %llu/%llu blocks\n", me, path,
             sb->s_inodes_count - sb->s_free_inodes_count, sb->s_inodes_count,
             (unsigned long long)(blocks - freeb), (unsigned long long)blocks);
  } else {
    printf("%s: %d problem%s found. this reads the file system but does not\n"
           "rebuild it; take the image to Linux's e2fsck to repair it.\n", me,
           problems, problems == 1 ? "" : "s");
  }
  return problems;
}

// ---- changing the few things that can be changed ----

static int
tune(struct fs *fs, const char *path)
{
  struct ext4_super_block *sb = &fs->sb;
  int changed = 0;

  if (new_label) {
    memset(sb->s_volume_name, 0, sizeof(sb->s_volume_name));
    snprintf(sb->s_volume_name, sizeof(sb->s_volume_name), "%s", new_label);
    changed = 1;
  }
  if (new_uuid) {
    unsigned char u[16];
    if (strcmp(new_uuid, "null") == 0 || strcmp(new_uuid, "clear") == 0) {
      memset(u, 0, sizeof(u));
    } else if (strcmp(new_uuid, "random") == 0 || strcmp(new_uuid, "time") == 0) {
      if (getrandom(u, sizeof(u), 0) != sizeof(u)) {
        fprintf(stderr, "%s: cannot get random bytes\n", me);
        return -1;
      }
      u[6] = (u[6] & 0x0f) | 0x40;   // version 4
      u[8] = (u[8] & 0x3f) | 0x80;   // variant 1
    } else if (parse_uuid(new_uuid, u) < 0) {
      fprintf(stderr, "%s: %s is not a uuid\n", me, new_uuid);
      return -1;
    }
    // every structure's checksum is seeded from the uuid, so changing
    // it on a file system with metadata_csum would make every one of
    // them wrong
    if (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
      fprintf(stderr, "%s: this file system checksums its metadata from the\n"
                      "uuid, so changing the uuid would make every checksum\n"
                      "wrong. it is refused rather than done by halves.\n", me);
      return -1;
    }
    memmove(sb->s_uuid, u, 16);
    changed = 1;
  }
  if (new_max_mounts >= 0) {
    sb->s_max_mnt_count = (uint16)(short)new_max_mounts;
    changed = 1;
  }
  if (new_interval >= 0) {
    sb->s_checkinterval = (uint32)new_interval;
    changed = 1;
  }
  if (new_reserved >= 0) {
    uint64 blocks = sb->s_blocks_count_lo | ((uint64)sb->s_blocks_count_hi << 32);
    uint64 r = blocks * (uint64)new_reserved / 100;
    sb->s_r_blocks_count_lo = (uint32)r;
    sb->s_r_blocks_count_hi = (uint32)(r >> 32);
    changed = 1;
  }
  if (new_errors) {
    sb->s_errors = strcmp(new_errors, "remount-ro") == 0   ? 2
                   : strcmp(new_errors, "panic") == 0 ? 3 : 1;
    changed = 1;
  }

  if (!changed)
    return 0;
  if (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
    sb->s_checksum = super_csum(sb);
  if (write_at(fs->fd, sb, sizeof(*sb), 1024) < 0) {
    fprintf(stderr, "%s: %s: cannot write the superblock: %s\n", me, path,
            strerror(errno));
    return -1;
  }
  if (verbose)
    printf("%s: the superblock of %s is written\n", me, path);
  return 0;
}

// ---- resize ----

static int
resize(struct fs *fs, const char *path, long long want)
{
  const struct ext4_super_block *sb = &fs->sb;
  uint64 blocks = sb->s_blocks_count_lo | ((uint64)sb->s_blocks_count_hi << 32);

  if (want == 0) {
    // with no size, say how small it could be made
    uint64 freeb = sb->s_free_blocks_count_lo |
                   ((uint64)sb->s_free_blocks_count_hi << 32);
    printf("Estimated minimum size of the filesystem: %llu\n",
           (unsigned long long)(blocks - freeb));
    return 0;
  }
  printf("%s: %s is %llu blocks of %u bytes.\n", me, path,
         (unsigned long long)blocks, fs->blocksize);
  if ((uint64)want == blocks) {
    printf("The filesystem is already %llu blocks long. Nothing to do!\n",
           (unsigned long long)blocks);
    return 0;
  }
  // growing means new groups, new bitmaps and a new descriptor table;
  // shrinking means moving every block that is in the way. neither is
  // something to do by halves.
  fprintf(stderr,
          "%s: this tool does not move blocks or build new groups, so it\n"
          "cannot %s a file system. Make a new one with mkfs.ext4 and copy\n"
          "the files across, or resize it on Linux.\n",
          me, (uint64)want > blocks ? "grow" : "shrink");
  return 1;
}

static void
usage(void)
{
  fprintf(stderr,
          "usage: dumpe2fs [-h] DEVICE\n"
          "       tune2fs -l DEVICE\n"
          "       tune2fs [-L LABEL] [-U UUID] [-c N] [-i N] [-m PCT]"
          " [-e BEHAVIOUR] DEVICE\n"
          "       fsck.ext4 [-nfpy] DEVICE\n"
          "       resize2fs DEVICE [BLOCKS]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  int i = 1, list_only = 0;

  me = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  int is_fsck = strncmp(me, "fsck", 4) == 0 || strcmp(me, "e2fsck") == 0;
  int is_tune = strncmp(me, "tune2fs", 7) == 0;
  int is_resize = strncmp(me, "resize2fs", 9) == 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'h': header_only = 1; break;
      case 'l': list_only = 1; break;
      case 'f': force = 1; break;
      case 'n': no_changes = 1; break;
      case 'y': no_changes = 0; break;
      case 'p': case 'a': preen = 1; break;
      case 'v': verbose = 1; break;
      case 'b': break;   // the backup superblocks; there is one reader here
      case 'M': case 'P': break;
      case 'L': case 'U': case 'c': case 'i': case 'm': case 'e': case 'd': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          usage();
        switch (*p) {
        case 'L': new_label = v; break;
        case 'U': new_uuid = v; break;
        case 'c': new_max_mounts = atol(v); break;
        case 'i': new_interval = atol(v) * 86400; break;
        case 'm': new_reserved = atol(v); break;
        case 'e': new_errors = v; break;
        }
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }
  if (i >= argc)
    usage();

  const char *path = argv[i++];
  int want_write = is_tune && !list_only &&
                   (new_label || new_uuid || new_errors ||
                    new_max_mounts >= 0 || new_interval >= 0 ||
                    new_reserved >= 0);

  struct fs fs;
  if (open_fs(path, &fs, want_write) < 0)
    exit(8);

  if (is_fsck) {
    check_fs(&fs, path);
    close(fs.fd);
    exit(status);
  }
  if (is_resize) {
    long long want = i < argc ? atoll(argv[i]) : 0;
    int r = resize(&fs, path, want);
    close(fs.fd);
    exit(r);
  }
  if (want_write) {
    int r = tune(&fs, path);
    close(fs.fd);
    exit(r < 0 ? 1 : 0);
  }

  print_super(&fs);
  if (!header_only && !is_tune)
    print_groups(&fs);
  close(fs.fd);
  exit(0);
}
