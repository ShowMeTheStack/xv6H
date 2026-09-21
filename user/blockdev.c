// blockdev: ask a block device about itself, or tell it something.
//
//   blockdev --getsize64|--getsz|--getss|--getbsz|--getro DEVICE
//   blockdev --setro|--setrw|--flushbufs|--rereadpt DEVICE
//
// every one of these is one ioctl, and they are the ioctls Linux uses,
// so a program written against Linux finds what it expects.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define BLKROSET     0x125d
#define BLKROGET     0x125e
#define BLKRRPART    0x125f
#define BLKGETSIZE   0x1260
#define BLKFLSBUF    0x1261
#define BLKSSZGET    0x1268
#define BLKBSZGET    0x80081270
#define BLKGETSIZE64 0x80081272

static void
usage(void)
{
  fprintf(stderr, "usage: blockdev --getsize64|--getsz|--getss|--getbsz|--getro|"
                  "--setro|--setrw|--flushbufs|--rereadpt DEVICE\n");
}

int
main(int argc, char *argv[])
{
  if (argc != 3) {
    usage();
    return 1;
  }
  const char *op = argv[1], *dev = argv[2];
  int wr = strcmp(op, "--setro") == 0 || strcmp(op, "--setrw") == 0;
  int fd = open(dev, wr ? O_RDWR : O_RDONLY);

  if (fd < 0) {
    fprintf(stderr, "blockdev: %s: %s\n", dev, strerror(errno));
    return 1;
  }

  unsigned long long v64 = 0;
  int v32 = 0, r = -1;

  if (strcmp(op, "--getsize64") == 0) {
    if ((r = ioctl(fd, BLKGETSIZE64, &v64)) == 0)
      printf("%llu\n", v64);
  } else if (strcmp(op, "--getsz") == 0) {
    if ((r = ioctl(fd, BLKGETSIZE, &v64)) == 0)
      printf("%llu\n", v64);
  } else if (strcmp(op, "--getss") == 0) {
    if ((r = ioctl(fd, BLKSSZGET, &v32)) == 0)
      printf("%d\n", v32);
  } else if (strcmp(op, "--getbsz") == 0) {
    if ((r = ioctl(fd, BLKBSZGET, &v32)) == 0)
      printf("%d\n", v32);
  } else if (strcmp(op, "--getro") == 0) {
    if ((r = ioctl(fd, BLKROGET, &v32)) == 0)
      printf("%d\n", v32);
  } else if (strcmp(op, "--setro") == 0 || strcmp(op, "--setrw") == 0) {
    v32 = strcmp(op, "--setro") == 0;
    r = ioctl(fd, BLKROSET, &v32);
  } else if (strcmp(op, "--flushbufs") == 0) {
    r = ioctl(fd, BLKFLSBUF, 0);
  } else if (strcmp(op, "--rereadpt") == 0) {
    r = ioctl(fd, BLKRRPART, 0);
  } else {
    usage();
    close(fd);
    return 1;
  }

  if (r < 0) {
    fprintf(stderr, "blockdev: %s %s: %s\n", op, dev, strerror(errno));
    close(fd);
    return 1;
  }
  close(fd);
  return 0;
}
