//
// virtual virtio-console: the guest's console as a queue at 0x10004000
// (irq 4), device id 3. Queue 0 carries what the host types to the guest,
// queue 1 what the guest prints.
//
// Why it exists: a booting xv6 guest makes 2840 emulated device accesses
// and 2319 of them are the 16550, because its driver reads the line status
// register to find the transmit register free and then writes one
// character -- two exits a character, at about sixteen microseconds an
// exit. A descriptor full of output is one notification however many
// characters are in it.
//
// Both directions end in the same rings the 16550 uses (vm->cons_out and
// vm->cons_in), so a host that has attached to a machine's console cannot
// tell which of the two devices the guest chose, and a guest that drives
// neither is no worse off than before.
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
#include "vmm.h"
#include "hyp.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc
#define VIRTIO_ID_CONSOLE 3
#define MAXQ 256
#define CHUNK 512      // most of one descriptor taken at a time

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
  vm->con.intr_status |= 1;
  vic_set_level(vm, G_CON_IRQ, 1);
}

// is the guest actually using this device? The 16550 stays wired up
// either way, and this is what says whether anything is listening here.
int
vvcon_active(struct vm *vm)
{
  struct vvcon *c = &vm->con;

  return c->present && (c->status & VIRTIO_CONFIG_S_DRIVER_OK) && c->q[1].ready;
}

// caller holds vm->lock. mark one descriptor chain used.
static void
complete(struct vm *vm, struct vq_state *q, uint16 head, uint32 len)
{
  uint16 used_idx;

  if (rd16(vm, q->used + 2, &used_idx) < 0)
    return;
  uint32 elem[2] = {head, len};
  if (vm_copyout(vm, q->used + 4 + 8 * (used_idx % q->num), elem, 8) < 0)
    return;
  __sync_synchronize();
  wr16(vm, q->used + 2, used_idx + 1);
}

// the guest notified queue 1: take what it printed.
static void
vvcon_tx(struct vm *vm)
{
  struct vvcon *c = &vm->con;
  char buf[CHUNK];

  acquire(&vm->lock);
  c->notifies++;
  struct vq_state *q = &c->q[1];
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
    // walk the chain into a local buffer: the emit below can sleep, and
    // it must not do so holding a half-walked queue.
    uint16 idx = head;
    int got = 0, hops = 0;
    while (hops++ < 16) {
      struct gdesc d;
      if (idx >= q->num || vm_copyin(vm, &d, q->desc + 16 * idx, sizeof(d)) < 0)
        break;
      if (!(d.flags & VRING_DESC_F_WRITE)) {
        uint32 len = d.len;
        if (len > (uint32)(CHUNK - got))
          len = CHUNK - got;
        if (len && vm_copyin(vm, buf + got, d.addr, len) < 0)
          break;
        got += len;
      }
      if (!(d.flags & VRING_DESC_F_NEXT) || got >= CHUNK)
        break;
      idx = d.next;
    }
    complete(vm, q, head, 0);
    q->last_avail++;
    c->tx_bytes += got;
    any = 1;
    if (got > 0) {
      release(&vm->lock);
      vm_cons_emit(vm, buf, got);
      acquire(&vm->lock);
    }
  }
  // the guest may have asked not to be told: it knows the descriptor is
  // done, because this notify is what did it. VRING_AVAIL_F_NO_INTERRUPT.
  uint16 flags = 0;
  if (any && rd16(vm, q->avail, &flags) == 0 && !(flags & 1))
    raise_irq(vm);
  release(&vm->lock);
}

// hand the guest whatever the host has typed, into the buffers it posted
// on queue 0. Called when input arrives and when the guest posts more.
void
vvcon_rx(struct vm *vm)
{
  struct vvcon *c = &vm->con;

  acquire(&vm->lock);
  struct vq_state *q = &c->q[0];
  if (!c->present || !q->ready || q->num == 0 || q->num > MAXQ ||
      !(c->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
    release(&vm->lock);
    return;
  }
  int any = 0;
  while (!cring_empty(&vm->cons_in)) {
    uint16 avail_idx, head;
    if (rd16(vm, q->avail + 2, &avail_idx) < 0 || avail_idx == q->last_avail)
      break;  // the guest has posted nothing to read into
    if (rd16(vm, q->avail + 4 + 2 * (q->last_avail % q->num), &head) < 0)
      break;
    struct gdesc d;
    if (head >= q->num || vm_copyin(vm, &d, q->desc + 16 * head, sizeof(d)) < 0)
      break;
    if (!(d.flags & VRING_DESC_F_WRITE))
      break;
    uint32 room = d.len > CHUNK ? CHUNK : d.len;
    char buf[CHUNK];
    uint32 n = 0;
    while (n < room && !cring_empty(&vm->cons_in))
      buf[n++] = cring_get(&vm->cons_in);
    if (n == 0)
      break;
    if (vm_copyout(vm, d.addr, buf, n) < 0)
      break;
    complete(vm, q, head, n);
    q->last_avail++;
    c->rx_bytes += n;
    any = 1;
  }
  if (any)
    raise_irq(vm);
  release(&vm->lock);
}

static int
vvcon_read(struct vm *vm, struct vdev *dev, uint64 off, int size, uint64 *val)
{
  struct vvcon *c = &vm->con;
  uint64 v = 0;

  if (off >= VIRTIO_MMIO_CONFIG) {
    // struct virtio_console_config: cols, rows, max_nr_ports, emerg_wr.
    // No feature of ours advertises any of it, so it reads as zero.
    *val = 0;
    return 0;
  }
  if (size != 4)
    return -1;
  acquire(&vm->lock);
  uint32 qs = c->queue_sel;   // unsigned on purpose: see vvnet_read
  switch (off) {
  case VIRTIO_MMIO_MAGIC_VALUE: v = 0x74726976; break;
  case VIRTIO_MMIO_VERSION: v = 2; break;
  case VIRTIO_MMIO_DEVICE_ID: v = c->present ? VIRTIO_ID_CONSOLE : 0; break;
  case VIRTIO_MMIO_VENDOR_ID: v = 0x554d4551; break;
  case VIRTIO_MMIO_DEVICE_FEATURES:
    // VIRTIO_F_VERSION_1 (bit 32) and nothing else: no SIZE, no
    // MULTIPORT, no EMERG_WRITE. A console that is two queues and
    // nothing else needs no feature of its own.
    v = (c->device_features_sel == 1) ? 1 : 0;
    break;
  case VIRTIO_MMIO_QUEUE_NUM_MAX: v = (qs < 2) ? MAXQ : 0; break;
  case VIRTIO_MMIO_QUEUE_READY: v = (qs < 2) ? c->q[qs].ready : 0; break;
  case VIRTIO_MMIO_INTERRUPT_STATUS: v = c->intr_status; break;
  case VIRTIO_MMIO_STATUS: v = c->status; break;
  case VIRTIO_MMIO_CONFIG_GENERATION: v = 0; break;
  default: v = 0; break;
  }
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vvcon_write(struct vm *vm, struct vdev *dev, uint64 off, int size, uint64 val)
{
  struct vvcon *c = &vm->con;

  if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
    if (val == 1)
      vvcon_tx(vm);
    else if (val == 0)
      vvcon_rx(vm);   // the guest posted buffers to read into
    return 0;
  }
  if (size != 4)
    return -1;
  acquire(&vm->lock);
  uint32 qs = c->queue_sel;
  struct vq_state *q = (qs < 2) ? &c->q[qs] : 0;
  switch (off) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: c->device_features_sel = val; break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL: c->driver_features_sel = val; break;
  case VIRTIO_MMIO_DRIVER_FEATURES: break;
  case VIRTIO_MMIO_QUEUE_SEL: c->queue_sel = val; break;
  case VIRTIO_MMIO_QUEUE_NUM: if (q) q->num = val; break;
  case VIRTIO_MMIO_QUEUE_READY:
    if (q) {
      q->ready = val & 1;
      q->last_avail = 0;
    }
    break;
  case VIRTIO_MMIO_INTERRUPT_ACK:
    c->intr_status &= ~val;
    vic_set_level(vm, G_CON_IRQ, c->intr_status != 0);
    break;
  case VIRTIO_MMIO_STATUS:
    c->status = val;
    if (val == 0) {
      for (int i = 0; i < 2; i++) {
        c->q[i].ready = 0;
        c->q[i].num = 0;
        c->q[i].last_avail = 0;
      }
      c->intr_status = 0;
      vic_set_level(vm, G_CON_IRQ, 0);
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
vvcon_init(struct vm *vm)
{
  struct vvcon *c = &vm->con;

  memset(c, 0, sizeof(*c));
  c->present = (vm->cfg.flags & VMF_CONQUEUE) ? 1 : 0;
  if (c->present)
    vm_add_dev(vm, "virtio-con", G_CON_IRQ, OBS_VIRTIO_CON, G_CON_BASE + vm->mmio_shift, 0x1000, vvcon_read, vvcon_write, 0);
}
