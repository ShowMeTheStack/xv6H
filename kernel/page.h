#ifndef XV6_PAGE_H
#define XV6_PAGE_H
//
// the page frame database and the page allocator interface.
//
// one struct page per 4 KB frame of RAM, placed at the top of RAM by
// kinit(). the database is never initialized as a whole: a frame's entry
// is valid only while the frame belongs to an allocated block (PG_INIT) or
// is the head of a free block (PG_FREE). frames inside a free block have
// no valid entry, and the allocator never reads one: a block's buddy is
// always either allocated, the head of a free block, or outside the
// allocatable range (see kalloc.c).
//

#define MAXORDER 30 // 1 GiB blocks

// flags
#define PG_INIT     0x0001 // the entry is valid (the frame belongs to an allocated block)
#define PG_FREE     0x0002 // head of a free block of `order`
#define PG_HEAD     0x0004 // head of an allocated block of `order`
#define PG_RESERVED 0x0008
#define PG_ANON     0x0010 // reserved for later milestones
#define PG_FILE     0x0020
#define PG_GUEST    0x0040
#define PG_COW      0x0080
#define PG_DIRTY    0x0100
#define PG_LOCKED   0x0200
#define PG_KSM      0x0400

struct page {
  uint32 refcount;   // references to the block whose head this is
  uint16 flags;
  uint8 order;       // order of the block this entry heads (free or allocated)
  uint8 hart;        // per-hart cache it was last freed to
  uint64 index;      // file page index, or gpa >> 12 (later)
  void *owner;       // inode, vm, memcg (later)
  struct page *next, *prev;
};

extern struct page *pages;      // the database, indexed by (pa - plat.ram_base) >> PGSHIFT

// F13: memory that arrived after boot is a second range with its own page
// database, carved from its own first pages (kalloc_grow). Two ranges at most:
// the machine as it booted, and what it has been given since.
struct pgrange {
  uint64 base, end;             // physical range
  uint64 alloc_start, alloc_end; // the allocatable part (after its database)
  struct page *pages;
};
extern struct pgrange pgr[2];
extern int npgr;

static inline struct page *
PA2PAGE(uint64 pa)
{
  if (npgr > 1 && pa >= pgr[1].base)
    return &pgr[1].pages[(pa - pgr[1].base) >> PGSHIFT];
  return &pages[(pa - pgr[0].base) >> PGSHIFT];
}

static inline uint64
PAGE2PA(struct page *pg)
{
  if (npgr > 1 && pg >= pgr[1].pages)
    return pgr[1].base + ((uint64)(pg - pgr[1].pages) << PGSHIFT);
  return pgr[0].base + ((uint64)(pg - pages) << PGSHIFT);
}

struct meminfo {
  uint64 total_pages;     // frames the allocator manages
  uint64 free_pages;      // free now (global lists plus per-hart caches)
  uint64 db_pages;        // frames taken by the database itself
  uint64 reserve_pages;   // frames kept for the host (HOSTMEM)
  uint64 nfree[MAXORDER + 1]; // free blocks per order
};

void kinit(void);
void *kalloc(void);                  // one page (order 0), filled with junk
void kfree(void *pa);                // free a block from kalloc() or kalloc_order()
void *kalloc_order(int order);       // 2^order pages, naturally aligned, not zeroed
void kfree_order(void *pa, int order);
void kmem_split(void *pa, int order); // one block becomes 2^order pages
void *kalloc_guest(int order);       // like kalloc_order, but leaves the host its reserve
void page_get(struct page *pg);
void page_put(struct page *pg);      // frees the block at zero
uint64 kmem_free_pages(void);
void kmem_info(struct meminfo *mi);

#endif
