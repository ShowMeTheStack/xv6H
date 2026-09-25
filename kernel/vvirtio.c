//
// virtual virtio-mmio block device (virtio 1.x "modern" mmio transport,
// legacy-style split virtqueue), backed either by a disk image file in
// the host file system (vm->disk_ip, read and written through the
// inode) or by a whole block device the guest was given (vm->disk,
// read and written through the block layer, with no host file system
// in the way).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "file.h"
#include "virtio.h"
#include "platform.h"
#include "blk.h"
#include "blkdev.h"
#include "hyp.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc

#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_S_OK    0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

#define MAXQ 256

struct gdesc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};

// ---- guest memory helpers ----

static int
rd16(struct vm *vm, uint64 gpa, uint16 *v)
{
  return vm_copyin(vm, v, gpa, 2);
}

static int
wr16(struct vm *vm, uint64 gpa, uint16 v)
{
  return vm_copyout(vm, gpa, &v, 2);
}

// ---- request processing ----

// transfer len bytes between guest memory at gpa and the disk image
// file at sector, through the bounce page. returns 0 on success.
static int
xfer(struct vm *vm, struct vvirtio *b, uint64 sector, uint64 gpa, uint32 len, int write)
{
  struct inode *ip = vm->disk_ip;
  struct gendisk *d = vm->disk;
  uint64 off = sector * 512;
  if ((ip == 0 && d == 0) || len == 0 || (len % 512) != 0 || off + len > b->capacity * 512)
    return -1;
  while (len > 0) {
    uint32 n = len > PGSIZE ? PGSIZE : len;
    if (write) {
      if (vm_copyin(vm, b->bounce, gpa, n) < 0)
        return -1;
      int r;
      if (d) {
        r = blkdev_write(d, 0, (uint64)b->bounce, off, n);
      } else {
        ilock(ip);
        r = writei(ip, 0, (uint64)b->bounce, off, n);
        iunlock(ip);
      }
      if (r != (int)n)
        return -1;
    } else {
      int r;
      if (d) {
        r = blkdev_read(d, 0, (uint64)b->bounce, off, n);
      } else {
        ilock(ip);
        r = readi(ip, 0, (uint64)b->bounce, off, n);
        iunlock(ip);
      }
      if (r != (int)n)
        return -1;
      if (vm_copyout(vm, gpa, b->bounce, n) < 0)
        return -1;
    }
    off += n;
    gpa += n;
    len -= n;
  }
  return 0;
}

// handle the descriptor chain starting at head; returns the number of
// bytes written into guest buffers (for the used ring), or -1 if the
// chain itself is malformed.
static int
do_request(struct vm *vm, struct vvirtio *b, uint16 head)
{
  struct gdesc d;
  struct virtio_blk_req req;
  uint32 num = b->queue_num;
  uint16 idx = head;
  int n = 0, written = 0;
  uint8 status = VIRTIO_BLK_S_OK;
  uint64 status_gpa = 0;
  uint64 sector = 0;
  int type = -1;

  for (;;) {
    if (idx >= num || n++ > 512)
      return -1;
    if (vm_copyin(vm, &d, b->desc + 16 * idx, sizeof(d)) < 0)
      return -1;
    int last = !(d.flags & VRING_DESC_F_NEXT);
    if (type < 0) {
      // first descriptor: the request header, device-readable.
      if (d.len < sizeof(req) || (d.flags & VRING_DESC_F_WRITE) ||
          vm_copyin(vm, &req, d.addr, sizeof(req)) < 0)
        return -1;
      type = req.type;
      sector = req.sector;
    } else if (last) {
      // last descriptor: the status byte, device-writable.
      if (d.len < 1 || !(d.flags & VRING_DESC_F_WRITE))
        return -1;
      status_gpa = d.addr;
    } else {
      // data buffer.
      if (status == VIRTIO_BLK_S_OK) {
        if (type == VIRTIO_BLK_T_IN) {
          if (!(d.flags & VRING_DESC_F_WRITE) ||
              xfer(vm, b, sector, d.addr, d.len, 0) < 0)
            status = VIRTIO_BLK_S_IOERR;
          else
            written += d.len;
        } else if (type == VIRTIO_BLK_T_OUT) {
          if ((d.flags & VRING_DESC_F_WRITE) ||
              xfer(vm, b, sector, d.addr, d.len, 1) < 0)
            status = VIRTIO_BLK_S_IOERR;
        } else if (type == VIRTIO_BLK_T_FLUSH) {
          // nothing to do: the host disk is written through.
        } else {
          status = VIRTIO_BLK_S_UNSUPP;
        }
        sector += d.len / 512;
      }
    }
    if (last)
      break;
    idx = d.next;
  }
  if (status_gpa == 0)
    return -1;
  if (vm_copyout(vm, status_gpa, &status, 1) < 0)
    return -1;
  return written + 1;
}

static void
process_queue(struct vm *vm, struct vvirtio *b)
{
  acquiresleep(&b->qlock);
  if (!b->queue_ready || b->queue_num == 0 || b->queue_num > MAXQ) {
    releasesleep(&b->qlock);
    return;
  }
  uint32 num = b->queue_num;
  int any = 0;
  // one ring's worth of requests per call, and no more: a doorbell is
  // a notification that the ring has something in it, not a licence
  // to hold this thread while the guest keeps writing. Whatever is
  // still there when this returns is taken on the next round of the
  // worker, which can see that the VM is going away.
  for (uint32 done = 0; done < num; done++) {
    uint16 avail_idx, head, used_idx;
    if (rd16(vm, b->avail + 2, &avail_idx) < 0)
      break;
    if (avail_idx == b->last_avail)
      break;
    // the ring holds `num' descriptors, so at most num can be
    // outstanding. Anything more is not a queue state that could have
    // arisen: the guest has written nonsense into the index (or into
    // the whole ring). Catch up to it rather than walk it.
    if ((uint16)(avail_idx - b->last_avail) > num) {
      if (vm_gripe(&b->desync))
        printk("hyp: vm %d: virtio available index %d is %d ahead of a "
               "%d-entry ring (%ld times)\n", vm->id, avail_idx,
               (uint16)(avail_idx - b->last_avail), num, b->desync);
      b->last_avail = avail_idx;
      break;
    }
    if (rd16(vm, b->avail + 4 + 2 * (b->last_avail % num), &head) < 0)
      break;
    int len = do_request(vm, b, head);
    if (len < 0) {
      if (vm_gripe(&b->malformed))
        printk("hyp: vm %d: malformed virtio request (head %d, %ld so far)\n",
               vm->id, head, b->malformed);
      len = 0;
    }
    // used ring entry: {id, len}
    if (rd16(vm, b->used + 2, &used_idx) < 0)
      break;
    uint32 elem[2] = {head, (uint32)len};
    if (vm_copyout(vm, b->used + 4 + 8 * (used_idx % num), elem, 8) < 0)
      break;
    __sync_synchronize();
    if (wr16(vm, b->used + 2, used_idx + 1) < 0)
      break;
    b->last_avail++;
    any = 1;
  }
  releasesleep(&b->qlock);

  if (any) {
    acquire(&vm->lock);
    b->intr_status |= 1;
    vic_set_level(vm, G_VIRTIO_IRQ, 1);
    release(&vm->lock);
  }
}

// ---- the queue thread ----
//
// the guest writes to QUEUE_NOTIFY and the host reads a block from a
// disk, which sleeps. Doing that on the vcpu's own exit stops the
// guest for the length of the I/O even when it had other work to do,
// which is the thing an operating system exists to avoid. So the
// notification only wakes a thread, and the vcpu goes straight back
// into the guest; the interrupt that says the block has arrived is
// raised by the thread when it has.
static void
blk_worker(void *arg)
{
  struct vm *vm = arg;
  struct vvirtio *b = &vm->blk;

  for (;;) {
    acquire(&vm->lock);
    while (!b->wake && !b->stop) {
      sleep_prepare(&b->wake);
      release(&vm->lock);
      sleep();
      acquire(&vm->lock);
    }
    if (b->stop) {
      b->running = 0;
      release(&vm->lock);
      wakeup(&b->stop);
      return;
    }
    b->wake = 0;
    b->busy = 1;
    release(&vm->lock);
    process_queue(vm, b);
    acquire(&vm->lock);
    b->busy = 0;
    release(&vm->lock);
    wakeup((void *)&b->busy);
  }
}

// The vcpus being stopped is not the machine being still: this thread
// reads blocks into guest memory and writes them out of it, and it does
// that whether or not anybody is inside the guest. A save waits here
// first, or it writes down pages the disk is in the middle of filling --
// which is what a migrated guest's "panic: kerneltrap" was, ten pages of
// its block cache holding what they held before the read landed.
int
vvirtio_quiesce(struct vm *vm)
{
  struct vvirtio *b = &vm->blk;

  acquire(&vm->lock);
  for (int tries = 0; tries < 500; tries++) {
    if (!b->running || (!b->busy && !b->wake)) {
      release(&vm->lock);
      return 0;
    }
    sleep_prepare((void *)&b->busy);
    release(&vm->lock);
    sleep();
    acquire(&vm->lock);
  }
  release(&vm->lock);
  return -1;
}

// the VM is going. the thread is told to leave and waited for, because
// what it is holding -- the disk, the bounce page -- is about to be
// freed underneath it.
void
vvirtio_stop(struct vm *vm)
{
  struct vvirtio *b = &vm->blk;

  acquire(&vm->lock);
  if (!b->running) {
    release(&vm->lock);
    return;
  }
  b->stop = 1;
  wakeup(&b->wake);
  while (b->running) {
    sleep_prepare(&b->stop);
    release(&vm->lock);
    sleep();
    acquire(&vm->lock);
  }
  release(&vm->lock);
}

// ---- mmio transport ----

static int
vvirtio_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  struct vvirtio *b = &vm->blk;
  uint64 v = 0;

  if (off >= VIRTIO_MMIO_CONFIG) {
    // config space: u64 capacity in sectors at offset 0.
    uint64 cfg = off - VIRTIO_MMIO_CONFIG;
    if (cfg < 8)
      v = (b->capacity >> (8 * cfg)) & ((size == 8) ? ~0UL : (1UL << (8 * size)) - 1);
    *val = v;
    return 0;
  }
  if (size != 4)
    return -1;

  acquire(&vm->lock);
  switch (off) {
  case VIRTIO_MMIO_MAGIC_VALUE: v = 0x74726976; break;
  case VIRTIO_MMIO_VERSION: v = 2; break;
  case VIRTIO_MMIO_DEVICE_ID: v = b->capacity ? 2 : 0; break;
  case VIRTIO_MMIO_VENDOR_ID: v = 0x554d4551; break;
  case VIRTIO_MMIO_DEVICE_FEATURES:
    // sel 1 bit 0 = VIRTIO_F_VERSION_1; nothing else.
    v = (b->device_features_sel == 1) ? 1 : 0;
    break;
  case VIRTIO_MMIO_QUEUE_NUM_MAX: v = (b->queue_sel == 0) ? MAXQ : 0; break;
  case VIRTIO_MMIO_QUEUE_READY: v = (b->queue_sel == 0) ? b->queue_ready : 0; break;
  case VIRTIO_MMIO_INTERRUPT_STATUS: v = b->intr_status; break;
  case VIRTIO_MMIO_STATUS: v = b->status; break;
  case VIRTIO_MMIO_CONFIG_GENERATION: v = 0; break;
  default: v = 0; break;
  }
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vvirtio_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  struct vvirtio *b = &vm->blk;

  if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
    if (val == 0) {
      acquire(&vm->lock);
      b->wake = 1;
      int running = b->running;
      release(&vm->lock);
      if (running)
        wakeup(&b->wake);
      else
        process_queue(vm, b);   // no thread: do it here, as it used to
    }
    return 0;
  }
  if (size != 4)
    return -1;

  acquire(&vm->lock);
  switch (off) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: b->device_features_sel = val; break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL: b->driver_features_sel = val; break;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    if (b->driver_features_sel == 0)
      b->driver_features = (b->driver_features & ~0xffffffffUL) | val;
    else if (b->driver_features_sel == 1)
      b->driver_features = (b->driver_features & 0xffffffffUL) | (val << 32);
    break;
  case VIRTIO_MMIO_QUEUE_SEL: b->queue_sel = val; break;
  case VIRTIO_MMIO_QUEUE_NUM:
    if (b->queue_sel == 0)
      b->queue_num = val;
    break;
  case VIRTIO_MMIO_QUEUE_READY:
    if (b->queue_sel == 0) {
      b->queue_ready = val & 1;
      b->last_avail = 0;
    }
    break;
  case VIRTIO_MMIO_INTERRUPT_ACK:
    b->intr_status &= ~val;
    vic_set_level(vm, G_VIRTIO_IRQ, b->intr_status != 0);
    break;
  case VIRTIO_MMIO_STATUS:
    b->status = val;
    if (val == 0) {
      // reset
      b->queue_ready = 0;
      b->queue_num = 0;
      b->last_avail = 0;
      b->intr_status = 0;
      b->driver_features = 0;
      vic_set_level(vm, G_VIRTIO_IRQ, 0);
    }
    break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW:
    if (b->queue_sel == 0) b->desc = (b->desc & ~0xffffffffUL) | val;
    break;
  case VIRTIO_MMIO_QUEUE_DESC_HIGH:
    if (b->queue_sel == 0) b->desc = (b->desc & 0xffffffffUL) | (val << 32);
    break;
  case VIRTIO_MMIO_DRIVER_DESC_LOW:
    if (b->queue_sel == 0) b->avail = (b->avail & ~0xffffffffUL) | val;
    break;
  case VIRTIO_MMIO_DRIVER_DESC_HIGH:
    if (b->queue_sel == 0) b->avail = (b->avail & 0xffffffffUL) | (val << 32);
    break;
  case VIRTIO_MMIO_DEVICE_DESC_LOW:
    if (b->queue_sel == 0) b->used = (b->used & ~0xffffffffUL) | val;
    break;
  case VIRTIO_MMIO_DEVICE_DESC_HIGH:
    if (b->queue_sel == 0) b->used = (b->used & 0xffffffffUL) | (val << 32);
    break;
  default:
    break;
  }
  release(&vm->lock);
  return 0;
}

void
vvirtio_init(struct vm *vm)
{
  struct vvirtio *b = &vm->blk;
  memset(b, 0, sizeof(*b));
  initsleeplock(&b->qlock, "vvirtio");
  b->bounce = kalloc();
  b->capacity = 0;
  if (vm->disk) {
    b->capacity = vm->disk->nsectors;
  } else if (vm->disk_ip) {
    ilock(vm->disk_ip);
    b->capacity = vm->disk_ip->size / 512;
    iunlock(vm->disk_ip);
  }
  vm_add_dev(vm, "virtio-blk", G_VIRTIO_IRQ, G_VIRTIO_BASE, 0x1000, vvirtio_read, vvirtio_write, 0);
  if (b->capacity && kthread_create(blk_worker, vm, "vblk") >= 0)
    b->running = 1;
  else if (b->capacity)
    printk("hyp: vm %d: no disk thread; its I/O will stop its vcpu\n", vm->id);
}
