//
// driver for a virtio-balloon device: giving this machine's memory
// back to whoever is underneath it.
//
// only a guest has one. The host writes a target into the device's
// configuration -- how many 4 KB pages it would like back -- and this
// driver allocates that many pages so that nothing here will touch
// them, then tells the host which they are. The host takes the
// backing away, and the pages cost this machine nothing but an entry
// in a list. When the target falls, the pages are freed again and the
// host is told; the first touch of one faults a fresh page in.
//
// the work is done by a thread rather than in the interrupt handler:
// allocating a thousand pages is not something to do with interrupts
// off, and a balloon has no deadline.
//
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "virtio.h"
#include "platform.h"
#include "page.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024

#define VIRTIO_ID_BALLOON 5
#define VIRTIO_BALLOON_F_STATS_VQ 1

#define INFLATEQ 0
#define DEFLATEQ 1
#define QSIZE 8            // descriptors per queue
#define BATCH 256          // page frame numbers in one message

// the pages this machine has set aside.
//
// the obvious place to keep the list is inside the pages themselves,
// a word of each pointing at the next.  That does not work here, and
// the reason is the whole point of a balloon: once the host has taken
// a page away there is nothing behind it, and reading that word
// faults a fresh zeroed page in and reads a zero.  The list would end
// at its first element and every page after it would be lost.
//
// so the addresses are held outside, in pages of their own that are
// never offered to the host: 511 addresses and a link, which is one
// page of bookkeeping for every two megabytes given away.
#define PER_IDX ((PGSIZE / (int)sizeof(uint64)) - 1)

struct idxpage {
  struct idxpage *next;
  uint64 pa[PER_IDX];
};

static struct {
  uint64 base;
  int slot, irq;
  int present;
  struct spinlock lock;
  struct virtq_desc *desc[2];
  struct virtq_avail *avail[2];
  struct virtq_used *used[2];
  uint16 used_idx[2];
  uint32 *batch;           // the page frame numbers being reported
  struct idxpage *list;    // the addresses of the pages given to the host
  int listn;               // how many of the head page's slots are used
  uint64 nheld;
  int wake;                // the target moved
} bal;

// remember a page we are about to hand over. bal.lock held.
static int
held_push(uint64 pa)
{
  if (bal.list == 0 || bal.listn == PER_IDX) {
    struct idxpage *n = kalloc();
    if (n == 0)
      return -1;
    n->next = bal.list;
    bal.list = n;
    bal.listn = 0;
  }
  bal.list->pa[bal.listn++] = pa;
  bal.nheld++;
  return 0;
}

// take one back. bal.lock held; returns 0 when there are none.
static uint64
held_pop(void)
{
  if (bal.list == 0)
    return 0;
  if (bal.listn == 0) {
    struct idxpage *old = bal.list;
    bal.list = old->next;
    kfree(old);
    if (bal.list == 0)
      return 0;
    bal.listn = PER_IDX;
  }
  bal.nheld--;
  return bal.list->pa[--bal.listn];
}

#define R(r) ((volatile uint32 *)(bal.base + (r)))

static uint32
config_u32(int off)
{
  return *(volatile uint32 *)(bal.base + VIRTIO_MMIO_CONFIG + off);
}

static void
vq_init(int qn)
{
  *R(VIRTIO_MMIO_QUEUE_SEL) = qn;
  if (*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio balloon queue ready");
  if (*R(VIRTIO_MMIO_QUEUE_NUM_MAX) < QSIZE)
    panic("virtio balloon queue too small");
  bal.desc[qn] = kalloc();
  bal.avail[qn] = kalloc();
  bal.used[qn] = kalloc();
  if (!bal.desc[qn] || !bal.avail[qn] || !bal.used[qn])
    panic("virtio balloon kalloc");
  memset(bal.desc[qn], 0, PGSIZE);
  memset(bal.avail[qn], 0, PGSIZE);
  memset(bal.used[qn], 0, PGSIZE);
  *R(VIRTIO_MMIO_QUEUE_NUM) = QSIZE;
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)bal.desc[qn];
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)bal.desc[qn] >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)bal.avail[qn];
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)bal.avail[qn] >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)bal.used[qn];
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)bal.used[qn] >> 32;
  *R(VIRTIO_MMIO_QUEUE_READY) = 1;
}

// hand one buffer of page frame numbers to the host and wait for it to
// come back. the queue holds one message at a time, which is all a
// balloon needs and makes the waiting simple.
static void
report(int qn, int n)
{
  struct virtq_desc *d = &bal.desc[qn][0];

  d->addr = (uint64)bal.batch;
  d->len = n * sizeof(uint32);
  d->flags = 0;
  d->next = 0;
  bal.avail[qn]->ring[bal.avail[qn]->idx % QSIZE] = 0;
  io_fence();
  bal.avail[qn]->idx++;
  io_fence();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = qn;

  // the host answers from the same call, so this is nearly always
  // already done by the time we look
  for (int spin = 0; spin < 1000000; spin++) {
    io_fence();
    if (bal.used[qn]->idx != bal.used_idx[qn]) {
      bal.used_idx[qn] = bal.used[qn]->idx;
      return;
    }
  }
}

// set aside `want` more pages and tell the host about them
static void
inflate(uint64 want)
{
  while (want > 0) {
    int n = 0;
    while (n < BATCH && (uint64)n < want) {
      void *pa = kalloc();
      if (pa == 0)
        break;   // nothing left to give: what is gathered still goes
      acquire(&bal.lock);
      int ok = held_push((uint64)pa) == 0;
      release(&bal.lock);
      if (!ok) {
        kfree(pa);
        break;
      }
      bal.batch[n++] = (uint64)pa >> 12;
    }
    if (n == 0)
      return;
    report(INFLATEQ, n);
    want -= n;
  }
}

// take `give` pages back and tell the host they are in use again
static void
deflate(uint64 give)
{
  while (give > 0) {
    uint64 batch[BATCH];
    int n = 0;
    acquire(&bal.lock);
    while (n < BATCH && (uint64)n < give) {
      uint64 pa = held_pop();
      if (pa == 0)
        break;
      batch[n] = pa;
      bal.batch[n] = pa >> 12;
      n++;
    }
    release(&bal.lock);
    if (n == 0)
      return;
    // the host is told first: until it knows, the page it thinks it
    // owns must not be written, and kfree writes junk into it
    report(DEFLATEQ, n);
    for (int i = 0; i < n; i++)
      kfree((void *)batch[i]);
    give -= n;
  }
}

// the thread that follows the target
static void
balloon_thread(void *arg)
{
  for (;;) {
    uint32 target = config_u32(0);
    uint64 have = bal.nheld;

    if (target > have) {
      inflate(target - have);
    } else if (target < have) {
      deflate(have - target);
    } else {
      acquire(&bal.lock);
      while (!bal.wake) {
        sleep_prepare(&bal.wake);
        release(&bal.lock);
        sleep_intr();
        acquire(&bal.lock);
      }
      bal.wake = 0;
      release(&bal.lock);
    }
  }
}

void
virtio_balloon_intr(int idx)
{
  if (!bal.present || idx != bal.slot)
    return;
  uint32 st = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 3;
  if (st == 0)
    return;
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = st;
  if (st & 2) {
    // the configuration changed: the target moved
    acquire(&bal.lock);
    bal.wake = 1;
    release(&bal.lock);
    wakeup(&bal.wake);
  }
}

// how many pages this machine has given away
uint64
virtio_balloon_pages(void)
{
  return bal.present ? bal.nheld : 0;
}

void
virtio_balloon_rebase(uint64 old, uint64 nw)
{
  if (bal.base == old)
    bal.base = nw;
}

int
virtio_balloon_init(void)
{
  initlock(&bal.lock, "balloon");
  bal.present = 0;
  for (int i = 0; i < plat.nvirtio; i++) {
    bal.base = plat.virtio_base[i];
    if (*R(VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976 &&
        *R(VIRTIO_MMIO_VERSION) == 2 &&
        *R(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_ID_BALLOON) {
      bal.slot = i;
      bal.irq = plat.virtio_irq[i];
      bal.present = 1;
      break;
    }
  }
  if (!bal.present)
    return -1;

  uint32 status = 0;
  *R(VIRTIO_MMIO_STATUS) = status;
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  *R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 1;
  if (!(*R(VIRTIO_MMIO_DEVICE_FEATURES) & 1))
    panic("virtio balloon: not a modern device");
  // no optional features: the statistics queue is the host's to ask
  // for and this machine has nothing interesting to say on it
  *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = 0;
  *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 1;
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = 1;

  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;
  if (!(*R(VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio balloon FEATURES_OK unset");

  bal.batch = kalloc();
  if (bal.batch == 0)
    panic("virtio balloon kalloc");
  vq_init(INFLATEQ);
  vq_init(DEFLATEQ);

  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  printk("virtio balloon: present\n");
  kthread_create(balloon_thread, 0, "balloon");
  return 0;
}
