//
// driver for qemu's virtio network device (virtio-mmio, modern).
//
// qemu ... -netdev user,id=n0 -device virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1
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
#include "net.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024

#define VIRTIO_NET_F_MAC 5

// the virtio-net header that precedes every frame (12 bytes with VERSION_1).
struct virtio_net_hdr {
  uint8 flags;
  uint8 gso_type;
  uint16 hdr_len;
  uint16 gso_size;
  uint16 csum_start;
  uint16 csum_offset;
  uint16 num_buffers;
} __attribute__((packed));
#define VNET_HDR_LEN 12

#define RXQ 0
#define TXQ 1
#define QSIZE 256 // descriptors per queue (power of two, <= NUM_MAX): the device's maximum. 16 dropped 41 frames a side per migration run, 64 still ~30 a crossing at the transmit ring

struct vq {
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  uint16 used_idx;
  struct mbuf *bufs[QSIZE]; // mbuf owning descriptor i
  char free[QSIZE];
};

static struct {
  uint64 base;
  int idx;    // platform virtio slot
  int irq;
  uint8 mac[ETHADDR_LEN];
  struct vq q[2];
  struct spinlock lock;
  int present;
} nic;

#define R(r) ((volatile uint32 *)(nic.base + (r)))

static void
vq_init(struct vq *q, int qn)
{
  *R(VIRTIO_MMIO_QUEUE_SEL) = qn;
  if (*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio net queue ready");
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (max < QSIZE)
    panic("virtio net queue too small");
  q->desc = kalloc();
  q->avail = kalloc();
  q->used = kalloc();
  if (!q->desc || !q->avail || !q->used)
    panic("virtio net kalloc");
  memset(q->desc, 0, PGSIZE);
  memset(q->avail, 0, PGSIZE);
  memset(q->used, 0, PGSIZE);
  *R(VIRTIO_MMIO_QUEUE_NUM) = QSIZE;
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)q->desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)q->desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)q->avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)q->avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)q->used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)q->used >> 32;
  *R(VIRTIO_MMIO_QUEUE_READY) = 1;
  for (int i = 0; i < QSIZE; i++)
    q->free[i] = 1;
}

static int
vq_alloc(struct vq *q)
{
  for (int i = 0; i < QSIZE; i++) {
    if (q->free[i]) {
      q->free[i] = 0;
      return i;
    }
  }
  return -1;
}

// post one empty mbuf for the device to fill with a received frame.
// returns 0 when it could not (no descriptor, or no memory for the mbuf).
static int rx_posted;          // buffers the device holds; the ring is full at QSIZE
uint64 net_rx_nobuf;           // times a buffer could not be posted (memory)

static int
rx_post(void)
{
  struct vq *q = &nic.q[RXQ];
  int i = vq_alloc(q);
  if (i < 0)
    return 0;
  struct mbuf *m = mbufalloc(0);
  if (m == 0) {
    q->free[i] = 1;
    net_rx_nobuf++;
    return 0;
  }
  q->bufs[i] = m;
  q->desc[i].addr = (uint64)m->buf;
  q->desc[i].len = MBUF_SIZE;
  q->desc[i].flags = VRING_DESC_F_WRITE;
  q->desc[i].next = 0;
  q->avail->ring[q->avail->idx % QSIZE] = i;
  io_fence();
  q->avail->idx++;
  rx_posted++;
  return 1;
}

// the ring back to full. A buffer that could not be posted -- kalloc had
// nothing, as on a host taking in a guest -- used to be a buffer the ring
// had lost for good: one frame's worth of receive capacity gone per failure,
// and once all sixteen were gone every inbound frame was dropped at the
// device, for ever (hunt3_7 crossing 97: the sender's last sections, its FIN
// and ten retransmissions all vanished; the receiver sat ESTABLISHED with
// nothing to read). Now the count is kept and the ring is refilled after
// every interrupt and from the timer, so memory coming back brings the
// receiver back. caller holds nic.lock.
static void
rx_refill(void)
{
  while (rx_posted < QSIZE)
    if (!rx_post())
      break;
}

// from net_timer(): a ring starved of buffers gets them back once memory does
void
virtio_net_refill(void)
{
  if (!nic.present)
    return;
  acquire(&nic.lock);
  if (rx_posted < QSIZE) {
    rx_refill();
    io_fence();
    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;
  }
  release(&nic.lock);
}

int
virtio_net_present(void)
{
  return nic.present;
}

// the register window moved (machine.c, REV_DEVICES): repoint the driver.
void
virtio_net_rebase(uint64 old, uint64 nw)
{
  if (nic.base == old)
    nic.base = nw;
}

int
virtio_net_init(void)
{
  initlock(&nic.lock, "virtio_net");
  nic.present = 0;
  for (int i = 0; i < plat.nvirtio; i++) {
    nic.base = plat.virtio_base[i];
    if (*R(VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976 && *R(VIRTIO_MMIO_VERSION) == 2 &&
        *R(VIRTIO_MMIO_DEVICE_ID) == 1) {
      nic.idx = i;
      nic.irq = plat.virtio_irq[i];
      nic.present = 1;
      break;
    }
  }
  if (!nic.present)
    return -1;

  uint32 status = 0;
  *R(VIRTIO_MMIO_STATUS) = status;
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // features: only the MAC address (sel 0) and VERSION_1 (sel 1, bit 0).
  *R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 0;
  uint32 f0 = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  *R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 1;
  uint32 f1 = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  if (!(f1 & 1))
    panic("virtio net: not a modern device");
  *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = f0 & (1 << VIRTIO_NET_F_MAC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 1;
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = 1;

  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;
  if (!(*R(VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio net FEATURES_OK unset");

  for (int i = 0; i < ETHADDR_LEN; i++)
    nic.mac[i] = *(volatile uint8 *)(nic.base + VIRTIO_MMIO_CONFIG + i);

  vq_init(&nic.q[RXQ], RXQ);
  vq_init(&nic.q[TXQ], TXQ);

  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  rx_refill();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;

  memmove(netcfg.mac, nic.mac, ETHADDR_LEN);
  char macs[18];
  printk("virtio net: mac %s\n", mac2str(nic.mac, macs));
  return 0;
}

// how many frames the transmit ring can take right now. TCP asks before it
// sends a burst: a segment handed to a full ring was dropped and counted as
// sent, and every such drop was a retransmission timeout later (hunt3_1000:
// 5,221 transmit drops in 178 crossings, and one connection reset).
int
virtio_net_tx_room(void)
{
  if (!nic.present)
    return 1 << 20;
  acquire(&nic.lock);
  struct vq *q = &nic.q[TXQ];
  while (q->used_idx != q->used->idx) {
    io_fence();
    int id = q->used->ring[q->used_idx % QSIZE].id;
    if (q->bufs[id]) {
      mbuffree(q->bufs[id]);
      q->bufs[id] = 0;
    }
    q->free[id] = 1;
    q->used_idx++;
  }
  int room = 0;
  for (int i = 0; i < QSIZE; i++)
    room += q->free[i] ? 1 : 0;
  release(&nic.lock);
  return room;
}

// transmit an ethernet frame. takes ownership of m. may be called from
// interrupt context. returns 0 or -1 (dropped).
int
virtio_net_tx(struct mbuf *m)
{
  if (!nic.present) {
    mbuffree(m);
    return -1;
  }
  acquire(&nic.lock);
  struct vq *q = &nic.q[TXQ];

  // reclaim completed transmissions first.
  while (q->used_idx != q->used->idx) {
    io_fence();
    int id = q->used->ring[q->used_idx % QSIZE].id;
    if (q->bufs[id]) {
      mbuffree(q->bufs[id]);
      q->bufs[id] = 0;
    }
    q->free[id] = 1;
    q->used_idx++;
  }

  int i = vq_alloc(q);
  if (i < 0) {
    release(&nic.lock);
    mbuffree(m);
    net_tx_dropped++;
    return -1;
  }
  // the virtio-net header goes in front of the frame.
  char *h = mbufpush(m, VNET_HDR_LEN);
  memset(h, 0, VNET_HDR_LEN);
  q->bufs[i] = m;
  q->desc[i].addr = (uint64)m->head;
  q->desc[i].len = m->len;
  q->desc[i].flags = 0;
  q->desc[i].next = 0;
  q->avail->ring[q->avail->idx % QSIZE] = i;
  io_fence();
  q->avail->idx++;
  io_fence();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = TXQ;
  net_tx_packets++;
  release(&nic.lock);
  return 0;
}

void
virtio_net_intr(int idx)
{
  if (!nic.present || idx != nic.idx)
    return;
  acquire(&nic.lock);
  uint32 st = *R(VIRTIO_MMIO_INTERRUPT_STATUS);
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = st & 0x3;
  io_fence();
  if (st & 2) {
    // a configuration change (F19): the device's identity may have moved
    uint8 mac[ETHADDR_LEN];
    for (int i = 0; i < ETHADDR_LEN; i++)
      mac[i] = *(volatile uint8 *)(nic.base + VIRTIO_MMIO_CONFIG + i);
    if (memcmp(mac, nic.mac, ETHADDR_LEN) != 0) {
      memmove(nic.mac, mac, ETHADDR_LEN);
      net_mac_changed(mac);
      char macs[18];
      printk("virtio net: mac changed to %s\n", mac2str(nic.mac, macs));
    }
  }

  struct vq *q = &nic.q[RXQ];
  while (q->used_idx != q->used->idx) {
    io_fence();
    int id = q->used->ring[q->used_idx % QSIZE].id;
    uint32 len = q->used->ring[q->used_idx % QSIZE].len;
    q->used_idx++;
    struct mbuf *m = q->bufs[id];
    q->bufs[id] = 0;
    q->free[id] = 1;
    rx_posted--;
    if (m && len > VNET_HDR_LEN) {
      m->head = m->buf + VNET_HDR_LEN;
      m->len = len - VNET_HDR_LEN;
      net_rx_packets++;
      release(&nic.lock);
      bridge_rx_uplink(m); // takes ownership
      acquire(&nic.lock);
    } else if (m) {
      mbuffree(m);
    }
    rx_post();
  }
  rx_refill();      // and whatever earlier failures left unposted
  io_fence();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;

  // completed transmissions.
  q = &nic.q[TXQ];
  while (q->used_idx != q->used->idx) {
    io_fence();
    int id = q->used->ring[q->used_idx % QSIZE].id;
    if (q->bufs[id]) {
      mbuffree(q->bufs[id]);
      q->bufs[id] = 0;
    }
    q->free[id] = 1;
    q->used_idx++;
  }
  release(&nic.lock);
}
