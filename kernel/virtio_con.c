//
// virtio-console driver: the guest's side of the console queue.
//
// Only xv6 running as a guest of xv6H has one of these (the host's own
// machine has a 16550 and nothing at 0x10004000), and only when the
// machine was created with `vmm start -Q'. What it is for is the cost of
// the alternative: the 16550's driver reads the line status register to
// find the transmit register free and then writes one character, and each
// of those is an exit costing about sixteen microseconds, so a guest pays
// two exits a character and about 37 ms of its boot. A descriptor is one
// exit for as much output as fits in it.
//
// Output only, and deliberately. Input arrives through the 16550, which is
// still there: both devices draw on the same two rings inside the host, so
// the two halves are one console, and a person typing is not a cost worth
// a second interrupt path. The transmit queue is all this sets up.
//
// The completion is synchronous by construction and this is the whole
// reason the driver is so short: the host takes the descriptor inside the
// store to QUEUE_NOTIFY -- the notify *is* the trap that makes it run --
// so when that store returns the bytes are already in the host's console
// ring. Nothing here waits, and nothing here needs an interrupt; the
// available ring says NO_INTERRUPT so that the host does not raise one.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "virtio.h"
#include "platform.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_ID_CONSOLE 3
#define VRING_AVAIL_F_NO_INTERRUPT 1
#define RXQ 0
#define TXQ 1
#define CQSIZE 8         // descriptors; one is in flight at a time
#define RXBUF 64         // bytes per receive buffer (F18: input, when it is the console)

// The rings are static rather than kalloc'd, and that is the point: this
// has to run before the first printk, because the lines a guest prints
// while booting are most of what it prints at all. kalloc is not ready
// that early, and the kernel's own memory is mapped at its physical
// address, so a static is an address the device can be given as it
// stands. A page of output buffer and under a kilobyte of rings.
static struct virtq_desc con_desc[CQSIZE] __attribute__((aligned(16)));
static struct virtq_avail con_avail __attribute__((aligned(16)));
static struct virtq_used con_used __attribute__((aligned(16)));
static char con_buf[PGSIZE] __attribute__((aligned(16)));
static struct virtq_desc rx_desc[CQSIZE] __attribute__((aligned(16)));
static struct virtq_avail rx_avail __attribute__((aligned(16)));
static struct virtq_used rx_used __attribute__((aligned(16)));
static char rx_buf[CQSIZE][RXBUF] __attribute__((aligned(16)));
static uint16 rx_used_idx;

static struct {
  uint64 base;
  int idx;         // the platform slot, for the interrupt dispatch
  int present;
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  char *buf;
  struct spinlock lock;
} con;

#define R(r) ((volatile uint32 *)(con.base + (r)))

int
virtio_con_present(void)
{
  return con.present;
}

// find a virtio-mmio slot holding a console, set up its transmit queue.
// Called from main() after kinit, and silent when there is no such device,
// which is the normal case: this machine is only a guest sometimes.
void
virtio_con_rebase(uint64 old, uint64 nw)
{
  if (con.base == old)
    con.base = nw;
}

void
virtio_con_init(void)
{
  initlock(&con.lock, "vcon");
  for (int i = 0; i < plat.nvirtio && i < PLAT_MAXVIRTIO; i++) {
    con.base = plat.virtio_base[i];
    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(VIRTIO_MMIO_VERSION) != 2 ||
        *R(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_CONSOLE ||
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551)
      continue;

    uint32 status = 0;
    *R(VIRTIO_MMIO_STATUS) = status;
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // VERSION_1 and nothing else: a console that is a queue needs no
    // feature of its own, and this driver asks for none.
    *R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 1;
    if (!(*R(VIRTIO_MMIO_DEVICE_FEATURES) & 1))
      continue;                      // not a modern device; leave it alone
    *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = 0;
    *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 1;
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = 1;
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;
    if (!(*R(VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK))
      continue;

    con.idx = i;
    // F18: the receive queue, so that input comes this way too when this is
    // the console: eight small buffers posted, an interrupt when one fills
    *R(VIRTIO_MMIO_QUEUE_SEL) = RXQ;
    if (*R(VIRTIO_MMIO_QUEUE_NUM_MAX) < CQSIZE)
      continue;
    memset(rx_desc, 0, sizeof(rx_desc));
    memset(&rx_avail, 0, sizeof(rx_avail));
    memset(&rx_used, 0, sizeof(rx_used));
    rx_used_idx = 0;
    for (int k = 0; k < CQSIZE; k++) {
      rx_desc[k].addr = (uint64)rx_buf[k];
      rx_desc[k].len = RXBUF;
      rx_desc[k].flags = VRING_DESC_F_WRITE;
      rx_desc[k].next = 0;
      rx_avail.ring[k] = k;
    }
    *R(VIRTIO_MMIO_QUEUE_NUM) = CQSIZE;
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)rx_desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)rx_desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)&rx_avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)&rx_avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)&rx_used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)&rx_used >> 32;
    *R(VIRTIO_MMIO_QUEUE_READY) = 1;
    __sync_synchronize();
    rx_avail.idx = CQSIZE;
    __sync_synchronize();

    *R(VIRTIO_MMIO_QUEUE_SEL) = TXQ;
    if (*R(VIRTIO_MMIO_QUEUE_NUM_MAX) < CQSIZE)
      continue;
    con.desc = con_desc;
    con.avail = &con_avail;
    con.used = &con_used;
    con.buf = con_buf;
    memset(con.desc, 0, sizeof(con_desc));
    memset(con.avail, 0, sizeof(con_avail));
    memset(con.used, 0, sizeof(con_used));
    *R(VIRTIO_MMIO_QUEUE_NUM) = CQSIZE;
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)con.desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)con.desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)con.avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)con.avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)con.used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)con.used >> 32;
    *R(VIRTIO_MMIO_QUEUE_READY) = 1;
    // the host completes inside the notify, so an interrupt would only be
    // a second exit telling us what we already know.
    con.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;
    con.present = 1;
    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;   // the buffers are posted
    printk("virtio console: queue at 0x%lx, %d descriptors\n", con.base, CQSIZE);
    return;
  }
  con.base = 0;
}

// F18: input arrived on the receive queue (or a transmit completed): each
// byte goes to the console's line discipline and the buffer is posted again.
void
virtio_con_intr(int idx)
{
  if (!con.present || idx != con.idx)
    return;
  acquire(&con.lock);
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
  __sync_synchronize();
  while (rx_used_idx != rx_used.idx) {
    __sync_synchronize();
    int id = rx_used.ring[rx_used_idx % CQSIZE].id;
    uint32 len = rx_used.ring[rx_used_idx % CQSIZE].len;
    rx_used_idx++;
    if (id >= 0 && id < CQSIZE) {
      for (uint32 k = 0; k < len && k < RXBUF; k++)
        consoleintr(rx_buf[id][k]);
      rx_avail.ring[rx_avail.idx % CQSIZE] = id;
      __sync_synchronize();
      rx_avail.idx++;
    }
  }
  __sync_synchronize();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;
  release(&con.lock);
}

// F18/F17: the device left the machine (its node is gone from the tree)
void
virtio_con_detach(void)
{
  con.present = 0;
  con.base = 0;
}

// put n bytes on the console. Safe from printk with interrupts off and
// from a panicking hart: it takes con.lock, never sleeps, and does not
// wait for anything except the store that hands the buffer over.
void
virtio_con_write(const char *s, int n)
{
  extern volatile int panicking;

  if (!con.present)
    return;
  if (panicking == 0)
    acquire(&con.lock);
  while (n > 0) {
    int k = n > PGSIZE ? PGSIZE : n;
    memmove(con.buf, s, k);
    // one descriptor, index 0: only one buffer is ever in flight, because
    // the host has finished with it before the notify returns.
    con.desc[0].addr = (uint64)con.buf;
    con.desc[0].len = k;
    con.desc[0].flags = 0;
    con.desc[0].next = 0;
    con.avail->ring[con.avail->idx % CQSIZE] = 0;
    __sync_synchronize();
    con.avail->idx++;
    __sync_synchronize();
    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = TXQ;
    s += k;
    n -= k;
  }
  if (panicking == 0)
    release(&con.lock);
}
