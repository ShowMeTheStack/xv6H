// cksum: CRC-32 and byte count of each file (or stdin), as POSIX cksum
// prints them: "crc length name". The tests use it to show that a guest's
// data survived something the host did to the machine underneath it.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static uint32_t table[256];

static void
mktable(void)
{
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i << 24;
    for (int k = 0; k < 8; k++)
      c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
    table[i] = c;
  }
}

static int
one(int fd, const char *name)
{
  static unsigned char buf[8192];
  uint32_t crc = 0;
  unsigned long len = 0;
  int n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++)
      crc = (crc << 8) ^ table[((crc >> 24) ^ buf[i]) & 0xff];
    len += n;
  }
  if (n < 0) {
    fprintf(stderr, "cksum: read %s failed\n", name);
    return 1;
  }
  // the length is folded in, least significant byte first, then inverted
  for (unsigned long l = len; l; l >>= 8)
    crc = (crc << 8) ^ table[((crc >> 24) ^ (l & 0xff)) & 0xff];
  crc = ~crc;
  if (name[0])
    printf("%u %lu %s\n", (unsigned)crc, len, name);
  else
    printf("%u %lu\n", (unsigned)crc, len);
  return 0;
}

int
main(int argc, char *argv[])
{
  mktable();
  if (argc < 2)
    return one(0, "");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "cksum: cannot open %s\n", argv[i]);
      rc = 1;
      continue;
    }
    rc |= one(fd, argv[i]);
    close(fd);
  }
  return rc;
}
