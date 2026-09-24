//
// virtual virtio-net device: a guest NIC at 0x10002000 (irq 2), wired
// to the software switch (bridge.c). receive queue 0, transmit queue 1,
// the 12-byte virtio-net header, feature bits MAC and VERSION_1 only.
//
// vvnet_rx() is called from interrupt or vcpu context to hand a frame
// to the guest: it fills the next buffer the guest posted (or drops).
// guest transmissions are taken from the queue when the guest notifies.
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
#include "net.h"
#include "hyp.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc
#define VIRTIO_NET_F_MAC 5
#define VNET_HDR_LEN 12
#define MAXQ 256

struct gdesc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};

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

// caller holds vm->lock.
static void
raise_irq(struct vm *vm)
{
  vm->vnet.intr_status |= 1;
  vic_set_level(vm, G_VNET_IRQ, 1);
}

// deliver a frame into the guest's receive queue. takes ownership.
void
vvnet_rx(struct vm *vm, struct mbuf *m)
{
  struct vvnet *n = &vm->vnet;
  acquire(&vm->lock);
  struct vq_state *q = &n->q[0];
  if (!n->present || !q->ready || q->num == 0 || q->num > MAXQ || !(n->status & VIRTIO_CONFIG_S_DRIVER_OK))
    goto drop;
  uint16 avail_idx, head;
  if (rd16(vm, q->avail + 2, &avail_idx) < 0 || avail_idx == q->last_avail)
    goto drop; // no buffers posted
  if (rd16(vm, q->avail + 4 + 2 * (q->last_avail % q->num), &head) < 0)
    goto drop;
  // walk the chain of writable buffers: header then data.
  struct gdesc d;
  uint16 idx = head;
  uint32 need = VNET_HDR_LEN + m->len, done = 0;
  char hdr[VNET_HDR_LEN];
  memset(hdr, 0, sizeof(hdr));
  hdr[10] = 1; // num_buffers = 1
  int hops = 0;
  while (done < need && hops++ < 16) {
    if (idx >= q->num || vm_copyin(vm, &d, q->desc + 16 * idx, sizeof(d)) < 0)
      goto drop;
    if (!(d.flags & VRING_DESC_F_WRITE))
      goto drop;
    uint32 space = d.len, off = 0;
    while (space > 0 && done < need) {
      uint32 chunk;
      if (done < VNET_HDR_LEN) {
        chunk = VNET_HDR_LEN - done;
        if (chunk > space)
          chunk = space;
        if (vm_copyout(vm, d.addr + off, hdr + done, chunk) < 0)
          goto drop;
      } else {
        chunk = need - done;
        if (chunk > space)
          chunk = space;
        if (vm_copyout(vm, d.addr + off, m->head + (done - VNET_HDR_LEN), chunk) < 0)
          goto drop;
      }
      done += chunk;
      off += chunk;
      space -= chunk;
    }
    if (!(d.flags & VRING_DESC_F_NEXT))
      break;
    idx = d.next;
  }
  if (done < need)
    goto drop; // buffers too small
  uint16 used_idx;
  if (rd16(vm, q->used + 2, &used_idx) < 0)
    goto drop;
  uint32 elem[2] = {head, done};
  if (vm_copyout(vm, q->used + 4 + 8 * (used_idx % q->num), elem, 8) < 0)
    goto drop;
  __sync_synchronize();
  wr16(vm, q->used + 2, used_idx + 1);
  q->last_avail++;
  n->rx_frames++;
  raise_irq(vm);
  release(&vm->lock);
  mbuffree(m);
  return;
drop:
  n->rx_dropped++;
  release(&vm->lock);
  mbuffree(m);
}

// the guest notified the transmit queue: pull frames and switch them.
static void
vvnet_tx(struct vm *vm)
{
  struct vvnet *n = &vm->vnet;
  acquire(&vm->lock);
  struct vq_state *q = &n->q[1];
  if (!q->ready || q->num == 0 || q->num > MAXQ) {
    release(&vm->lock);
    return;
  }
  int any = 0;
  for (int rounds = 0; rounds < 64; rounds++) {
    uint16 avail_idx, head;
    if (rd16(vm, q->avail + 2, &avail_idx) < 0 || avail_idx == q->last_avail)
      break;
    if (rd16(vm, q->avail + 4 + 2 * (q->last_avail % q->num), &head) < 0)
      break;
    struct mbuf *m = mbufalloc(MBUF_HEADROOM);
    struct gdesc d;
    uint16 idx = head;
    uint32 got = 0;
    int ok = (m != 0), hops = 0;
    while (ok && hops++ < 16) {
      if (idx >= q->num || vm_copyin(vm, &d, q->desc + 16 * idx, sizeof(d)) < 0 || (d.flags & VRING_DESC_F_WRITE)) {
        ok = 0;
        break;
      }
      uint32 len = d.len;
      if (got + len > VNET_HDR_LEN + 1600) {
        ok = 0;
        break;
      }
      char *dst;
      if (got >= VNET_HDR_LEN) {
        dst = mbufput(m, len);
        if (vm_copyin(vm, dst, d.addr, len) < 0)
          ok = 0;
      } else {
        // header (skipped) possibly followed by data in the same buffer
        uint32 skip = VNET_HDR_LEN - got;
        if (skip > len)
          skip = len;
        if (len > skip) {
          dst = mbufput(m, len - skip);
          if (vm_copyin(vm, dst, d.addr + skip, len - skip) < 0)
            ok = 0;
        }
      }
      got += len;
      if (!(d.flags & VRING_DESC_F_NEXT))
        break;
      idx = d.next;
    }
    uint16 used_idx;
    if (rd16(vm, q->used + 2, &used_idx) == 0) {
      uint32 elem[2] = {head, 0};
      vm_copyout(vm, q->used + 4 + 8 * (used_idx % q->num), elem, 8);
      __sync_synchronize();
      wr16(vm, q->used + 2, used_idx + 1);
    }
    q->last_avail++;
    any = 1;
    if (ok && m->len >= 14) {
      n->tx_frames++;
      release(&vm->lock);
      bridge_tx_guest(vm, m); // takes ownership
      acquire(&vm->lock);
    } else if (m) {
      n->tx_dropped++;
      mbuffree(m);
    }
  }
  if (any)
    raise_irq(vm);
  release(&vm->lock);
}

static int
vvnet_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  struct vvnet *n = &vm->vnet;
  uint64 v = 0;
  if (off >= VIRTIO_MMIO_CONFIG) {
    uint64 cfg = off - VIRTIO_MMIO_CONFIG;
    // mac[6] at 0, status (u16, link up) at 6
    for (int i = 0; i < size && i < 8; i++) {
      uint64 c = cfg + i;
      uint8 b = c < 6 ? n->mac[c] : (c == 6 ? 1 : 0);
      v |= (uint64)b << (8 * i);
    }
    *val = v;
    return 0;
  }
  if (size != 4)
    return -1;
  acquire(&vm->lock);
  // unsigned, and deliberately: queue_sel is whatever the guest last
  // wrote to it, and `int qs = ...; if (qs < 2)' let a guest choose
  // 0x80000000, pass the test with a negative number and have the
  // host index q[] with it.
  uint32 qs = n->queue_sel;
  switch (off) {
  case VIRTIO_MMIO_MAGIC_VALUE: v = 0x74726976; break;
  case VIRTIO_MMIO_VERSION: v = 2; break;
  case VIRTIO_MMIO_DEVICE_ID: v = n->present ? 1 : 0; break;
  case VIRTIO_MMIO_VENDOR_ID: v = 0x554d4551; break;
  case VIRTIO_MMIO_DEVICE_FEATURES:
    v = (n->device_features_sel == 0) ? (1 << VIRTIO_NET_F_MAC) : (n->device_features_sel == 1 ? 1 : 0);
    break;
  case VIRTIO_MMIO_QUEUE_NUM_MAX: v = (qs < 2) ? MAXQ : 0; break;
  case VIRTIO_MMIO_QUEUE_READY: v = (qs < 2) ? n->q[qs].ready : 0; break;
  case VIRTIO_MMIO_INTERRUPT_STATUS: v = n->intr_status; break;
  case VIRTIO_MMIO_STATUS: v = n->status; break;
  case VIRTIO_MMIO_CONFIG_GENERATION: v = 0; break;
  default: v = 0; break;
  }
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vvnet_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  struct vvnet *n = &vm->vnet;
  if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
    if (val == 1)
      vvnet_tx(vm);
    return 0;
  }
  if (size != 4)
    return -1;
  acquire(&vm->lock);
  uint32 qs = n->queue_sel;   // unsigned: see vvnet_read
  struct vq_state *q = (qs < 2) ? &n->q[qs] : 0;
  switch (off) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: n->device_features_sel = val; break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL: n->driver_features_sel = val; break;
  case VIRTIO_MMIO_DRIVER_FEATURES: break;
  case VIRTIO_MMIO_QUEUE_SEL: n->queue_sel = val; break;
  case VIRTIO_MMIO_QUEUE_NUM: if (q) q->num = val; break;
  case VIRTIO_MMIO_QUEUE_READY:
    if (q) {
      q->ready = val & 1;
      q->last_avail = 0;
    }
    break;
  case VIRTIO_MMIO_INTERRUPT_ACK:
    n->intr_status &= ~val;
    vic_set_level(vm, G_VNET_IRQ, n->intr_status != 0);
    break;
  case VIRTIO_MMIO_STATUS:
    n->status = val;
    if (val == 0) {
      for (int i = 0; i < 2; i++) {
        n->q[i].ready = 0;
        n->q[i].num = 0;
        n->q[i].last_avail = 0;
      }
      n->intr_status = 0;
      vic_set_level(vm, G_VNET_IRQ, 0);
    }
    break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW: if (q) q->desc = (q->desc & ~0xffffffffUL) | val; break;
  case VIRTIO_MMIO_QUEUE_DESC_HIGH: if (q) q->desc = (q->desc & 0xffffffffUL) | (val << 32); break;
  case VIRTIO_MMIO_DRIVER_DESC_LOW: if (q) q->avail = (q->avail & ~0xffffffffUL) | val; break;
  case VIRTIO_MMIO_DRIVER_DESC_HIGH: if (q) q->avail = (q->avail & 0xffffffffUL) | (val << 32); break;
  case VIRTIO_MMIO_DEVICE_DESC_LOW: if (q) q->used = (q->used & ~0xffffffffUL) | val; break;
  case VIRTIO_MMIO_DEVICE_DESC_HIGH: if (q) q->used = (q->used & 0xffffffffUL) | (val << 32); break;
  default: break;
  }
  release(&vm->lock);
  return 0;
}

void
vvnet_init(struct vm *vm)
{
  struct vvnet *n = &vm->vnet;
  memset(n, 0, sizeof(*n));
  n->present = virtio_net_present();
  // a stable, locally administered MAC per VM slot
  n->mac[0] = 0x52;
  n->mac[1] = 0x54;
  n->mac[2] = 0x00;
  n->mac[3] = 0x78;
  n->mac[4] = 0x76;
  n->mac[5] = 0x10 + vm->id;
  vm_add_dev(vm, G_VNET_BASE, 0x1000, vvnet_read, vvnet_write, 0);
}
