// blkid: what file system is on a block device, and what it calls
// itself.
//
//   blkid [DEVICE...]
//
// with no arguments it says something about every block device it can
// open. it reads the superblock and nothing else: a device that holds
// no file system this knows about is reported as such rather than
// guessed at.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

// the parts of the ext4 superblock this needs, at their own offsets
#define SB_OFF        1024
#define SB_MAGIC_OFF  56
#define SB_UUID_OFF   104
#define SB_LABEL_OFF  120
#define SB_MAGIC      0xef53

static void
uuid_string(const unsigned char *u, char *out)
{
  static const char hex[] = "0123456789abcdef";
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
report(const char *dev, int quiet)
{
  unsigned char sb[1024];
  char uuid[40], label[17];
  int fd;

  if ((fd = open(dev, O_RDONLY)) < 0) {
    if (!quiet)
      fprintf(stderr, "blkid: %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (pread(fd, sb, sizeof(sb), SB_OFF) != (long)sizeof(sb)) {
    close(fd);
    if (!quiet)
      fprintf(stderr, "blkid: %s: cannot read a superblock\n", dev);
    return 1;
  }
  close(fd);

  unsigned magic = sb[SB_MAGIC_OFF] | (sb[SB_MAGIC_OFF + 1] << 8);
  if (magic != SB_MAGIC) {
    if (!quiet)
      printf("%s: no file system this knows about\n", dev);
    return 1;
  }
  uuid_string(sb + SB_UUID_OFF, uuid);
  memmove(label, sb + SB_LABEL_OFF, 16);
  label[16] = 0;

  printf("%s:", dev);
  if (label[0])
    printf(" LABEL=\"%s\"", label);
  printf(" UUID=\"%s\" TYPE=\"ext4\"\n", uuid);
  return 0;
}

int
main(int argc, char *argv[])
{
  if (argc > 1) {
    int bad = 0;
    for (int i = 1; i < argc; i++)
      bad |= report(argv[i], 0);
    return bad;
  }

  // no arguments: everything in /dev that answers
  DIR *d = opendir("/dev");
  if (d == 0) {
    fprintf(stderr, "blkid: /dev: %s\n", strerror(errno));
    return 1;
  }
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    char path[128];
    if (e->d_name[0] == '.')
      continue;
    snprintf(path, sizeof(path), "/dev/%s", e->d_name);
    report(path, 1);
  }
  closedir(d);
  return 0;
}
