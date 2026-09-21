// dd: copy a file, or part of one, with a block size of your choosing.
//
//   dd [if=FILE] [of=FILE] [bs=N] [count=N] [skip=N] [seek=N]
//      [conv=notrunc,sync,fsync]
//
// what it is for here is the things only it can do: writing an image
// onto a block device, reading a partition table out of one, copying a
// range out of the middle of a disk. N may end in k, M or G.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static long long
number(const char *s)
{
  char *end;
  long long v = strtoll(s, &end, 0);

  if (*end == 'k' || *end == 'K')
    v *= 1024;
  else if (*end == 'M')
    v *= 1024 * 1024;
  else if (*end == 'G')
    v *= 1024LL * 1024 * 1024;
  else if (*end == 'b')
    v *= 512;
  return v;
}

int
main(int argc, char *argv[])
{
  const char *inf = 0, *outf = 0;
  long long bs = 512, count = -1, skip = 0, seek = 0;
  int notrunc = 0, dosync = 0, dofsync = 0;
  int in = 0, out = 1;
  long long full = 0, partial = 0, bytes = 0;
  char *buf;

  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (strncmp(a, "if=", 3) == 0)
      inf = a + 3;
    else if (strncmp(a, "of=", 3) == 0)
      outf = a + 3;
    else if (strncmp(a, "bs=", 3) == 0)
      bs = number(a + 3);
    else if (strncmp(a, "count=", 6) == 0)
      count = number(a + 6);
    else if (strncmp(a, "skip=", 5) == 0)
      skip = number(a + 5);
    else if (strncmp(a, "seek=", 5) == 0)
      seek = number(a + 5);
    else if (strncmp(a, "conv=", 5) == 0) {
      if (strstr(a + 5, "notrunc"))
        notrunc = 1;
      if (strstr(a + 5, "fsync"))
        dofsync = 1;
      if (strstr(a + 5, "sync"))
        dosync = 1;
    } else {
      fprintf(stderr, "dd: unknown operand %s\n", a);
      return 1;
    }
  }
  if (bs <= 0) {
    fprintf(stderr, "dd: a block size of %lld makes no sense\n", bs);
    return 1;
  }
  if ((buf = malloc(bs)) == 0) {
    fprintf(stderr, "dd: no memory for a %lld-byte buffer\n", bs);
    return 1;
  }

  if (inf && (in = open(inf, O_RDONLY)) < 0) {
    fprintf(stderr, "dd: %s: %s\n", inf, strerror(errno));
    return 1;
  }
  if (outf) {
    // a block device must not be truncated, and conv=notrunc says so
    // for a file; everything else is created and emptied.
    int flags = O_WRONLY | O_CREAT;
    if (!notrunc)
      flags |= O_TRUNC;
    if ((out = open(outf, flags, 0644)) < 0) {
      fprintf(stderr, "dd: %s: %s\n", outf, strerror(errno));
      return 1;
    }
  }
  if (skip && lseek(in, skip * bs, SEEK_SET) < 0) {
    fprintf(stderr, "dd: cannot skip on the input: %s\n", strerror(errno));
    return 1;
  }
  if (seek && lseek(out, seek * bs, SEEK_SET) < 0) {
    fprintf(stderr, "dd: cannot seek on the output: %s\n", strerror(errno));
    return 1;
  }

  for (long long i = 0; count < 0 || i < count; i++) {
    long n = read(in, buf, bs);
    if (n < 0) {
      fprintf(stderr, "dd: read: %s\n", strerror(errno));
      return 1;
    }
    if (n == 0)
      break;
    if (n < bs) {
      if (dosync) {
        memset(buf + n, 0, bs - n); // conv=sync pads the short block
        n = bs;
        full++;
      } else {
        partial++;
      }
    } else {
      full++;
    }
    long done = 0;
    while (done < n) {
      long w = write(out, buf + done, n - done);
      if (w <= 0) {
        fprintf(stderr, "dd: write: %s\n", strerror(errno));
        return 1;
      }
      done += w;
    }
    bytes += n;
  }
  if (dofsync && fsync(out) < 0) {
    fprintf(stderr, "dd: fsync: %s\n", strerror(errno));
    return 1;
  }
  fprintf(stderr, "%lld+%lld records in\n%lld+%lld records out\n", full, partial, full, partial);
  fprintf(stderr, "%lld bytes copied\n", bytes);
  return 0;
}
