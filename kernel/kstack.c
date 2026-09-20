//
// kernel stacks for tasks: KSTACK_PAGES pages each, mapped at a slot in
// a reserved region of the kernel's address space just below the
// trampoline, with an unmapped guard page under every stack so that an
// overflow faults instead of corrupting a neighbour.
//
// slots are handed out from a bitmap. a freed slot is not reused at
// once: another hart that ran the task may still hold a translation for
// the slot's addresses, so freed slots wait in a quarantine until every
// hart has flushed its TLB (an IPI round, done once per QUARANTINE
// releases rather than once per exit), and only then are their pages
// freed and the slots made available again.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"
#include "page.h"

#define SLOT_PAGES  (KSTACK_PAGES + 1)  // the guard page and the stack
#define SLOT_BYTES  ((uint64)SLOT_PAGES * PGSIZE)
#define QUARANTINE  64

static struct {
  struct spinlock lock;
  uint64 base;           // first slot's address (the guard page of slot 0)
  uint64 nslots;
  uint64 *bitmap;        // 1 = in use (or quarantined)
  uint64 next;           // where to search next
  uint64 nused;
  // slots waiting for the flush
  uint64 q[QUARANTINE];
  int nq;
} ks;

static volatile int flush_pending[NCPU]; // harts that still have to flush

// the region: 64 GiB below the trampoline in Sv48, 4 GiB in Sv39, both
// above any RAM the direct map can hold in that mode.
void
kstackinit(void)
{
  initlock(&ks.lock, "kstack");
  uint64 region = (plat.mmu_levels == 4) ? (64UL << 30) : (4UL << 30);
  ks.base = TRAMPOLINE - region;
  if (ks.base < plat.ram_end)
    panic("kstackinit: the stack region overlaps RAM");
  ks.nslots = region / SLOT_BYTES;
  uint64 words = (ks.nslots + 63) / 64;
  int order = 0;
  while (((uint64)PGSIZE << order) < words * 8)
    order++;
  ks.bitmap = kalloc_order(order);
  if (ks.bitmap == 0)
    panic("kstackinit: bitmap");
  memset(ks.bitmap, 0, (uint64)PGSIZE << order);
  ks.next = 0;
  ks.nused = 0;
  ks.nq = 0;
}

// the address of the stack's lowest byte for a slot.
static uint64
slot_va(uint64 slot)
{
  return ks.base + slot * SLOT_BYTES + PGSIZE; // above the guard page
}

// allocate a slot and map fresh pages under it. returns the stack's
// lowest address, or 0. the caller uses [va, va + KSTACK_PAGES * PGSIZE).
uint64
kstack_alloc(void)
{
  // first fit from the bottom: the slots in use stay clustered, so the
  // page-table pages of the region stay few (a slot cycling through the
  // whole region would leave a table page behind every 58 slots).
  acquire(&ks.lock);
  uint64 slot = ks.nslots;
  uint64 words = (ks.nslots + 63) / 64;
  for (uint64 w = 0; w < words; w++) {
    if (ks.bitmap[w] == ~0UL)
      continue;
    for (int b = 0; b < 64; b++) {
      uint64 i = w * 64 + b;
      if (i >= ks.nslots)
        break;
      if ((ks.bitmap[w] & (1UL << b)) == 0) {
        ks.bitmap[w] |= 1UL << b;
        ks.nused++;
        slot = i;
        break;
      }
    }
    if (slot != ks.nslots)
      break;
  }
  release(&ks.lock);
  if (slot == ks.nslots)
    return 0;
  uint64 va = slot_va(slot);
  for (int i = 0; i < KSTACK_PAGES; i++) {
    void *pa = kalloc();
    if (pa == 0 || kvmmap_rt(va + i * PGSIZE, (uint64)pa, PGSIZE, PTE_R | PTE_W) < 0) {
      if (pa)
        kfree(pa);
      // undo what was mapped
      for (int j = 0; j < i; j++) {
        uint64 p = kvmunmap_rt(va + j * PGSIZE, PGSIZE);
        if (p)
          kfree((void *)p);
      }
      acquire(&ks.lock);
      ks.bitmap[slot / 64] &= ~(1UL << (slot % 64));
      ks.nused--;
      release(&ks.lock);
      return 0;
    }
  }
  return va;
}

// every hart flushes its TLB; this hart waits for the others.
static void
flush_all_harts(void)
{
  int me = cpuid();
  for (int i = 0; i < plat.ncpu; i++) {
    if (i == me)
      continue;
    flush_pending[i] = 1;
    intc_ipi(i, IPI_TLBFLUSH);
  }
  sfence_vma();
  for (int i = 0; i < plat.ncpu; i++) {
    if (i == me)
      continue;
    while (flush_pending[i])
      ;
  }
}

// called from the IPI handler on a hart asked to flush.
void
kstack_flush_ack(void)
{
  sfence_vma();
  flush_pending[cpuid()] = 0;
}

// free the pages of quarantined slots and make the slots available.
// ks.lock held; releases and re-acquires it around the flush.
static uint64 drain_slots[QUARANTINE];
static uint64 pas[QUARANTINE][KSTACK_PAGES];
static uint64 tables[QUARANTINE * 2]; // emptied page-table pages
static int ntables;
static int draining;

static void
drain_quarantine(void)
{
  uint64 *slots = drain_slots;
  int n = ks.nq;
  for (int i = 0; i < n; i++)
    slots[i] = ks.q[i];
  ks.nq = 0;
  draining = 1;
  release(&ks.lock);
  // the mappings are torn down first, then every hart forgets them,
  // then the frames may be reused.
  for (int i = 0; i < n; i++)
    for (int j = 0; j < KSTACK_PAGES; j++)
      pas[i][j] = kvmunmap_rt(slot_va(slots[i]) + j * PGSIZE, PGSIZE);
  // page-table pages of the region that became empty go too, so a burst
  // of processes leaves nothing behind.
  ntables = 0;
  for (int i = 0; i < n; i++) {
    uint64 lo = slot_va(slots[i]), hi = lo + KSTACK_PAGES * PGSIZE - 1;
    uint64 t = kvmprune_rt(lo);
    if (t)
      tables[ntables++] = t;
    if ((hi >> 21) != (lo >> 21) && (t = kvmprune_rt(hi)) != 0)
      tables[ntables++] = t;
  }
  push_off();
  flush_all_harts();
  pop_off();
  for (int i = 0; i < n; i++)
    for (int j = 0; j < KSTACK_PAGES; j++)
      if (pas[i][j])
        kfree((void *)pas[i][j]);
  for (int i = 0; i < ntables; i++)
    kfree((void *)tables[i]);
  acquire(&ks.lock);
  for (int i = 0; i < n; i++) {
    ks.bitmap[slots[i] / 64] &= ~(1UL << (slots[i] % 64));
    ks.nused--;
  }
  draining = 0;
}

// release a stack. the caller must not be running on it.
void
kstack_free(uint64 va)
{
  uint64 slot = (va - PGSIZE - ks.base) / SLOT_BYTES;
  if (slot >= ks.nslots || slot_va(slot) != va)
    panic("kstack_free: bad stack");
  acquire(&ks.lock);
  while (ks.nq == QUARANTINE) {
    // a drain is in progress on another hart and the queue refilled
    // meanwhile: wait for it to finish, then drain again.
    if (!draining) {
      drain_quarantine();
      continue;
    }
    release(&ks.lock);
    for (volatile int i = 0; i < 1000; i++)
      ;
    acquire(&ks.lock);
  }
  ks.q[ks.nq++] = slot;
  if (ks.nq == QUARANTINE && !draining)
    drain_quarantine();
  release(&ks.lock);
}

// flush the quarantine now: pages of released stacks become free. used
// before reporting free memory, so the count is exact.
void
kstack_drain(void)
{
  acquire(&ks.lock);
  if (ks.nq > 0 && !draining)
    drain_quarantine();
  release(&ks.lock);
}

void
kstack_stats(uint64 *nused, uint64 *nslots)
{
  acquire(&ks.lock);
  *nused = ks.nused;
  *nslots = ks.nslots;
  release(&ks.lock);
}
