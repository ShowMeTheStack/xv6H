#ifndef XV6_MM_H
#define XV6_MM_H
//
// a process's address space: a page table plus a sorted list of virtual
// memory areas. pages are allocated on first touch (a fault), file-backed
// areas read their pages from the inode, fork shares pages copy-on-write,
// and mmap/munmap/mprotect operate on the areas.
//
// layout (Sv48 or Sv39, plat.maxva at the top):
//   [0, brk)                       text, data, bss (ELF segments), then the heap
//   [stack_bottom, stack_top)      the stack, STACK_MAX bytes, grown by faults
//   mmap areas                     below the stack, allocated downward from mmap_base
//   TRAPFRAME, TRAMPOLINE          the two kernel pages (not areas)
//

#define STACK_MAX   (8UL << 20)   // 8 MB stack region, guard page below it
#define MMAP_GAP    (1UL << 30)   // between the stack region and the first mmap

// how far the stack and the mmap area may be slid down, in pages: a
// megabyte and a gigabyte respectively. See aslr_slide() in mm.c and
// /sys/kernel/randomize_va_space.
#define ASLR_STACK_PAGES  256
#define ASLR_MMAP_PAGES   262144
#define ASLR_PIE_PAGES    65536
#define ASLR_BRK_PAGES    2048

// vma.prot (mman.h's PROT_* have the same values)
#define VMA_READ  1
#define VMA_WRITE 2
#define VMA_EXEC  4
// vma.flags
#define VMA_ANON   0x10  // zero-filled pages
#define VMA_FILE   0x20  // pages come from ip at off + (va - start), up to filesz_end, zero beyond
#define VMA_SHARED 0x40  // fork shares the pages, and writes are visible to all (no COW)
#define VMA_STACK  0x80
#define VMA_HEAP   0x100
#define VMA_FIXED  0x200 // an ELF segment or the stack: never merged

struct vma {
  uint64 start, end;      // page aligned, end exclusive
  int prot;
  int flags;
  struct inode *ip;       // VMA_FILE: referenced (idup) for the area's life
  uint64 off;             // file offset of start
  uint64 filesz_end;      // VMA_FILE: address where file data ends (rest is zero)
  struct vma *next;
};

struct mm {
  struct spinlock lock;   // the areas and the page table
  int ref;                // threads sharing this address space
  pagetable_t pagetable;
  struct vma *vmas;       // sorted by start
  uint64 brk;             // end of the heap (sbrk)
  uint64 heap_start;      // where the heap began (end of the last ELF segment)
  uint64 stack_top, stack_bottom;
  uint64 mmap_base;       // next mmap goes below this
  uint64 pages;           // resident user pages (statistics)
  struct vma *dead;       // areas waiting for their inode reference to be dropped
  uint64 tf_slots[THREAD_TF_SLOTS / 64]; // trapframe pages in use by extra threads
};

// mm.c
struct mm *mm_create(void);                       // empty, with trampoline and trapframe mapped by the caller
struct mm *mm_dup(struct mm *mm);                 // another thread shares it
int mm_free(struct mm *mm);                       // drops a reference; 1 if that was the last one
int mm_fork(struct mm *dst, struct mm *src);      // copy areas, share pages copy-on-write
struct vma *vma_find(struct mm *mm, uint64 va);
int vma_add(struct mm *mm, uint64 start, uint64 end, int prot, int flags, struct inode *ip, uint64 off,
            uint64 filesz_end);
int vma_remove(struct mm *mm, uint64 start, uint64 end); // unmap and drop the range from every area
int mm_fault(struct mm *mm, uint64 va, int write);        // 0 handled, -1 not an address of this space
uint64 mm_fault_pa(struct mm *mm, uint64 va, int write);  // the page's physical address after handling, or 0
int mm_brk(struct mm *mm, uint64 newbrk, int eager);      // grow or shrink the heap
uint64 mm_brk_max(struct mm *mm);                 // the highest the heap may grow
uint64 mm_mmap(struct mm *mm, uint64 addr, uint64 len, int prot, int flags, struct inode *ip, uint64 off);
int mm_mprotect(struct mm *mm, uint64 addr, uint64 len, int prot);
int mm_setup_stack(struct mm *mm);                // the stack area below the trapframe
int mm_add_trapframe(struct mm *mm, struct proc *p);  // map a thread's trapframe, set p->tf_va
void mm_del_trapframe(struct mm *mm, struct proc *p);

#endif
