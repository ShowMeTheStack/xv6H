//
// a disk in RAM (the ZedBoard: Rocket has no virtio, and the ARM loads the
// image into DDR before letting Rocket go). The device tree says where:
//
//   ramdisk@8a000000 { compatible = "xv6h,ramdisk"; reg = <0 0x8a000000 0 0x5e00000>; };
//
// outside the memory node, so the allocator never hands it out; the kernel
// maps it (vm.c). It is registered as vda when there is no virtio disk, so
// the root is found where it always is.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "platform.h"
#include "blk.h"

static struct gendisk *rd_disk;

static int
rd_submit(struct gendisk *disk, struct bio *bio)
{
  uint64 off = bio->sector * 512, left = (uint64)bio->nsectors * 512;
  (void)disk;

  if (off + left > plat.ramdisk_size) {
    bio->error = -1;
  } else {
    for (int i = 0; i < bio->npages && left > 0; i++) {
      uint64 n = left < PGSIZE ? left : PGSIZE;
      char *disk_at = (char *)(plat.ramdisk_base + off);
      if (bio->write)
        memmove(disk_at, bio->pages[i], n);
      else
        memmove(bio->pages[i], disk_at, n);
      off += n;
      left -= n;
    }
  }
  bio->done = 1;
  if (bio->callback)
    bio->callback(bio);
  return bio->error;
}

// A flush makes what was written visible in DDR to whoever reads it from outside -- the
// ZedBoard's ARM over JTAG, which is how a machine saved on the board leaves it. Rocket's
// data cache is write-back and has no flush instruction a supervisor may use (no Zicbom), so
// the lines are pushed out by reading 256 KB of other memory, sixteen times the cache.
static volatile uint64 rd_evict_sink;

static int
rd_flush(struct gendisk *disk)
{
  (void)disk;
  extern char end[];
  volatile uint64 *p = (volatile uint64 *)PGROUNDUP((uint64)end);
  uint64 s = 0;
  for (int i = 0; i < (256 << 10) / 8; i += 8)     // one load per 64-byte line
    s += p[i];
  rd_evict_sink = s;
  return 0;
}

static const struct disk_ops rd_ops = {
    .submit = rd_submit,
    .flush = rd_flush,
};

// after the virtio disks: vda if there are none, else the next name
void
ramdisk_register(void)
{
  if (plat.ramdisk_size == 0)
    return;
  char name[DISKNAME] = "vda";
  while (blk_find(name) != 0 && name[2] < 'z')
    name[2]++;
  rd_disk = blk_register(name, &rd_ops, 0, plat.ramdisk_size / 512, 512);
  if (rd_disk == 0)
    return;
  printk("blk: %s: RAM disk at 0x%lx, %ld MB\n", name, plat.ramdisk_base, plat.ramdisk_size >> 20);
  blk_scan_partitions(rd_disk);
}
