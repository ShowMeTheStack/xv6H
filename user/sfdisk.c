// sfdisk, and fdisk -l: read and write a partition table.
//
//   sfdisk -l DEVICE            what the table says
//   sfdisk [--label gpt] DEVICE < script
//
// the script is one line per partition, in sfdisk's own shape:
//
//   start=2048, size=20480, type=83
//   start=22528, size=20480, type=83
//
// start and size are in 512-byte sectors; a missing start follows the
// last partition, a missing size fills the disk. type is the MBR byte
// (83 for Linux, 0c for FAT32) or, for GPT, "L" for a Linux file
// system partition. after writing, the kernel is asked to read the
// table again, so /dev/vdb1 exists when this returns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define BLKGETSIZE64 0x80081272
#define BLKSSZGET    0x1268
#define BLKRRPART    0x125f

#define MAXPART 32
#define SECTOR 512

struct part {
  unsigned long long start, size;
  int type;      // the MBR type byte
  int gpt_linux; // for GPT: a Linux file system partition
};

// the GUID of a Linux file system partition, as GPT stores it
static const unsigned char GPT_LINUX[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                            0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

static unsigned crc32_tab[256];
static int crc32_ready;

static void
crc32_init(void)
{
  for (unsigned i = 0; i < 256; i++) {
    unsigned c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc32_tab[i] = c;
  }
  crc32_ready = 1;
}

// the ordinary CRC-32 that GPT uses, which is not the crc32c ext4 uses
static unsigned
crc32(const void *data, int len)
{
  const unsigned char *p = data;
  unsigned c = 0xffffffffu;

  if (!crc32_ready)
    crc32_init();
  while (len-- > 0)
    c = crc32_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

static void
put32(unsigned char *p, unsigned v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static void
put64(unsigned char *p, unsigned long long v)
{
  put32(p, (unsigned)v);
  put32(p + 4, (unsigned)(v >> 32));
}

static unsigned
get32(const unsigned char *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long
get64(const unsigned char *p)
{
  return (unsigned long long)get32(p) | ((unsigned long long)get32(p + 4) << 32);
}

// ---- listing ----

static int
list(const char *dev)
{
  unsigned char mbr[SECTOR], hdr[SECTOR];
  unsigned long long size = 0;
  int fd = open(dev, O_RDONLY);

  if (fd < 0) {
    fprintf(stderr, "sfdisk: %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (ioctl(fd, BLKGETSIZE64, &size) < 0)
    size = 0;
  if (pread(fd, mbr, SECTOR, 0) != SECTOR) {
    fprintf(stderr, "sfdisk: %s: cannot read sector 0\n", dev);
    close(fd);
    return 1;
  }
  printf("Disk %s: %llu bytes, %llu sectors\n", dev, size, size / SECTOR);

  if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
    printf("no partition table\n");
    close(fd);
    return 0;
  }

  // a protective MBR (one partition of type 0xee) means the real table
  // is the GPT in sector 1
  if (mbr[446 + 4] == 0xee) {
    if (pread(fd, hdr, SECTOR, SECTOR) != SECTOR || memcmp(hdr, "EFI PART", 8) != 0) {
      printf("a protective MBR with no GPT behind it\n");
      close(fd);
      return 1;
    }
    unsigned long long entries = get64(hdr + 72);
    unsigned n = get32(hdr + 80), esz = get32(hdr + 84);
    printf("Disklabel type: gpt\n");
    printf("%-14s %10s %10s %10s  %s\n", "Device", "Start", "End", "Sectors", "Type");
    for (unsigned i = 0; i < n && i < MAXPART; i++) {
      unsigned char e[128];
      if (esz > sizeof(e))
        break;
      if (pread(fd, e, esz, entries * SECTOR + (unsigned long long)i * esz) != (long)esz)
        break;
      unsigned long long first = get64(e + 32), last = get64(e + 40);
      if (first == 0 && last == 0)
        continue;
      printf("%s%-*u %10llu %10llu %10llu  %s\n", dev, (int)(14 - strlen(dev)), i + 1, first, last,
             last - first + 1, memcmp(e, GPT_LINUX, 16) == 0 ? "Linux filesystem" : "other");
    }
    close(fd);
    return 0;
  }

  printf("Disklabel type: dos\n");
  printf("%-14s %10s %10s %10s  %s\n", "Device", "Start", "End", "Sectors", "Id");
  for (int i = 0; i < 4; i++) {
    const unsigned char *e = mbr + 446 + i * 16;
    unsigned long long first = get32(e + 8), count = get32(e + 12);
    if (e[4] == 0 || count == 0)
      continue;
    printf("%s%-*d %10llu %10llu %10llu  %02x\n", dev, (int)(14 - strlen(dev)), i + 1, first,
           first + count - 1, count, e[4]);
  }
  close(fd);
  return 0;
}

// ---- writing ----

static int
read_script(struct part *p, int max, int gpt)
{
  char line[256];
  int n = 0;

  while (n < max && fgets(line, sizeof(line), stdin)) {
    char *s = line;
    unsigned long long start = 0, size = 0;
    int type = gpt ? 0 : 0x83, have_start = 0, have_size = 0;

    while (*s == ' ' || *s == '\t')
      s++;
    if (*s == 0 || *s == '\n' || *s == '#')
      continue;
    // a bare "-" is a partition with everything defaulted
    for (char *tok = strtok(s, ",\n"); tok; tok = strtok(0, ",\n")) {
      while (*tok == ' ')
        tok++;
      if (strncmp(tok, "start=", 6) == 0) {
        start = strtoull(tok + 6, 0, 0);
        have_start = 1;
      } else if (strncmp(tok, "size=", 5) == 0) {
        size = strtoull(tok + 5, 0, 0);
        have_size = 1;
      } else if (strncmp(tok, "type=", 5) == 0) {
        const char *v = tok + 5;
        if (*v == 'L')
          type = gpt ? 0 : 0x83;
        else
          type = (int)strtoul(v, 0, 16);
      }
    }
    p[n].start = have_start ? start : 0;
    p[n].size = have_size ? size : 0;
    p[n].type = type;
    p[n].gpt_linux = 1;
    n++;
  }
  return n;
}

static int
write_mbr(int fd, struct part *p, int n, unsigned long long sectors)
{
  unsigned char mbr[SECTOR];

  if (n > 4) {
    fprintf(stderr, "sfdisk: a dos label holds four partitions\n");
    return 1;
  }
  memset(mbr, 0, sizeof(mbr));
  for (int i = 0; i < n; i++) {
    unsigned char *e = mbr + 446 + i * 16;
    e[0] = 0;          // not bootable
    e[4] = p[i].type;  // the type byte
    put32(e + 8, (unsigned)p[i].start);
    put32(e + 12, (unsigned)p[i].size);
    // the CHS fields are what a modern table leaves alone
    e[1] = 0xfe;
    e[2] = 0xff;
    e[3] = 0xff;
    e[5] = 0xfe;
    e[6] = 0xff;
    e[7] = 0xff;
  }
  mbr[510] = 0x55;
  mbr[511] = 0xaa;
  (void)sectors;
  if (pwrite(fd, mbr, SECTOR, 0) != SECTOR) {
    fprintf(stderr, "sfdisk: cannot write the table: %s\n", strerror(errno));
    return 1;
  }
  return 0;
}

static int
write_gpt(int fd, struct part *p, int n, unsigned long long sectors)
{
  unsigned char mbr[SECTOR], hdr[SECTOR], *tab;
  unsigned long long first_usable = 34, last_usable = sectors - 34;
  int esz = 128, nent = 128;
  int tabsz = esz * nent;

  if (sectors < 68) {
    fprintf(stderr, "sfdisk: the disk is too small for a gpt\n");
    return 1;
  }
  if ((tab = calloc(1, tabsz)) == 0)
    return 1;

  // the protective MBR: one partition of type 0xee covering the disk
  memset(mbr, 0, sizeof(mbr));
  mbr[446 + 4] = 0xee;
  put32(mbr + 446 + 8, 1);
  put32(mbr + 446 + 12, sectors - 1 > 0xffffffffull ? 0xffffffffu : (unsigned)(sectors - 1));
  mbr[510] = 0x55;
  mbr[511] = 0xaa;

  for (int i = 0; i < n; i++) {
    unsigned char *e = tab + (long)i * esz;
    memmove(e, GPT_LINUX, 16);
    // a unique id per partition: the disk's own plus the index is
    // enough here, and nothing depends on it being random.
    for (int k = 0; k < 16; k++)
      e[16 + k] = (unsigned char)(0x40 + i * 16 + k);
    put64(e + 32, p[i].start);
    put64(e + 40, p[i].start + p[i].size - 1);
    put64(e + 48, 0);
    // the name, in UTF-16: "part" and the number
    const char *nm = "part";
    for (int k = 0; nm[k]; k++)
      e[56 + k * 2] = nm[k];
    e[56 + 4 * 2] = (unsigned char)('1' + i);
  }

  memset(hdr, 0, sizeof(hdr));
  memmove(hdr, "EFI PART", 8);
  put32(hdr + 8, 0x00010000); // revision 1.0
  put32(hdr + 12, 92);        // header size
  put64(hdr + 24, 1);         // this header's own sector
  put64(hdr + 32, sectors - 1);
  put64(hdr + 40, first_usable);
  put64(hdr + 48, last_usable);
  for (int k = 0; k < 16; k++)
    hdr[56 + k] = (unsigned char)(0x10 + k); // the disk's guid
  put64(hdr + 72, 2);                        // the entries start here
  put32(hdr + 80, nent);
  put32(hdr + 84, esz);
  put32(hdr + 88, crc32(tab, tabsz));
  put32(hdr + 16, 0);
  put32(hdr + 16, crc32(hdr, 92));

  int bad = 0;
  bad |= pwrite(fd, mbr, SECTOR, 0) != SECTOR;
  bad |= pwrite(fd, hdr, SECTOR, SECTOR) != SECTOR;
  bad |= pwrite(fd, tab, tabsz, 2ull * SECTOR) != tabsz;

  // the backup: the table just below the last sector, and the header in it
  unsigned long long btab = sectors - 1 - (tabsz / SECTOR);
  put64(hdr + 24, sectors - 1);
  put64(hdr + 32, 1);
  put64(hdr + 72, btab);
  put32(hdr + 16, 0);
  put32(hdr + 16, crc32(hdr, 92));
  bad |= pwrite(fd, tab, tabsz, btab * SECTOR) != tabsz;
  bad |= pwrite(fd, hdr, SECTOR, (sectors - 1) * SECTOR) != SECTOR;
  free(tab);
  if (bad) {
    fprintf(stderr, "sfdisk: cannot write the table: %s\n", strerror(errno));
    return 1;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *dev = 0;
  int gpt = 0, dolist = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0)
      dolist = 1;
    else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
      gpt = strcmp(argv[++i], "gpt") == 0;
    else if (argv[i][0] == '-') {
      fprintf(stderr, "usage: sfdisk [-l] [--label dos|gpt] DEVICE\n");
      return 1;
    } else
      dev = argv[i];
  }
  if (dev == 0) {
    fprintf(stderr, "usage: sfdisk [-l] [--label dos|gpt] DEVICE\n");
    return 1;
  }
  if (dolist)
    return list(dev);

  struct part p[MAXPART];
  int n = read_script(p, MAXPART, gpt);
  if (n == 0) {
    fprintf(stderr, "sfdisk: no partitions in the script\n");
    return 1;
  }

  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "sfdisk: %s: %s\n", dev, strerror(errno));
    return 1;
  }
  unsigned long long size = 0;
  if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
    fprintf(stderr, "sfdisk: %s: cannot ask how big it is\n", dev);
    close(fd);
    return 1;
  }
  unsigned long long sectors = size / SECTOR;

  // fill in what the script left out: a partition starts after the one
  // before it, and the last one runs to the end.
  unsigned long long next = gpt ? 2048 : 2048;
  unsigned long long end = gpt ? sectors - 34 : sectors;
  for (int i = 0; i < n; i++) {
    if (p[i].start == 0)
      p[i].start = next;
    if (p[i].size == 0)
      p[i].size = end - p[i].start;
    if (p[i].start + p[i].size > end) {
      fprintf(stderr, "sfdisk: partition %d runs past the end of the disk\n", i + 1);
      close(fd);
      return 1;
    }
    next = p[i].start + p[i].size;
  }

  int r = gpt ? write_gpt(fd, p, n, sectors) : write_mbr(fd, p, n, sectors);
  if (r == 0 && ioctl(fd, BLKRRPART, 0) < 0)
    fprintf(stderr, "sfdisk: the table is written; the kernel would not read it again: %s\n",
            strerror(errno));
  close(fd);
  if (r == 0)
    printf("%s: %d partition%s written\n", dev, n, n == 1 ? "" : "s");
  return r;
}
