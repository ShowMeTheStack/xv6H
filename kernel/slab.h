#ifndef XV6_SLAB_H
#define XV6_SLAB_H
//
// fixed-size object caches over the page allocator. a cache hands out
// objects of one size from slabs (blocks of 2^order pages); freed objects
// go back to their slab's free list, and a slab whose objects are all
// free is returned to the page allocator.
//
struct slab;

struct kmem_cache {
  char name[16];
  uint objsize;           // rounded up to the alignment
  uint align;
  uint per_slab;          // objects per slab
  int order;              // pages per slab, as a buddy order
  struct spinlock lock;
  struct slab *partial;   // slabs with free objects (circular list)
  struct slab *full;      // slabs with none
  uint64 nslabs, nalloc;  // slabs owned, objects handed out
  void (*ctor)(void *);   // run on every object when its slab is created
};

struct kmem_cache *kmem_cache_create(const char *name, uint size, uint align, void (*ctor)(void *));
void *kmem_cache_alloc(struct kmem_cache *c);   // not zeroed unless the ctor zeroes
void kmem_cache_free(struct kmem_cache *c, void *obj);
void kmem_cache_stats(struct kmem_cache *c, uint64 *nslabs, uint64 *nalloc);

#endif
