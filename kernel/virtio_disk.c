//
// driver for qemu's virtio disk devices.
// uses qemu's mmio interface to virtio.
// supports several devices (one per virtio-mmio slot found in the
// device tree with device id 2); disk 0 holds the root file system,
// the others are raw block devices used as guest disks by the hypervisor.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "platform.h"

// the address of virtio mmio register r of disk d.
#define R(d, r) ((volatile uint32 *)((d)->base + (r)))

static struct disk {
  uint64 base;     // mmio base, 0 if no disk in this slot
  int irq;
  uint64 capacity; // in 512-byte sectors

  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    volatile int *done;
    char status;
  } info[NUM];

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;

} disks[NVIRTIO];

static int ndisks;

static void
disk_init(struct disk *d)
{
  uint32 status = 0;

  initlock(&d->vdisk_lock, "virtio_disk");

  // reset device
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(d, VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_FLUSH);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(d, VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(d, VIRTIO_MMIO_STATUS);
  if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0.
  *R(d, VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if (*R(d, VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32 max = *R(d, VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (max == 0)
    panic("virtio disk has no queue 0");
  if (max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  d->desc = kalloc();
  d->avail = kalloc();
  d->used = kalloc();
  if (!d->desc || !d->avail || !d->used)
    panic("virtio disk kalloc");
  memset(d->desc, 0, PGSIZE);
  memset(d->avail, 0, PGSIZE);
  memset(d->used, 0, PGSIZE);

  // set queue size.
  *R(d, VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(d, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)d->desc;
  *R(d, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)d->desc >> 32;
  *R(d, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)d->avail;
  *R(d, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)d->avail >> 32;
  *R(d, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)d->used;
  *R(d, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)d->used >> 32;

  // queue is ready.
  *R(d, VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  for (int i = 0; i < NUM; i++)
    d->free[i] = 1;

  // capacity (sectors) lives in the config space at offset 0x100.
  d->capacity = *R(d, VIRTIO_MMIO_CONFIG) | ((uint64)*R(d, VIRTIO_MMIO_CONFIG + 4) << 32);

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // intc.c and trap.c arrange for interrupts from d->irq.
}

// probe every virtio-mmio slot the device tree lists; slot i with a
// block device becomes disk i. slots without a device are skipped but
// keep their index so that disk numbering matches qemu's bus numbering.
void
virtio_disk_init(void)
{
  ndisks = 0;
  for (int i = 0; i < plat.nvirtio && i < NVIRTIO; i++) {
    struct disk *d = &disks[i];
    d->base = plat.virtio_base[i];
    d->irq = plat.virtio_irq[i];
    if (*R(d, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(d, VIRTIO_MMIO_VERSION) != 2 || *R(d, VIRTIO_MMIO_DEVICE_ID) != 2 ||
        *R(d, VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
      d->base = 0;
      continue;
    }
    disk_init(d);
    ndisks = i + 1;
  }
  if (ndisks == 0 || disks[0].base == 0)
    panic("could not find virtio disk");
}

int
virtio_disk_count(void)
{
  return ndisks;
}

int
virtio_disk_present(int idx)
{
  return idx >= 0 && idx < NVIRTIO && disks[idx].base != 0;
}

uint64
virtio_disk_capacity(int idx)
{
  if (!virtio_disk_present(idx))
    return 0;
  return disks[idx].capacity;
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc(struct disk *d)
{
  for (int i = 0; i < NUM; i++) {
    if (d->free[i]) {
      d->free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(struct disk *d, int i)
{
  if (i >= NUM)
    panic("free_desc 1");
  if (d->free[i])
    panic("free_desc 2");
  d->desc[i].addr = 0;
  d->desc[i].len = 0;
  d->desc[i].flags = 0;
  d->desc[i].next = 0;
  d->free[i] = 1;
  wakeup(&d->free[0]);
}

// free a chain of descriptors.
static void
free_chain(struct disk *d, int i)
{
  while (1) {
    int flag = d->desc[i].flags;
    int nxt = d->desc[i].next;
    free_desc(d, i);
    if (flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(struct disk *d, int *idx)
{
  for (int i = 0; i < 3; i++) {
    idx[i] = alloc_desc(d);
    if (idx[i] < 0) {
      for (int j = 0; j < i; j++)
        free_desc(d, idx[j]);
      return -1;
    }
  }
  return 0;
}

// submit one read or write of len bytes (a multiple of 512) at the
// given 512-byte sector, and wait for it to complete. returns the
// device's status byte (0 = ok).
static int
disk_submit(struct disk *d, uint64 sector, void *data, uint32 len, int write)
{
  volatile int done = 0;

  acquire(&d->vdisk_lock);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while (1) {
    if (alloc3_desc(d, idx) == 0) {
      break;
    }
    sleep_prepare(&d->free[0]);
    release(&d->vdisk_lock);
    sleep();
    acquire(&d->vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &d->ops[idx[0]];

  if (write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  d->desc[idx[0]].addr = (uint64)buf0;
  d->desc[idx[0]].len = sizeof(struct virtio_blk_req);
  d->desc[idx[0]].flags = VRING_DESC_F_NEXT;
  d->desc[idx[0]].next = idx[1];

  d->desc[idx[1]].addr = (uint64)data;
  d->desc[idx[1]].len = len;
  if (write)
    d->desc[idx[1]].flags = 0; // device reads data
  else
    d->desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes data
  d->desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  d->desc[idx[1]].next = idx[2];

  d->info[idx[0]].status = 0xff; // device writes 0 on success
  d->desc[idx[2]].addr = (uint64)&d->info[idx[0]].status;
  d->desc[idx[2]].len = 1;
  d->desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  d->desc[idx[2]].next = 0;

  // record the completion flag for virtio_disk_intr().
  d->info[idx[0]].done = &done;

  // tell the device the first index in our chain of descriptors.
  d->avail->ring[d->avail->idx % NUM] = idx[0];

  io_fence();

  // tell the device another avail ring entry is available.
  d->avail->idx += 1; // not % NUM ...

  io_fence();

  *R(d, VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while (done == 0) {
    sleep_prepare((void *)&done);
    release(&d->vdisk_lock);
    sleep();
    acquire(&d->vdisk_lock);
  }

  int status = d->info[idx[0]].status;
  d->info[idx[0]].done = 0;
  free_chain(d, idx[0]);

  release(&d->vdisk_lock);
  return status;
}

// buffer-cache entry point: dev is the xv6 device number (ROOTDEV=1 is disk 0).
void
virtio_disk_rw(struct buf *b, int write)
{
  int idx = b->dev - 1;
  if (!virtio_disk_present(idx))
    panic("virtio_disk_rw: no such disk");
  uint64 sector = b->blockno * (BSIZE / 512);
  if (disk_submit(&disks[idx], sector, b->data, BSIZE, write) != 0)
    panic("virtio_disk_rw status");
}

void
virtio_disk_intr(int idx)
{
  if (!virtio_disk_present(idx))
    return;
  struct disk *d = &disks[idx];

  acquire(&d->vdisk_lock);

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(d, VIRTIO_MMIO_INTERRUPT_ACK) = *R(d, VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  io_fence();

  // the device increments d->used->idx when it
  // adds an entry to the used ring.

  while (d->used_idx != d->used->idx) {
    io_fence();
    int id = d->used->ring[d->used_idx % NUM].id;

    volatile int *done = d->info[id].done;
    if (done) {
      *done = 1;
      wakeup((void *)done);
    }

    d->used_idx += 1;
  }

  release(&d->vdisk_lock);
}
