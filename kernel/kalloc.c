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
#include "blk.h"
#include "platform.h"
#include "page.h"

extern char end[]; // first address after the kernel (kernel.ld)

struct page *pages;
struct pgrange pgr[2];   // F13
int npgr = 1;

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
  struct spinlock lock; // held for the hart's own use and for stealing
  struct page *head;
  int n;
} pcache[NCPU];

#define BLOCK_BYTES(order) ((uint64)PGSIZE << (order))

// does [a, a+bytes) lie within one range's allocatable part? (F13: two ranges)
static int
in_alloc_range(uint64 a, uint64 bytes)
{
  if (a >= km.alloc_start && a + bytes <= km.alloc_end)
    return 1;
  if (npgr > 1 && a >= pgr[1].alloc_start && a + bytes <= pgr[1].alloc_end)
    return 1;
  return 0;
}

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
    // the buddy must lie in the same range's allocatable part (F13: two ranges)
    uint64 lo = km.alloc_start, hi = km.alloc_end;
    if (npgr > 1 && pa >= pgr[1].base) {
      lo = pgr[1].alloc_start;
      hi = pgr[1].alloc_end;
    }
    if (bpa < lo || bpa + BLOCK_BYTES(order) > hi)
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
    initlock(&pcache[i].lock, "pcache");
    pcache[i].head = 0;
    pcache[i].n = 0;
  }

  // the database covers every frame of RAM and takes the top of it.
  // (below the description slots the machine keeps at the very top: resv_top)
  uint64 nframes = (plat.ram_end - plat.ram_base) >> PGSHIFT;
  uint64 dbbytes = (nframes * sizeof(struct page) + (2UL << 20) - 1) & ~((2UL << 20) - 1);
  uint64 dbbase = (plat.ram_end - plat.resv_top - dbbytes) & ~((2UL << 20) - 1);
  pages = (struct page *)dbbase;
  pgr[0].base = plat.ram_base;      // PA2PAGE/PAGE2PA go through the range table from here on (F13)
  pgr[0].end = plat.ram_end;
  pgr[0].pages = pages;
  npgr = 1;
  km.db_pages = (plat.ram_end - dbbase) >> PGSHIFT;

  km.alloc_start = PGROUNDUP((uint64)end);
  km.alloc_end = dbbase;
  if (km.alloc_end <= km.alloc_start + 64 * PGSIZE)
    panic("kinit: no memory");
  km.total = (km.alloc_end - km.alloc_start) >> PGSHIFT;
  km.reserve = HOSTMEM >> PGSHIFT;
  if (km.reserve > km.total / 2)
    km.reserve = km.total / 2;
  if (plat.is_guest)
    km.reserve = 0;   // the reserve is a host's, kept for its guests; a guest has none to keep (F14: the balloon could not reach it)

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
  pgr[0].alloc_start = km.alloc_start;
  pgr[0].alloc_end = km.alloc_end;
}

// F13: memory that arrived after boot, [base, end). Mapped into the kernel's
// direct map, given a page database out of its own first pages, and its
// blocks handed to the free lists exactly as kinit does for the first range.
int
kalloc_grow(uint64 base, uint64 end)
{
  if (npgr > 1 || end <= base || (base & (PGSIZE - 1)) || (end & (PGSIZE - 1)))
    return -1;
  kvmmap_rt(base, base, end - base, PTE_R | PTE_W);
  uint64 nframes = (end - base) >> PGSHIFT;
  uint64 dbbytes = PGROUNDUP(nframes * sizeof(struct page));
  if (base + dbbytes + 64 * PGSIZE > end)
    return -1;
  struct page *db = (struct page *)base;
  memset(db, 0, dbbytes);
  acquire(&km.lock);
  pgr[1].base = base;
  pgr[1].end = end;
  pgr[1].alloc_start = base + dbbytes;
  pgr[1].alloc_end = end;
  pgr[1].pages = db;
  __sync_synchronize();
  npgr = 2;
  uint64 pa = pgr[1].alloc_start;
  int nblocks = 0;
  while (pa < end) {
    int order = 0;
    while (order < MAXORDER && (pa & (BLOCK_BYTES(order + 1) - 1)) == 0 &&
           pa + BLOCK_BYTES(order + 1) <= end)
      order++;
    list_add(order, PA2PAGE(pa));
    km.free_pages += 1UL << order;
    km.total += 1UL << order;
    pa += BLOCK_BYTES(order);
    nblocks++;
  }
  km.db_pages += dbbytes >> PGSHIFT;
  release(&km.lock);
  printk("kalloc: %ld MB more in %d blocks at 0x%lx, page database %ld KB\n",
         (end - base) >> 20, nblocks, base, dbbytes >> 10);
  return 0;
}

// ---- the interface ----

// a block of 2^order pages, and if there is none, the block cache is
// asked to give back what it is holding only because it might be
// useful again.
//
// kalloc() has always done this and kalloc_order() did not, which is
// how a machine with a gigabyte of clean cached blocks and no free
// pages could panic in the object cache: the slab allocator asks for
// its pages here. blk_reclaim takes no sleeping lock and refuses to
// run inside the cache's own lock, so this is safe wherever an
// allocation is.
static struct page *
alloc_or_reclaim(int order, uint64 floor)
{
  struct page *pg = 0;

  acquire(&km.lock);
  if (km.free_pages >= floor + (1UL << order))
    pg = alloc_block(order);
  release(&km.lock);
  if (pg || blk_reclaim(64UL << order) == 0)
    return pg;
  acquire(&km.lock);
  if (km.free_pages >= floor + (1UL << order))
    pg = alloc_block(order);
  release(&km.lock);
  return pg;
}

void *
kalloc_order(int order)
{
  if (order < 0 || order > MAXORDER)
    panic("kalloc_order");
  struct page *pg = alloc_or_reclaim(order, 0);
  return pg ? (void *)PAGE2PA(pg) : 0;
}

// for guest memory: never eat into the host's reserve.
void *
kalloc_guest(int order)
{
  struct page *pg = alloc_or_reclaim(order, km.reserve);
  return pg ? (void *)PAGE2PA(pg) : 0;
}

// break an allocated block into single pages, each of which can be
// freed on its own afterwards.
//
// the buddy allocator hands out a 2 MB block as one thing and takes
// it back as one thing.  A guest whose memory is being reclaimed a
// page at a time -- by a balloon, or by same-page merging -- needs
// the pieces separable.  Nothing moves and nothing is copied: the
// frames are where they were, and what changes is what the page
// database says about them, which is what kfree reads.  The free
// lists and the free count do not change, so km.lock is not needed;
// these entries belong to the block's owner, who is calling.
void
kmem_split(void *pa, int order)
{
  uint64 a = (uint64)pa;

  if (order <= 0)
    return;
  if (!in_alloc_range(a, BLOCK_BYTES(order)) || (a & (BLOCK_BYTES(order) - 1)))
    panic("kmem_split: bad block");
  struct page *head = PA2PAGE(a);
  if ((head->flags & (PG_INIT | PG_HEAD)) != (PG_INIT | PG_HEAD) ||
      head->order != order)
    panic("kmem_split: not an allocated block of that order");
  uint64 n = 1UL << order;
  for (uint64 i = 0; i < n; i++) {
    struct page *pg = head + i;
    pg->flags = PG_INIT | PG_HEAD;
    pg->order = 0;
    pg->refcount = 1;
  }
}

void
kfree_order(void *pa, int order)
{
  uint64 a = (uint64)pa;
  if (!in_alloc_range(a, BLOCK_BYTES(order)) || (a & (BLOCK_BYTES(order) - 1)))
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

// refill this hart's cache from a 32-page block (pcache[id].lock held).
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

// return half of this hart's cache to the global lists (pcache[id].lock held).
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

// memory is short: take a page from another hart's cache, if any has one.
static struct page *
pcache_steal(int me)
{
  for (int i = 0; i < NCPU; i++) {
    if (i == me || pcache[i].n == 0)
      continue;
    acquire(&pcache[i].lock);
    struct page *pg = pcache[i].head;
    if (pg) {
      pcache[i].head = pg->next;
      pcache[i].n--;
    }
    release(&pcache[i].lock);
    if (pg)
      return pg;
  }
  return 0;
}

static void *kalloc_once(void);

static void *
kalloc_once(void)
{
  push_off();
  int id = cpuid();
  acquire(&pcache[id].lock);
  if (pcache[id].n == 0)
    pcache_fill(id);
  struct page *pg = pcache[id].head;
  if (pg) {
    pcache[id].head = pg->next;
    pcache[id].n--;
  }
  release(&pcache[id].lock);
  if (pg == 0) {
    // no 32-page block was free: a single page, then other harts' caches.
    acquire(&km.lock);
    pg = alloc_block(0);
    release(&km.lock);
    if (pg == 0)
      pg = pcache_steal(id);
  }
  pop_off();
  if (pg == 0)
    return 0;
  pg->refcount = 1; // a recycled page comes out of the cache with 0
  pg->next = pg->prev = 0;
  void *pa = (void *)PAGE2PA(pg);
  memset(pa, 5, PGSIZE); // junk, to catch uninitialized use
  return pa;
}

// one page. when there is none, the block cache is asked for the pages
// it is holding only because they might be useful again: a clean cached
// block is a copy of what is on the disk, and a page is worth more to
// whoever needs one now.
void *
kalloc(void)
{
  void *pa = kalloc_once();

  if (pa == 0 && blk_reclaim(64) > 0)
    pa = kalloc_once();
  return pa;
}

void
kfree(void *pa)
{
  uint64 a = (uint64)pa;
  if ((a % PGSIZE) != 0 || !in_alloc_range(a, PGSIZE))
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
  acquire(&pcache[id].lock);
  pg->hart = id;
  pg->refcount = 0;
  pg->next = pcache[id].head;
  pcache[id].head = pg;
  pcache[id].n++;
  if (pcache[id].n > PCACHE_MAX)
    pcache_drain(id);
  release(&pcache[id].lock);
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

// how many pages are free: the global lists plus what the harts are
// holding in their caches.
//
// every count is read under its own lock. Reading them loose is
// cheaper and almost always right, but "almost always right" is no use
// to the thing this number is for -- a test that starts a guest, kills
// it, and asks whether every page came back. A page of slack there is
// a page of leak that nobody notices. The lock order is the one kfree
// uses: a hart's cache first, then the global lists.
// F21: a page of zeroes. With Zicboz, one cbo.zero per cache block -- at the
// block size the machine is described with, which is the one fact here a
// wrong value of does not fail loudly: a stride longer than the block leaves
// every other block as it was. A revision changes plat.cboz_block, and the
// next page is zeroed at the new stride.
void
page_zero(void *pa)
{
  int blk = plat.cboz_block;
  if (blk >= 16 && blk <= PGSIZE && (blk & (blk - 1)) == 0) {
    for (char *p = (char *)pa; p < (char *)pa + PGSIZE; p += blk)
      asm volatile(".insn i 0x0f, 0x2, x0, %0, 4" ::"r"(p) : "memory");   // cbo.zero (p)
    return;
  }
  memset(pa, 0, PGSIZE);
}

// what the machine has, in pages: grows with F13 (the reserve, if any, is
// still counted; it is the machine's)
uint64
kmem_total_pages(void)
{
  return km.total;
}

uint64
kmem_free_pages(void)
{
  uint64 n = 0;

  for (int i = 0; i < NCPU; i++) {
    acquire(&pcache[i].lock);
    n += pcache[i].n;
    release(&pcache[i].lock);
  }
  acquire(&km.lock);
  n += km.free_pages;
  release(&km.lock);
  return n;
}

void
kmem_info(struct meminfo *mi)
{
  uint64 cached = 0;

  for (int i = 0; i < NCPU; i++) {
    acquire(&pcache[i].lock);
    cached += pcache[i].n;
    release(&pcache[i].lock);
  }
  acquire(&km.lock);
  mi->total_pages = km.total;
  mi->free_pages = km.free_pages + cached;
  mi->db_pages = km.db_pages;
  mi->reserve_pages = km.reserve;
  for (int o = 0; o <= MAXORDER; o++)
    mi->nfree[o] = km.nfree[o];
  release(&km.lock);
}
