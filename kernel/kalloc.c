//
// physical memory: a buddy allocator over the page frame database.
//
// blocks are 2^order pages, naturally aligned, order 0 (4 KB) to
// MAXORDER (1 GiB). the database (page.h) sits at the top of RAM and is
// initialized lazily: kinit() seeds the free lists with the largest
// aligned blocks that tile the allocatable range, writing one entry per
// block; entries of the frames inside an allocated block are written
// when the block is handed out. booting with 512 GiB touches kilobytes.
//
// the invariant that makes lazy initialization safe: when a block of
// order k is allocated, the other half of its order-(k+1) pair is either
// allocated (entries valid), the head of a free block (entry valid), or
// split into smaller blocks (whose heads are valid), or outside the
// allocatable range (never examined). the entry of a frame inside a free
// block is garbage, and only such frames are never a buddy of an
// allocated block.
//
// per-hart caches of single pages keep kalloc()/kfree() off the global
// lock in the common case.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"
#include "page.h"

extern char end[]; // first address after the kernel (kernel.ld)

struct page *pages;

static struct {
  struct spinlock lock;
  struct page *free[MAXORDER + 1]; // circular doubly linked lists, or 0
  uint64 nfree[MAXORDER + 1];
  uint64 alloc_start, alloc_end;   // the allocatable range [start, end)
  uint64 total, db_pages, free_pages, reserve;
} km;

#define PCACHE_FILL 32
#define PCACHE_MAX  96
static struct {
  struct page *head;
  int n;
} pcache[NCPU];

#define BLOCK_BYTES(order) ((uint64)PGSIZE << (order))

// ---- free lists (km.lock held) ----

static void
list_add(int order, struct page *pg)
{
  pg->flags = PG_FREE;
  pg->order = order;
  struct page *h = km.free[order];
  if (h == 0) {
    pg->next = pg->prev = pg;
    km.free[order] = pg;
  } else {
    pg->next = h;
    pg->prev = h->prev;
    h->prev->next = pg;
    h->prev = pg;
  }
  km.nfree[order]++;
}

static void
list_del(int order, struct page *pg)
{
  if (pg->next == pg) {
    km.free[order] = 0;
  } else {
    pg->prev->next = pg->next;
    pg->next->prev = pg->prev;
    if (km.free[order] == pg)
      km.free[order] = pg->next;
  }
  pg->flags = 0;
  km.nfree[order]--;
}

// the entries of an allocated block become valid.
static void
init_block(struct page *head, int order)
{
  uint64 n = 1UL << order;
  for (uint64 i = 0; i < n; i++) {
    struct page *pg = head + i;
    pg->refcount = 0;
    pg->flags = PG_INIT;
    pg->order = 0;
    pg->index = 0;
    pg->owner = 0;
    pg->next = pg->prev = 0;
  }
  head->flags = PG_INIT | PG_HEAD;
  head->order = order;
  head->refcount = 1;
}

static struct page *
alloc_block(int order)
{
  for (int o = order; o <= MAXORDER; o++) {
    struct page *pg = km.free[o];
    if (pg == 0)
      continue;
    list_del(o, pg);
    while (o > order) {
      o--;
      struct page *buddy = pg + (1UL << o);
      list_add(o, buddy);
    }
    init_block(pg, order);
    km.free_pages -= 1UL << order;
    return pg;
  }
  return 0;
}

static void
free_block(struct page *pg, int order)
{
  uint64 pa = PAGE2PA(pg);
  km.free_pages += 1UL << order; // the buddies merged below are already counted
  while (order < MAXORDER) {
    uint64 bpa = pa ^ BLOCK_BYTES(order);
    if (bpa < km.alloc_start || bpa + BLOCK_BYTES(order) > km.alloc_end)
      break;
    struct page *bp = PA2PAGE(bpa);
    if (!(bp->flags & PG_FREE) || bp->order != order)
      break;
    list_del(order, bp);
    if (bpa < pa) {
      pa = bpa;
      pg = bp;
    }
    order++;
  }
  list_add(order, pg);
}

// ---- init ----

void
kinit(void)
{
  initlock(&km.lock, "kmem");
  for (int i = 0; i < NCPU; i++) {
    pcache[i].head = 0;
    pcache[i].n = 0;
  }

  // the database covers every frame of RAM and takes the top of it.
  uint64 nframes = (plat.ram_end - plat.ram_base) >> PGSHIFT;
  uint64 dbbytes = (nframes * sizeof(struct page) + (2UL << 20) - 1) & ~((2UL << 20) - 1);
  uint64 dbbase = (plat.ram_end - dbbytes) & ~((2UL << 20) - 1);
  pages = (struct page *)dbbase;
  km.db_pages = (plat.ram_end - dbbase) >> PGSHIFT;

  km.alloc_start = PGROUNDUP((uint64)end);
  km.alloc_end = dbbase;
  if (km.alloc_end <= km.alloc_start + 64 * PGSIZE)
    panic("kinit: no memory");
  km.total = (km.alloc_end - km.alloc_start) >> PGSHIFT;
  km.reserve = HOSTMEM >> PGSHIFT;
  if (km.reserve > km.total / 2)
    km.reserve = km.total / 2;

  // tile the range with the largest aligned blocks that fit: one entry
  // written per block, the rest of the database untouched.
  uint64 pa = km.alloc_start;
  int nblocks = 0;
  while (pa < km.alloc_end) {
    int order = 0;
    while (order < MAXORDER && (pa & (BLOCK_BYTES(order + 1) - 1)) == 0 &&
           pa + BLOCK_BYTES(order + 1) <= km.alloc_end)
      order++;
    list_add(order, PA2PAGE(pa));
    km.free_pages += 1UL << order;
    pa += BLOCK_BYTES(order);
    nblocks++;
  }
  printk("kalloc: %ld MB in %d blocks, page database %ld MB at 0x%lx, host reserve %ld MB\n",
         km.total >> 8, nblocks, km.db_pages >> 8, dbbase, km.reserve >> 8);
}

// ---- the interface ----

void *
kalloc_order(int order)
{
  if (order < 0 || order > MAXORDER)
    panic("kalloc_order");
  acquire(&km.lock);
  struct page *pg = alloc_block(order);
  release(&km.lock);
  return pg ? (void *)PAGE2PA(pg) : 0;
}

// for guest memory: never eat into the host's reserve.
void *
kalloc_guest(int order)
{
  acquire(&km.lock);
  struct page *pg = 0;
  if (km.free_pages >= km.reserve + (1UL << order))
    pg = alloc_block(order);
  release(&km.lock);
  return pg ? (void *)PAGE2PA(pg) : 0;
}

void
kfree_order(void *pa, int order)
{
  uint64 a = (uint64)pa;
  if (a < km.alloc_start || a + BLOCK_BYTES(order) > km.alloc_end || (a & (BLOCK_BYTES(order) - 1)))
    panic("kfree_order: bad block");
  struct page *pg = PA2PAGE(a);
  if ((pg->flags & (PG_INIT | PG_HEAD)) != (PG_INIT | PG_HEAD) || pg->order != order)
    panic("kfree_order: not an allocated block of that order");
  if (order == 0) {
    kfree(pa);
    return;
  }
  acquire(&km.lock);
  free_block(pg, order);
  release(&km.lock);
}

// refill this hart's cache from a 32-page block.
static void
pcache_fill(int id)
{
  acquire(&km.lock);
  struct page *blk = alloc_block(5);
  release(&km.lock);
  if (blk == 0)
    return;
  // the block becomes 32 single pages
  for (int i = 0; i < PCACHE_FILL; i++) {
    struct page *pg = blk + i;
    pg->flags = PG_INIT | PG_HEAD;
    pg->order = 0;
    pg->refcount = 1;
    pg->next = pcache[id].head;
    pcache[id].head = pg;
    pcache[id].n++;
  }
}

// return half of this hart's cache to the global lists.
static void
pcache_drain(int id)
{
  acquire(&km.lock);
  while (pcache[id].n > PCACHE_MAX / 2) {
    struct page *pg = pcache[id].head;
    pcache[id].head = pg->next;
    pcache[id].n--;
    free_block(pg, 0);
  }
  release(&km.lock);
}

void *
kalloc(void)
{
  push_off();
  int id = cpuid();
  if (pcache[id].n == 0)
    pcache_fill(id);
  struct page *pg = pcache[id].head;
  if (pg) {
    pcache[id].head = pg->next;
    pcache[id].n--;
  }
  pop_off();
  if (pg == 0) {
    // the cache could not be filled with a 32-page block: try a single page.
    acquire(&km.lock);
    pg = alloc_block(0);
    release(&km.lock);
    if (pg == 0)
      return 0;
  }
  pg->next = pg->prev = 0;
  void *pa = (void *)PAGE2PA(pg);
  memset(pa, 5, PGSIZE); // junk, to catch uninitialized use
  return pa;
}

void
kfree(void *pa)
{
  uint64 a = (uint64)pa;
  if ((a % PGSIZE) != 0 || a < km.alloc_start || a >= km.alloc_end)
    panic("kfree");
  struct page *pg = PA2PAGE(a);
  if ((pg->flags & (PG_INIT | PG_HEAD)) != (PG_INIT | PG_HEAD))
    panic("kfree: not the head of an allocated block");
  if (pg->order != 0) {
    acquire(&km.lock);
    free_block(pg, pg->order);
    release(&km.lock);
    return;
  }
  memset(pa, 1, PGSIZE); // junk, to catch dangling references
  push_off();
  int id = cpuid();
  pg->hart = id;
  pg->refcount = 0;
  pg->next = pcache[id].head;
  pcache[id].head = pg;
  pcache[id].n++;
  if (pcache[id].n > PCACHE_MAX)
    pcache_drain(id);
  pop_off();
}

void
page_get(struct page *pg)
{
  __sync_fetch_and_add(&pg->refcount, 1);
}

void
page_put(struct page *pg)
{
  if (__sync_sub_and_fetch(&pg->refcount, 1) == 0) {
    pg->refcount = 1; // kfree expects an allocated head
    kfree((void *)PAGE2PA(pg));
  }
}

uint64
kmem_free_pages(void)
{
  uint64 n = km.free_pages;
  for (int i = 0; i < NCPU; i++)
    n += pcache[i].n;
  return n;
}

void
kmem_info(struct meminfo *mi)
{
  acquire(&km.lock);
  mi->total_pages = km.total;
  mi->free_pages = km.free_pages;
  for (int i = 0; i < NCPU; i++)
    mi->free_pages += pcache[i].n;
  mi->db_pages = km.db_pages;
  mi->reserve_pages = km.reserve;
  for (int o = 0; o <= MAXORDER; o++)
    mi->nfree[o] = km.nfree[o];
  release(&km.lock);
}
