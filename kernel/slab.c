//
// fixed-size object caches (slab.h).
//
// a slab is one block of 2^order pages: a header at its start, then the
// objects. free objects are threaded through their first word. the cache
// keeps its slabs on two circular lists, partial and full, and frees a
// slab to the page allocator when every object in it is free again.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "platform.h"
#include "page.h"
#include "slab.h"

struct slab {
  struct kmem_cache *cache;
  struct slab *next, *prev;
  uint nfree;
  void *freelist;
  // objects follow, from objs
  char *objs;
};

#define MAXCACHES 64
static struct kmem_cache caches[MAXCACHES];
static int ncaches;
static struct spinlock caches_lock;

static void
list_add(struct slab **head, struct slab *s)
{
  if (*head == 0) {
    s->next = s->prev = s;
    *head = s;
  } else {
    s->next = *head;
    s->prev = (*head)->prev;
    (*head)->prev->next = s;
    (*head)->prev = s;
  }
}

static void
list_del(struct slab **head, struct slab *s)
{
  if (s->next == s) {
    *head = 0;
  } else {
    s->prev->next = s->next;
    s->next->prev = s->prev;
    if (*head == s)
      *head = s->next;
  }
  s->next = s->prev = 0;
}

struct kmem_cache *
kmem_cache_create(const char *name, uint size, uint align, void (*ctor)(void *))
{
  if (align < 8)
    align = 8;
  size = (size + align - 1) & ~(align - 1);
  acquire(&caches_lock);
  if (ncaches == MAXCACHES)
    panic("kmem_cache_create: too many caches");
  struct kmem_cache *c = &caches[ncaches++];
  release(&caches_lock);
  memset(c, 0, sizeof(*c));
  safestrcpy(c->name, (char *)name, sizeof(c->name));
  initlock(&c->lock, c->name);
  c->objsize = size;
  c->align = align;
  c->ctor = ctor;
  // pick the slab size: at least 8 objects per slab, at most 2 MB slabs.
  uint hdr = (sizeof(struct slab) + align - 1) & ~(align - 1);
  int order = 0;
  while (order < 9 && ((uint64)PGSIZE << order) < hdr + 8UL * size)
    order++;
  c->order = order;
  c->per_slab = (((uint64)PGSIZE << order) - hdr) / size;
  if (c->per_slab == 0)
    panic("kmem_cache_create: object too large");
  return c;
}

// grow the cache by one slab (c->lock held).
static struct slab *
slab_new(struct kmem_cache *c)
{
  char *mem = kalloc_order(c->order);
  if (mem == 0)
    return 0;
  struct slab *s = (struct slab *)mem;
  uint hdr = (sizeof(struct slab) + c->align - 1) & ~(c->align - 1);
  s->cache = c;
  s->objs = mem + hdr;
  s->nfree = c->per_slab;
  s->freelist = 0;
  // thread the free list backwards so objects come out in address order.
  for (int i = c->per_slab - 1; i >= 0; i--) {
    char *o = s->objs + (uint64)i * c->objsize;
    if (c->ctor)
      c->ctor(o);
    *(void **)o = s->freelist;
    s->freelist = o;
  }
  c->nslabs++;
  return s;
}

void *
kmem_cache_alloc(struct kmem_cache *c)
{
  acquire(&c->lock);
  struct slab *s = c->partial;
  if (s == 0) {
    s = slab_new(c);
    if (s == 0) {
      release(&c->lock);
      return 0;
    }
    list_add(&c->partial, s);
  }
  void *o = s->freelist;
  s->freelist = *(void **)o;
  s->nfree--;
  if (s->nfree == 0) {
    list_del(&c->partial, s);
    list_add(&c->full, s);
  }
  c->nalloc++;
  release(&c->lock);
  return o;
}

void
kmem_cache_free(struct kmem_cache *c, void *obj)
{
  // the slab header is at the start of the object's block.
  uint64 mask = ((uint64)PGSIZE << c->order) - 1;
  struct slab *s = (struct slab *)((uint64)obj & ~mask);
  if (s->cache != c)
    panic("kmem_cache_free: wrong cache");
  acquire(&c->lock);
  if (s->nfree == 0) {
    list_del(&c->full, s);
    list_add(&c->partial, s);
  }
  *(void **)obj = s->freelist;
  s->freelist = obj;
  s->nfree++;
  c->nalloc--;
  if (s->nfree == c->per_slab && (c->partial != s || s->next != s)) {
    // a wholly free slab, and not the only one: give it back.
    list_del(&c->partial, s);
    c->nslabs--;
    release(&c->lock);
    kfree_order(s, c->order);
    return;
  }
  release(&c->lock);
}

void
kmem_cache_stats(struct kmem_cache *c, uint64 *nslabs, uint64 *nalloc)
{
  acquire(&c->lock);
  *nslabs = c->nslabs;
  *nalloc = c->nalloc;
  release(&c->lock);
}

void
slabinit(void)
{
  initlock(&caches_lock, "caches");
}
