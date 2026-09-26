//
// xv6 hypervisor core: VM and vcpu lifecycle, guest memory (G-stage
// page tables over 2 MB chunks of a memory pool), guest entry/exit
// and the exit dispatcher.
//
// a vcpu is run by an ordinary xv6 process that called the vm_run
// system call; vm_run() below loops between guest execution and
// handling whatever made the guest exit, exactly the way usertrap()
// alternates between a user process and its traps.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "blk.h"
#include "fs.h"
#include "file.h"
#include "elf.h"
#include "stat.h"
#include "platform.h"
#include "intc.h"
#include "imsic.h"
#include "page.h"
#include "hyp.h"
#include "vmstate.h"
#include "net.h"
#include "cgw.h"
#include "errno.h"

struct vm *vmtab[NVM];

// how many VMs exist, which is what /sys/hypervisor/vms reports
int
vm_count(void)
{
  int n = 0;

  for (int i = 0; i < NVM; i++)
    if (vmtab[i] && vmtab[i]->state != VM_FREE)
      n++;
  return n;
}
static struct spinlock vmslock;

void guestret(struct vcpu *v);
void guestvec(void);
void kernelvec(void);

_Static_assert(__builtin_offsetof(struct vcpu, hostctx) == 256, "vcpu layout");
_Static_assert(__builtin_offsetof(struct vcpu, host_tp) == 368, "vcpu layout");

// ---- guest memory, on demand, 2 MB at a time or 4 KB at a time ----
//
// a chunk of guest RAM is 2 MB, which is what a G-stage megapage
// covers, and backing one with a single block of host memory is the
// cheap thing to do: one page table entry, one allocation, and the
// hardware walks one level less. That is what happens while the host
// still has 2 MB blocks to give and the VM did not ask for otherwise.
//
// but a megapage is one thing: it cannot be given back in pieces, and
// two guests cannot be made to share half of it. A balloon taking a
// page back, same-page merging pointing two guests at one frame, and
// copy-on-write after a snapshot all need the page to be the unit. So
// a chunk has a second form -- 512 frames, mapped one at a time -- and
// gstage_split turns the first into the second without moving
// anything: the block stays where it is, the page database is told it
// is now 512 separate pages (kmem_split), and the megapage entry is
// replaced by a table of 512 leaves.
//
// which form a chunk has is in the low bit of its entry in vm->chunk;
// nothing above this file needs to know, because vm_gpa2hpa answers
// with a host address and the length that is contiguous from it.

static int gstage_map_chunk(struct vm *vm, uint64 gpa, uint64 hpa);
static int gstage_map_page(struct vm *vm, uint64 gpa, uint64 hpa);
static void gstage_unmap_page(struct vm *vm, uint64 gpa);
static int gstage_split(struct vm *vm, int k, uint64 *pages);

// may this VM hold `bytes` more of the host's memory?
//
// the limit is the whole of a VM's memory control: it may be given
// more memory than it will ever be allowed to hold, because nothing
// is backed until it is touched, and this is where the promise is
// kept. Over the limit the answer is no, and the fault handler --
// which, unlike this, is allowed to wait -- asks the guest for memory
// back and tries again.
static int
mem_charge(struct vm *vm, uint64 bytes)
{
  int ok;

  if (vm->mem_limit == 0)
    return 0;
  acquire(&vm->memlock);
  ok = vm->resident + bytes <= vm->mem_limit;
  release(&vm->memlock);
  return ok ? 0 : -2;
}

// back one 4 KB page of chunk k, giving the chunk its array of frames
// first if it has none. returns 0 when the page is backed.
static int
touch_page(struct vm *vm, int k, uint64 in)
{
  uint64 *pages;
  int i = in / PGSIZE;

  // the common case is a page that is already there. an entry only
  // ever goes from zero to a frame under the lock, and reading an
  // aligned word cannot tear, so seeing a frame here means there is
  // one; seeing zero means taking the lock and looking properly.
  uint64 c = vm->chunk[k];
  if (c && gc_split(c) && gc_pages(c)[i])
    return 0;

  acquire(&vm->memlock);
  if (vm->chunk[k] && !gc_split(vm->chunk[k])) {
    release(&vm->memlock);
    return 0;   // it became a megapage while we were on our way here
  }
  pages = gc_split(vm->chunk[k]) ? gc_pages(vm->chunk[k]) : 0;
  release(&vm->memlock);

  if (pages == 0) {
    uint64 *np = (uint64 *)kalloc();
    if (np == 0)
      return -1;
    memset(np, 0, PGSIZE);
    acquire(&vm->memlock);
    if (vm->chunk[k] == 0) {
      vm->chunk[k] = (uint64)np | GC_SPLIT;
      vm->nsplit++;
      np = 0;
    }
    pages = gc_split(vm->chunk[k]) ? gc_pages(vm->chunk[k]) : 0;
    release(&vm->memlock);
    if (np)
      kfree(np);
    if (pages == 0)
      return 0;   // another vcpu made it a megapage
  }

  if (pages[i])
    return 0;
  int over = mem_charge(vm, PGSIZE);
  if (over < 0)
    return over;
  void *pa = kalloc_guest(0);
  if (pa == 0)
    return -1;
  memset(pa, 0, PGSIZE);
  acquire(&vm->memlock);
  if (pages[i] == 0) {
    if (gstage_map_page(vm, G_RAM_BASE + (uint64)k * HCHUNK + in,
                        (uint64)pa) == 0) {
      pages[i] = (uint64)pa;
      vm->resident += PGSIZE;
      pa = 0;
    }
  }
  release(&vm->memlock);
  if (pa) {
    kfree(pa);
    return pages[i] ? 0 : -1;
  }
  return 0;
}

// back the granule holding gpa, if gpa is guest RAM and it is not
// backed yet. returns 0 when it is backed (now or before), 1 when gpa
// is not guest RAM, -1 when no memory could be had.
int
vm_touch(struct vm *vm, uint64 gpa)
{
  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return 1;
  uint64 off = gpa - G_RAM_BASE;
  int k = off / HCHUNK;
  uint64 c = vm->chunk[k];

  if (c && !gc_split(c))
    return 0;
  if (c == 0 && vm->prefer_huge && mem_charge(vm, HCHUNK) == 0) {
    // allocate and zero outside the lock; another vcpu may win the race
    void *pa = kalloc_guest(HCHUNK_ORDER);
    if (pa) {
      memset(pa, 0, HCHUNK);
      acquire(&vm->memlock);
      if (vm->chunk[k] == 0) {
        if (gstage_map_chunk(vm, G_RAM_BASE + (uint64)k * HCHUNK,
                             (uint64)pa) == 0) {
          vm->chunk[k] = (uint64)pa;
          vm->resident += HCHUNK;
          pa = 0;
        }
      }
      release(&vm->memlock);
      if (pa) {
        kfree_order(pa, HCHUNK_ORDER);
        return vm->chunk[k] ? 0 : -1;
      }
      return 0;
    }
    // no 2 MB block to be had. what is left of memory is in smaller
    // pieces, and a guest that can run on those is better than a
    // guest that cannot run at all.
  }
  return touch_page(vm, k, off % HCHUNK);
}

// make the chunk holding gpa page-granular, backing it first if it is
// not backed at all. 0 on success.
int
vm_split(struct vm *vm, uint64 gpa)
{
  int r;

  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return -1;
  int k = (gpa - G_RAM_BASE) / HCHUNK;
  if ((r = vm_touch(vm, gpa)) != 0)
    return r;
  if (gc_split(vm->chunk[k]))
    return 0;
  // the page that will hold the 512 frames, allocated before the lock
  // is taken because kalloc may ask the block cache for memory back
  uint64 *pages = (uint64 *)kalloc();
  if (pages == 0)
    return -1;
  acquire(&vm->memlock);
  r = gstage_split(vm, k, pages);
  release(&vm->memlock);
  if (r != 0)
    kfree(pages);
  if (r == 0)
    vm_fence(vm);
  return r < 0 ? -1 : 0;
}

// take one 4 KB page of guest RAM away: the guest's next touch of it
// faults and gets a fresh zeroed page. this is what a balloon
// inflating does, and what a page that has been merged away no longer
// needs. the chunk is split first if it is still a megapage.
int
vm_unback(struct vm *vm, uint64 gpa)
{
  uint64 frame = 0;

  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return -1;
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK;
  int i = (off % HCHUNK) / PGSIZE;

  if (vm->chunk[k] == 0)
    return 0;   // nothing there to take
  uint64 *spare = 0;
  if (!gc_split(vm->chunk[k]) && (spare = (uint64 *)kalloc()) == 0)
    return -1;

  acquire(&vm->memlock);
  if (vm->chunk[k] == 0) {
    release(&vm->memlock);
    if (spare)
      kfree(spare);
    return 0;
  }
  if (!gc_split(vm->chunk[k])) {
    if (spare == 0 || gstage_split(vm, k, spare) < 0) {
      release(&vm->memlock);
      if (spare)
        kfree(spare);
      return -1;
    }
    spare = 0;   // the chunk owns it now
  }
  uint64 *pages = gc_pages(vm->chunk[k]);
  frame = pages[i];
  if (frame) {
    pages[i] = 0;
    vm->resident -= PGSIZE;
    gstage_unmap_page(vm, G_RAM_BASE + off);
  }
  release(&vm->memlock);
  if (spare)
    kfree(spare);
  // the frame goes back to the host only once no vcpu can reach it
  vm_fence_sync(vm);
  if (frame)
    kfree((void *)frame);
  return 0;
}

// ---- init ----

void
hypinit(void)
{
  initlock(&vmslock, "vms");
  for (int i = 0; i < NVM; i++)
    vmtab[i] = 0;
  if (!plat.has_h) {
    printk("hyp: no hypervisor extension; guests disabled\n");
    return;
  }
  hypinithart();
  printk("hyp: hypervisor extension present, %ld MB free for guests, %s G-stage, %d guest interrupt files/hart\n",
         (kmem_free_pages() >> 8), plat.mmu_levels == 4 ? "sv48x4" : "sv39x4", vimsic_geilen());
}

// per-hart hypervisor CSR setup (every hart calls this from main()).
void
hypinithart(void)
{
  if (!plat.has_h)
    return;
  // exceptions the guest kernel handles itself: misaligned fetch,
  // breakpoint, misaligned/faulting loads and stores, ecall from VU,
  // and its own page faults. illegal instructions (2) and ecall from
  // VS (10) come to us; guest page faults (20-23) always do.
  csr_write(CSR_HEDELEG, (1L << 0) | (1L << 1) | (1L << 3) | (1L << 4) | (1L << 5) |
                         (1L << 6) | (1L << 7) | (1L << 8) | (1L << 12) | (1L << 13) |
                         (1L << 15));
  // VS-level software, timer and external interrupts go to the guest.
  csr_write(CSR_HIDELEG, HVIP_VSSIP | HVIP_VSTIP | HVIP_VSEIP);
  // guests may read cycle, time and instret.
  csr_write(CSR_HCOUNTEREN, 7);
  // guests use the Sstc stimecmp (as vstimecmp), and their page tables
  // rely on hardware A/D bit updates (xv6 never sets A/D itself).
  csr_write(CSR_HENVCFG, HENVCFG_STCE | HENVCFG_ADUE);
  csr_write(CSR_HTIMEDELTA, 0);
  csr_write(CSR_HGEIE, 0);
  csr_write(CSR_HVIP, 0);
  csr_write(CSR_HGATP, 0);
  vimsic_hartinit();
}

// ---- G-stage page tables (Sv48x4 when the host runs Sv48, else Sv39x4) ----

// a page for a G-stage table. the tables are ordinary pages: one per
// 2 MB of a chunk that has been split, which is unbounded in a way
// that a fixed pool is not, and each one is given back by
// gstage_free_tables when the VM goes.
//
// kalloc_order(0) rather than kalloc(): this runs with vm->lock held,
// and kalloc may ask the block cache to give pages back, which sleeps.
// The order-0 path takes km.lock and nothing else.
static uint64 *
pt_page(struct vm *vm)
{
  uint64 *p = kalloc_order(0);

  if (p == 0)
    return 0;
  memset(p, 0, PGSIZE);
  vm->pttables++;
  return p;
}

// G-stage leaf bits: U is required of a leaf, and A and D are preset
// because nothing here reads them.
#define GPTE_LEAF (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | (1L << 6) | (1L << 7))

// the entry that translates gpa at `level`, making the tables above it
// if `alloc`. level 1 is a 2 MB entry, level 0 a 4 KB one. the root has
// 2048 entries (11 index bits): Sv48x4 walks root(bits 39..49) -> level
// 2 -> level 1; Sv39x4 walks root(bits 30..40) -> level 1. vm->lock is
// held, or the VM is not running yet.
static uint64 *
gstage_pte(struct vm *vm, uint64 gpa, int level, int alloc)
{
  uint64 *t = vm->hgatp_root;
  int top = (plat.mmu_levels == 4) ? 3 : 2;

  for (int l = top; l > level; l--) {
    uint64 idx = (l == top) ? ((gpa >> (12 + 9 * l)) & 0x7ff) : PX(l, gpa);
    uint64 *next;
    if (t[idx] & PTE_V) {
      if (t[idx] & (PTE_R | PTE_W | PTE_X))
        return 0;   // a bigger leaf already covers it
      next = (uint64 *)PTE2PA(t[idx]);
    } else {
      if (!alloc)
        return 0;
      next = pt_page(vm);
      if (next == 0)
        return 0;
      t[idx] = PA2PTE(next) | PTE_V;
    }
    t = next;
  }
  return &t[level == top ? ((gpa >> (12 + 9 * level)) & 0x7ff) : PX(level, gpa)];
}

// map one 2 MB guest-physical chunk to host physical hpa. 0, or -1
// when the tables above it could not be made.
static int
gstage_map_chunk(struct vm *vm, uint64 gpa, uint64 hpa)
{
  uint64 *pte = gstage_pte(vm, gpa, 1, 1);

  if (pte == 0)
    return -1;
  *pte = PA2PTE(hpa) | GPTE_LEAF;
  return 0;
}

// map one 4 KB page
static int
gstage_map_page(struct vm *vm, uint64 gpa, uint64 hpa)
{
  uint64 *pte = gstage_pte(vm, gpa, 0, 1);

  if (pte == 0)
    return -1;
  *pte = PA2PTE(hpa) | GPTE_LEAF;
  return 0;
}

// unmap one 4 KB page. the caller flushes.
static void
gstage_unmap_page(struct vm *vm, uint64 gpa)
{
  uint64 *pte = gstage_pte(vm, gpa, 0, 0);

  if (pte)
    *pte = 0;
}

// a megapage becomes 512 pages of the same memory: one entry becomes a
// table of 512 leaves, the buddy block is broken up in the page
// database so its frames can be freed on their own, and nothing is
// copied or moved.
//
// `pages` is the page that will hold the 512 frames, allocated by the
// caller because this runs with vm->lock held. returns 0 when it was
// used, 1 when it was not needed, -1 on failure.
static int
gstage_split(struct vm *vm, int k, uint64 *pages)
{
  uint64 c = vm->chunk[k];

  if (c == 0 || gc_split(c))
    return 1;
  uint64 gpa = G_RAM_BASE + (uint64)k * HCHUNK;
  uint64 *pte = gstage_pte(vm, gpa, 1, 0);
  if (pte == 0)
    return -1;
  uint64 *t0 = pt_page(vm);
  if (t0 == 0)
    return -1;
  for (int i = 0; i < HCHUNK_PAGES; i++) {
    pages[i] = c + (uint64)i * PGSIZE;
    t0[i] = PA2PTE(pages[i]) | GPTE_LEAF;
  }
  *pte = PA2PTE(t0) | PTE_V;
  kmem_split((void *)c, HCHUNK_ORDER);
  vm->chunk[k] = (uint64)pages | GC_SPLIT;
  vm->nsplit++;
  return 0;
}

// free a table and everything below it. `below` is how many levels of
// table are under this one: an entry that is a leaf names guest
// memory, which vm_free_resources gives back, not this.
static void
free_subtable(struct vm *vm, uint64 *t, int below)
{
  if (below > 0)
    for (int i = 0; i < 512; i++) {
      uint64 pte = t[i];
      if ((pte & PTE_V) && !(pte & (PTE_R | PTE_W | PTE_X)))
        free_subtable(vm, (uint64 *)PTE2PA(pte), below - 1);
    }
  kfree((void *)t);
  vm->pttables--;
}

static void
gstage_free_tables(struct vm *vm)
{
  uint64 *root = vm->hgatp_root;
  int below = (plat.mmu_levels == 4) ? 2 : 1;

  if (root == 0)
    return;
  for (int i = 0; i < 2048; i++) {
    uint64 pte = root[i];
    if ((pte & PTE_V) && !(pte & (PTE_R | PTE_W | PTE_X)))
      free_subtable(vm, (uint64 *)PTE2PA(pte), below);
  }
  kfree_order(root, 2);   // 16 KB, 16 KB-aligned
  vm->hgatp_root = 0;
}

// ---- pages shared between guests (ksm.c) ----
//
// the four calls below are the whole of what same-page merging needs
// from this file: write-protect a page and hold on to its frame,
// let it be written again, point a page at somebody else's frame, and
// undo that when the guest writes. Everything about the G-stage and
// about locking stays here.

// write-protect the page holding gpa and return its frame with a
// reference taken, splitting the chunk if it is still a megapage.
// 0 if there is nothing there or it cannot be split.
uint64
vm_protect_page(struct vm *vm, uint64 gpa)
{
  uint64 frame = 0;

  if (vm_split(vm, gpa) < 0)
    return 0;
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK, i = (off % HCHUNK) / PGSIZE;

  acquire(&vm->memlock);
  uint64 c = vm->chunk[k];
  if (c && gc_split(c) && (frame = gc_pages(c)[i]) != 0) {
    uint64 *pte = gstage_pte(vm, G_RAM_BASE + off, 0, 0);
    if (pte && (*pte & PTE_V)) {
      *pte &= ~(uint64)PTE_W;
      page_get(PA2PAGE(frame));
    } else {
      frame = 0;
    }
  }
  release(&vm->memlock);
  if (frame)
    vm_fence_sync(vm);
  return frame;
}

// let the page be written again, and drop the reference
// vm_protect_page took.
//
// Not while a snapshot or a migration is running, though. Same-page
// merging write-protects a page to compare it and lets it be written
// again when the comparison fails -- and a snapshot has write-protected
// every page for its own reasons, so this would hand the guest a page it
// can change without anybody hearing about it. The page stays read-only;
// the guest's next write faults, the snapshot is told, and the fault
// makes it writable again (vm_unshare). vm_snap_unprotect puts back what
// is left at the end.
//
// (Seven pages of a migrated guest's kernel arrived holding what they
// held a round earlier because of this, and the guest panicked in its
// own lock code a moment after it started running.)
void
vm_unprotect_page(struct vm *vm, uint64 gpa, uint64 frame)
{
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK, i = (off % HCHUNK) / PGSIZE;

  acquire(&vm->memlock);
  uint64 c = vm->chunk[k];
  if (c && gc_split(c) && gc_pages(c)[i] == frame && !vm->snap.active) {
    uint64 *pte = gstage_pte(vm, G_RAM_BASE + off, 0, 0);
    if (pte && (*pte & PTE_V) && !(PA2PAGE(frame)->flags & PG_KSM))
      *pte |= PTE_W;
  }
  release(&vm->memlock);
  if (frame)
    page_put(PA2PAGE(frame));
}

// point this VM's page at `frame`, read-only, and give up the frame it
// had. the caller has write-protected both and found them equal.
// returns 0 on success.
int
vm_share_page(struct vm *vm, uint64 gpa, uint64 frame)
{
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK, i = (off % HCHUNK) / PGSIZE;
  uint64 old = 0;

  acquire(&vm->memlock);
  uint64 c = vm->chunk[k];
  uint64 *pte = c && gc_split(c) ? gstage_pte(vm, G_RAM_BASE + off, 0, 0) : 0;
  if (pte == 0 || !(*pte & PTE_V) || gc_pages(c)[i] == 0 ||
      gc_pages(c)[i] == frame) {
    release(&vm->memlock);
    return -1;
  }
  old = gc_pages(c)[i];
  gc_pages(c)[i] = frame;
  *pte = PA2PTE(frame) | (GPTE_LEAF & ~(uint64)PTE_W);
  // the frame is shared from now on, and the count says by how many
  PA2PAGE(frame)->flags |= PG_KSM;
  page_get(PA2PAGE(frame));
  release(&vm->memlock);

  // the old frame goes back only once no vcpu can still reach it
  vm_fence_sync(vm);
  if (PA2PAGE(old)->flags & PG_KSM)
    page_put(PA2PAGE(old));
  else
    kfree((void *)old);
  return 0;
}

// ---- cloning: one machine's pages in two machines (§9) ----
//
// A clone has the memory its original had, and neither of them has a copy
// of it: every page is mapped into both, read-only, and the first write
// by either of them faults and gets a copy of its own (vm_unshare, which
// is the same path same-page merging uses -- a clone is that, arranged
// deliberately rather than discovered).
//
// The two locks are never held together. The original's page is
// write-protected and a reference taken under its lock; the clone's page
// is installed under the clone's. In between the frame is held by that
// reference and cannot go anywhere.
int
vm_clone_page(struct vm *dst, struct vm *src, uint64 gpa)
{
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK, i = (off % HCHUNK) / PGSIZE;
  uint64 frame = 0;

  if (vm_split(src, G_RAM_BASE + off) < 0)
    return -1;
  acquire(&src->memlock);
  uint64 c = src->chunk ? src->chunk[k] : 0;
  if (c && gc_split(c) && (frame = gc_pages(c)[i]) != 0) {
    uint64 *pte = gstage_pte(src, G_RAM_BASE + off, 0, 0);
    if (pte && (*pte & PTE_V)) {
      *pte &= ~(uint64)PTE_W;
      PA2PAGE(frame)->flags |= PG_KSM;
      page_get(PA2PAGE(frame));
    } else {
      frame = 0;
    }
  }
  release(&src->memlock);
  if (frame == 0)
    return 0;                 // nothing there: the clone has nothing either
  vm_fence_sync(src);

  // and into the clone, which has nothing at this address yet. Its chunk
  // has to be the page-at-a-time kind before one page can be put in it:
  // an empty one of those is a page of zeroes and costs nothing until
  // something is mapped through it.
  uint64 *spare = (uint64 *)kalloc();
  if (spare == 0) {
    page_put(PA2PAGE(frame));
    return -1;
  }
  memset(spare, 0, PGSIZE);
  int r = -1;
  acquire(&dst->memlock);
  if (dst->chunk == 0) {
    release(&dst->memlock);
    kfree(spare);
    page_put(PA2PAGE(frame));
    return -1;
  }
  if (dst->chunk[k] == 0) {
    dst->chunk[k] = (uint64)spare | GC_SPLIT;
    dst->nsplit++;
    spare = 0;
  } else if (!gc_split(dst->chunk[k])) {
    // it was backed by a 2 MB block (the clone's own faults got there
    // first): split it, which is what gstage_split is for
    if (gstage_split(dst, k, spare) == 0)
      spare = 0;
  }
  uint64 dc = dst->chunk[k];
  if (dc && gc_split(dc) && gc_pages(dc)[i] == 0) {
    // gstage_pte(vm, gpa, level, alloc): a page is level 0, and the
    // tables above it have to be made, because the clone has nothing
    // here yet. (Level 1 with no allocation -- which this asked for at
    // first -- is a 2 MB entry that is not there, so every page failed.)
    uint64 *pte = gstage_pte(dst, G_RAM_BASE + off, 0, 1);
    if (pte) {
      gc_pages(dc)[i] = frame;
      *pte = PA2PTE(frame) | (GPTE_LEAF & ~(uint64)PTE_W);
      dst->resident += PGSIZE;
      r = 0;
    }
  } else if (dc && gc_split(dc) && gc_pages(dc)[i] == frame) {
    r = 0;                   // already there: nothing to do
  }
  release(&dst->memlock);
  if (spare)
    kfree(spare);
  if (r < 0)
    page_put(PA2PAGE(frame));
  return r;
}

// ---- a snapshot's write protection (kernel/vmstate.c has the rest) ----
//
// every backed page read-only, and every unbacked one marked as owed to
// nobody: a page the guest had not touched when the snapshot began is not
// part of the machine that was snapshotted, whatever it does with it
// afterwards. memlock is taken a chunk at a time rather than for the
// whole walk, so a fault on another hart waits for one chunk at worst.
void
vm_snap_protect(struct vm *vm, uint32 *done, uint32 *dirty, int npages)
{
  for (int k = 0; k < vm->nchunks; k++) {
    uint64 gpa0 = G_RAM_BASE + (uint64)k * HCHUNK;
    acquire(&vm->memlock);
    int backed = vm->chunk && vm->chunk[k] != 0;
    release(&vm->memlock);
    if (!backed) {
      // nothing to protect, and nothing anybody is owed. A pre-copy has
      // no `done' bitmap: it sends what is dirty and asks the memory
      // itself what is backed, round by round.
      if (done)
        for (int i = 0; i < HCHUNK_PAGES; i++)
          if (k * HCHUNK_PAGES + i < npages)
            vmsnap_set(done, k * HCHUNK_PAGES + i);
      continue;
    }
    int split_ok = 1;
    // a chunk still backed by one 2 MB block has to become pages before
    // any one of them can be made read-only on its own
    vm_split(vm, gpa0);
    acquire(&vm->memlock);
    uint64 c = vm->chunk ? vm->chunk[k] : 0;
    for (int i = 0; i < HCHUNK_PAGES; i++) {
      int idx = k * HCHUNK_PAGES + i;
      if (idx >= npages)
        break;
      uint64 frame = (c && gc_split(c)) ? gc_pages(c)[i] : 0;
      uint64 *pte = frame ? gstage_pte(vm, gpa0 + (uint64)i * PGSIZE, 0, 0) : 0;
      if (pte && (*pte & PTE_V))
        *pte &= ~(uint64)PTE_W;
      else if (done)
        vmsnap_set(done, idx);
      else if (dirty && frame == 0 && c) {
        // a chunk that is still one 2 MB block -- vm_split had no memory
        // for the page table -- stays writable, and a write to it faults
        // on nothing. A pre-copy carries all of it again rather than
        // sending a page the guest may have changed underneath it.
        vmsnap_mark(dirty, idx);
        split_ok = 0;
      }
    }
    release(&vm->memlock);
    if (!split_ok)
      printk("hyp: vm %d: chunk %d would not split; its pages go again\n",
             vm->id, k);
  }
  vm_fence_sync(vm);
}

// and afterwards: a page the guest never wrote to while the snapshot ran
// is still read-only and has to be writable again. One that is shared
// with another machine stays read-only, which is what PG_KSM means.
void
vm_snap_unprotect(struct vm *vm)
{
  for (int k = 0; k < vm->nchunks; k++) {
    uint64 gpa0 = G_RAM_BASE + (uint64)k * HCHUNK;
    acquire(&vm->memlock);
    uint64 c = vm->chunk ? vm->chunk[k] : 0;
    if (c && gc_split(c)) {
      for (int i = 0; i < HCHUNK_PAGES; i++) {
        uint64 frame = gc_pages(c)[i];
        if (frame == 0)
          continue;
        uint64 *pte = gstage_pte(vm, gpa0 + (uint64)i * PGSIZE, 0, 0);
        if (pte && (*pte & PTE_V) && !(PA2PAGE(frame)->flags & PG_KSM))
          *pte |= PTE_W;
      }
    }
    release(&vm->memlock);
  }
  vm_fence_sync(vm);
}

// a store to a page this VM maps read-only. if it is shared, the VM is
// given a copy of its own; if it was only write-protected while it was
// being compared, it becomes writable again. returns 1 when something
// was done, 0 when the page was already writable (another hart got
// there first), -1 when it is not a page this can help with.
// Record that this guest has looked at something about its machine, the first
// time it does so. Called from paths that already trap, so the cost is a test
// and a branch on an exit that already costs microseconds.
//
// The point of keeping the *time* as well as the fact: a guest's compatibility
// set collapses as it learns about its machine, and how fast that happens is
// the thing worth measuring. `obs_t0' is the tick the machine started.
void
vm_observe(struct vm *vm, uint32 what)
{
  if (vm == 0 || (vm->obs & what))
    return;
  acquire(&vm->lock);
  uint32 fresh = what & ~vm->obs;
  if (fresh) {
    vm->obs |= fresh;
    uint64 now = r_time();
    for (int i = 0; i < OBS_COUNT; i++)
      if (fresh & (1u << i))
        vm->obs_first[i] = (vm->obs_t0 && now > vm->obs_t0) ? now - vm->obs_t0 : 0;
  }
  release(&vm->lock);
}

// Ask a guest to look at its machine again.
//
// The description is rebuilt where the guest already has it, the generation is
// bumped, and the guest is poked with a supervisor software interrupt. The poke
// deliberately does not go through the interrupt controller: a revision may be
// *changing* the interrupt controller, and a notification that depends on the
// thing being replaced cannot work. A software interrupt with no IPI reason
// behind it is a channel the guest never sees otherwise.
//
// This returns as soon as the guest has been told. Whether it adopted anything
// is in `revise_adopted' once `revise_acked' catches up with `revise_gen';
// a guest that does not know about revision never acks, and nothing breaks --
// it keeps the description it had, which is still true of the machine it is on.
int
vm_revise(struct vm *vm, uint32 what)
{
  if (vm == 0 || vm->state != VM_RUNNING)
    return -1;

  // rebuild the blob in place, so a guest that re-reads the address it was
  // given at boot sees the new description
  int n = hyp_make_dtb(vm, vm->dtb_gpa, VM_DTB_MAX);
  if (n < 0)
    return -1;

  acquire(&vm->lock);
  vm->revise_gen++;
  vm->revise_adopted = 0;
  vm->revise_asked = r_time();
  vm->revise_done = 0;
  struct vcpu *v = &vm->vcpu[0];
  v->ssip_req = 1;          // delivered as VSSIP on the next entry
  release(&vm->lock);
  vcpu_kick(v);             // and wake it if it is idle
  printk("hyp: vm %d: revision %d asked for (what=0x%x)\n", vm->id,
         vm->revise_gen, what);
  return 0;
}

int
vm_unshare(struct vm *vm, uint64 gpa)
{
  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return -1;
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK, i = (off % HCHUNK) / PGSIZE;
  void *fresh = 0;
  uint64 old = 0;
  int r = 0;

  // a copy may be needed, and allocating is not something to do with
  // the lock held; an unused page costs one kfree
  fresh = kalloc();

  acquire(&vm->memlock);
  uint64 c = vm->chunk[k];
  uint64 *pte = c && gc_split(c) ? gstage_pte(vm, G_RAM_BASE + off, 0, 0) : 0;
  if (pte == 0 || !(*pte & PTE_V)) {
    r = -1;
  } else if (*pte & PTE_W) {
    r = 0;   // already writable
  } else {
    uint64 frame = gc_pages(c)[i];
    struct page *pg = PA2PAGE(frame);
    if ((pg->flags & PG_KSM) && pg->refcount > 1) {
      if (fresh == 0) {
        r = -1;
      } else {
        memmove(fresh, (void *)frame, PGSIZE);
        gc_pages(c)[i] = (uint64)fresh;
        // read-only again while a snapshot is running, for the same reason
        // vm_unprotect_page leaves it alone: the copy this guest is about to
        // write to is a page the snapshot has already sent, and if it is
        // handed back writable the next write to it goes unrecorded. The
        // write that brought us here is charged to the snapshot by
        // vmsnap_before_write; the ones after it need a fault of their own.
        *pte = PA2PTE(fresh) | GPTE_LEAF;
        if (vm->snap.active)
          *pte &= ~(uint64)PTE_W;
        old = frame;
        fresh = 0;
        r = 1;
      }
    } else {
      // nobody else has it: writing again is all it takes -- unless a
      // snapshot is running, in which case this page is write-protected
      // because the snapshot wants to hear about every write to it, not
      // just the first. Leaving PTE_W set here is what made five pages of
      // a 48 MB migration change with nobody marking them.
      pg->flags &= ~(uint16)PG_KSM;
      if (!vm->snap.active)
        *pte |= PTE_W;
      r = 1;
    }
  }
  release(&vm->memlock);

  if (r == 1)
    vm_fence_sync(vm);
  if (old)
    page_put(PA2PAGE(old));
  if (fresh)
    kfree(fresh);
  return r;
}

// where gpa is in host memory, and how many bytes are contiguous from
// there. memlock held, and nothing is backed here: a chunk backed by
// one block answers with the rest of the chunk, a chunk backed a page
// at a time with the rest of the page, and an unbacked one with -1.
static int
gpa2hpa_locked(struct vm *vm, uint64 gpa, uint64 *hpa, uint64 *len)
{
  if (vm->chunk == 0)
    return -1;        // its memory has been freed: nothing is backed
  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return -1;
  uint64 off = gpa - G_RAM_BASE;
  uint64 c = vm->chunk[off / HCHUNK];
  uint64 in = off % HCHUNK;

  if (c == 0)
    return -1;
  if (!gc_split(c)) {
    *hpa = c + in;
    *len = HCHUNK - in;
    return 0;
  }
  uint64 frame = gc_pages(c)[in / PGSIZE];
  if (frame == 0)
    return -1;
  *hpa = frame + (in % PGSIZE);
  *len = PGSIZE - (in % PGSIZE);
  return 0;
}

// the same, backing what is not backed, and answering with a bare
// pointer that is only good while nothing takes the page away.
//
// that is true for the whole of a VM's creation, which is the only
// thing that uses this: the guest is not running, so its balloon
// cannot be inflating. Everything that runs alongside a guest goes
// through vm_copyin and vm_copyout, which hold the lock across the
// copy for exactly this reason.
int
vm_gpa2hpa(struct vm *vm, uint64 gpa, uint64 *hpa, uint64 *len)
{
  int r;

  if (vm_touch(vm, gpa) != 0)
    return -1;
  acquire(&vm->memlock);
  r = gpa2hpa_locked(vm, gpa, hpa, len);
  release(&vm->memlock);
  return r;
}

// one step of a copy: back the page if it is not backed, then take the
// lock and find it. returns with memlock held on success, because the
// address is only meaningful while it is: a balloon inflating on
// another hart could otherwise take the page away and free it between
// finding it and reading it, and a guest that arranged for that would
// have the host reading memory it had given back.
static int
copy_step(struct vm *vm, uint64 gpa, uint64 *hpa, uint64 *len)
{
  for (int tries = 0; tries < 2; tries++) {
    if (vm_touch(vm, gpa) != 0)
      return -1;
    acquire(&vm->memlock);
    if (gpa2hpa_locked(vm, gpa, hpa, len) == 0)
      return 0;
    release(&vm->memlock);
    // it was taken away between backing it and looking: once is a
    // race worth retrying, twice is a guest doing it on purpose
  }
  return -1;
}

int
vm_copyin(struct vm *vm, void *dst, uint64 gpa, uint64 n)
{
  while (n > 0) {
    uint64 hpa, len;
    if (copy_step(vm, gpa, &hpa, &len) < 0)
      return -1;
    if (len > n)
      len = n;
    memmove(dst, (void *)hpa, len);
    release(&vm->memlock);
    dst = (char *)dst + len;
    gpa += len;
    n -= len;
  }
  return 0;
}

int
vm_copyout(struct vm *vm, uint64 gpa, void *src, uint64 n)
{
  // Every host write into guest memory comes through here: a block the
  // disk read, a frame that arrived, the console's echo. None of them
  // faults, so a snapshot or a pre-copy in progress would never hear
  // about them and would carry the page as it was before -- a guest that
  // read a file while it was being migrated would arrive with the old
  // contents of the buffer it read into. Marking the pages dirty is all
  // that is needed and is safe from any context: this is called with
  // netlock held, where writing a page to a file would not be.
  if (vm->snap.active && vm->snap.dirty && n > 0 &&
      gpa >= G_RAM_BASE && gpa < G_RAM_BASE + vm->ram_size) {
    uint64 last = gpa + n - 1;
    if (last >= G_RAM_BASE + vm->ram_size)
      last = G_RAM_BASE + vm->ram_size - 1;
    for (uint64 p = gpa & ~(uint64)(PGSIZE - 1); p <= last; p += PGSIZE) {
      int idx = (int)((p - G_RAM_BASE) / PGSIZE);
      if (idx < vm->snap.npages)
        vmsnap_mark(vm->snap.dirty, idx);
    }
  }
  while (n > 0) {
    uint64 hpa, len;
    if (copy_step(vm, gpa, &hpa, &len) < 0)
      return -1;
    if (len > n)
      len = n;
    memmove((void *)hpa, src, len);
    release(&vm->memlock);
    src = (char *)src + len;
    gpa += len;
    n -= len;
  }
  return 0;
}

// ---- VM lifecycle ----

struct vm *
vm_lookup(int id)
{
  if (id < 0 || id >= NVM)
    return 0;
  return vmtab[id]; // 0 for a slot never used
}

// who may touch a machine: whoever created it, and anybody with
// ---- machines behind a gateway ----

int
cgw_members(struct cgw *g)
{
  int n = 0;

  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm && vm->state != VM_FREE && vm->gw == g)
      n++;
  }
  return n;
}

struct vm *
cgw_member_by_ip(struct cgw *g, uint32 ip)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm && vm->state == VM_RUNNING && vm->gw == g && vm->cfg.ip == ip)
      return vm;
  }
  return 0;
}

// the machine behind this gateway that published this port, and the port
// it means inside. 1 if there is one.
int
cgw_published(struct cgw *g, uint8 proto, uint16 hport, struct vm **out,
              uint16 *gport)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state != VM_RUNNING || vm->gw != g || vm->cfg.ip == 0)
      continue;
    if (vm_port_lookup(vm, proto, hport, gport)) {
      if (out)
        *out = vm;
      return 1;
    }
  }
  return 0;
}

// ---- published ports ----
//
// A machine's ports are a small list rather than a table: eight is more
// than a guest with one address has any use for, and a linear walk of
// eight costs less than anything cleverer would.

int
vm_port_add(struct vm *vm, uint8 proto, uint16 hport, uint16 gport)
{
  if (hport == 0 || (proto != IPPROTO_TCP && proto != IPPROTO_UDP))
    return -EINVAL;
  if (gport == 0)
    gport = hport;
  acquire(&vm->portlock);
  for (int i = 0; i < vm->nports; i++) {
    if (vm->ports[i].proto == proto && vm->ports[i].hport == hport) {
      vm->ports[i].gport = gport;      // the same port, aimed elsewhere
      release(&vm->portlock);
      return 0;
    }
  }
  if (vm->nports >= NPPORT) {
    release(&vm->portlock);
    return -ENOSPC;
  }
  vm->ports[vm->nports].proto = proto;
  vm->ports[vm->nports].hport = hport;
  vm->ports[vm->nports].gport = gport;
  vm->nports++;
  release(&vm->portlock);
  return 0;
}

int
vm_port_rm(struct vm *vm, uint8 proto, uint16 hport)
{
  int found = -ESRCH;

  acquire(&vm->portlock);
  for (int i = 0; i < vm->nports; i++) {
    if (vm->ports[i].proto != proto || vm->ports[i].hport != hport)
      continue;
    vm->ports[i] = vm->ports[vm->nports - 1];
    vm->nports--;
    found = 0;
    break;
  }
  release(&vm->portlock);
  return found;
}

// is this port published, and what port inside does it mean? Called on
// every frame the switch would deliver to the machine, so it takes the
// lock and nothing else.
int
vm_port_lookup(struct vm *vm, uint8 proto, uint16 hport, uint16 *gport)
{
  int found = 0;

  acquire(&vm->portlock);
  for (int i = 0; i < vm->nports; i++) {
    if (vm->ports[i].proto == proto && vm->ports[i].hport == hport) {
      if (gport)
        *gport = vm->ports[i].gport;
      found = 1;
      break;
    }
  }
  release(&vm->portlock);
  return found;
}

// CAP_SYS_ADMIN. Every hypervisor call goes through here, so a user
// who guesses another user's VM id learns only that it is not theirs.
int
vm_access(struct vm *vm)
{
  struct proc *p = myproc();

  if (vm == 0)
    return 0;
  if (vm->owner == p->cred.euid)
    return 1;
  // a descriptor is a capability: whoever holds one -- because it was
  // passed to them over a socket, or inherited -- may work the machine
  // it names, whoever they are.
  if (proc_has_file(p, FD_VM, vm))
    return 1;
  return cap_ok(CAP_SYS_ADMIN);
}

// a machine by name. Names are unique among one user's machines, so
// two people may each have a `web' and neither can reach the other's;
// a search by somebody privileged finds their own first and then the
// rest, which is the only case where the answer depends on the order
// of the table.
struct vm *
vm_by_name(const char *name)
{
  struct vm *any = 0;

  if (name == 0 || name[0] == 0)
    return 0;
  acquire(&vmslock);
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state == VM_FREE)
      continue;
    if (strncmp(vm->cfg.name, name, VM_NAMELEN) != 0)
      continue;
    if (vm->owner == myproc()->cred.euid) {
      release(&vmslock);
      return vm;
    }
    if (any == 0 && cap_ok(CAP_SYS_ADMIN))
      any = vm;
  }
  release(&vmslock);
  return any;
}

// a descriptor names a machine: while one is open the machine is not
// reused, and closing the last one stops an unnamed machine. A named
// machine is a thing in its own right -- `vmm ls' shows it, anyone who
// may reach it can open it again -- so it stays.
struct vm *
vm_fdget(struct vm *vm)
{
  acquire(&vm->lock);
  vm->fdref++;
  release(&vm->lock);
  return vm;
}

// the whole table, for /proc/vmm: every machine, whoever it belongs to.
// procfs checks the reader.
int
vm_list(struct vmentry *out, int max)
{
  int n = 0;

  acquire(&vmslock);
  for (int i = 0; i < NVM && n < max; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state == VM_FREE)
      continue;
    struct vmentry *e = &out[n++];
    e->id = vm->id;
    e->state = vm->state;
    e->kind = vm->vic_kind;
    e->nvcpu = vm->nvcpu;
    e->fdref = vm->fdref;
    e->owner = vm->owner;
    e->mem_mb = vm->ram_size / (1024 * 1024);
    e->resident_mb = vm->resident / (1024 * 1024);
    safestrcpy(e->name, vm->cfg.name, VM_NAMELEN);
  }
  release(&vmslock);
  return n;
}

// a guest can do the same wrong thing as fast as it can run, and a
// host that printed every one would fill its console and spend its
// time formatting. The first few are printed, then one in every
// 4096, and the count is what the message carries.
int
vm_gripe(uint64 *count)
{
  uint64 n = ++(*count);

  return n <= 8 || (n & 4095) == 0;
}

// which machine a descriptor names, for fstat and for /proc
int
vm_fdid(struct vm *vm)
{
  return vm->id;
}

void
vm_fdput(struct vm *vm)
{
  acquire(&vm->lock);
  int last = (--vm->fdref == 0);
  int named = vm->cfg.name[0] != 0;
  int gone = vm->state == VM_FREE;
  if (vm->fdref < 0)
    panic("vm_fdput");
  release(&vm->lock);
  if (last && !named && !gone)
    vm_destroy(vm);
}

static void
vm_free_resources(struct vm *vm)
{
  // the switch may still hold frames for this VM: from now on its NIC
  // drops them instead of writing into memory that is about to be freed.
  // (callers hold vm->lock, as vvnet_rx does.)
  vm->vnet.present = 0;
  vm->vnet.q[0].ready = 0;
  vm->vnet.q[1].ready = 0;
  acquire(&vm->memlock);
  if (vm->chunk) {
    for (int i = 0; i < vm->nchunks; i++) {
      uint64 c = vm->chunk[i];
      if (c == 0)
        continue;
      if (gc_split(c)) {
        // `frames' rather than `pages': PA2PAGE names the page
        // database, and a local of that name would hide it
        uint64 *frames = gc_pages(c);
        for (int j = 0; j < HCHUNK_PAGES; j++) {
          if (frames[j] == 0)
            continue;
          // a frame another guest is also mapping goes back only when
          // the last of them lets go of it
          if (PA2PAGE(frames[j])->flags & PG_KSM)
            page_put(PA2PAGE(frames[j]));
          else
            kfree((void *)frames[j]);
        }
        kfree((void *)frames);
      } else {
        kfree_order((void *)c, HCHUNK_ORDER);
      }
      vm->chunk[i] = 0;
    }
    // the count first: a reader checks it before indexing the table
    // (ksm_scan_chunk does, twice), so the table must not go while a
    // count that admits an index is still there to be read.
    vm->nchunks = 0;
    __sync_synchronize();
    kfree((void *)vm->chunk);
    vm->chunk = 0;
  }
  vm->nchunks = 0;
  vm->resident = 0;
  vm->nsplit = 0;
  // the tables come last: freeing them walks them, and a chunk's
  // memory is named by vm->chunk rather than by what they say
  gstage_free_tables(vm);
  release(&vm->memlock);
  if (vm->blk.bounce)
    kfree(vm->blk.bounce);
  vm->blk.bounce = 0;
  for (int i = 0; i < NVCPU; i++) {
    if (vm->vcpu[i].vstate)
      kfree(vm->vcpu[i].vstate);
    vm->vcpu[i].vstate = 0;
  }
}

// release the disk image file; must not hold spinlocks (may sleep).
static void
vm_put_disk(struct inode *ip)
{
  if (ip) {
    iput(ip);
  }
}

// a block device the guest had is the host's again. what the guest
// wrote went through this kernel's cache, so it is written out first;
// then the cache lets go of the device, because what is on it now is
// the guest's file system and not what the host last read.
static void
vm_put_device(struct gendisk *d)
{
  if (d) {
    blk_sync(d);
    blk_forget(d);
    d->claimed = 0;
  }
}

// is the file, or the device, already the disk of another VM?
static int
disk_in_use(struct vm *self, struct inode *ip, struct gendisk *d)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *o = vmtab[i];
    if (o == 0 || o == self || o->state == VM_FREE)
      continue;
    if (d && o->disk == d)
      return 1;
    if (ip && o->disk_ip && o->disk_ip->dev == ip->dev && o->disk_ip->inum == ip->inum)
      return 1;
  }
  return 0;
}

static void
vcpu_reset(struct vcpu *v, struct vm *vm, int id)
{
  char *vstate = v->vstate;
  memset(v, 0, sizeof(*v));
  v->vstate = vstate;
  if (vstate)
    memset(vstate, 0, PGSIZE);
  v->vm = vm;
  v->id = id;
  v->running_hart = -1;
  v->pc = vm->entry;
  v->regs[10] = id;          // a0 = hartid
  v->regs[11] = vm->dtb_gpa; // a1 = device tree
  v->priv = 1;               // VS mode
  v->in_mmode = 1;           // running the guest's machine-mode boot code
  v->vstimecmp = ~0L;
  v->scounteren = 7; // as firmware would: user access to cycle, time, instret
  v->last_hart = -1;
  // with VMF_HSM only vcpu 0 boots; the others wait for SBI hart_start.
  v->hsm_state = ((vm->cfg.flags & VMF_HSM) && id != 0) ? HSM_STOPPED : HSM_STARTED;
}

// SBI HSM hart_start: give a stopped vcpu a fresh state and let it run.
// caller holds vm->lock.
int
vcpu_hsm_start(struct vcpu *v, uint64 addr, uint64 opaque)
{
  if (v->hsm_state != HSM_STOPPED)
    return -1;
  memset(v->regs, 0, sizeof(v->regs));
  memset(v->fregs, 0, sizeof(v->fregs));
  v->fcsr = 0;
  memset(v->vcsrs, 0, sizeof(v->vcsrs));
  if (v->vstate)
    memset(v->vstate, 0, PGSIZE);
  v->pc = addr;
  v->regs[10] = v->id;
  v->regs[11] = opaque;
  v->priv = 1;
  v->in_mmode = 0;
  v->vsstatus = 0;
  v->vsie = 0;
  v->vstvec = 0;
  v->vsscratch = 0;
  v->vsepc = 0;
  v->vscause = 0;
  v->vstval = 0;
  v->vsatp = 0;
  v->vstimecmp = ~0L;
  v->vssip = 0;
  v->ssip_req = 0;
  v->scounteren = 7;
  v->senvcfg = 0;
  v->hsm_state = HSM_STARTED;
  wakeup(v);
  return 0;
}

// SBI RFENCE: make v execute a fence before it next runs guest code.
// caller holds vm->lock.
void
vcpu_request_fence(struct vcpu *v, int what)
{
  __atomic_fetch_or(&v->fence_req, what, __ATOMIC_SEQ_CST);
  vcpu_kick(v);
}

// the VM is at its memory limit and a vcpu is waiting to go further.
// ask the guest, through its balloon, to give back the difference
// between what it has been given and what it may hold, and wait a
// tick for it to answer. returns 0 if it is worth trying again.
//
// the order is the only one that is fair: the guest is asked first,
// because it knows which of its pages matter and the host does not;
// only when it will not or cannot answer is the VM stopped.
int
vm_reclaim(struct vm *vm, int tries)
{
  if (vm->mem_limit == 0 || tries > 100)
    return -1;
  vm->stalls++;
  if (vm->balloon.present && vm->ram_size > vm->mem_limit)
    vballoon_set(vm, vm->ram_size - vm->mem_limit);
  acquire(&tickslock);
  sleep_prepare(&ticks);
  release(&tickslock);
  sleep();
  return 0;
}

// the G-stage mapping of this VM has changed, so every vcpu of it must
// throw away what it has cached about guest-physical addresses. a vcpu
// inside the guest is kicked out to do it; one that is not running
// does it when it next goes in.
//
// the fence is asked for rather than executed here because a G-stage
// TLB entry belongs to the hart that walked it, and this hart is not
// necessarily one of them.
void
vm_fence(struct vm *vm)
{
  acquire(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_request_fence(&vm->vcpu[i], FENCE_GVMA);
  release(&vm->lock);
}

// the same, and then wait until no vcpu can still be using what the
// mapping used to say.
//
// asking is not enough when the page is about to be given back to the
// host: a vcpu inside the guest still has the old translation in its
// TLB and would go on writing to a page that by then belonged to
// somebody else. So this waits until every vcpu has either taken the
// fence -- which it does on its way in, before any guest instruction
// -- or left the guest, which it must do to take one.
//
// it terminates because a vcpu cannot stay in the guest: the kick is
// an interrupt, and failing that the timer is.
void
vm_fence_sync(struct vm *vm)
{
  vm_fence(vm);
  for (int round = 0; round < 1000; round++) {
    int busy = 0;
    acquire(&vm->lock);
    for (int i = 0; i < vm->nvcpu; i++) {
      struct vcpu *v = &vm->vcpu[i];
      if (v->running_hart >= 0 && v->running_hart != cpuid() &&
          (__atomic_load_n(&v->fence_req, __ATOMIC_SEQ_CST) & FENCE_GVMA))
        busy = 1;
    }
    release(&vm->lock);
    if (!busy)
      return;
    if (round < 64)
      continue;   // the IPI is on its way; spinning is cheaper than sleeping
    acquire(&tickslock);
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
  }
  printk("hyp: vm %d: a vcpu will not take a G-stage fence\n", vm->id);
}

static int vm_load(struct vm *vm);

// create a VM from cfg: memory, devices, the disk image file, and the
// kernel loaded into guest RAM. returns the id, or -1.
// a slot being filled in: claimed, but not yet VM_NEW, because
// vm_create zeroes the whole structure -- state included -- after it
// has taken the slot. Without this a second vm_create could take the
// same slot during that window.
static char vmclaim[NVM];

static int vm_create_flags(struct vmconfig *cfg, int noload);

static int
vm_create_flags(struct vmconfig *cfg, int noload)
{
  if (!plat.has_h)
    return -1;
  int nvcpu = cfg->nvcpu;
  uint64 mem_bytes = (uint64)cfg->mem_mb * 1024 * 1024;
  if (nvcpu < 1 || nvcpu > NVCPU)
    return -1;
  if (mem_bytes < 8 * HCHUNK || (mem_bytes % HCHUNK) != 0 || mem_bytes > VM_MAXMEM)
    return -1;
  if (cfg->kernel[0] == 0 && !noload)
    return -1;

  // a slot: a VM object is allocated the first time its slot is used and
  // kept for reuse (struct vm is large: vcpus, interrupt controller
  // state, console rings).
  uint uid = myproc()->cred.euid;

  acquire(&vmslock);
  // a name is how a machine is found again (vm_open), so one user may
  // not have two of the same name at once.
  if (cfg->name[0]) {
    for (int i = 0; i < NVM; i++) {
      struct vm *o = vmtab[i];
      if (o && o->state != VM_FREE && o->owner == uid &&
          strncmp(o->cfg.name, cfg->name, VM_NAMELEN) == 0) {
        release(&vmslock);
        return -1;
      }
    }
  }
  struct vm *vm = 0;
  for (int i = 0; i < NVM; i++) {
    if (vmtab[i] == 0) {
      int order = 0;
      while (((uint64)PGSIZE << order) < sizeof(struct vm))
        order++;
      struct vm *nv = kalloc_order(order);
      if (nv == 0)
        break;
      memset(nv, 0, sizeof(*nv));
      initlock(&nv->lock, "vm");
      initlock(&nv->memlock, "vmmem");
      initsleeplock(&nv->snap.lock, "vmsnap");
      nv->id = i;
      nv->state = VM_FREE;
      vmtab[i] = nv;
    }
    // a free slot is reusable only once no descriptor names it: a
    // machine that has exited still answers through a descriptor
    // somebody holds, and must not turn into a different machine
    // underneath them.
    if (vmtab[i]->state == VM_FREE && vmtab[i]->fdref == 0 && !vmclaim[i]) {
      vm = vmtab[i];
      vmclaim[i] = 1;
      vm->state = VM_NEW;
      break;
    }
  }
  release(&vmslock);
  if (vm == 0)
    return -1;

  int id = vm->id;
  struct spinlock lk = vm->lock, mlk = vm->memlock;
  memset(vm, 0, sizeof(*vm));
  vm->lock = lk;
  vm->memlock = mlk;
  vm->id = id;
  vm->state = VM_NEW;
  vm->owner = uid;
  vm->cfg = *cfg;
  // the observation clock starts here, after the memset above and on the one
  // path both a fresh slot and a reused one go through
  vm->obs = 0;
  vm->obs_t0 = r_time();
  initlock(&vm->portlock, "vmports");
  // a machine whose gateway is a gateway object is behind it, on that
  // object's private subnet rather than on the switch's own segment.
  // That is the whole of how a machine gets there: the address it was
  // given says where it is.
  vm->gw = 0;
  if (cfg->net == 2 && cfg->ip) {
    struct cgw *g = cfg->gwname[0] ? cgw_find(cfg->gwname)
                                   : (cfg->gw ? cgw_by_addr(cfg->gw) : 0);
    // the address it was given has to be on that gateway's subnet, and
    // the gateway has to be its default route: a machine behind a
    // gateway reaches everything else through it.
    if (g && cfg->gw == g->addr && (cfg->ip & g->mask) == (g->addr & g->mask))
      vm->gw = g;

  }
  vm->nports = cfg->nports > NPPORT ? NPPORT : cfg->nports;
  for (int i = 0; i < vm->nports; i++)
    vm->ports[i] = cfg->ports[i];
  acquire(&vmslock);
  vmclaim[id] = 0;   // the slot says what it is again
  release(&vmslock);
  vm->nvcpu = nvcpu;
  vm->ram_size = mem_bytes;
  vm->nchunks = mem_bytes / HCHUNK;
  vm->resident = 0;
  vm->prefer_huge = (cfg->flags & VMF_NOHUGE) == 0;
  vm->mem_limit = (uint64)cfg->mem_limit_mb * 1024 * 1024;
  if (vm->mem_limit > mem_bytes)
    vm->mem_limit = 0;   // a limit above the memory it has is no limit

  // its share of the host's processors. All of its vcpus are in one
  // scheduler group, so `-w 2048' is twice a nice-0 task's share of
  // the machine whether the guest has one vcpu or eight, and `-w' on
  // two guests divides the machine between the guests rather than
  // between their vcpus.
  vm->sg = sched_group_create(cfg->name[0] ? cfg->name : "vm", cfg->weight);
  if (vm->sg == 0)
    goto fail;
  if (cfg->cpu_max_us > 0)
    sched_group_set_max(vm->sg, cfg->cpu_max_us, cfg->cpu_period_us);

  // the chunk table: one host address per 2 MB of guest RAM, filled in as
  // the guest touches its memory (vm_touch).
  {
    uint64 bytes = (uint64)vm->nchunks * sizeof(uint64);
    int order = 0;
    while (((uint64)PGSIZE << order) < bytes)
      order++;
    vm->chunk = kalloc_order(order);
    if (vm->chunk == 0)
      goto fail;
    memset(vm->chunk, 0, (uint64)PGSIZE << order);
  }

  // the G-stage root: 16 KB, naturally aligned, which is what an
  // x4 root has to be. everything below it is an ordinary page.
  vm->hgatp_root = kalloc_order(2);
  if (vm->hgatp_root == 0)
    goto fail;
  memset(vm->hgatp_root, 0, 4 * PGSIZE);
  vm->hgatp = (plat.mmu_levels == 4 ? HGATP_MODE_SV48X4 : HGATP_MODE_SV39X4) |
              ((uint64)(id + 1) << HGATP_VMID_SHIFT) |
              ((uint64)vm->hgatp_root >> 12);

  // the guest device tree sits in the last 64 KB of guest RAM.
  vm->dtb_gpa = G_RAM_BASE + vm->ram_size - 0x10000;

  // the disk: a regular file in the host file system, or a whole block
  // device -- a disk, a partition, a loop device -- handed to the guest
  // as it is. a device is faster (no host file system in the way) and
  // is what a guest that owns its storage wants; while a guest has one
  // the host will not mount it and no other guest may take it.
  if (cfg->disk[0]) {
    struct inode *ip = namei(cfg->disk);
    if (ip == 0) {
      goto fail;
    }
    ilock(ip);
    int isdev = ip->type == T_DEVICE && ip->major == DEV_BLOCK;
    int minor = ip->minor;
    int ok = isdev || (ip->type == T_FILE && ip->size >= 512);
    iunlock(ip);
    if (!ok) {
      iput(ip);
      goto fail;
    }
    if (isdev) {
      struct gendisk *d = blk_by_index(minor);
      if (d == 0 || d->nsectors == 0 || d->claimed || disk_in_use(vm, 0, d)) {
        iput(ip);
        goto fail;
      }
      d->claimed = 1;
      vm->disk = d;
      iput(ip); // the device node was only how the disk was named
    } else {
      if (disk_in_use(vm, ip, 0)) {
        iput(ip);
        goto fail;
      }
      vm->disk_ip = ip;
    }
  }

  vm->vic_kind = plat.intc_kind;
  vm->imsic_hw = (plat.intc_kind == INTC_IMSIC && vimsic_geilen() > 0 && !(cfg->flags & VMF_SOFTIMSIC));

  vm->ndev = 0;
  vuart_init(vm);
  vclint_init(vm);
  vic_init(vm);
  vvirtio_init(vm);
  vvnet_init(vm);
  vvcon_init(vm);
  vballoon_init(vm);
  vrtc_init(vm);
  if (vm->blk.bounce == 0)
    goto fail;

  if (!noload && vm_load(vm) < 0)
    goto fail;

  for (int i = 0; i < nvcpu; i++) {
    if (plat.has_v && (vm->vcpu[i].vstate = kalloc()) == 0)
      goto fail;
    vcpu_reset(&vm->vcpu[i], vm, i);
  }

  audit("vm_create", cfg->name, (uint64)id, (uint64)cfg->mem_mb, 1);
  return id;

fail:
  vvirtio_stop(vm);
  vm_free_resources(vm);
  {
    struct inode *ip = vm->disk_ip;
    struct gendisk *d = vm->disk;
    vm->disk_ip = 0;
    vm->disk = 0;
    vm_put_disk(ip);
    vm_put_device(d);
  }
  vm->state = VM_FREE;
  return -1;
}

// copy n bytes of file ip at offset foff into guest RAM at gpa.
static int
load_range(struct vm *vm, struct inode *ip, uint64 gpa, uint64 foff, uint64 n)
{
  while (n > 0) {
    uint64 hpa, len;
    if (vm_gpa2hpa(vm, gpa, &hpa, &len) < 0)
      return -1;
    if (len > n)
      len = n;
    if (readi(ip, 0, hpa, foff, len) != len)
      return -1;
    gpa += len;
    foff += len;
    n -= len;
  }
  return 0;
}

static int
zero_range(struct vm *vm, uint64 gpa, uint64 n)
{
  while (n > 0) {
    uint64 hpa, len;
    if (vm_gpa2hpa(vm, gpa, &hpa, &len) < 0)
      return -1;
    if (len > n)
      len = n;
    memset((void *)hpa, 0, len);
    gpa += len;
    n -= len;
  }
  return 0;
}

// does [gpa, gpa+n) overlap the device tree area?
static int
hits_dtb(struct vm *vm, uint64 gpa, uint64 n)
{
  uint64 dtb_end = G_RAM_BASE + vm->ram_size;
  return gpa < dtb_end && gpa + n > vm->dtb_gpa;
}

// load the guest kernel from the host file system into guest RAM: an
// ELF whose segments and entry lie anywhere in guest RAM, or (VMF_RAW)
// a flat binary placed at cfg.load_addr, which is also the entry point.
// then generate the guest's device tree.
static int
vm_load(struct vm *vm)
{
  struct vmconfig *cfg = &vm->cfg;
  struct inode *ip;
  struct elfhdr elf;
  struct proghdr ph;
  int ret = -1;

  if ((ip = namei(cfg->kernel)) == 0) {
    return -1;
  }
  ilock(ip);
  if (ip->type != T_FILE)
    goto done;

  if (cfg->flags & VMF_RAW) {
    uint64 gpa = cfg->load_addr, n = ip->size;
    if (gpa < G_RAM_BASE || gpa + n > G_RAM_BASE + vm->ram_size || hits_dtb(vm, gpa, n))
      goto done;
    if (load_range(vm, ip, gpa, 0, n) < 0)
      goto done;
    vm->entry = gpa;
    ret = 0;
    goto done;
  }

  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto done;
  if (elf.magic != ELF_MAGIC)
    goto done;
  if (elf.entry < G_RAM_BASE || elf.entry >= G_RAM_BASE + vm->ram_size)
    goto done;

  for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto done;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto done;
    uint64 gpa = ph.paddr ? ph.paddr : ph.vaddr;
    if (gpa + ph.memsz < gpa || gpa < G_RAM_BASE ||
        gpa + ph.memsz > G_RAM_BASE + vm->ram_size || hits_dtb(vm, gpa, ph.memsz))
      goto done;
    if (load_range(vm, ip, gpa, ph.off, ph.filesz) < 0)
      goto done;
    if (zero_range(vm, gpa + ph.filesz, ph.memsz - ph.filesz) < 0)
      goto done;
  }
  vm->entry = elf.entry;
  ret = 0;

done:
  iunlockput(ip);
  if (ret < 0)
    return -1;

  // device tree, in the 64 KB reserved at the top of guest RAM.
  if (hyp_make_dtb(vm, vm->dtb_gpa, VM_DTB_MAX) <= 0)
    ret = -1;
  return ret;
}

// ask every vcpu to leave the guest and stop.
void
vm_halt(struct vm *vm)
{
  acquire(&vm->lock);
  if (vm->state == VM_RUNNING || vm->state == VM_NEW || vm->state == VM_PAUSED)
    vm->state = VM_HALTING;
  wakeup((void *)&vm->state);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);
  release(&vm->lock);
}

int
vm_pause(struct vm *vm)
{
  acquire(&vm->lock);
  if (vm->state != VM_RUNNING) {
    release(&vm->lock);
    return -1;
  }
  vm->state = VM_PAUSED;
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);
  struct sched_group *sg = vm->sg;
  release(&vm->lock);
  // a paused machine costs nothing: its vcpus sleep on vm->state, and
  // its group is frozen so that a vcpu on its way to that sleep is
  // not scheduled in the meantime.
  sched_group_freeze(sg, 1);
  return 0;
}

int
vm_resume(struct vm *vm)
{
  acquire(&vm->lock);
  if (vm->state != VM_PAUSED) {
    release(&vm->lock);
    return -1;
  }
  vm->state = VM_RUNNING;
  struct sched_group *sg = vm->sg;
  release(&vm->lock);
  sched_group_freeze(sg, 0);
  acquire(&vm->lock);
  wakeup((void *)&vm->state);
  release(&vm->lock);
  return 0;
}

int
vm_destroy(struct vm *vm)
{
  // before anything: the disk thread reads and writes guest memory,
  // and this may be about to free it
  vvirtio_stop(vm);
  acquire(&vm->lock);
  if (vm->state == VM_FREE) {
    release(&vm->lock);
    return -1;
  }
  audit("vm_destroy", vm->cfg.name, (uint64)vm->id, vm->ram_size / (1024 * 1024), 1);
  if (vm->vcpus_running == 0) {
    // nobody is inside vm_run: free right here.
    vm_free_resources(vm);
    struct inode *ip = vm->disk_ip;
    struct gendisk *d = vm->disk;
    vm->disk_ip = 0;
    vm->disk = 0;
    vm->state = VM_FREE;
    struct sched_group *sg = vm->sg;
    vm->sg = 0;
    wakeup(&vm->cons_out);
    release(&vm->lock);
    sched_group_destroy(sg);
    vm_put_disk(ip);
    vm_put_device(d);
    return 0;
  }
  vm->state = VM_HALTING;
  // a frozen machine cannot notice it has been told to stop
  struct sched_group *sg = vm->sg;
  wakeup((void *)&vm->state);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);
  release(&vm->lock);
  sched_group_freeze(sg, 0);   // so its vcpus can run to their exit
  return 0;
}

int
vm_create(struct vmconfig *cfg)
{
  return vm_create_flags(cfg, 0);
}

int
vm_create_blank(struct vmconfig *cfg)
{
  return vm_create_flags(cfg, 1);
}

// the last thing a restore does. Every vcpu is about to enter on a hart
// it has never run on, with a G-stage that was built page by page
// underneath it while it did not exist, so the first entry flushes
// everything; and no host process is inside vm_run yet.
void
vmstate_restored(struct vm *vm)
{
  acquire(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++) {
    struct vcpu *v = &vm->vcpu[i];
    v->last_hart = -1;
    v->running_hart = -1;
    v->p = 0;
    v->idle = 0;
    v->fence_req |= FENCE_I | FENCE_VMA | FENCE_GVMA;
  }
  release(&vm->lock);
}

// make a vcpu notice a change: wake it if it is idle in wfi, or make
// it exit if it is running the guest on another hart. caller holds
// vm->lock.
void
vcpu_kick(struct vcpu *v)
{
  if (v->idle)
    wakeup(v);
  int h = v->running_hart;
  if (h >= 0 && h != cpuid())
    intc_ipi(h, IPI_KICK);
}

// is a virtual supervisor external interrupt pending for v?
// caller holds vm->lock.
int
vm_seip(struct vcpu *v)
{
  return vic_seip(v);
}

// deliver a synchronous exception to the guest, as hardware would when
// the exception is delegated to VS mode.
void
vcpu_inject(struct vcpu *v, uint64 cause, uint64 tval)
{
  uint64 s = v->vsstatus;
  s &= ~SSTATUS_SPIE;
  if (s & SSTATUS_SIE)
    s |= SSTATUS_SPIE;
  s &= ~SSTATUS_SIE;
  s &= ~SSTATUS_SPP;
  if (v->priv)
    s |= SSTATUS_SPP;
  v->vsstatus = s;
  v->vsepc = v->pc;
  v->vscause = cause;
  v->vstval = tval;
  v->pc = v->vstvec & ~3L;
  v->priv = 1;
  v->injected++;
  // a guest without a trap vector cannot handle this: say so once or twice.
  if (cause == EXC_ILLEGAL && v->vstvec == 0 && v->injected <= 3)
    printk("hyp: vm %d vcpu %d: illegal instruction 0x%lx at guest pc 0x%lx, guest has no trap vector\n",
           v->vm->id, v->id, tval, v->vsepc);
}

// ---- guest entry / exit ----

static uint64 hart_hgatp[NCPU];
// write a VS CSR only if what the hardware has is not already it
#define VS_LOAD(csr, field)                                                    \
  do {                                                                         \
    if (!same || v->hw_##field != v->field) {                                   \
      csr_write(csr, v->field);                                                 \
      v->hw_##field = v->field;                                                 \
    }                                                                          \
  } while (0)

static void
vcpu_enter(struct vcpu *v)
{
  struct vm *vm = v->vm;
  uint64 hvip = 0;

  acquire(&vm->lock);
  v->running_hart = cpuid();
  if (v->ssip_req) {
    v->ssip_req = 0;
    v->vssip = 1;
  }
  if (v->vssip)
    hvip |= HVIP_VSSIP;
  hvip |= vic_vcpu_enter(v);
  release(&vm->lock);

  int hart = cpuid();
  // Writing the thirteen VS CSRs on every entry and reading them back on
  // every exit is a measurable part of what an exit costs on an emulated
  // machine: vmbench says 15.5 us for an emulated load, and skipping the
  // writes that were not needed took about 700 ns off it -- five per cent.
  //
  // It is not done, and the reason is worth writing down. The obvious way
  // is a shadow of what was last put in the hardware, written only when
  // software has changed the value since the exit read it. Built that way,
  // an xv6 guest died at its first page fault, and a check after the
  // conditional writes said why: the hardware held an *older* vstimecmp,
  // vsstatus, vsepc and vscause than the shadow claimed, with the shadow
  // and the structure agreeing -- so something changes these registers
  // between an exit and the next entry without going through either. Until
  // that something is found, the entry writes all of them: five per cent
  // is not worth a guest that dies for a reason nobody has explained.
  csr_write(CSR_VSSTATUS, v->vsstatus);
  csr_write(CSR_VSIE, v->vsie);
  csr_write(CSR_VSTVEC, v->vstvec);
  csr_write(CSR_VSSCRATCH, v->vsscratch);
  csr_write(CSR_VSEPC, v->vsepc);
  csr_write(CSR_VSCAUSE, v->vscause);
  csr_write(CSR_VSTVAL, v->vstval);
  csr_write(CSR_VSATP, v->vsatp);
  csr_write(CSR_VSTIMECMP, v->vstimecmp);
  if (plat.intc_kind == INTC_IMSIC)
    csr_write(CSR_VSISELECT, v->vsiselect);
  csr_write(CSR_HVIP, hvip);

  if (hart_hgatp[hart] != vm->hgatp) {
    csr_write(CSR_HGATP, vm->hgatp);
    asm volatile("hfence.gvma zero, zero" ::: "memory");
    hart_hgatp[hart] = vm->hgatp;
  }
  // VS-stage TLB entries are per hart: a vcpu resuming on another hart,
  // or one asked for a remote fence (SBI RFENCE), flushes them here.
  int fences = __atomic_exchange_n(&v->fence_req, 0, __ATOMIC_SEQ_CST);
  if (v->last_hart != hart || (fences & FENCE_VMA))
    asm volatile("hfence.vvma zero, zero" ::: "memory");
  if (fences & FENCE_GVMA)
    asm volatile("hfence.gvma zero, zero" ::: "memory");
  if (fences & FENCE_I)
    asm volatile("fence.i" ::: "memory");
  v->last_hart = hart;

  uint64 hs = HSTATUS_SPV | HSTATUS_VTW;
  if (v->priv)
    hs |= HSTATUS_SPVP;
  hs |= ((uint64)v->vgein << HSTATUS_VGEIN_SHIFT) & HSTATUS_VGEIN_MASK;
  csr_write(CSR_HSTATUS, hs);

  csr_write(0x106, v->scounteren); // scounteren
  csr_write(0x10a, v->senvcfg);    // senvcfg

  // load the guest's floating-point registers; the FPU must be on at
  // the HS level for the guest to use it at all. Clean afterwards, so
  // that a guest that does not touch the FPU costs nothing on exit.
  uint64 ss = r_sstatus();
  ss = (ss & ~SSTATUS_FS_MASK) | SSTATUS_FS_INITIAL;
  w_sstatus(ss);
  fp_restore(v->fregs);
  w_fcsr(v->fcsr);
  ss = r_sstatus();
  ss = (ss & ~SSTATUS_FS_MASK) | SSTATUS_FS_CLEAN;
  if (v->vstate) {
    w_sstatus((ss & ~SSTATUS_VS_MASK) | SSTATUS_VS_INITIAL);
    vec_restore(v->vstate, v->vcsrs);
    ss = (r_sstatus() & ~SSTATUS_VS_MASK) | SSTATUS_VS_CLEAN;
  }
  ss &= ~(SSTATUS_SPP | SSTATUS_SPIE);
  if (v->priv)
    ss |= SSTATUS_SPP;
  w_sstatus(ss);
  w_sepc(v->pc);
  w_stvec((uint64)guestvec);

  // flush the icache in case the guest image was just written.
  asm volatile("fence.i" ::: "memory");

  guestret(v);

  // back from guestvec with interrupts off. the trap set hstatus.SPV;
  // clear it, or the next sret on this hart (to an ordinary user
  // process) would enter virtualized mode.
  w_stvec((uint64)kernelvec);
  v->priv = (r_sstatus() & SSTATUS_SPP) ? 1 : 0;
  // keep SPVP so that hlvx (instruction fetch for emulation) is
  // checked with the guest's privilege.
  csr_write(CSR_HSTATUS, v->priv ? HSTATUS_SPVP : 0);
  v->pc = r_sepc();
  v->priv = (r_sstatus() & SSTATUS_SPP) ? 1 : 0;
  v->vsstatus = csr_read(CSR_VSSTATUS);
  v->vsie = csr_read(CSR_VSIE);
  v->vstvec = csr_read(CSR_VSTVEC);
  v->vsscratch = csr_read(CSR_VSSCRATCH);
  v->vsepc = csr_read(CSR_VSEPC);
  v->vscause = csr_read(CSR_VSCAUSE);
  v->vstval = csr_read(CSR_VSTVAL);
  v->vsatp = csr_read(CSR_VSATP);
  v->vstimecmp = csr_read(CSR_VSTIMECMP);
  v->vssip = (csr_read(CSR_HVIP) & HVIP_VSSIP) ? 1 : 0;
  if (plat.intc_kind == INTC_IMSIC)
    v->vsiselect = csr_read(CSR_VSISELECT);
  v->scounteren = csr_read(0x106);
  csr_write(0x106, 7); // back to the host's setting
  v->senvcfg = csr_read(0x10a);
  // the guest touched the FPU (hardware marks HS sstatus.FS Dirty too).
  // FP/vector state is saved if either the HS view or the guest's own
  // vsstatus says Dirty: hardware marks both for instructions, but qemu
  // marks only vsstatus on fcsr/vcsr writes.
  uint64 hs_st = r_sstatus();
  if ((hs_st & SSTATUS_FS_MASK) == SSTATUS_FS_DIRTY ||
      (v->vsstatus & SSTATUS_FS_MASK) == SSTATUS_FS_DIRTY) {
    v->fcsr = r_fcsr();
    fp_save(v->fregs);
  }
  if (v->vstate && ((hs_st & SSTATUS_VS_MASK) == SSTATUS_VS_DIRTY ||
                    (v->vsstatus & SSTATUS_VS_MASK) == SSTATUS_VS_DIRTY))
    vec_save(v->vstate, v->vcsrs);
  v->exits++;

  acquire(&vm->lock);
  v->running_hart = -1;
  vic_vcpu_exit(v);
  release(&vm->lock);

  // the exit-rate throttle.
  //
  // a guest that traps to the host thousands of times a second --
  // reading an emulated device register in a loop, say -- makes the
  // host do all the work while the guest's own processor time looks
  // small, because most of it is spent in the host. It is charged for
  // it here: past a rate no ordinary guest reaches, the vcpu gives up
  // the processor on its way back in, which puts the cost where the
  // exits are and stops one machine from spending the host on
  // nothing. Ordinary guests never reach it -- an xv6 guest booting
  // takes a few thousand exits in all.
  uint64 now = r_time();
  uint64 window = plat.timebase ? plat.timebase / 10 : 1000000;  // 100 ms
  if (now - v->exit_window_start >= window) {
    v->exit_window_start = now;
    v->exit_window_count = 0;
  }
  if (++v->exit_window_count > EXIT_RATE_LIMIT) {
    v->exit_throttles++;
    v->exit_window_count = 0;
    v->exit_window_start = now;
    yield();
  }
}

// the guest executed wfi: sleep until something is pending for it.
//
// before sleeping, a nudge for the siblings. A guest with more than
// one vcpu spins on its own locks, and a vcpu that spins waiting for
// a lock held by a sibling the host has not run yet wastes the whole
// slice. wfi is where a guest says it has nothing to do; giving the
// rest of the slice to a sibling of the same machine costs nothing
// here and is worth a whole slice there. The two are in the same
// scheduler group, so `giving' means no more than going to the back
// of it -- which is what sleeping does anyway, and the count is what
// says how often it mattered.
static void
vcpu_idle(struct vcpu *v)
{
  struct vm *vm = v->vm;

  if (vm->nvcpu > 1) {
    for (int i = 0; i < vm->nvcpu; i++) {
      struct vcpu *s = &vm->vcpu[i];
      if (s != v && s->p && !s->idle && s->hsm_state == HSM_STARTED) {
        v->nudges++;
        break;
      }
    }
  }
  acquire(&vm->lock);
  while (!(v->vssip || v->ssip_req || vic_seip(v) || r_time() >= v->vstimecmp ||
           vm->state != VM_RUNNING || killed(v->p))) {
    v->idle = 1;
    v->wfi_sleeps++;
    sleep_prepare(v);
    release(&vm->lock);
    // and the host's timer is set for this guest's own deadline before
    // the sleep, not at the next tick. timer_rearm() takes the first of
    // the tick, the slice and every idle guest's virtual timer
    // (hyp_next_deadline) -- but nothing called it when a vcpu went to
    // sleep, so a guest asking to be woken in 100 microseconds waited
    // for the next host tick instead: 3.2 milliseconds, measured.
    timer_rearm();
    sleep();
    acquire(&vm->lock);
    v->idle = 0;
  }
  release(&vm->lock);
}

// the timer tick on hart 0 calls this: wake idle vcpus whose virtual
// timer has expired (their vstimecmp is not loaded in hardware while
// they sleep).
void
hyp_tick(void)
{
  if (!plat.has_h)
    return;
  uint64 now = r_time();
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state != VM_RUNNING)
      continue;
    for (int j = 0; j < vm->nvcpu; j++) {
      struct vcpu *v = &vm->vcpu[j];
      if (v->idle && now >= v->vstimecmp)
        wakeup(v);
    }
  }
}

// the earliest virtual timer deadline among idle vcpus, or dflt.
uint64
hyp_next_deadline(uint64 dflt)
{
  if (!plat.has_h)
    return dflt;
  uint64 best = dflt;
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state != VM_RUNNING)
      continue;
    for (int j = 0; j < vm->nvcpu; j++) {
      struct vcpu *v = &vm->vcpu[j];
      if (v->idle && v->hsm_state == HSM_STARTED && v->vstimecmp < best)
        best = v->vstimecmp;
    }
  }
  return best;
}

void
hyp_sgei(void)
{
  vimsic_sgei();
}

// which bucket an exit falls in. The interrupt bit is the top one; the
// rest are exception causes.
static int
exit_kind(uint64 scause)
{
  if ((long)scause < 0) {
    uint64 code = scause & 0xff;
    if (code == 5)
      return XK_TIMER;          // supervisor timer
    if (code == 9 || code == 10)
      return XK_EXTERNAL;       // supervisor or guest external
    return XK_OTHER;
  }
  switch (scause) {
  case EXC_LOAD_GPF:  return XK_MMIO_LOAD;
  case EXC_STORE_GPF: return XK_MMIO_STORE;
  case EXC_INST_GPF:  return XK_INST_FAULT;
  case EXC_VIRT_INST: return XK_CSR;
  case EXC_ILLEGAL:   return XK_ILLEGAL;
  case EXC_ECALL_VS:  return XK_ECALL;
  default:            return XK_OTHER;
  }
}

static int
handle_exit(struct vcpu *v)
{
  uint64 scause = r_scause();
  uint64 stval = r_stval();
  uint64 htval = csr_read(CSR_HTVAL);
  v->last_cause = scause;
  v->exit_hist[exit_kind(scause)]++;
  v->last_gpa = (htval << 2) | (stval & 3);

  if (scause >> 63) {
    // host interrupt while the guest was running.
    int which = devintr();
    if (which == 0) {
      printk("hyp: unexpected interrupt scause=0x%lx\n", scause);
      return -1;
    }
    if (which == 2)
      yield();
    return 0;
  }

  // the faulting instruction must be fetched now, on this hart, while
  // the guest's vsatp/hgatp are still loaded and before an interrupt
  // could move this process to another hart.
  uint32 inst = 0;
  switch (scause) {
  case EXC_ILLEGAL:
  case EXC_VIRT_INST:
    inst = stval ? (uint32)stval : hyp_fetch(v, v->pc);
    break;
  case EXC_LOAD_GPF:
  case EXC_STORE_GPF:
  case EXC_INST_GPF: {
    // guest RAM is backed on first touch; a fault inside RAM just needs
    // the chunk (before intr_on: the fetch below must stay on this hart).
    uint64 gpa = (htval << 2) | (stval & 3);
    int t = vm_touch(v->vm, gpa);
    if (t == -2) {
      // at its limit. asking the guest for memory back means waiting
      // for it, and waiting means letting interrupts in -- so this is
      // done here and not with the rest, which runs with them off so
      // that the fetch below stays on this hart. Nothing after this
      // fetches: either the page is there and we go back to the run
      // loop, which re-enters the guest with the CSRs set up for
      // whichever hart we are on by then, or the VM is stopped.
      intr_on();
      for (int tries = 0; t == -2 && vm_reclaim(v->vm, tries) == 0; tries++)
        t = vm_touch(v->vm, gpa);
      if (t == 0)
        return 0;
      printk("hyp: vm %d: at its %ld MB limit and it will not come down; halting the vm\n",
             v->vm->id, (long)(v->vm->mem_limit >> 20));
      vm_halt(v->vm);
      return -1;
    }
    if (t > 0 && v->vm->snap.active && v->vm->snap.precopy) {
      // a page backed while a pre-copy is running is a page the round
      // that is sweeping may already have passed, and it is mapped
      // writable from the start, so nothing will fault on it again:
      // this is the only chance to say it has to go over.
      int idx = (int)((gpa - G_RAM_BASE) / PGSIZE);
      if (gpa >= G_RAM_BASE && idx < v->vm->snap.npages && v->vm->snap.dirty)
        vmsnap_mark(v->vm->snap.dirty, idx);
    }
    if (t == 0) {
      // the page is there. a store that still faulted is a store to a
      // page mapped read-only because it is shared, or because a
      // snapshot has not written it out yet: the snapshot gets it as it
      // was, and then the writer gets a copy of its own (vm_unshare).
      if (scause == EXC_STORE_GPF) {
        if (v->vm->snap.active)
          vmsnap_before_write(v->vm, gpa);
        vm_unshare(v->vm, gpa);
      }
      return 0;
    }
    if (t < 0) {
      printk("hyp: vm %d: out of memory backing guest RAM at gpa 0x%lx; halting the vm\n", v->vm->id, gpa);
      vm_halt(v->vm);
      return -1;
    }
    if (scause != EXC_INST_GPF)
      inst = hyp_fetch(v, v->pc);
    break;
  }
  }

  intr_on();

  switch (scause) {
  case EXC_ILLEGAL:
    return hyp_illegal(v, inst);
  case EXC_VIRT_INST:
    if (inst == 0x10500073) { // wfi
      vcpu_idle(v);
      v->pc += 4;
      return 0;
    }
    if (inst == 0x00d00073 || inst == 0x01d00073) { // wrs.nto / wrs.sto (Zawrs)
      v->pc += 4;
      return 0;
    }
    return hyp_virtinst(v, inst);
  case EXC_ECALL_VS:
    return hyp_ecall(v);
  case EXC_LOAD_GPF:
  case EXC_STORE_GPF: {
    uint64 gpa = (htval << 2) | (stval & 3);
    return hyp_mmio(v, gpa, scause == EXC_STORE_GPF, inst);
  }
  case EXC_INST_GPF:
    if (vm_gripe(&v->vm->g_fault))
      printk("hyp: vm %d vcpu %d: instruction fetch from unmapped gpa 0x%lx "
             "(pc 0x%lx, %ld times)\n", v->vm->id, v->id,
             (htval << 2) | (stval & 3), v->pc, v->vm->g_fault);
    vcpu_inject(v, EXC_INST_ACCESS, stval);
    return 0;
  default:
    printk("hyp: vm %d vcpu %d: unexpected scause 0x%lx pc 0x%lx stval 0x%lx htval 0x%lx\n",
           v->vm->id, v->id, scause, v->pc, stval, htval);
    return -1;
  }
}

// run vcpu vcpuid of vm in the calling process until the VM halts.
int
vm_run(struct vm *vm, int vcpuid)
{
  struct proc *p = myproc();

  if (vcpuid < 0 || vcpuid >= vm->nvcpu)
    return -1;
  struct vcpu *v = &vm->vcpu[vcpuid];

  acquire(&vm->lock);
  if ((vm->state != VM_NEW && vm->state != VM_RUNNING) || v->p != 0) {
    release(&vm->lock);
    return -1;
  }
  v->p = p;
  p->vcpu = v;          // the timer path asks (timer_rearm)
  vm->vcpus_running++;
  vm->state = VM_RUNNING;
  struct sched_group *sg = vm->sg;
  release(&vm->lock);

  // from here this process's processor time is the guest's
  acquire(&p->lock);
  sched_group_join(p, sg);
  release(&p->lock);

  int rc = 0;
  for (;;) {
    if (killed(p))
      break;
    if (vm->state == VM_PAUSED) {
      acquire(&vm->lock);
      while (vm->state == VM_PAUSED && !killed(p)) {
        sleep_prepare((void *)&vm->state);
        release(&vm->lock);
        sleep();
        acquire(&vm->lock);
      }
      release(&vm->lock);
      continue;
    }
    if (vm->state != VM_RUNNING)
      break;
    if (v->hsm_state == HSM_STOPPED) {
      // SBI HSM: wait for hart_start from another vcpu.
      acquire(&vm->lock);
      while (v->hsm_state == HSM_STOPPED && vm->state == VM_RUNNING && !killed(p)) {
        v->idle = 1;
        sleep_prepare(v);
        release(&vm->lock);
        sleep();
        acquire(&vm->lock);
        v->idle = 0;
      }
      release(&vm->lock);
      continue;
    }
    intr_off();
    vcpu_enter(v);
    if (handle_exit(v) < 0) {
      rc = -1;
      vm_halt(vm);
      break;
    }
  }
  intr_on();

  struct inode *ip = 0;
  // out of the guest's group: whatever this process does next is its
  // own, not the machine's.
  acquire(&p->lock);
  sched_group_join(p, 0);
  release(&p->lock);

  acquire(&vm->lock);
  v->p = 0;
  p->vcpu = 0;
  v->running_hart = -1;
  v->idle = 0;
  vm->vcpus_running--;
  if (vm->state == VM_RUNNING || vm->state == VM_PAUSED) {
    // a vcpu left on its own (killed): stop the whole VM.
    vm->state = VM_HALTING;
    wakeup((void *)&vm->state);
    for (int i = 0; i < vm->nvcpu; i++)
      vcpu_kick(&vm->vcpu[i]);
  }
  struct gendisk *dev = 0;
  int last = vm->vcpus_running == 0;
  release(&vm->lock);

  if (last) {
    // the disk thread is reading and writing this guest's memory, so
    // it has to be gone before the memory is. Waiting is something
    // only a thread with no lock may do, which is why the lock was
    // dropped first; no vcpu is running by now, so nothing can start
    // the VM up again in between.
    vvirtio_stop(vm);
    acquire(&vm->lock);
    vm_free_resources(vm);
    ip = vm->disk_ip;
    dev = vm->disk;
    vm->disk_ip = 0;
    vm->disk = 0;
    vm->state = VM_FREE;
    sg = vm->sg;
    vm->sg = 0;
    wakeup(&vm->cons_out); // an attached console reader sees the end
    release(&vm->lock);
    sched_group_destroy(sg);
  }
  vm_put_disk(ip);
  vm_put_device(dev);
  return rc;
}

// statistics for tests: out[0] = free pool chunks, out[1] = total IPIs,
// out[2..] per-VM exit counts.
void
hyp_stats(uint64 *out, int n)
{
  if (n > 0) {
    kstack_drain();
    // what a guest could have: the pages that are free, and the pages
    // the block cache is holding only because they might be useful.
    out[0] = (kmem_free_pages() + blk_reclaimable()) >> 8; // MB
  }
  if (n > 1) {
    uint64 t = 0;
    for (int i = 0; i < NCPU; i++)
      t += ipi_count[i];
    out[1] = t;
  }
  // the same number as out[0] but in pages, which is what a test for
  // a leak has to compare: a megabyte is 256 pages, and rounding to
  // megabytes hides everything smaller than that
  if (n > 2 + NVM)
    out[2 + NVM] = kmem_free_pages() + blk_reclaimable();
  // 3+NVM..6+NVM: same-page merging -- on, scanned, merged, shared
  if (n > 3 + NVM)
    ksm_stats(&out[3 + NVM], n - (3 + NVM));
  for (int i = 0; i < NVM && 2 + i < n; i++) {
    uint64 e = 0;
    struct vm *vm = vmtab[i];
    if (vm)
      for (int j = 0; j < vm->nvcpu; j++)
        e += vm->vcpu[j].exits;
    out[2 + i] = e;
  }
}
