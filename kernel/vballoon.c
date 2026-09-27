//
// virtio-balloon: giving guest memory back to the host.
//
// the host writes a target into the device's configuration -- how many
// 4 KB pages of its own memory it would like the guest to stop using --
// and raises a configuration-change interrupt. The guest's driver
// allocates that many pages, so that nothing else in the guest will
// touch them, and hands their page frame numbers over on the inflate
// queue. The host then takes the backing away (vm_unback), and the
// memory is the host's again.
//
// deflating is the same in reverse and costs the host nothing: the
// guest frees the pages it had set aside and reports them on the
// deflate queue, and because guest memory here is backed on demand,
// the guest's next touch of one faults a fresh zeroed page in. There
// is nothing for the host to hand back.
//
// what makes this safe is that the host never takes a page the guest
// has not offered. A balloon is a request; a guest that does not
// answer keeps everything it has, and a guest that lies about a page
// it is still using only corrupts itself.
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
#include "page.h"
#include "blk.h"
#include "hyp.h"

#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc

#define MAXQ 256
#define VIRTIO_ID_BALLOON 5

// the balloon's page size is 4 KB whatever the host's is, which is
// what the specification says and what the guest's driver assumes
#define BALLOON_PFN_SHIFT 12

struct gdesc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};

static void
raise_irq(struct vm *vm, uint32 bits)
{
  vm->balloon.intr_status |= bits;
  vic_set_level(vm, vm_irq(vm, G_BALLOON_IRQ), 1);
}

// one buffer of page frame numbers. `take` says whether the pages are
// being handed to the host (inflate) or asked back (deflate).
static int
do_pfns(struct vm *vm, uint64 gpa, uint32 len, int take)
{
  uint32 pfns[64];
  int done = 0;

  while (len >= 4) {
    uint32 n = len > sizeof(pfns) ? sizeof(pfns) : len;
    n &= ~3u;
    if (vm_copyin(vm, pfns, gpa, n) < 0)
      return done;
    for (uint32 i = 0; i < n / 4; i++) {
      uint64 pgpa = (uint64)pfns[i] << BALLOON_PFN_SHIFT;
      if (take) {
        // a page the guest has set aside: take its backing away. a
        // page outside guest RAM is the guest's mistake, and saying no
        // to it is the whole of the answer.
        if (vm_unback(vm, pgpa) == 0)
          done++;
      } else {
        // deflate: nothing to do but count it. the page is unbacked,
        // and the guest's next touch of it brings a zeroed one back.
        done++;
      }
    }
    gpa += n;
    len -= n;
  }
  return done;
}

// the statistics the guest reports: pairs of (u16 tag, u64 value)
static void
do_stats(struct vm *vm, uint64 gpa, uint32 len)
{
  struct {
    uint16 tag;
    uint64 val;
  } __attribute__((packed)) e;

  while (len >= sizeof(e)) {
    if (vm_copyin(vm, &e, gpa, sizeof(e)) < 0)
      return;
    if (e.tag < 8)
      vm->balloon.stats[e.tag] = e.val;
    vm->balloon.have_stats = 1;
    gpa += sizeof(e);
    len -= sizeof(e);
  }
}

// walk a queue's available ring. vm->lock is not held: taking pages
// away sleeps on nothing but it does take the memory lock, and the
// ring itself is only touched by this one queue's notification.
static void
process_queue(struct vm *vm, int qi)
{
  struct vballoon *b = &vm->balloon;
  struct vq_state *q = &b->q[qi];
  int any = 0, cfg = 0;

  // The statistics queue is not drained when the guest kicks it. The guest
  // posts a buffer and the device is to use it *when it wants a report*
  // (the spec's words; QEMU asks only when its monitor does). Taking it at
  // once made Linux's driver -- which posts the next buffer as soon as one
  // is taken -- run round the queue at the speed of its workqueue: 11,000
  // interrupts a second, a Linux guest crawling, and a spinlock touched
  // before its probe had initialised it (tm1: `spinlock bad magic' in
  // stats_request). The poller (policy_once, once a second) takes one.
  if (qi == 2 && !b->stats_go)
    return;

  // bounded, for the reason process_queue in vvirtio.c is bounded: a
  // guest that keeps the available index moving must not be able to
  // keep this loop running.
  for (int done = 0; done < MAXQ; done++) {
    if (qi == 2 && any)
      break;              // one report per poll
    uint16 avail_idx, head, used_idx;
    acquire(&vm->lock);
    uint32 num = q->num;
    int ready = q->ready && num > 0 && num <= MAXQ;
    uint64 avail = q->avail, desc = q->desc, used = q->used;
    uint16 last = q->last_avail;
    release(&vm->lock);
    if (!ready)
      break;

    uint16 ai;
    if (vm_copyin(vm, &ai, avail + 2, 2) < 0)
      break;
    avail_idx = ai;
    if (avail_idx == last)
      break;
    if ((uint16)(avail_idx - last) > (uint16)num) {
      if (vm_gripe(&b->desync))
        printk("hyp: vm %d: balloon available index %d is ahead of a "
               "%d-entry ring (%ld times)\n", vm->id, avail_idx, num,
               b->desync);
      acquire(&vm->lock);
      q->last_avail = avail_idx;
      release(&vm->lock);
      break;
    }
    if (vm_copyin(vm, &head, avail + 4 + 2 * (last % num), 2) < 0)
      break;

    // the chain: buffers of page frame numbers, or of statistics
    struct gdesc d;
    uint16 idx = head;
    int hops = 0, got = 0;
    while (hops++ < 16) {
      if (idx >= num || vm_copyin(vm, &d, desc + 16 * idx, sizeof(d)) < 0)
        break;
      if (qi == 2)
        do_stats(vm, d.addr, d.len);
      else
        got += do_pfns(vm, d.addr, d.len, qi == 0);
      if (!(d.flags & VRING_DESC_F_NEXT))
        break;
      idx = d.next;
    }

    acquire(&vm->lock);
    if (qi == 0) {
      b->actual += got;
      b->inflated += got;
    } else if (qi == 1) {
      b->actual = b->actual > (uint32)got ? b->actual - got : 0;
      b->deflated += got;
    }
    cfg = qi != 2;
    release(&vm->lock);

    if (vm_copyin(vm, &used_idx, used + 2, 2) < 0)
      break;
    uint32 elem[2] = {head, 0};
    if (vm_copyout(vm, used + 4 + 8 * (used_idx % num), elem, 8) < 0)
      break;
    __sync_synchronize();
    uint16 ni = used_idx + 1;
    if (vm_copyout(vm, used + 2, &ni, 2) < 0)
      break;
    acquire(&vm->lock);
    q->last_avail++;
    release(&vm->lock);
    any = 1;
  }

  if (any) {
    acquire(&vm->lock);
    // bit 0 is "a buffer was used", bit 1 "the configuration changed";
    // `actual` has moved, so the guest is told both
    raise_irq(vm, cfg ? 3 : 1);
    release(&vm->lock);
  }
}

// ---- the host's side ----

// ask the guest to give `bytes` of its memory back. the guest answers
// in its own time, and vminfo reports how far it has got.
int
vballoon_set(struct vm *vm, uint64 bytes)
{
  uint32 pages = bytes / PGSIZE;

  if (!vm->balloon.present)
    return -1;
  if (bytes > vm->ram_size)
    return -1;
  acquire(&vm->lock);
  vm->balloon.target = pages;
  vm->balloon.config_gen++;
  raise_irq(vm, 2);   // the configuration changed
  release(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);
  return 0;
}

// ---- the policy thread ----
//
// a balloon is usually driven by whoever runs the machine: `vmm
// balloon` says how much to ask for and that is that. This thread is
// what happens when nobody is watching and the host runs out anyway.
//
// it only ever acts under real pressure -- free memory below the
// reserve the allocator keeps for the host itself -- and then it asks
// the VM holding the most, and only for as much as is missing. When
// memory comes back it lowers what it asked for, so a guest that gave
// up memory in a bad minute gets it back. A target a person set by
// hand is never lowered below what they asked for.

static uint64 policy_floor[NVM];   // what `vmm balloon` asked for, per VM

// take the statistics report a guest has posted, if any: once a second
static void
stats_poll(void)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state != VM_RUNNING || !vm->balloon.present)
      continue;
    vm->balloon.stats_go = 1;
    process_queue(vm, 2);
    vm->balloon.stats_go = 0;
  }
}

static void
policy_once(void)
{
  struct meminfo mi;
  uint64 free, want;

  stats_poll();
  kmem_info(&mi);
  free = mi.free_pages + blk_reclaimable();
  if (mi.reserve_pages == 0)
    return;

  if (free < mi.reserve_pages) {
    // short: the difference, from whichever VM has the most
    want = (mi.reserve_pages - free) * PGSIZE;
    struct vm *worst = 0;
    for (int i = 0; i < NVM; i++) {
      struct vm *vm = vmtab[i];
      if (vm == 0 || vm->state == VM_FREE || !vm->balloon.present)
        continue;
      if (worst == 0 || vm->resident > worst->resident)
        worst = vm;
    }
    if (worst == 0 || worst->resident <= want)
      return;   // taking it all would not leave the guest a machine
    uint64 target = (uint64)worst->balloon.target * PGSIZE + want;
    if (target > worst->ram_size / 2)
      target = worst->ram_size / 2;   // never more than half
    if (target > (uint64)worst->balloon.target * PGSIZE)
      vballoon_set(worst, target);
    return;
  }

  // comfortable again: give back what this thread asked for, a
  // sixteenth of the VM's memory at a time so it is not a lurch
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state == VM_FREE || !vm->balloon.present)
      continue;
    uint64 have = (uint64)vm->balloon.target * PGSIZE;
    if (have <= policy_floor[i])
      continue;
    uint64 step = vm->ram_size / 16;
    uint64 target = have > step ? have - step : 0;
    if (target < policy_floor[i])
      target = policy_floor[i];
    vballoon_set(vm, target);
  }
}

static void
policy_thread(void *arg)
{
  for (;;) {
    for (int i = 0; i < 10; i++) {   // once a second
      acquire(&tickslock);
      sleep_prepare(&ticks);
      release(&tickslock);
      sleep();
    }
    policy_once();
  }
}

// ---- mmio transport ----

static int
vballoon_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  struct vballoon *b = &vm->balloon;
  uint64 v = 0;

  if (off >= VIRTIO_MMIO_CONFIG) {
    // config space: u32 num_pages, u32 actual, both little-endian
    uint64 c = off - VIRTIO_MMIO_CONFIG;
    acquire(&vm->lock);
    uint32 words[2] = {b->target, b->actual};
    release(&vm->lock);
    if (c + size <= 8)
      memmove(&v, (char *)words + c, size);
    *val = v;
    return 0;
  }
  if (size != 4)
    return -1;

  acquire(&vm->lock);
  switch (off) {
  case VIRTIO_MMIO_MAGIC_VALUE: v = 0x74726976; break;
  case VIRTIO_MMIO_VERSION: v = 2; break;
  case VIRTIO_MMIO_DEVICE_ID: v = b->present ? VIRTIO_ID_BALLOON : 0; break;
  case VIRTIO_MMIO_VENDOR_ID: v = 0x554d4551; break;
  case VIRTIO_MMIO_DEVICE_FEATURES:
    // sel 0: the balloon's own features; sel 1 bit 0: VIRTIO_F_VERSION_1
    v = (b->device_features_sel == 1) ? 1 : (1u << VIRTIO_BALLOON_F_STATS_VQ);
    break;
  case VIRTIO_MMIO_QUEUE_NUM_MAX: v = (b->queue_sel < 3) ? MAXQ : 0; break;
  case VIRTIO_MMIO_QUEUE_READY:
    v = (b->queue_sel < 3) ? b->q[b->queue_sel].ready : 0;
    break;
  case VIRTIO_MMIO_INTERRUPT_STATUS: v = b->intr_status; break;
  case VIRTIO_MMIO_STATUS: v = b->status; break;
  case VIRTIO_MMIO_CONFIG_GENERATION: v = b->config_gen; break;
  default: v = 0; break;
  }
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vballoon_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  struct vballoon *b = &vm->balloon;

  if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
    if (val < 3)
      process_queue(vm, val);
    return 0;
  }
  if (size != 4)
    return -1;

  acquire(&vm->lock);
  // unsigned: the guest chooses this value, and a signed one below 3
  // includes every negative number there is (see vvnet_read).
  uint32 sel = b->queue_sel;
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
    if (sel < 3)
      b->q[sel].num = val;
    break;
  case VIRTIO_MMIO_QUEUE_READY:
    if (sel < 3) {
      b->q[sel].ready = val & 1;
      b->q[sel].last_avail = 0;
    }
    break;
  case VIRTIO_MMIO_INTERRUPT_ACK:
    b->intr_status &= ~(uint32)val;
    vic_set_level(vm, vm_irq(vm, G_BALLOON_IRQ), b->intr_status != 0);
    break;
  case VIRTIO_MMIO_STATUS:
    b->status = val;
    if (val == 0) {
      for (int i = 0; i < 3; i++) {
        b->q[i].ready = 0;
        b->q[i].num = 0;
        b->q[i].last_avail = 0;
      }
      b->intr_status = 0;
      b->driver_features = 0;
      b->actual = 0;
      vic_set_level(vm, vm_irq(vm, G_BALLOON_IRQ), 0);
    }
    break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW:
    if (sel < 3) b->q[sel].desc = (b->q[sel].desc & ~0xffffffffUL) | val;
    break;
  case VIRTIO_MMIO_QUEUE_DESC_HIGH:
    if (sel < 3) b->q[sel].desc = (b->q[sel].desc & 0xffffffffUL) | (val << 32);
    break;
  case VIRTIO_MMIO_DRIVER_DESC_LOW:
    if (sel < 3) b->q[sel].avail = (b->q[sel].avail & ~0xffffffffUL) | val;
    break;
  case VIRTIO_MMIO_DRIVER_DESC_HIGH:
    if (sel < 3) b->q[sel].avail = (b->q[sel].avail & 0xffffffffUL) | (val << 32);
    break;
  case VIRTIO_MMIO_DEVICE_DESC_LOW:
    if (sel < 3) b->q[sel].used = (b->q[sel].used & ~0xffffffffUL) | val;
    break;
  case VIRTIO_MMIO_DEVICE_DESC_HIGH:
    if (sel < 3) b->q[sel].used = (b->q[sel].used & 0xffffffffUL) | (val << 32);
    break;
  default:
    break;
  }
  release(&vm->lock);
  return 0;
}

// what a person asked for by hand, which the policy thread will not
// undo. vm_ctl calls this; the thread calls vballoon_set directly.
// F14: a machine arriving with a balloon the source inflated for this host's
// sake keeps it: the policy thread's floor is set to what the guest holds, or
// it would hand the memory back within seconds and the room this host had for
// the machine would be gone (f14d: 16 MB negotiated, 24 MB resident a moment
// after arrival).
void
vballoon_pin(struct vm *vm)
{
  if (vm->id >= 0 && vm->id < NVM)
    policy_floor[vm->id] = (uint64)vm->balloon.target * PGSIZE;
}

int
vballoon_request(struct vm *vm, uint64 bytes)
{
  int r = vballoon_set(vm, bytes);

  if (r == 0 && vm->id >= 0 && vm->id < NVM)
    policy_floor[vm->id] = bytes;
  return r;
}

void
vballoon_init(struct vm *vm)
{
  static int policy_started;
  struct vballoon *b = &vm->balloon;

  memset(b, 0, sizeof(*b));
  b->present = 1;
  if (vm->id >= 0 && vm->id < NVM)
    policy_floor[vm->id] = 0;
  vm_add_dev(vm, "balloon", G_BALLOON_IRQ, OBS_BALLOON, G_BALLOON_BASE + vm->mmio_shift, 0x1000, vballoon_read, vballoon_write, 0);
  if (!policy_started) {
    policy_started = 1;
    if (kthread_create(policy_thread, 0, "balloond") < 0)
      printk("hyp: no balloon policy thread\n");
  }
}
