//
// address spaces: areas, demand paging, copy-on-write (mm.h).
//
// every user page holds one reference on its frame (page_get/page_put);
// fork shares frames between parent and child with PTE_COW set and PTE_W
// clear on private areas, and the first write to such a page copies it
// (or takes it over when the frame's reference count is back to one).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "page.h"
#include "slab.h"
#include "fs.h"
#include "file.h"
#include "mm.h"

static struct kmem_cache *vma_cache, *mm_cache;

void
mminit(void)
{
  vma_cache = kmem_cache_create("vma", sizeof(struct vma), 8, 0);
  mm_cache = kmem_cache_create("mm", sizeof(struct mm), 8, 0);
}

// ---- areas ----

struct vma *
vma_find(struct mm *mm, uint64 va)
{
  for (struct vma *v = mm->vmas; v; v = v->next) {
    if (va < v->start)
      return 0;
    if (va < v->end)
      return v;
  }
  return 0;
}

// the first area whose end is above va (for range walks)
static struct vma *
vma_after(struct mm *mm, uint64 va)
{
  for (struct vma *v = mm->vmas; v; v = v->next)
    if (v->end > va)
      return v;
  return 0;
}

static int
vma_overlaps(struct mm *mm, uint64 start, uint64 end)
{
  for (struct vma *v = mm->vmas; v; v = v->next) {
    if (v->start >= end)
      return 0;
    if (v->end > start)
      return 1;
  }
  return 0;
}

int
vma_add(struct mm *mm, uint64 start, uint64 end, int prot, int flags, struct inode *ip, uint64 off,
        uint64 filesz_end)
{
  if ((start | end) & (PGSIZE - 1) || start >= end || end > plat.maxva || vma_overlaps(mm, start, end))
    return -1;
  struct vma *v = kmem_cache_alloc(vma_cache);
  if (v == 0)
    return -1;
  v->start = start;
  v->end = end;
  v->prot = prot;
  v->flags = flags;
  v->ip = ip;
  v->off = off;
  v->filesz_end = filesz_end;
  // insert sorted
  struct vma **pp = &mm->vmas;
  while (*pp && (*pp)->start < start)
    pp = &(*pp)->next;
  v->next = *pp;
  *pp = v;
  return 0;
}

// unmap [start, end) of the page table, dropping the pages' references.
static void
unmap_pages(struct mm *mm, uint64 start, uint64 end)
{
  if (end > start) {
    uint64 before = mm->pages;
    uvmunmap_count(mm->pagetable, start, (end - start) / PGSIZE, &mm->pages);
    (void)before;
  }
}

// mm->lock is held: dropping an inode reference may sleep, so the area
// waits on a list until the caller has let the lock go.
static void
vma_release_locked(struct mm *mm, struct vma *v)
{
  if (v->ip == 0) {
    kmem_cache_free(vma_cache, v);
    return;
  }
  v->next = mm->dead;
  mm->dead = v;
}

// release what vma_release_locked() put aside. no lock is held.
static void
mm_reap(struct mm *mm)
{
  while (mm->dead) {
    struct vma *v = mm->dead;
    mm->dead = v->next;
    if (v->ip) {
      iput(v->ip);
    }
    kmem_cache_free(vma_cache, v);
  }
}

// remove [start, end) from the areas: trims, splits or deletes them, and
// unmaps the pages. mm->lock is held.
static int
vma_remove_locked(struct mm *mm, uint64 start, uint64 end)
{
  if ((start | end) & (PGSIZE - 1) || start >= end)
    return -1;
  struct vma **pp = &mm->vmas;
  while (*pp) {
    struct vma *v = *pp;
    if (v->start >= end)
      break;
    if (v->end <= start) {
      pp = &v->next;
      continue;
    }
    uint64 lo = start > v->start ? start : v->start;
    uint64 hi = end < v->end ? end : v->end;
    unmap_pages(mm, lo, hi);
    if (lo == v->start && hi == v->end) {
      *pp = v->next;
      vma_release_locked(mm, v);
      continue;
    }
    if (lo == v->start) {
      // trim the front
      if (v->ip)
        v->off += hi - v->start;
      v->start = hi;
    } else if (hi == v->end) {
      v->end = lo;
    } else {
      // a hole in the middle: split
      struct vma *w = kmem_cache_alloc(vma_cache);
      if (w == 0)
        return -1;
      *w = *v;
      if (v->ip)
        w->ip = idup(v->ip);
      w->start = hi;
      if (w->ip)
        w->off = v->off + (hi - v->start);
      v->end = lo;
      w->next = v->next;
      v->next = w;
    }
    pp = &v->next;
  }
  return 0;
}

int
vma_remove(struct mm *mm, uint64 start, uint64 end)
{
  acquire(&mm->lock);
  int r = vma_remove_locked(mm, start, end);
  release(&mm->lock);
  mm_reap(mm);
  return r;
}

// ---- the address space ----

struct mm *
mm_create(void)
{
  struct mm *mm = kmem_cache_alloc(mm_cache);
  if (mm == 0)
    return 0;
  memset(mm, 0, sizeof(*mm));
  initlock(&mm->lock, "mm");
  mm->ref = 1;
  mm->pagetable = uvmcreate();
  if (mm->pagetable == 0) {
    kmem_cache_free(mm_cache, mm);
    return 0;
  }
  mm->stack_top = THREAD_TF_BOT;
  mm->stack_bottom = mm->stack_top - STACK_MAX;
  mm->mmap_base = mm->stack_bottom - MMAP_GAP;
  return mm;
}

int
mm_setup_stack(struct mm *mm)
{
  return vma_add(mm, mm->stack_bottom, mm->stack_top, VMA_READ | VMA_WRITE, VMA_ANON | VMA_STACK | VMA_FIXED,
                 0, 0, 0);
}

// map a trapframe page for a new thread of this address space, and
// record where it went in p->tf_va.
int
mm_add_trapframe(struct mm *mm, struct proc *p)
{
  acquire(&mm->lock);
  for (int i = 0; i < THREAD_TF_SLOTS; i++) {
    if (mm->tf_slots[i / 64] & (1UL << (i % 64)))
      continue;
    uint64 va = THREAD_TF_TOP - (uint64)(i + 1) * PGSIZE;
    if (mappages(mm->pagetable, va, PGSIZE, (uint64)p->trapframe, PTE_R | PTE_W) < 0) {
      release(&mm->lock);
      return -1;
    }
    mm->tf_slots[i / 64] |= 1UL << (i % 64);
    p->tf_va = va;
    release(&mm->lock);
    return 0;
  }
  release(&mm->lock);
  return -1;
}

void
mm_del_trapframe(struct mm *mm, struct proc *p)
{
  if (p->tf_va == 0 || p->tf_va >= THREAD_TF_TOP)
    return;
  int i = (THREAD_TF_TOP - p->tf_va) / PGSIZE - 1;
  acquire(&mm->lock);
  uvmunmap(mm->pagetable, p->tf_va, 1, 0);
  if (i >= 0 && i < THREAD_TF_SLOTS)
    mm->tf_slots[i / 64] &= ~(1UL << (i % 64));
  release(&mm->lock);
  p->tf_va = 0;
}

// another thread of the same process shares this address space.
struct mm *
mm_dup(struct mm *mm)
{
  acquire(&mm->lock);
  mm->ref++;
  release(&mm->lock);
  return mm;
}

// unmap and release every area, then the tables. the trampoline and
// trapframe mappings are the caller's (proc_mm_free). the last thread
// to let go does the work.
int
mm_free(struct mm *mm)
{
  acquire(&mm->lock);
  int n = --mm->ref;
  release(&mm->lock);
  if (n > 0)
    return 0; // other threads are still using it
  while (mm->vmas) {
    struct vma *v = mm->vmas;
    unmap_pages(mm, v->start, v->end);
    mm->vmas = v->next;
    v->next = mm->dead;
    mm->dead = v;
  }
  mm_reap(mm); // nobody else holds this address space now
  kmem_cache_free(mm_cache, mm);
  return 1;
}

// copy every present page below end from src into dst: private areas
// become copy-on-write in both, shared ones just gain a reference.
static int
share_range(struct mm *dst, struct mm *src, pagetable_t spt, int level, uint64 base)
{
  uint64 span = (uint64)PGSIZE << (9 * level);
  for (int i = 0; i < 512; i++) {
    uint64 lo = base + i * span;
    if (lo >= src->stack_top)
      break;
    pte_t pte = spt[i];
    if ((pte & PTE_V) == 0)
      continue;
    if (level > 0) {
      if (share_range(dst, src, (pagetable_t)PTE2PA(pte), level - 1, lo) < 0)
        return -1;
      continue;
    }
    if ((pte & PTE_U) == 0)
      continue; // the trampoline
    struct vma *v = vma_find(src, lo);
    if (v == 0)
      continue; // a page outside any area should not exist
    uint64 flags = PTE_FLAGS(pte);
    if (!(v->flags & VMA_SHARED) && (flags & PTE_W)) {
      flags = (flags & ~PTE_W) | PTE_COW;
      spt[i] = PA2PTE(PTE2PA(pte)) | flags;
    }
    if (mappages(dst->pagetable, lo, PGSIZE, PTE2PA(pte), flags) != 0)
      return -1;
    page_get(PA2PAGE(PTE2PA(pte)));
    dst->pages++;
  }
  return 0;
}

int
mm_fork(struct mm *dst, struct mm *src)
{
  acquire(&src->lock);
  // the areas
  struct vma **tail = &dst->vmas;
  for (struct vma *v = src->vmas; v; v = v->next) {
    struct vma *w = kmem_cache_alloc(vma_cache);
    if (w == 0) {
      release(&src->lock);
      return -1;
    }
    *w = *v;
    if (w->ip)
      w->ip = idup(w->ip);
    w->next = 0;
    *tail = w;
    tail = &w->next;
  }
  dst->brk = src->brk;
  dst->heap_start = src->heap_start;
  dst->stack_top = src->stack_top;
  dst->stack_bottom = src->stack_bottom;
  dst->mmap_base = src->mmap_base;
  // the pages
  if (share_range(dst, src, src->pagetable, plat.mmu_levels - 1, 0) < 0) {
    release(&src->lock);
    return -1;
  }
  sfence_vma(); // the parent's PTEs lost their write bits
  release(&src->lock);
  return 0;
}

// ---- faults ----

// a private page written for the first time since it was shared: give
// the writer its own copy, or the frame itself if nobody else holds it.
static int
cow_break(struct mm *mm, pte_t *pte)
{
  uint64 pa = PTE2PA(*pte);
  struct page *pg = PA2PAGE(pa);
  uint64 flags = (PTE_FLAGS(*pte) & ~PTE_COW) | PTE_W;
  if (pg->refcount == 1) {
    *pte = PA2PTE(pa) | flags;
  } else {
    char *mem = kalloc();
    if (mem == 0)
      return -1;
    memmove(mem, (void *)pa, PGSIZE);
    *pte = PA2PTE((uint64)mem) | flags;
    page_put(pg);
  }
  sfence_vma();
  return 0;
}

// bring the page holding va into the table according to its area. the
// page is filled without the lock (a file read sleeps), and the table is
// checked again before it is installed: another thread may have faulted
// on the same address meanwhile.
static uint64
fault_in(struct mm *mm, struct vma *v, uint64 va)
{
  va = PGROUNDDOWN(va);
  char *mem = kalloc();
  if (mem == 0)
    return 0;
  struct proc *me = myproc();
  if (v->flags & VMA_FILE) {
    // file data up to filesz_end, zero beyond
    uint64 n = 0;
    if (me)
      me->majflt++; // it had to be read from a file
    if (va < v->filesz_end) {
      n = v->filesz_end - va;
      if (n > PGSIZE)
        n = PGSIZE;
      ilock(v->ip);
      int got = readi(v->ip, 0, (uint64)mem, v->off + (va - v->start), n);
      iunlock(v->ip);
      if (got < 0) {
        kfree(mem);
        return 0;
      }
      n = got; // a mapping past the end of the file reads as zeros
    }
    if (n < PGSIZE)
      memset(mem + n, 0, PGSIZE - n);
  } else {
    memset(mem, 0, PGSIZE);
    if (me)
      me->minflt++; // a zero page, no I/O
  }
  int perm = PTE_U;
  if (v->prot & VMA_READ)
    perm |= PTE_R;
  if (v->prot & VMA_WRITE)
    perm |= PTE_W;
  if (v->prot & VMA_EXEC)
    perm |= PTE_X;
  acquire(&mm->lock);
  pte_t *pte = walk(mm->pagetable, va, 0);
  if (pte && (*pte & PTE_V)) {
    // another thread got there first
    uint64 pa = PTE2PA(*pte);
    release(&mm->lock);
    kfree(mem);
    return pa;
  }
  if (mappages(mm->pagetable, va, PGSIZE, (uint64)mem, perm) != 0) {
    release(&mm->lock);
    kfree(mem);
    return 0;
  }
  mm->pages++;
  release(&mm->lock);
  return (uint64)mem;
}

// resolve a fault at va: returns the page's physical address, or 0 when
// va is not a valid address of this space (or memory ran out).
uint64
mm_fault_pa(struct mm *mm, uint64 va, int write)
{
  if (va >= plat.maxva)
    return 0;
  acquire(&mm->lock);
  struct vma *v = vma_find(mm, va);
  if (v == 0 || (write && !(v->prot & VMA_WRITE)) || (!write && !(v->prot & VMA_READ))) {
    release(&mm->lock);
    return 0;
  }
  pte_t *pte = walk(mm->pagetable, va, 0);
  if (pte && (*pte & PTE_V)) {
    uint64 pa;
    if (write && !(*pte & PTE_W)) {
      if (!(*pte & PTE_COW) || cow_break(mm, pte) < 0) {
        release(&mm->lock);
        return 0;
      }
    }
    pa = PTE2PA(*pte);
    release(&mm->lock);
    return pa;
  }
  struct vma area = *v; // the area may change once the lock is dropped
  release(&mm->lock);
  return fault_in(mm, &area, va);
}

int
mm_fault(struct mm *mm, uint64 va, int write)
{
  return mm_fault_pa(mm, va, write) ? 0 : -1;
}

// ---- brk, mmap, mprotect ----

// the heap area is [heap_start, brk). eager growth populates the pages.
// how far the heap may grow: up to the next area above it, or to the
// guard page below the stack when there is nothing in between. an
// allocator that maps memory of its own puts a ceiling here, which is
// why this is a question the kernel has to answer rather than a
// constant.
uint64
mm_brk_max(struct mm *mm)
{
  uint64 top = mm->stack_bottom - PGSIZE;

  acquire(&mm->lock);
  for (struct vma *v = mm->vmas; v; v = v->next) {
    if (v->start >= PGROUNDUP(mm->brk) && v->start < top) {
      top = v->start;
      break; // the areas are sorted, so this is the lowest one above
    }
  }
  release(&mm->lock);
  return top;
}

int
mm_brk(struct mm *mm, uint64 newbrk, int eager)
{
  acquire(&mm->lock);
  if (newbrk < mm->heap_start) {
    release(&mm->lock);
    return -1;
  }
  uint64 oldend = PGROUNDUP(mm->brk), newend = PGROUNDUP(newbrk);
  if (newend > oldend) {
    // must fit below the mmap areas and the stack: no other area in between
    if (newend > mm->stack_bottom - PGSIZE || vma_overlaps(mm, oldend, newend)) {
      release(&mm->lock);
      return -1;
    }
    struct vma *v = vma_find(mm, oldend - 1);
    if (v && (v->flags & VMA_HEAP) && v->end == oldend) {
      v->end = newend;
    } else if (vma_add(mm, oldend, newend, VMA_READ | VMA_WRITE, VMA_ANON | VMA_HEAP, 0, 0, 0) < 0) {
      release(&mm->lock);
      return -1;
    }
    if (eager) {
      release(&mm->lock);
      for (uint64 a = oldend; a < newend; a += PGSIZE) {
        if (mm_fault_pa(mm, a, 1) == 0) {
          vma_remove(mm, oldend, newend);
          return -1;
        }
      }
      acquire(&mm->lock);
    }
  } else if (newend < oldend) {
    vma_remove_locked(mm, newend, oldend);
  }
  mm->brk = newbrk;
  release(&mm->lock);
  mm_reap(mm);
  return 0;
}

// a free range of len bytes below mmap_base (top-down), or 0.
static uint64
mmap_find(struct mm *mm, uint64 len)
{
  uint64 hi = mm->mmap_base;
  while (hi >= len + PGSIZE) {
    uint64 lo = hi - len;
    // the highest area below hi
    struct vma *hit = 0;
    for (struct vma *v = mm->vmas; v; v = v->next) {
      if (v->start >= hi)
        break;
      if (v->end > lo)
        hit = v;
    }
    if (hit == 0) {
      if (lo < PGROUNDUP(mm->brk) + PGSIZE)
        return 0; // would collide with the heap
      return lo;
    }
    hi = hit->start;
  }
  return 0;
}

uint64
mm_mmap(struct mm *mm, uint64 addr, uint64 len, int prot, int flags, struct inode *ip, uint64 off)
{
  len = PGROUNDUP(len);
  if (len == 0)
    return 0;
  acquire(&mm->lock);
  if (flags & VMA_FIXED) {
    if (addr & (PGSIZE - 1)) {
      release(&mm->lock);
      return 0;
    }
    vma_remove_locked(mm, addr, addr + len); // MAP_FIXED replaces
  } else {
    addr = mmap_find(mm, len);
    if (addr == 0) {
      release(&mm->lock);
      return 0;
    }
  }
  int vflags = (flags & VMA_SHARED) | (ip ? VMA_FILE : VMA_ANON);
  uint64 filesz_end = 0;
  if (ip) {
    idup(ip);
    filesz_end = addr + len; // reads beyond the file's size return zeros (readi returns short)
  }
  if (vma_add(mm, addr, addr + len, prot, vflags, ip, off, filesz_end) < 0) {
    release(&mm->lock);
    if (ip) {
      iput(ip);
    }
    return 0;
  }
  if (addr < mm->mmap_base)
    mm->mmap_base = addr;
  release(&mm->lock);
  mm_reap(mm);
  return addr;
}

int
mm_mprotect(struct mm *mm, uint64 addr, uint64 len, int prot)
{
  len = PGROUNDUP(len);
  if ((addr & (PGSIZE - 1)) || len == 0)
    return -1;
  acquire(&mm->lock);
  // every page of the range must be inside an area
  for (uint64 a = addr; a < addr + len;) {
    struct vma *v = vma_find(mm, a);
    if (v == 0) {
      release(&mm->lock);
      return -1;
    }
    a = v->end;
  }
  // split at the edges so the change applies exactly to the range
  for (uint64 edge = addr; edge <= addr + len; edge += len) {
    struct vma *v = vma_find(mm, edge);
    if (v && v->start != edge && edge != addr + len) {
      struct vma *w = kmem_cache_alloc(vma_cache);
      if (w == 0) {
        release(&mm->lock);
        return -1;
      }
      *w = *v;
      if (w->ip)
        w->ip = idup(w->ip);
      w->start = edge;
      if (w->ip)
        w->off = v->off + (edge - v->start);
      v->end = edge;
      w->next = v->next;
      v->next = w;
    }
    if (len == 0)
      break;
  }
  {
    struct vma *v = vma_find(mm, addr + len - 1);
    if (v && v->end != addr + len) {
      struct vma *w = kmem_cache_alloc(vma_cache);
      if (w == 0) {
        release(&mm->lock);
        return -1;
      }
      *w = *v;
      if (w->ip)
        w->ip = idup(w->ip);
      w->start = addr + len;
      if (w->ip)
        w->off = v->off + (addr + len - v->start);
      v->end = addr + len;
      w->next = v->next;
      v->next = w;
    }
  }
  for (struct vma *v = vma_after(mm, addr); v && v->start < addr + len; v = v->next) {
    v->prot = prot;
    // present pages: apply now (write permission is re-granted lazily
    // through the COW path, so a shared frame is never made writable)
    for (uint64 a = v->start; a < v->end; a += PGSIZE) {
      pte_t *pte = walk(mm->pagetable, a, 0);
      if (pte == 0 || !(*pte & PTE_V))
        continue;
      uint64 f = PTE_FLAGS(*pte) & ~(PTE_R | PTE_X);
      if (prot & VMA_READ)
        f |= PTE_R;
      if (prot & VMA_EXEC)
        f |= PTE_X;
      if (!(prot & VMA_WRITE))
        f &= ~PTE_W;
      *pte = PA2PTE(PTE2PA(*pte)) | f;
    }
  }
  sfence_vma();
  release(&mm->lock);
  return 0;
}
