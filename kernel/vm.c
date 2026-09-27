#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "platform.h"
#include "page.h"
#include "mm.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];    // kernel.ld: the end of the kernel's code
extern char erodata[];  // ... and of its constants

extern char trampoline[]; // trampoline.S

static void unmap_range(pagetable_t pt, int level, uint64 base, uint64 start, uint64 end, int do_free, uint64 *counter);
static int copy_range(pagetable_t old, pagetable_t new, int level, uint64 base, uint64 end, uint64 *done);

// the kernel's page table has plat.mmu_levels levels; level 3 exists only
// in Sv48. kwalk() returns the PTE for va at the given level, creating the
// intermediate tables when alloc is set.
static pte_t *
kwalk(pagetable_t pt, uint64 va, int level, int alloc)
{
  for (int l = plat.mmu_levels - 1; l > level; l--) {
    pte_t *pte = &pt[PX(l, va)];
    if (*pte & PTE_V) {
      if (*pte & (PTE_R | PTE_W | PTE_X))
        panic("kwalk: inside a large page");
      pt = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pt = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pt, 0, PGSIZE);
      *pte = PA2PTE(pt) | PTE_V;
    }
  }
  return &pt[PX(level, va)];
}

// map [va, va+sz) to [pa, pa+sz) in the kernel table with the largest
// pages the alignment allows: 1 GiB (level 2), 2 MB (level 1), 4 KB.
static void
kmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if ((va | pa | sz) & (PGSIZE - 1))
    panic("kmap: alignment");
  uint64 end = va + sz;
  while (va < end) {
    int level = 0;
    uint64 left = end - va;
    if (((va | pa) & ((1UL << 30) - 1)) == 0 && left >= (1UL << 30))
      level = 2;
    else if (((va | pa) & ((1UL << 21) - 1)) == 0 && left >= (1UL << 21))
      level = 1;
    pte_t *pte = kwalk(kpgtbl, va, level, 1);
    if (pte == 0)
      panic("kmap: out of memory");
    if (*pte & PTE_V)
      panic("kmap: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    uint64 step = PGSIZE << (9 * level);
    va += step;
    pa += step;
  }
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, plat.uart_base, plat.uart_base, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interfaces
  for (int i = 0; i < plat.nvirtio; i++)
    kvmmap(kpgtbl, plat.virtio_base[i], plat.virtio_base[i], PGSIZE, PTE_R | PTE_W);

  // interrupt controller(s)
  if (plat.intc_kind == INTC_PLIC) {
    kvmmap(kpgtbl, plat.plic_base, plat.plic_base, plat.plic_size, PTE_R | PTE_W);
  } else {
    kvmmap(kpgtbl, plat.aplic_m_base, plat.aplic_m_base, plat.aplic_size, PTE_R | PTE_W);
    kvmmap(kpgtbl, plat.aplic_s_base, plat.aplic_s_base, plat.aplic_size, PTE_R | PTE_W);
    if (plat.intc_kind == INTC_IMSIC)
      kvmmap(kpgtbl, plat.imsic_s_base, plat.imsic_s_base, plat.imsic_s_size, PTE_R | PTE_W);
  }

  // CLINT, for inter-processor interrupts (msip)
  kvmmap(kpgtbl, plat.clint_base, plat.clint_base, 0x10000, PTE_R | PTE_W);

  // the finisher: a halfword written here stops the machine. it is one
  // page and it is what reboot(2) uses, so it has to be mapped before
  // anyone asks to turn the machine off.
  kvmmap(kpgtbl, SIFIVE_TEST, SIFIVE_TEST, PGSIZE, PTE_R | PTE_W);
  if (plat.rtc_base)
    kvmmap(kpgtbl, plat.rtc_base, plat.rtc_base, PGSIZE, PTE_R | PTE_W);

  // the kernel's own image, one permission at a time: code that can be
  // executed and not written, constants that can be neither, and
  // everything after them -- data, and all of physical RAM -- that can
  // be written and not executed. Nothing anywhere is both writable and
  // executable; kvm_check_wx() at the end of this function says so
  // rather than trusting it.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, (uint64)erodata - (uint64)etext,
         PTE_R);

  // all of physical RAM, with 2 MB and 1 GiB pages wherever the
  // alignment allows (512 GiB take a few hundred entries).
  kvmmap(kpgtbl, (uint64)erodata, (uint64)erodata, plat.ram_end - (uint64)erodata,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  kvm_check_wx(kpgtbl);
  return kpgtbl;
}

// walk a page table and panic if any leaf is both writable and
// executable. Cheap, done once, and worth doing: W^X that nothing
// checks is a comment.
static void
check_wx(pagetable_t pt, int level, uint64 va)
{
  for (int i = 0; i < 512; i++) {
    pte_t pte = pt[i];
    if ((pte & PTE_V) == 0)
      continue;
    uint64 a = va | ((uint64)i << (12 + 9 * level));
    if (pte & (PTE_R | PTE_W | PTE_X)) {
      if ((pte & PTE_W) && (pte & PTE_X)) {
        printk("kvm: 0x%lx is writable and executable\n", a);
        panic("kvm_check_wx");
      }
      continue;
    }
    if (level > 0)
      check_wx((pagetable_t)PTE2PA(pte), level - 1, a);
  }
}

void
kvm_check_wx(pagetable_t pt)
{
  check_wx(pt, plat.mmu_levels - 1, 0);
}

// mappings added to the kernel table while the system runs (kernel
// stacks): serialized by kpt_lock. no flush is needed for a new mapping;
// the caller of kvmunmap_rt arranges the TLB flush on every hart.
static struct spinlock kpt_lock;

// Is this virtual address already mapped in the kernel table?
//
// A non-panicking walk, unlike kwalk: the device ranges of two interrupt
// controllers overlap by design (a PLIC and an APLIC's M domain both live at
// 0x0c000000), and a guest adopting a new controller has to be able to ask
// rather than assume. Large-page leaves count as mapped -- that is what
// kwalk panics on.
int
kvm_mapped(uint64 va)
{
  pagetable_t pt = kernel_pagetable;

  for (int l = plat.mmu_levels - 1; l >= 0; l--) {
    pte_t *pte = &pt[PX(l, va)];
    if (!(*pte & PTE_V))
      return 0;
    if (*pte & (PTE_R | PTE_W | PTE_X))
      return 1;             // a leaf, of whatever size, covers this address
    pt = (pagetable_t)PTE2PA(*pte);
  }
  return 0;
}

int
kvmmap_rt(uint64 va, uint64 pa, uint64 sz, int perm)
{
  acquire(&kpt_lock);
  for (uint64 off = 0; off < sz; off += PGSIZE) {
    pte_t *pte = kwalk(kernel_pagetable, va + off, 0, 1);
    if (pte == 0) {
      release(&kpt_lock);
      return -1;
    }
    if (*pte & PTE_V)
      panic("kvmmap_rt: remap");
    *pte = PA2PTE(pa + off) | perm | PTE_V;
  }
  release(&kpt_lock);
  return 0;
}

// if the level-0 table covering va holds no mapping any more, unlink it
// from its parent and return its page for the caller to free after the
// TLB flush; 0 otherwise.
uint64
kvmprune_rt(uint64 va)
{
  uint64 pa = 0;
  acquire(&kpt_lock);
  pte_t *parent = kwalk(kernel_pagetable, va, 1, 0);
  if (parent && (*parent & PTE_V) && !(*parent & (PTE_R | PTE_W | PTE_X))) {
    pagetable_t t = (pagetable_t)PTE2PA(*parent);
    int empty = 1;
    for (int i = 0; i < 512; i++)
      if (t[i]) {
        empty = 0;
        break;
      }
    if (empty) {
      *parent = 0;
      pa = (uint64)t;
    }
  }
  release(&kpt_lock);
  return pa;
}

// clear a 4 KB kernel mapping; returns the physical address it had, or 0.
uint64
kvmunmap_rt(uint64 va, uint64 sz)
{
  uint64 pa = 0;
  acquire(&kpt_lock);
  for (uint64 off = 0; off < sz; off += PGSIZE) {
    pte_t *pte = kwalk(kernel_pagetable, va + off, 0, 0);
    if (pte && (*pte & PTE_V)) {
      if (off == 0)
        pa = PTE2PA(*pte);
      *pte = 0;
    }
  }
  release(&kpt_lock);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  kmap(kpgtbl, va, pa, sz, perm);
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  initlock(&kpt_lock, "kpt");
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_KSATP(kernel_pagetable, plat.mmu_levels));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= plat.maxva)
    panic("walk");

  for (int level = plat.mmu_levels - 1; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= plat.maxva)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");
  if (npages == 0)
    return;
  unmap_range(pagetable, plat.mmu_levels - 1, 0, va, va + npages * PGSIZE, do_free, 0);
}

// unmap npages user pages from va, dropping their references and
// decrementing *counter for each user page removed.
void
uvmunmap_count(pagetable_t pagetable, uint64 va, uint64 npages, uint64 *counter)
{
  if (npages == 0)
    return;
  unmap_range(pagetable, plat.mmu_levels - 1, 0, va, va + npages * PGSIZE, 1, counter);
}

// walk the table hierarchically over [start, end): subtrees that were
// never created (lazily grown memory) are skipped in one step, so
// unmapping a process that grew to the top of a 128 TiB address space
// costs what it mapped, not what it reserved. pt covers [base, base + 512 * span).
static void
unmap_range(pagetable_t pt, int level, uint64 base, uint64 start, uint64 end, int do_free, uint64 *counter)
{
  uint64 span = (uint64)PGSIZE << (9 * level);
  int first = (start > base) ? (start - base) / span : 0;
  for (int i = first; i < 512; i++) {
    uint64 lo = base + i * span;
    if (lo >= end)
      break;
    pte_t pte = pt[i];
    if ((pte & PTE_V) == 0)
      continue;
    if (level > 0) {
      if (pte & (PTE_R | PTE_W | PTE_X))
        panic("unmap_range: large page in a user table");
      unmap_range((pagetable_t)PTE2PA(pte), level - 1, lo, start, end, do_free, counter);
    } else {
      if (do_free)
        page_put(PA2PAGE(PTE2PA(pte)));
      if (counter && (pte & PTE_U))
        (*counter)--;
      pt[i] = 0;
    }
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      printk("freewalk: entry %d is a leaf, pte %p\n", i, (void *)pte);
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 done = 0;
  if (copy_range(old, new, plat.mmu_levels - 1, 0, sz, &done) < 0) {
    uvmunmap(new, 0, PGROUNDUP(done) / PGSIZE, 1);
    return -1;
  }
  return 0;
}

// copy the present pages below end from old into new, hierarchically
// (subtrees never created are skipped). *done is the address up to which
// new is populated, for cleanup on failure.
static int
copy_range(pagetable_t old, pagetable_t new, int level, uint64 base, uint64 end, uint64 *done)
{
  uint64 span = (uint64)PGSIZE << (9 * level);
  for (int i = 0; i < 512; i++) {
    uint64 lo = base + i * span;
    if (lo >= end)
      break;
    pte_t pte = old[i];
    if ((pte & PTE_V) == 0)
      continue;
    if (level > 0) {
      if (copy_range((pagetable_t)PTE2PA(pte), new, level - 1, lo, end, done) < 0)
        return -1;
    } else {
      uint64 pa = PTE2PA(pte);
      uint flags = PTE_FLAGS(pte);
      char *mem = kalloc();
      if (mem == 0)
        return -1;
      memmove(mem, (char *)pa, PGSIZE);
      if (mappages(new, lo, PGSIZE, (uint64)mem, flags) != 0) {
        kfree(mem);
        return -1;
      }
      *done = lo + PGSIZE;
    }
  }
  return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(struct mm *mm, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= plat.maxva)
      return -1;
    // the page must be writable by the process: absent pages are brought
    // in and copy-on-write pages copied, exactly as a store would.
    pa0 = mm_fault_pa(mm, va0, 1);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(struct mm *mm, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    if (va0 >= plat.maxva)
      return -1;
    pa0 = mm_fault_pa(mm, va0, 0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(struct mm *mm, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    if (va0 >= plat.maxva)
      return -1;
    pa0 = mm_fault_pa(mm, va0, 0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}


