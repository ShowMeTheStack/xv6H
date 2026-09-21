// losetup: attach a file to a loop device, or say what is attached.
//
//   losetup                       list the devices that have a file
//   losetup -f                    print the first free device
//   losetup -f --show FILE        attach FILE to a free device, print it
//   losetup /dev/loopN FILE       attach FILE to that device
//   losetup -d /dev/loopN         detach it
//   losetup -o OFF --sizelimit N  the part of the file to use
//   losetup -r                    attach it read-only
//   losetup -b BASE /dev/loopN DELTA
//                                 a copy-on-write view of BASE: reads
//                                 that have not been written come from
//                                 it, writes go to DELTA, and BASE is
//                                 never changed, so many devices can
//                                 share one image
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>

#define LOOP_SET_FD       0x4C00
#define LOOP_CLR_FD       0x4C01
#define LOOP_SET_STATUS64 0x4C04
#define LOOP_GET_STATUS64 0x4C05
#define LOOP_CTL_ADD      0x4C80
#define LOOP_CTL_REMOVE   0x4C81
#define LOOP_CTL_GET_FREE 0x4C82
#define LOOP_SET_COW      0x4C10

struct loop_cow {
  int base_fd, delta_fd;
};

#define LO_FLAGS_READ_ONLY 1
#define LO_NAME_SIZE 64

struct loop_info64 {
  unsigned long long lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
  unsigned lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
  unsigned char lo_file_name[LO_NAME_SIZE], lo_crypt_name[LO_NAME_SIZE], lo_encrypt_key[32];
  unsigned long long lo_init[2];
};

static int
control(void)
{
  int fd = open("/dev/loop-control", O_RDWR);

  if (fd < 0)
    fprintf(stderr, "losetup: /dev/loop-control: %s\n", strerror(errno));
  return fd;
}

static int
free_device(void)
{
  int c = control(), n;

  if (c < 0)
    return -1;
  n = ioctl(c, LOOP_CTL_GET_FREE, 0);
  close(c);
  if (n < 0)
    fprintf(stderr, "losetup: no free loop device: %s\n", strerror(errno));
  return n;
}

static int
attach(const char *dev, const char *file, unsigned long long off, unsigned long long limit, int ro)
{
  int lf = open(file, ro ? O_RDONLY : O_RDWR);
  if (lf < 0) {
    fprintf(stderr, "losetup: %s: %s\n", file, strerror(errno));
    return 1;
  }
  int ld = open(dev, O_RDWR);
  if (ld < 0) {
    fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (ioctl(ld, LOOP_SET_FD, (void *)(long)lf) < 0) {
    fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (off || limit || ro) {
    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    if (ioctl(ld, LOOP_GET_STATUS64, &info) == 0) {
      info.lo_offset = off;
      info.lo_sizelimit = limit;
      info.lo_flags = ro ? LO_FLAGS_READ_ONLY : 0;
      if (ioctl(ld, LOOP_SET_STATUS64, &info) < 0) {
        fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
        return 1;
      }
    }
  }
  close(ld);
  close(lf);
  return 0;
}

static int
detach(const char *dev)
{
  int ld = open(dev, O_RDWR);

  if (ld < 0) {
    fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (ioctl(ld, LOOP_CLR_FD, 0) < 0) {
    fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
    close(ld);
    return 1;
  }
  close(ld);
  return 0;
}

static void
show(const char *dev)
{
  struct loop_info64 info;
  int ld = open(dev, O_RDONLY);

  if (ld < 0)
    return;
  if (ioctl(ld, LOOP_GET_STATUS64, &info) == 0) {
    if (info.lo_init[0]) {
      // a copy-on-write device: how much of the base it has changed
      unsigned mapped = (unsigned)(info.lo_init[0] >> 32);
      unsigned total = (unsigned)info.lo_init[0];
      printf("%s: [%s] copy-on-write, %u of %u blocks changed\n", dev,
             (char *)info.lo_file_name, mapped, total);
    } else {
      printf("%s: [%s] offset %llu sizelimit %llu%s\n", dev, (char *)info.lo_file_name,
             info.lo_offset, info.lo_sizelimit,
             (info.lo_flags & LO_FLAGS_READ_ONLY) ? " (ro)" : "");
    }
  }
  close(ld);
}

static void
list(void)
{
  DIR *d = opendir("/dev");
  struct dirent *e;

  if (d == 0)
    return;
  while ((e = readdir(d)) != 0) {
    char path[128];
    if (strncmp(e->d_name, "loop", 4) != 0 || e->d_name[4] < '0' || e->d_name[4] > '9')
      continue;
    if (e->d_name[5]) // a partition of a loop device, not the device
      continue;
    snprintf(path, sizeof(path), "/dev/%s", e->d_name);
    show(path);
  }
  closedir(d);
}

int
main(int argc, char *argv[])
{
  unsigned long long off = 0, limit = 0;
  int ro = 0, want_free = 0, doshow = 0, dodetach = 0;
  const char *dev = 0, *file = 0, *base = 0;

  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (strcmp(a, "-f") == 0)
      want_free = 1;
    else if (strcmp(a, "--show") == 0)
      doshow = 1;
    else if (strcmp(a, "-r") == 0)
      ro = 1;
    else if (strcmp(a, "-d") == 0)
      dodetach = 1;
    else if (strcmp(a, "-o") == 0 && i + 1 < argc)
      off = strtoull(argv[++i], 0, 0);
    else if (strcmp(a, "--sizelimit") == 0 && i + 1 < argc)
      limit = strtoull(argv[++i], 0, 0);
    else if (strcmp(a, "-b") == 0 && i + 1 < argc)
      base = argv[++i];
    else if (a[0] == '-') {
      fprintf(stderr, "losetup: unknown option %s\n", a);
      return 1;
    } else if (dev == 0 && strncmp(a, "/dev/", 5) == 0)
      dev = a;
    else
      file = a;
  }

  if (dodetach) {
    if (dev == 0) {
      fprintf(stderr, "losetup: -d needs a device\n");
      return 1;
    }
    return detach(dev);
  }

  if (want_free && file == 0) {
    int n = free_device();
    if (n < 0)
      return 1;
    printf("/dev/loop%d\n", n);
    return 0;
  }

  if (file == 0 && dev == 0) {
    list();
    return 0;
  }

  char buf[32];
  if (dev == 0) {
    int n = free_device();
    if (n < 0)
      return 1;
    snprintf(buf, sizeof(buf), "/dev/loop%d", n);
    dev = buf;
  }
  if (base) {
    // a copy-on-write view: the base read-only, the delta made if the
    // file is not there yet
    struct loop_cow c;
    int bfd = open(base, O_RDONLY);
    if (bfd < 0) {
      fprintf(stderr, "losetup: %s: %s\n", base, strerror(errno));
      return 1;
    }
    if (file == 0) {
      fprintf(stderr, "losetup: -b needs a delta file as well\n");
      return 1;
    }
    int dfd = open(file, O_RDWR | O_CREAT, 0666);
    if (dfd < 0) {
      fprintf(stderr, "losetup: %s: %s\n", file, strerror(errno));
      return 1;
    }
    int ld = open(dev, O_RDWR);
    if (ld < 0) {
      fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
      return 1;
    }
    c.base_fd = bfd;
    c.delta_fd = dfd;
    if (ioctl(ld, LOOP_SET_COW, (unsigned long)&c) < 0) {
      fprintf(stderr, "losetup: %s: %s\n", dev, strerror(errno));
      return 1;
    }
    // the name a listing shows is the base, because that is what the
    // device is a view of
    struct loop_info64 info;
    if (ioctl(ld, LOOP_GET_STATUS64, &info) == 0) {
      memset(info.lo_file_name, 0, LO_NAME_SIZE);
      snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "%s", base);
      ioctl(ld, LOOP_SET_STATUS64, (unsigned long)&info);
    }
    close(ld);
    close(dfd);
    close(bfd);
    if (doshow)
      printf("%s\n", dev);
    return 0;
  }

  if (file == 0) {
    show(dev);
    return 0;
  }
  if (attach(dev, file, off, limit, ro) != 0)
    return 1;
  if (doshow)
    printf("%s\n", dev);
  return 0;
}
