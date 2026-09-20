//
// crc32c, the Castagnoli polynomial, which is what ext4 and jbd2 use
// for every checksum they carry.
//
// the table is built on first use rather than written out, because the
// 1 KB of generated entries says nothing a reader wants to know and the
// loop that makes it says everything.
//
#include "types.h"

#define CRC32C_POLY 0x82f63b78U // the reversed form of 0x1edc6f41

static uint32 table[256];
static int built;

static void
build(void)
{
  for (int i = 0; i < 256; i++) {
    uint32 crc = i;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (CRC32C_POLY & (uint32)(-(int32)(crc & 1)));
    table[i] = crc;
  }
  built = 1;
}

// the running checksum of a block of bytes. this is the bare crc, with
// neither the initial nor the final complement that a standalone
// CRC-32C of a message carries: ext4 seeds one checksum with the result
// of another -- the uuid seeds the file system, the file system seeds
// an inode, the inode seeds its directory blocks -- and a complement at
// either end would break the chain. a caller that wants the ordinary
// CRC-32C of a message passes ~0 and complements the result itself.
//
// e2fsprogs and Linux both spell it this way, which is the only reason
// it matters: a checksum is worth nothing unless the other reader of
// the disk computes the same one.
uint32
crc32c(uint32 crc, const void *data, int len)
{
  const uchar *p = data;

  if (!built)
    build();
  while (len-- > 0)
    crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return crc;
}
