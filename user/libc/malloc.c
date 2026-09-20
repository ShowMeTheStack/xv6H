//
// the allocator.
//
// memory comes from the kernel in arenas of a megabyte or more, mapped
// anonymously; inside an arena, blocks carry a header with their size
// and whether the block before them is free, which is what lets free()
// coalesce in both directions without a search. free blocks live in
// size-ordered bins, so a request is answered by the first block of the
// smallest bin that can hold it, split if the remainder is worth
// keeping. blocks of a quarter megabyte or more skip all of this and
// get a mapping of their own, returned to the kernel when they are
// freed, so one big short-lived buffer does not grow the heap forever.
//
// one lock covers everything. threads that allocate in parallel will
// contend on it; per-thread caches are the obvious next step and the
// structure here leaves room for them.
//
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include "kernel/riscv.h"
#include "user/thread.h"
#include "libc.h"

#define ALIGN      16UL
#define HDRSIZE    16UL                 // prev_size and size
#define MINCHUNK   (HDRSIZE + 2 * sizeof(void *))
#define ARENA_MIN  (1UL << 20)          // a megabyte at a time
#define MMAP_LIMIT (256UL << 10)        // above this, a mapping of its own
#define NBINS      64

// the low bits of the size word
#define C_INUSE  1UL
#define C_PINUSE 2UL
#define C_MMAP   4UL
#define C_FLAGS  7UL

struct chunk {
  size_t prev_size;   // the size of the block below, if that one is free;
                      // in a mapped block, the distance to the mapping's start
  size_t size;        // this block's size, with the flags above;
                      // in a mapped block, the length of the whole mapping
  struct chunk *next; // free blocks only: the bin's list
  struct chunk *prev;
};

#define CSIZE(c)    ((c)->size & ~C_FLAGS)
#define CHUNK(p)    ((struct chunk *)((char *)(p) - HDRSIZE))
#define MEM(c)      ((void *)((char *)(c) + HDRSIZE))
#define NEXTCHUNK(c) ((struct chunk *)((char *)(c) + CSIZE(c)))

static struct chunk *bins[NBINS];
static mutex_t lock = MUTEX_INIT;
static size_t heap_total, heap_used;

// bin 0..31 hold one size each, 32 bytes apart; above that each bin
// covers a power of two, which keeps the search short for big blocks.
static int
binof(size_t n)
{
  if (n < 32 * 32)
    return n / 32;
  int b = 32 + (63 - __builtin_clzl(n / 32)) * 4;
  return b < NBINS ? b : NBINS - 1;
}

static void
bin_insert(struct chunk *c)
{
  int b = binof(CSIZE(c));

  c->prev = 0;
  c->next = bins[b];
  if (bins[b])
    bins[b]->prev = c;
  bins[b] = c;
}

static void
bin_remove(struct chunk *c)
{
  int b = binof(CSIZE(c));

  if (c->prev)
    c->prev->next = c->next;
  else
    bins[b] = c->next;
  if (c->next)
    c->next->prev = c->prev;
  c->next = c->prev = 0;
}

// a free block records its size in the header of the block above, so
// that block can find it when it is freed in turn.
static void
mark_free(struct chunk *c)
{
  struct chunk *next = NEXTCHUNK(c);

  c->size &= ~C_INUSE;
  next->size &= ~C_PINUSE;
  next->prev_size = CSIZE(c);
}

// ask the kernel for another arena, big enough for `need` bytes. the
// arena keeps a sentinel block at each end so that coalescing never
// walks past it.
static struct chunk *
grow(size_t need)
{
  size_t len = need + 2 * HDRSIZE + ARENA_MIN;

  len = (len + PGSIZE - 1) & ~(PGSIZE - 1);
  char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return 0;
  heap_total += len;

  // the body, with a sentinel above it that is permanently in use
  struct chunk *c = (struct chunk *)p;
  c->prev_size = 0;
  c->size = (len - HDRSIZE) | C_PINUSE;
  struct chunk *top = NEXTCHUNK(c);
  top->prev_size = CSIZE(c);
  top->size = HDRSIZE | C_INUSE; // never allocated, never coalesced
  mark_free(c);
  bin_insert(c);
  return c;
}

// split off the tail of c if what is left is worth binning.
static void
split(struct chunk *c, size_t want)
{
  size_t have = CSIZE(c);

  if (have < want + MINCHUNK)
    return;
  struct chunk *rest = (struct chunk *)((char *)c + want);
  rest->size = (have - want) | C_PINUSE;
  c->size = want | (c->size & C_FLAGS);
  mark_free(rest);
  bin_insert(rest);
}

static void *
alloc_locked(size_t want)
{
  for (int b = binof(want); b < NBINS; b++) {
    for (struct chunk *c = bins[b]; c; c = c->next) {
      if (CSIZE(c) >= want) {
        bin_remove(c);
        split(c, want);
        c->size |= C_INUSE;
        NEXTCHUNK(c)->size |= C_PINUSE;
        heap_used += CSIZE(c);
        return MEM(c);
      }
    }
  }
  struct chunk *c = grow(want);
  if (c == 0)
    return 0;
  bin_remove(c);
  split(c, want);
  c->size |= C_INUSE;
  NEXTCHUNK(c)->size |= C_PINUSE;
  heap_used += CSIZE(c);
  return MEM(c);
}

void *
malloc(size_t n)
{
  if (n == 0)
    n = 1;
  size_t want = (n + HDRSIZE + ALIGN - 1) & ~(ALIGN - 1);
  if (want < MINCHUNK)
    want = MINCHUNK;
  if (want < n) { // the rounding overflowed
    errno = ENOMEM;
    return 0;
  }

  // a big request gets its own mapping, so that freeing it gives the
  // memory straight back to the kernel.
  if (want >= MMAP_LIMIT) {
    size_t len = (want + PGSIZE - 1) & ~(PGSIZE - 1);
    char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      errno = ENOMEM;
      return 0;
    }
    struct chunk *c = (struct chunk *)p;
    c->prev_size = 0; // the block starts where the mapping does
    c->size = len | C_INUSE | C_PINUSE | C_MMAP;
    return MEM(c);
  }

  mutex_lock(&lock);
  void *p = alloc_locked(want);
  mutex_unlock(&lock);
  if (p == 0)
    errno = ENOMEM;
  return p;
}

void
free(void *p)
{
  if (p == 0)
    return;

  struct chunk *c = CHUNK(p);
  if (c->size & C_MMAP) {
    munmap((char *)c - c->prev_size, CSIZE(c) + c->prev_size);
    return;
  }

  mutex_lock(&lock);
  heap_used -= CSIZE(c);

  // join the block above if it is free, then the one below
  struct chunk *next = NEXTCHUNK(c);
  if (!(next->size & C_INUSE)) {
    bin_remove(next);
    c->size = (CSIZE(c) + CSIZE(next)) | (c->size & C_FLAGS);
  }
  if (!(c->size & C_PINUSE)) {
    struct chunk *prev = (struct chunk *)((char *)c - c->prev_size);
    bin_remove(prev);
    prev->size = (CSIZE(prev) + CSIZE(c)) | (prev->size & C_FLAGS);
    c = prev;
  }
  mark_free(c);
  bin_insert(c);
  mutex_unlock(&lock);
}

void *
calloc(size_t n, size_t size)
{
  if (size && n > (size_t)-1 / size) {
    errno = ENOMEM;
    return 0;
  }
  size_t total = n * size;
  void *p = malloc(total);
  if (p) // fresh pages from the kernel are already zero, but reused ones are not
    memset(p, 0, total);
  return p;
}

void *
realloc(void *p, size_t n)
{
  if (p == 0)
    return malloc(n);
  if (n == 0) {
    free(p);
    return 0;
  }

  struct chunk *c = CHUNK(p);
  size_t have = CSIZE(c) - HDRSIZE - ((c->size & C_MMAP) ? c->prev_size : 0);
  if (!(c->size & C_MMAP)) {
    size_t want = (n + HDRSIZE + ALIGN - 1) & ~(ALIGN - 1);
    if (want < MINCHUNK)
      want = MINCHUNK;
    if (want <= CSIZE(c) && want >= MMAP_LIMIT) {
      // keep it where it is
    } else if (want <= CSIZE(c)) {
      mutex_lock(&lock);
      size_t before = CSIZE(c);
      split(c, want);
      heap_used -= before - CSIZE(c);
      mutex_unlock(&lock);
      return p;
    } else {
      // grow in place if the block above is free and big enough
      mutex_lock(&lock);
      struct chunk *next = NEXTCHUNK(c);
      if (!(next->size & C_INUSE) && CSIZE(c) + CSIZE(next) >= want && want < MMAP_LIMIT) {
        bin_remove(next);
        heap_used += CSIZE(next);
        c->size = (CSIZE(c) + CSIZE(next)) | (c->size & C_FLAGS);
        NEXTCHUNK(c)->size |= C_PINUSE;
        size_t before = CSIZE(c);
        split(c, want);
        heap_used -= before - CSIZE(c);
        mutex_unlock(&lock);
        return p;
      }
      mutex_unlock(&lock);
    }
  }

  void *q = malloc(n);
  if (q == 0)
    return 0;
  memcpy(q, p, have < n ? have : n);
  free(p);
  return q;
}

void *
reallocarray(void *p, size_t n, size_t size)
{
  if (size && n > (size_t)-1 / size) {
    errno = ENOMEM;
    return 0;
  }
  return realloc(p, n * size);
}

size_t
malloc_usable_size(void *p)
{
  if (p == 0)
    return 0;
  struct chunk *c = CHUNK(p);
  return CSIZE(c) - HDRSIZE - ((c->size & C_MMAP) ? c->prev_size : 0);
}

// an aligned block: over-allocate and hand back an aligned address
// inside it, turning what comes before into a free block of its own.
int
posix_memalign(void **out, size_t align, size_t n)
{
  if (align < sizeof(void *) || (align & (align - 1))) {
    return EINVAL;
  }
  if (align <= ALIGN) {
    void *p = malloc(n);
    if (p == 0)
      return ENOMEM;
    *out = p;
    return 0;
  }

  char *raw = malloc(n + align + MINCHUNK);
  if (raw == 0)
    return ENOMEM;
  uintptr_t addr = (uintptr_t)raw;
  uintptr_t aligned = (addr + align - 1) & ~(uintptr_t)(align - 1);
  if (aligned - addr > 0 && aligned - addr < MINCHUNK)
    aligned += align;

  if (aligned != addr) {
    struct chunk *c = CHUNK(raw);
    if (c->size & C_MMAP) {
      // a mapped block is not split: the aligned block records how far
      // it sits from the start of the mapping, and free() unmaps from
      // there. nothing is wasted that was not already mapped.
      struct chunk *h = CHUNK((void *)aligned);
      size_t off = (char *)h - ((char *)c - c->prev_size);
      h->prev_size = off;
      h->size = (CSIZE(c) + c->prev_size - off) | C_INUSE | C_PINUSE | C_MMAP;
      *out = (void *)aligned;
      return 0;
    }
    mutex_lock(&lock);
    struct chunk *head = CHUNK((void *)aligned);
    size_t lead = (char *)head - (char *)c;
    head->size = (CSIZE(c) - lead) | C_INUSE;
    head->prev_size = lead;
    c->size = lead | (c->size & C_FLAGS);
    mark_free(c);
    bin_insert(c);
    heap_used -= lead;
    mutex_unlock(&lock);
  }
  *out = (void *)aligned;
  return 0;
}

void *
aligned_alloc(size_t align, size_t n)
{
  void *p;
  int e = posix_memalign(&p, align, n);

  if (e) {
    errno = e;
    return 0;
  }
  return p;
}

void *
memalign(size_t align, size_t n)
{
  return aligned_alloc(align, n);
}

// what the heap looks like, for tests and for a future /proc entry.
void
__malloc_stats(size_t *total, size_t *used)
{
  mutex_lock(&lock);
  *total = heap_total;
  *used = heap_used;
  mutex_unlock(&lock);
}
