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
  fdtgen_init();
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
  // A machine that has not started yet may also be owed a revision: one that
  // has just arrived from a host of a different shape is asked before its vcpus
  // are spawned, so that the guest meets the new description on its first
  // instruction here. There is nothing to wake in that case, and the pending
  // VSSIP is delivered on the first entry.
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_NEW &&
                 vm->state != VM_PAUSED))
    return -1;

  // One thing at a time (L22: the model's host asks one revision and waits
  // for its ack; this one did not, and its ack handler took any ack as the
  // current generation's, defect 20). A revision asked over an unacknowledged
  // one would end the earlier alias with the guest's ack of the earlier
  // generation while the guest has yet to read the later description. A
  // guest that speaks the protocol acks within milliseconds: the second
  // revision waits for that, up to 5 s; one that never acks is the
  // watchdog's, and is not let to hold every revision after it.
  if (vm->revise_capable && vm->revise_acked != vm->revise_gen) {
    uint64 deadline = r_time() + 5 * HOST_TIMEBASE;
    while (vm->revise_acked != vm->revise_gen && r_time() < deadline) {
      acquire(&tickslock);
      sleep_prepare(&ticks);
      release(&tickslock);
      sleep();
    }
    if (vm->revise_acked != vm->revise_gen)
      printk("hyp: vm %d: revision %d asked over revision %d, unacknowledged for 5 s\n",
             vm->id, vm->revise_gen + 1, vm->revise_gen);
  }

  // The new description goes to a slot of its own, never over the blob the
  // guest booted from: Linux's live device tree keeps pointers into that
  // blob (property values are not copied at unflatten), and rewriting it in
  // place turned every `reg' in the running kernel's tree into whatever the
  // new blob had at that offset (lxp3: cpu@0 with reg 3). Two slots below the
  // boot blob, used alternately, so the one the guest may still be reading
  // is not the one being written. `describe' answers the current slot.
  uint64 slot1 = vm->dtb_gpa - VM_DTB_MAX, slot2 = vm->dtb_gpa - 2 * VM_DTB_MAX;
  uint64 gpa = vm->desc_gpa == slot1 ? slot2 : slot1;
  int n = hyp_make_dtb(vm, gpa, VM_DTB_MAX);
  if (n < 0)
    return -1;
  vm->desc_gpa = gpa;

  acquire(&vm->lock);
  vm->revise_gen++;
  vm->revise_what = what;
  vm->revise_adopted = 0;
  vm->revise_asked = r_time();
  vm->revise_repeat_at = vm->revise_asked;
  vm->revise_wd_printed = 0;
  vm->revise_done = 0;
  // every vcpu, not just the first. A guest with several harts has per-hart
  // controller state, and each hart has to bring its own up against the new
  // description. The obvious way -- have the first hart IPI the others -- does
  // not work, because sending an IPI goes *through* the controller
  // (intc_ipi(), which dispatches on plat.intc_kind) and a hart that has not
  // re-initialised its interrupt file would never receive it. So the host pokes
  // all of them over the same channel it used for the first, which depends on
  // nothing the revision is changing.
  for (int i = 0; i < vm->nvcpu; i++)
    vm->vcpu[i].ssip_req = 1;
  vmachrev_notify(vm);         // and over the anchor, for a guest that listens there (Linux)
  release(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);   // and wake the idle ones
  printk("hyp: vm %d: revision %d asked for (what=0x%x)\n", vm->id,
         vm->revise_gen, what);
  return 0;
}

// Give a running guest a different kind of interrupt controller.
//
// This is the thing that cannot be done anywhere else: a feature mask can hide a
// bit, but it cannot change which driver is bound or what a register file means.
// PLIC, APLIC and APLIC-MSI+IMSIC are three different programming models, and a
// guest that booted against one is holding a driver for it.
//
// The sequence, and the order is the whole of the correctness argument:
//
//   1. refuse if the guest is inside an interrupt handler (claimed but not
//      completed): it would complete against a controller that no longer exists.
//   2. remember which lines the devices are asserting -- the part the host owns.
//   3. switch kind, build the new model empty, re-assert those lines. The guest's
//      own configuration is deliberately not translated: it rebuilds it.
//   4. rebuild the device tree, which now describes the new controller.
//   5. poke the guest, which re-reads, re-binds its driver and re-enables its
//      sources. Interrupts asserted in (3) are delivered as soon as it does.
//
// Between (3) and (5) the guest has a controller it has not configured, so
// nothing is delivered. That window is the outage, and it is measured.
int
vm_revise_intc(struct vm *vm, int kind)
{
  // A paused machine may be revised too, and it is the safest moment to do it:
  // its vcpus are asleep, so there is no guest code between the old controller
  // going away and the new description arriving. The request waits as a pending
  // VSSIP and is delivered on the first entry after the resume. It is also the
  // only way to *construct* the case where a device is holding a line across the
  // exchange -- a running guest acknowledges its interrupts in microseconds, so
  // the window is too narrow to catch on purpose.
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (kind != VIC_PLIC && kind != VIC_APLIC && kind != VIC_IMSIC)
    return -EINVAL;
  if (kind == vm->vic_kind)
    return 0;                       // already what was asked for
  // the AIA models need the machine to have been built with them available
  if ((kind == VIC_APLIC || kind == VIC_IMSIC) && plat.intc_kind == INTC_PLIC)
    return -ENOTSUP;                // this host cannot present them honestly

  // Every vcpu out of the guest first, and kept out: rebuilding the model
  // zeroes each vcpu's IMSIC bookkeeping, and a vcpu inside the guest with a
  // hardware guest file loaded would come out to find `hwfile == 0', read
  // nothing back and free nothing -- the file leaks, still delivering to
  // nobody (a candidate for `frame nobody asked for'). vic_hold is checked in
  // vcpu_enter under the lock; the kick makes the running ones exit, and the
  // exit path (vic_vcpu_exit, under the lock) gives the file back.
  acquire(&vm->lock);
  vm->vic_hold = 1;
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);
  release(&vm->lock);
  for (int spins = 0; ; spins++) {
    int inside = 0;
    acquire(&vm->lock);
    for (int i = 0; i < vm->nvcpu; i++)
      if (vm->vcpu[i].running_hart >= 0 && vm->vcpu[i].running_hart != cpuid())
        inside++;
    if (inside == 0)
      break;                        // the lock is held from here
    release(&vm->lock);
    if (spins > 100000) {
      acquire(&vm->lock);
      vm->vic_hold = 0;
      release(&vm->lock);
      printk("hyp: vm %d: a vcpu would not leave the guest\n", vm->id);
      return -EAGAIN;
    }
  }
  uint64 levels = 0;
  if (vic_levels_snapshot(vm, &levels) < 0) {
    vm->vic_hold = 0;
    release(&vm->lock);
    return -EAGAIN;                 // inside a handler; the caller retries
  }
  int old = vm->vic_kind;
  vm->vic_kind = kind;
  vm->imsic_hw = (kind == VIC_IMSIC && vimsic_geilen() > 0 &&
                  !(vm->cfg.flags & VMF_SOFTIMSIC));
  vic_levels_restore(vm, levels, -1, 0);
  vm->vic_hold = 0;
  release(&vm->lock);

  printk("hyp: vm %d: interrupt controller %s -> %s, %d line(s) held\n", vm->id,
         vic_kindname(old), vic_kindname(kind), vic_held(levels));

  int r = vm_revise(vm, REV_INTC);
  if (r < 0) {
    // could not tell it; put it back rather than leave a guest with a
    // controller it has never heard of
    acquire(&vm->lock);
    vm->vic_kind = old;
    vic_levels_restore(vm, levels, -1, 0);
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F4: whether the machine has a CLINT -- the guest writes another hart's msip
// to send an IPI -- or sends its IPIs through SBI is a fact of the machine.
// Both end as a pending VSSIP on the target vcpu (vclint.c, hypsbi.c send_ipi),
// so the exchange carries nothing: the model appears or disappears and the
// guest is told (REV_DEVICES: a device came or went).
int
vm_revise_ipi(struct vm *vm, int sbi)
{
  sbi = sbi != 0;
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (sbi == vm->no_clint)
    return 0;
  acquire(&vm->lock);
  int old = vm->no_clint;
  vm->no_clint = sbi;
  if (sbi)
    vm_del_dev(vm, "clint");
  else
    vclint_add(vm);
  release(&vm->lock);
  printk("hyp: vm %d: ipis %s -> %s\n", vm->id, old ? "sbi" : "clint", sbi ? "sbi" : "clint");
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->no_clint = old;
    if (old)
      vm_del_dev(vm, "clint");
    else
      vclint_add(vm);
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F5: where the guest's timer is. With Sstc the guest writes stimecmp (as
// vstimecmp, in hardware, henvcfg.STCE=1) and VSTIP comes from the hardware
// comparator. Without it (STCE=0) stimecmp traps as a virtual instruction, the
// guest asks the SBI timer for its deadline -- which lands in the same
// v->vstimecmp -- and VSTIP is the hypervisor's to raise: hyp_next_deadline
// keeps the host timer at the earliest guest deadline, hyp_tick kicks the vcpu
// out when it passes, and vcpu_enter injects VSTIP through hvip while the
// deadline is behind. One machine, two timers; the description says which.
int
vm_revise_timer(struct vm *vm, int sbi)
{
  sbi = sbi != 0;
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (sbi == vm->no_sstc)
    return 0;
  acquire(&vm->lock);
  int old = vm->no_sstc;
  vm->no_sstc = sbi;
  release(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);    // re-enter with the other henvcfg
  printk("hyp: vm %d: timer %s -> %s\n", vm->id, old ? "sbi" : "sstc", sbi ? "sbi" : "sstc");
  int r = vm_revise(vm, REV_TIMER);
  if (r < 0) {
    acquire(&vm->lock);
    vm->no_sstc = old;
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F9: how the harts are numbered is a fact of the machine. The guest sees the
// numbers in mhartid, in a0 at boot and in the tree's cpu nodes, and uses them
// to address its harts through every controller (the CLINT's msip, the PLIC's
// contexts, the APLIC's IDCs and MSI targets, the IMSIC's files, the SBI's hart
// masks); the models translate with vm_vcpu_of_hart(). A guest that keeps a
// logical index apart from the hart id (xv6: tp) takes a renumbering as a
// change of the cpu nodes and rebinds its per-hart controller state.
int
vm_revise_hartid(struct vm *vm, int shift)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (shift < 0 || shift + vm->nvcpu > NVCPU)
    return -EINVAL;
  if (shift == vm->hart_shift)
    return 0;
  acquire(&vm->lock);
  int old = vm->hart_shift;
  vm->hart_shift = shift;
  vm->hart_alias_on = 1;          // the old numbers answer until the guest has the new ones
  vm->hart_alias_shift = old;
  release(&vm->lock);
  printk("hyp: vm %d: harts renumbered %d.. -> %d..\n", vm->id, old, shift);
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->hart_shift = old;
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F7/F8: how many of the machine's harts are running. The tree describes all
// of them; `status' says which. Up: the description changes first and the
// guest extends its tables, then the host starts the vcpus (SBI HSM, at the
// kernel's entry, as a hart started late boots) -- in that order, so that a
// hart finds its own id in the guest's table when it comes up. Down: the
// description changes and the guest drains and stops the harts itself
// (SBI HSM stop); the host waits to see them stopped.
extern struct spinlock tickslock;
extern uint ticks;

extern struct spinlock tickslock;
extern uint ticks;

static uint
now_ticks_hyp(void)
{
  acquire(&tickslock);
  uint t = ticks;
  release(&tickslock);
  return t;
}

int
vm_revise_cpus(struct vm *vm, int online)
{
  if (vm == 0 || vm->state != VM_RUNNING)   // the host waits for the guest to park its harts: not while paused
    return -EINVAL;
  if (online < 1 || online > vm->nvcpu)
    return -EINVAL;
  if (online == vm->online)
    return 0;
  int old = vm->online;
  acquire(&vm->lock);
  vm->online = online;
  release(&vm->lock);
  printk("hyp: vm %d: cpus %d -> %d\n", vm->id, old, online);
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->online = old;
    release(&vm->lock);
    return r;
  }
  // the guest's answer, then the harts
  uint deadline = now_ticks_hyp() + 300;
  while (now_ticks_hyp() < deadline) {
    acquire(&vm->lock);
    int acked = vm->revise_acked == vm->revise_gen;
    release(&vm->lock);
    if (acked)
      break;
    acquire(&tickslock);
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
  }
  if (online > old && VM_FOREIGN(vm)) {
    // a guest that boots its harts itself (Linux: cpu_up -> SBI HSM start at
    // its own secondary entry) starts them; the host starting them at the
    // kernel's entry would boot a second kernel
    printk("hyp: vm %d: hart(s) %d..%d for the guest to start\n", vm->id, old, online - 1);
  } else if (online > old) {
    acquire(&vm->lock);
    for (int i = old; i < online; i++) {
      struct vcpu *v = &vm->vcpu[i];
      if (v->hsm_state == HSM_STOPPED && vcpu_hsm_start(v, vm->entry, vm->dtb_gpa) == 0)
        wakeup(v);
    }
    release(&vm->lock);
    printk("hyp: vm %d: hart(s) %d..%d started\n", vm->id, old, online - 1);
  } else {
    // the guest parks them; seen here as HSM-stopped. A Linux guest takes a
    // second or two per hart (cpu_down: migrate its work, quiesce, HSM stop)
    deadline = now_ticks_hyp() + (VM_FOREIGN(vm) ? 3000 : 500);
    int left;
    do {
      left = 0;
      acquire(&vm->lock);
      for (int i = online; i < old; i++)
        if (vm->vcpu[i].hsm_state != HSM_STOPPED)
          left++;
      release(&vm->lock);
      if (left == 0)
        break;
      acquire(&tickslock);
      sleep_prepare(&ticks);
      release(&tickslock);
      sleep();
    } while (now_ticks_hyp() < deadline);
    printk("hyp: vm %d: hart(s) %d..%d %s\n", vm->id, online, old - 1, left ? "did not stop" : "parked");
  }
  return 0;
}

// F21: another cache-block size. The description changes; until the guest has
// adopted it, cbo.zero is done in software at the larger of the two sizes,
// so a page zeroed at either stride is a page of zeroes.
int
vm_revise_cboz(struct vm *vm, int bytes)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (bytes < 16 || bytes > PGSIZE || (bytes & (bytes - 1)))
    return -EINVAL;
  if (bytes == vm->cboz_block)
    return 0;
  acquire(&vm->lock);
  int old = vm->cboz_block;
  vm->cboz_prev = old;
  vm->cboz_block = bytes;
  release(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);    // re-enter with the other henvcfg
  printk("hyp: vm %d: cache block %d -> %d bytes (%s)\n", vm->id, old, bytes,
         bytes == plat.cboz_block ? "the host's, in hardware" : "not the host's, in software");
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->cboz_block = old;
    vm->cboz_prev = old;
    release(&vm->lock);
    return r;
  }
  return 0;
}

uint64 hyp_cboz_exits;

// F13: more RAM. The memory node grows; the new chunks are backed as the guest
// touches them, like the old ones; the guest is told (REV_MEMORY) and adds the
// range to its allocator. Only *up*: down is F14, negotiated through the
// balloon, because a description cannot take back memory a kernel has data in.
int
vm_grow(struct vm *vm, uint64 bytes)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED && vm->state != VM_NEW))
    return -EINVAL;
  if (bytes <= vm->ram_size || (bytes % HCHUNK) != 0 || bytes > VM_MAXMEM)
    return -EINVAL;
  if (vm->snap.active)
    return -EBUSY;                  // a copy is in progress with the old page count
  int nn = bytes / HCHUNK;
  uint64 tbytes = (uint64)nn * sizeof(uint64);
  int order = 0;
  while (((uint64)PGSIZE << order) < tbytes)
    order++;
  uint64 *nt = kalloc_order(order);
  if (nt == 0)
    return -ENOMEM;
  memset(nt, 0, (uint64)PGSIZE << order);
  acquire(&vm->memlock);
  uint64 *old = vm->chunk;
  int on = vm->nchunks;
  memmove(nt, old, (uint64)on * sizeof(uint64));
  vm->chunk = nt;                   // the pointer first: readers bound by nchunks
  __sync_synchronize();
  vm->nchunks = nn;
  vm->ram_size = bytes;
  vm->cfg.mem_mb = bytes >> 20;
  release(&vm->memlock);
  // the old table is not freed: vm_touch() and its kin read vm->chunk[k]
  // without memlock (k bounded by nchunks), and one of them may be holding the
  // old pointer this instant. A page or two per growth, kept.
  (void)old;
  printk("hyp: vm %d: memory %d -> %d MB\n", vm->id, (int)(((uint64)on * HCHUNK) >> 20), (int)(bytes >> 20));
  return 0;
}

// F18: the console is a class change beside the interrupt controller: a 16550
// the guest drives register by register, a virtio-console it feeds by
// descriptor, or the SBI debug console it calls into -- three programming
// models for one function. The description names the one this machine has;
// the guest rebinds its console to it and says so through the new one.
int
vm_revise_console(struct vm *vm, int kind)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (kind < CON_UART || kind > CON_SBI)
    return -EINVAL;
  if (kind == vm->console_kind)
    return 0;
  acquire(&vm->lock);
  int old = vm->console_kind;
  vm->console_kind = kind;
  if (kind == CON_VIRTIO && !vm->con.present) {
    vm->con.present = 1;
    vm->con_going = 0;
    vvcon_add(vm);
  } else if (kind != CON_VIRTIO && vm->con.present) {
    // the device leaves the description now and the machine at the ack: a
    // guest's driver tears down against a device that still answers (Linux's
    // virtio-console remove path read a window that was gone: fx7)
    vm->con_going = 1;
  }
  release(&vm->lock);
  static const char *names[] = {"uart", "virtio", "sbi"};
  printk("hyp: vm %d: console %s -> %s\n", vm->id, names[old], names[kind]);
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->console_kind = old;
    vm->con_going = 0;
    if (old == CON_VIRTIO && !vm->con.present) {
      vm->con.present = 1;
      vvcon_add(vm);
    } else if (old != CON_VIRTIO && vm->con.present) {
      vm->con.present = 0;
      vm_del_dev(vm, "virtio-con");
    }
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F16/F17: a device that comes or goes. The RTC is the one chosen for it: a
// machine may have a wall clock or not, the tree says which, and a guest that
// loses it keeps a software clock from the last reading while one that gains
// it re-anchors. Nothing to negotiate: an RTC is never "in use" in the sense
// a mounted disk is (F17's refusal is for those), so the removal is told and
// taken.
int
vm_revise_rtc(struct vm *vm, int on)
{
  on = on != 0;
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (on && !rtc_present())
    return -ENOTSUP;            // this host has no clock to model
  if (on == !vm->no_rtc)
    return 0;
  acquire(&vm->lock);
  int old = vm->no_rtc;
  vm->no_rtc = !on;
  if (on)
    vrtc_add(vm);
  else
    vm_del_dev(vm, "rtc");
  release(&vm->lock);
  printk("hyp: vm %d: rtc %s\n", vm->id, on ? "added" : "removed");
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->no_rtc = old;
    if (old)
      vm_del_dev(vm, "rtc");
    else
      vrtc_add(vm);
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F19: the NIC's identity. Not a fact of the *description* -- the tree does
// not carry a MAC -- but a fact of the machine the guest can observe, and one
// that changes at a host of another policy. It is told the way the device
// class provides: a configuration change, re-read by the driver.
int
vm_revise_mac(struct vm *vm, int alt)
{
  alt = alt != 0;
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (alt == vm->mac_alt)
    return 0;
  acquire(&vm->lock);
  vm->mac_alt = alt;
  vm->vnet.mac[3] = alt ? 0x79 : 0x78;
  vvnet_mac_changed(vm);
  release(&vm->lock);
  printk("hyp: vm %d: nic identity -> 52:54:00:%x:76:%x\n", vm->id, vm->vnet.mac[3], vm->vnet.mac[5]);
  // told through the device; a guest that speaks revision is nudged to look
  // as well (Linux's driver reads the address at probe only: machrev.ko
  // re-reads it on a revision; an xv6 guest finds nothing changed and says so)
  if (vm->revise_capable)
    vm_revise(vm, REV_DEVICES);
  return 0;
}

// F6: the timebase-frequency is a fact of the machine, and the one whose
// change has a stated cost. A machine described with a rate other than the
// host's cannot be allowed to read `time' directly (hcounteren.TM=0: the read
// traps and is scaled), and its timer cannot be the hardware comparator (the
// deadline is in its units): the SBI timer path of F5 carries it, with the
// deadline converted here. The guest's clock stays continuous across the
// change because the conversion is of the whole count, not of a delta.
//
// The *value* of the clock is continuous across a change of rate: what changes
// is how fast it advances from here. A kernel that computes elapsed ticks from
// two readings cannot survive its counter jumping to a tenth or ten times of
// itself (m2c: the guest never acknowledged). So guest time is
// tb_g0 + scale(X - tb_x0), with X = host time + htimedelta and (tb_x0, tb_g0)
// the moment and the value at the last change; and a machine described as
// 1 MHz from birth simply has tb_x0 = tb_g0 = 0.
static uint64
scale_by(uint64 x, uint64 num, uint64 den)
{
  return (x / den) * num + ((x % den) * num) / den;
}

uint64
vm_guest_time(struct vm *vm)
{
  uint64 x = r_time() + vm->htimedelta;
  if (!vm_scaled_time(vm))
    return x;
  return vm->tb_g0 + scale_by(x - vm->tb_x0, vm->timebase, HOST_TIMEBASE);
}

uint64
vm_guest_time_of(struct vm *vm, uint64 x)
{
  if (!vm_scaled_time(vm) || x == ~0UL)
    return x;
  if (x < vm->tb_x0)
    return vm->tb_g0;
  return vm->tb_g0 + scale_by(x - vm->tb_x0, vm->timebase, HOST_TIMEBASE);
}

uint64
vm_host_time_of(struct vm *vm, uint64 gt)
{
  if (!vm_scaled_time(vm) || gt == ~0UL)
    return gt;
  if (gt < vm->tb_g0)
    return vm->tb_x0;                       // already behind: fires at once
  return vm->tb_x0 + scale_by(gt - vm->tb_g0, HOST_TIMEBASE, vm->timebase);
}

int
vm_revise_timebase(struct vm *vm, uint32 hz)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (hz == HOST_TIMEBASE)
    hz = 0;
  if (hz != 0 && (hz < 1000 || hz > 1000000000))
    return -EINVAL;
  if (hz == vm->timebase)
    return 0;
  acquire(&vm->lock);
  uint32 old = vm->timebase;
  uint64 ox0 = vm->tb_x0, og0 = vm->tb_g0;
  // continuity: the value now, under the old rate, is where the new rate starts
  uint64 gnow = vm_guest_time(vm);
  vm->tb_x0 = r_time() + vm->htimedelta;
  vm->tb_g0 = gnow;
  vm->timebase = hz;
  // deadlines were set in the old units; they mean the same moment in the new
  for (int i = 0; i < vm->nvcpu; i++)
    ;                           // (vstimecmp is kept in host units: unchanged)
  release(&vm->lock);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);    // re-enter with the other hcounteren/henvcfg
  printk("hyp: vm %d: timebase %u -> %u Hz (clock at %lu)\n", vm->id, old ? old : (uint32)HOST_TIMEBASE,
         hz ? hz : (uint32)HOST_TIMEBASE, (unsigned long)gnow);
  int r = vm_revise(vm, REV_TIMER);
  if (r < 0) {
    acquire(&vm->lock);
    vm->timebase = old;
    vm->tb_x0 = ox0;
    vm->tb_g0 = og0;
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F14: how much of this host's memory a guest arriving here may be resident
// in -- what is free above the host's own reserve, capped by `guestmem=MB' on
// the command line (a host that keeps its guests small, or a test that wants
// a definite number).
uint64
hyp_guest_room_bytes(void)
{
  struct meminfo mi;
  kmem_info(&mi);
  uint64 room = mi.free_pages > mi.reserve_pages ? (mi.free_pages - mi.reserve_pages) * PGSIZE : 0;
  uint64 cap = plat_bootarg_num("guestmem") << 20;
  if (cap && cap < room)
    room = cap;
  return room;
}

// F22: the set of SBI extensions is a fact of the machine -- a firmware that
// lacks RFENCE or HSM is a different machine to boot on, and Linux decides at
// boot which paths to take from exactly this set. The host can withhold any of
// six (`nosbi=time', `nosbi=ipi', `nosbi=rfence', `nosbi=hsm', `nosbi=srst',
// `nosbi=dbcn'); the machine's own set is carried with it, and a change is a
// revision the guest re-probes for (REV_SBI).
uint32
sbi_hidden_default(void)
{
  uint32 h = 0;
  if (plat_bootarg("nosbi=time"))   h |= SBIH_TIME;
  if (plat_bootarg("nosbi=ipi"))    h |= SBIH_IPI;
  if (plat_bootarg("nosbi=rfence")) h |= SBIH_RFENCE;
  if (plat_bootarg("nosbi=hsm"))    h |= SBIH_HSM;
  if (plat_bootarg("nosbi=srst"))   h |= SBIH_SRST;
  if (plat_bootarg("nosbi=dbcn"))   h |= SBIH_DBCN;
  return h;
}

int
vm_revise_sbi(struct vm *vm, uint32 hidden)
{
  hidden &= SBIH_ALL;
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (hidden == vm->sbi_hidden)
    return 0;
  acquire(&vm->lock);
  uint32 old = vm->sbi_hidden;
  vm->sbi_hidden = hidden;
  release(&vm->lock);
  printk("hyp: vm %d: sbi extensions withheld 0x%x -> 0x%x\n", vm->id, old, hidden);
  int r = vm_revise(vm, REV_SBI);
  if (r < 0) {
    acquire(&vm->lock);
    vm->sbi_hidden = old;
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F23: whether an IMSIC guest's interrupts arrive through a hardware guest
// interrupt file or through the software model is *not* a fact of the machine:
// the guest sees the same IMSIC either way. It is a fact of the host, and it can
// be changed under the guest at any time -- vimsic_exit() reads the file back
// into the model on every exit and vimsic_enter() loads it into a file or not,
// so the switch is a flag and a kick. What differs is the exits per interrupt.
int
vm_set_imsic_hw(struct vm *vm, int on)
{
  if (vm == 0 || vm->state != VM_RUNNING)
    return -EINVAL;
  if (vm->vic_kind != VIC_IMSIC)
    return -ENOTSUP;
  if (on && (plat.intc_kind != INTC_IMSIC || vimsic_geilen() == 0))
    return -ENOTSUP;
  acquire(&vm->lock);
  int was = vm->imsic_hw;
  vm->imsic_hw = on != 0;
  release(&vm->lock);
  if (was != vm->imsic_hw)
    for (int i = 0; i < vm->nvcpu; i++)
      vcpu_kick(&vm->vcpu[i]);     // leave and re-enter: the model or the file
  printk("hyp: vm %d: imsic delivery %s -> %s\n", vm->id,
         was ? "hardware file" : "software model", vm->imsic_hw ? "hardware file" : "software model");
  return 0;
}

// F24: how the guest's memory is backed (2 MB blocks or 4 KB pages) is a fact of
// the host too. Splitting everything is what a snapshot does before it can
// write-protect a page; here it is done on request, so that the change can be
// made under a running guest and shown to be invisible to it.
int
vm_split_all(struct vm *vm)
{
  if (vm == 0 || vm->state != VM_RUNNING)
    return -EINVAL;
  int n = 0;
  for (int k = 0; k < vm->nchunks; k++) {
    acquire(&vm->memlock);
    int backed = vm->chunk && vm->chunk[k] != 0 && !gc_split(vm->chunk[k]);
    release(&vm->memlock);
    if (backed && vm_split(vm, G_RAM_BASE + (uint64)k * HCHUNK) == 0)
      n++;
  }
  printk("hyp: vm %d: %d block(s) split to pages\n", vm->id, n);
  return 0;
}

// F3: the number of the line a device raises is a fact of the machine, as its
// address is. Two numberings exist: the default (uart 10, virtio 1..4, rtc 11)
// and the alternative (`guestirq=alt': uart 7, virtio 11..14, rtc 15), which is
// how a host that wires its devices differently is made. Every place a model
// raises a line, registers itself, or is described in the tree goes through here.
int
irq_map(int alt, int irq)
{
  if (!alt)
    return irq;
  switch (irq) {
  case G_UART_IRQ:    return 7;
  case G_VIRTIO_IRQ:  return 11;
  case G_VNET_IRQ:    return 12;
  case G_BALLOON_IRQ: return 13;
  case G_CON_IRQ:     return 14;
  case G_RTC_IRQ:     return 15;
  }
  return irq;
}

int
vm_irq(struct vm *vm, int irq)
{
  // F3 for a guest that boots its own harts (Linux): the console's line stays
  // where it was. Linux's serial core has no way to move a port's interrupt
  // under a running console, and a console whose line has moved is a machine
  // nobody can talk to (lxmig2: irq axis, no prompt for 480 s). The virtio
  // and RTC lines follow (machrev.ko); this one is the second anchor.
  if (irq == G_UART_IRQ && VM_FOREIGN(vm))
    return irq;
  // F3's alias: until the guest has taken the new numbering, a device raises
  // on the number the guest knows it by. Raised on the new number, a line the
  // guest had given to another device brings that device's handler, which
  // finds nothing, returns, and is brought again: an exit storm (hunt4d
  // crossing 384, 1.8 M exits, the arrival never acknowledged), or Linux
  // disabling the line as "nobody cared" and the device it belonged to deaf.
  if (vm->irq_alias_on && vm_revising(vm))
    return irq_map(vm->irq_alias_alt, irq);
  return irq_map(vm->irq_alt, irq);
}

// The number a device is *described* with: the new numbering, from the moment
// the revision is decided. vm_irq() is the number a device *raises* on, which
// stays the old one until the ack (F3's alias). The description must use this
// one: built through vm_irq() it carried the old numbers while the revision
// hold was up, the guest compared and found "no-change" (d2reg,
// guest-revise-irqmap, both controllers) -- the alias had leaked into the
// description it was meant to bridge.
int
vm_irq_described(struct vm *vm, int irq)
{
  if (irq == G_UART_IRQ && VM_FOREIGN(vm))
    return irq;
  return irq_map(vm->irq_alt, irq);
}

// the acknowledgement: the guest has the new numbering; what the devices hold
// moves from the old numbers to the new ones. vm->lock held.
void
vm_irq_alias_end(struct vm *vm)
{
  if (!vm->irq_alias_on)
    return;
  vm->irq_alias_on = 0;
  uint64 levels = vm->vic_kind == VIC_PLIC ? vm->plic.level : vm->aplic.level;
  levels |= vm->quiet_set;        // what was raised during the quiet window, by its old number
  vm->quiet_set = 0;
  vm->irq_quiet = 0;
  uint64 moved = 0;
  for (int i = 0; i < vm->ndev; i++) {
    struct vdev *d = &vm->dev[i];
    if (d->irq0 < 0)
      continue;
    if (d->irq0 == G_UART_IRQ && VM_FOREIGN(vm))
      continue;
    if (levels & (1ull << irq_map(vm->irq_alias_alt, d->irq0)))
      moved |= 1ull << irq_map(vm->irq_alt, d->irq0);
    d->irq = irq_map(vm->irq_alt, d->irq0);
  }
  for (int irq = 1; irq < VPLIC_NIRQ; irq++)
    if (levels & (1ull << irq))
      vic_set_level(vm, irq, 0);
  for (int irq = 1; irq < VPLIC_NIRQ; irq++)
    if (moved & (1ull << irq))
      vic_set_level(vm, irq, 1);
}

// Renumber a running guest's interrupt lines and tell it. What the host owns --
// which lines the devices are holding asserted -- is carried across by number:
// a line held as 1 (virtio) is held as 11 afterwards. Everything the guest
// configured (enables, priorities) it rebuilds when it adopts the description
// (machine.c: intc_init() after the numbering changed), exactly as for a change
// of controller kind, and the model is rebuilt the same way.
int
vm_revise_irqmap(struct vm *vm, int alt)
{
  alt = alt != 0;
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (alt == vm->irq_alt)
    return 0;
  acquire(&vm->lock);
  uint64 levels = 0;
  if (vic_levels_snapshot(vm, &levels) < 0) {
    release(&vm->lock);
    return -EAGAIN;
  }
  int old = vm->irq_alt;
  // The same controller, the lines renumbered: until the guest has taken the
  // new numbering the devices go on raising on the old one (vm_irq's alias),
  // and what they hold moves to the new numbers at the acknowledgement
  // (vm_irq_alias_end). *Not* vic_levels_restore(): that rebuilds the model
  // from nothing, and on an IMSIC host with a hardware guest file loaded on a
  // running vcpu it zeroes the per-vcpu file bookkeeping underneath it (m2a).
  vm->irq_alias_on = 1;
  vm->irq_alias_alt = old;
  vm->irq_alt = alt;
  release(&vm->lock);
  printk("hyp: vm %d: interrupt lines renumbered %s -> %s, %d line(s) held on the old numbers until the ack\n", vm->id,
         old ? "alt" : "default", alt ? "alt" : "default", vic_held(levels));
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->irq_alias_on = 0;
    vm->irq_quiet = 0;
    vm->irq_alt = old;
    uint64 moved = 0;
    for (int irq = 1; irq < VPLIC_NIRQ; irq++)
      if (moved & (1ull << irq))
        vic_set_level(vm, irq, 0);
    for (int irq = 1; irq < VPLIC_NIRQ; irq++)
      if (levels & (1ull << irq))
        vic_set_level(vm, irq, 1);
    release(&vm->lock);
    return r;
  }
  return 0;
}

// Move a running guest's interrupt controller registers to another address (F2).
//
// The controller keeps its kind and all of its state; only the window the guest
// reaches it through changes, exactly as a virtio device's does on the layout
// axis. The models address their registers by offset from the window (vdev
// read/write take `off'), so the move is the window's base and the description.
// The guest, told, maps the new window and rebinds its driver (machine.c: it
// compares the bases, not only the kind).
int
vm_revise_intc_addr(struct vm *vm, uint64 shift)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (shift != 0 && shift != G_INTC_ALT_SHIFT)
    return -EINVAL;
  if (shift == vm->intc_shift)
    return 0;
  acquire(&vm->lock);
  uint64 old = vm->intc_shift;
  vm->map_seq++; __sync_synchronize();          // the map is being moved: lookups wait (hypmmio.c)
  for (int i = 0; i < vm->ndev; i++)
    if (vm->dev[i].obs == OBS_INTC)
      vm->dev[i].base = vm->dev[i].base - old + shift;
  vm->intc_shift = shift;
  vm->intc_alias_on = 1;          // the old window answers until the ack (Invariant 4, F2)
  vm->intc_alias_shift = old;
  __sync_synchronize(); vm->map_seq++;
  release(&vm->lock);
  printk("hyp: vm %d: interrupt controller registers +0x%lx -> +0x%lx\n", vm->id, old, shift);
  int r = vm_revise(vm, REV_INTC);
  if (r < 0) {
    acquire(&vm->lock);
    vm->map_seq++; __sync_synchronize();
    for (int i = 0; i < vm->ndev; i++)
      if (vm->dev[i].obs == OBS_INTC)
        vm->dev[i].base = vm->dev[i].base - shift + old;
    vm->intc_shift = old;
    vm->intc_alias_on = 0;
    __sync_synchronize(); vm->map_seq++;
    release(&vm->lock);
    return r;
  }
  return 0;
}

// F15, live: move a running machine's device windows a megabyte up the map (or
// back). Until now the map changed only on arrival at a host of another shape;
// the same mechanism -- the windows move, the old ones answer through the alias
// until the guest has taken the new map (hypmmio.c find_dev), the guest is
// told -- works under a running guest, and gives the alias window something
// to be stressed with (L33) and its cost something to be measured on (L20).
int
vm_revise_map(struct vm *vm, uint64 shift)
{
  if (vm == 0 || (vm->state != VM_RUNNING && vm->state != VM_PAUSED))
    return -EINVAL;
  if (shift != 0 && shift != G_MMIO_ALT_SHIFT)
    return -EINVAL;
  if (shift == vm->mmio_shift)
    return 0;
  acquire(&vm->lock);
  uint64 old = vm->mmio_shift;
  vm->map_seq++; __sync_synchronize();          // the map is being moved: lookups wait (hypmmio.c)
  for (int i = 0; i < vm->ndev; i++) {
    struct vdev *d = &vm->dev[i];
    if (!d->ghost && d->base >= G_VIRTIO_BASE + old && d->base < G_CON_BASE + 0x1000 + old)
      d->base = d->base - old + shift;
  }
  vm->mmio_shift = shift;
  vm->alias_on = 1;               // the old windows answer until it has taken the new map
  vm->alias_shift = old;
  __sync_synchronize(); vm->map_seq++;
  release(&vm->lock);
  printk("hyp: vm %d: device windows +0x%lx -> +0x%lx\n", vm->id, old, shift);
  int r = vm_revise(vm, REV_DEVICES);
  if (r < 0) {
    acquire(&vm->lock);
    vm->map_seq++; __sync_synchronize();
    for (int i = 0; i < vm->ndev; i++) {
      struct vdev *d = &vm->dev[i];
      if (!d->ghost && d->base >= G_VIRTIO_BASE + shift && d->base < G_CON_BASE + 0x1000 + shift)
        d->base = d->base - shift + old;
    }
    vm->mmio_shift = old;
    vm->alias_on = 0;
    __sync_synchronize(); vm->map_seq++;
    release(&vm->lock);
    return r;
  }
  return 0;
}

// Take the vector unit away from a running guest, or give it back.
//
// The second thing a feature mask cannot do. A mask can hide V from a guest that
// has not looked yet; it cannot take it away from one that has, because the
// register file is *state* -- thirty-two registers of vlenb bytes per task that
// has used it -- and that state has nowhere to go on a machine without the unit.
//
// So the host asks and the guest answers. Revoking is masking V out of the misa
// this machine is told about (hypcsr.c) and out of its device tree (fdtgen.c),
// and forcing vsstatus.VS to Off so that a vector instruction traps rather than
// quietly using registers the description says are absent. The guest checks
// whether any task is holding vector state and refuses if so -- which is the
// paper's argument in one exchange: what a machine may become depends on what
// its guest is relying on at that moment, not on what the fleet can offer.
int
vm_revise_vector(struct vm *vm, int on)
{
  if (vm == 0 || vm->state != VM_RUNNING)
    return -EINVAL;
  if (on && !plat.has_v)
    return -ENOTSUP;                  // this host has no vector unit to give
  if (on == !vm->vec_revoked)
    return 0;                         // already as asked

  acquire(&vm->lock);
  vm->vec_revoked = !on;
  vm->vec_pending = 1;
  // The description changes now; the unit does not. Invariant 4 holds for the
  // vector unit as for a device window: until the guest acknowledges, a task
  // that holds vector state keeps computing on it -- the guest may be about to
  // refuse because of that very task -- and forcing VS Off here made Linux's
  // first-use trap find a task with a save area and no VS (a WARN, a leak,
  // the registers' contents surviving only by luck). Off is enforced at the
  // acknowledgement that cedes the unit (hypsbi.c fid 3). Granted, nothing is
  // written either: a guest turns VS on itself when a task first uses the
  // unit (xv6 trap.c, Linux riscv_v_first_use_handler); VS=Initial written by
  // the host told Linux a task had live state and no save area -- an Oops on
  // the return to user (lxvs4).
  release(&vm->lock);

  printk("hyp: vm %d: vector unit %s\n", vm->id, on ? "granted" : "revoked");
  int r = vm_revise(vm, REV_VLEN);
  if (r < 0) {
    acquire(&vm->lock);
    vm->vec_revoked = !vm->vec_revoked;
    vm->vec_pending = 0;
    release(&vm->lock);
    return r;
  }
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
        if (vm->snap.active && !vm->snap.precopy)
          *pte &= ~(uint64)PTE_W;
        else if (vm->snap.active)
          vmsnap_before_write(vm, gpa);   // marked again here, see below
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
      //
      // A pre-copy is different: it re-protects every page at the start of
      // each round (vm_snap_protect, before the round's dirty set is taken),
      // so one fault per page per round is all it needs to hear -- the mark
      // that brought us here covers every later write until the next round.
      // Leaving the page read-only made every store a trap for the rest of
      // the round: a guest running `dd' under the copy took 3.8 million
      // faults and 249 s to move 48 MB (fam8, snap-migrate-busy), and the
      // destination gave up waiting. `nobody had marked' stays the check.
      //
      // The mark and the write-enable have to be one step. The caller marked
      // before taking memlock; a round's `take' (dirty -> send, lock-free) and
      // its re-protect (under memlock) can both run in between, and then the
      // mark sits in the set being sent while the page is handed back writable
      // *after* the re-protect made it read-only -- every later write of the
      // round unrecorded (busy3: `2 page(s) nobody had marked'). Marking again
      // here, under the lock the re-protect takes, puts the mark in the set
      // that the re-protect follows, whichever order the three ran in.
      pg->flags &= ~(uint16)PG_KSM;
      if (vm->snap.active && vm->snap.precopy)
        vmsnap_before_write(vm, gpa);
      if (!vm->snap.active || vm->snap.precopy)
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

// As vm_copyin, but a page that is not backed stays that way: -1 says so. The
// pre-copy sweep uses this. vm_copyin's copy_step goes through vm_touch, which
// *backs* an unbacked page -- writable, unprotected, unmarked -- and then
// reports it "not backed" to the sweep, which records zeroes for it; the guest
// writes it moments later without a fault (dd filling fresh tmpfs pages), and
// the page reaches the far end as zeroes. Found as `0x805f9000 changed under
// the copy and nobody had marked it (content)' (busy10c), by the verifying
// round. Older than everything in this file's journal.
int
vm_peek(struct vm *vm, void *dst, uint64 gpa, uint64 n)
{
  while (n > 0) {
    uint64 hpa, len;
    acquire(&vm->memlock);
    if (gpa2hpa_locked(vm, gpa, &hpa, &len) < 0) {
      release(&vm->memlock);
      return -1;
    }
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
  //
  // And the mark has to come *after* the write. Marked before, it could be
  // taken into a pre-copy round's send set and the page copied over, and
  // only then the memmove land: content changed, mark consumed, nobody the
  // wiser. That was `page 0x80139000 changed under the copy and nobody had
  // marked it' (busy6a) -- the virtio-blk used ring, written by the disk
  // thread. Marked after, the mark either follows the round's take (next
  // round's set) or precedes it, in which case the copy, which follows the
  // take, sees the write. Older than the one-fault-per-round change; found
  // by the verifying last round.
  uint64 gpa0 = gpa, n0 = n;
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
  if (vm->snap.active && vm->snap.dirty && n0 > 0 &&
      gpa0 >= G_RAM_BASE && gpa0 < G_RAM_BASE + vm->ram_size) {
    uint64 last = gpa0 + n0 - 1;
    if (last >= G_RAM_BASE + vm->ram_size)
      last = G_RAM_BASE + vm->ram_size - 1;
    for (uint64 p = gpa0 & ~(uint64)(PGSIZE - 1); p <= last; p += PGSIZE) {
      int idx = (int)((p - G_RAM_BASE) / PGSIZE);
      if (idx < vm->snap.npages)
        vmsnap_mark(vm->snap.dirty, idx);
    }
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
  v->regs[10] = id + vm->hart_shift;   // a0 = hartid (F9: as this machine numbers them)
  v->regs[11] = vm->dtb_gpa; // a1 = device tree
  v->priv = 1;               // VS mode
  v->in_mmode = 1;           // running the guest's machine-mode boot code
  v->vstimecmp = ~0L;
  v->scounteren = 7; // as firmware would: user access to cycle, time, instret
  v->last_hart = -1;
  // with VMF_HSM only vcpu 0 boots; the others wait for SBI hart_start.
  v->hsm_state = (((vm->cfg.flags & VMF_HSM) && id != 0) || id >= vm->online) ? HSM_STOPPED : HSM_STARTED;   // F7/F8
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
  v->regs[10] = v->id + v->vm->hart_shift;
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
    return -ENOTSUP;
  int nvcpu = cfg->nvcpu;
  uint64 mem_bytes = (uint64)cfg->mem_mb * 1024 * 1024;
  if (nvcpu < 1 || nvcpu > NVCPU)
    return -EINVAL;
  if (mem_bytes < 8 * HCHUNK || (mem_bytes % HCHUNK) != 0 || mem_bytes > VM_MAXMEM)
    return -EINVAL;
  if (cfg->kernel[0] == 0 && !noload)
    return -EINVAL;

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
        return -EEXIST;
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
    return -ENOSPC;

  int id = vm->id;
  int err = -ENOMEM;        // why a `goto fail' failed; `vmm' shows it
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
  vm->lazy_v = (cfg->flags & VMF_LAZYV) || plat_bootarg("lazyv=1");
  if (vm->lazy_v && plat.has_v)
    vm->vec_revoked = 1;          // hidden until asked for, by use
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

  // the guest device tree sits in the last 64 KB of guest RAM; revisions
  // go to the two 64 KB slots below it (hyp.h: desc_gpa)
  vm->dtb_gpa = G_RAM_BASE + vm->ram_size - 0x10000;
  vm->desc_gpa = vm->dtb_gpa;

  // the disk: a regular file in the host file system, or a whole block
  // device -- a disk, a partition, a loop device -- handed to the guest
  // as it is. a device is faster (no host file system in the way) and
  // is what a guest that owns its storage wants; while a guest has one
  // the host will not mount it and no other guest may take it.
  if (cfg->disk[0]) {
    struct inode *ip = namei(cfg->disk);
    if (ip == 0) {
      err = -ENOENT;
      goto fail;
    }
    ilock(ip);
    int isdev = ip->type == T_DEVICE && ip->major == DEV_BLOCK;
    int minor = ip->minor;
    int ok = isdev || (ip->type == T_FILE && ip->size >= 512);
    iunlock(ip);
    if (!ok) {
      iput(ip);
      err = -EINVAL;
      goto fail;
    }
    if (isdev) {
      struct gendisk *d = blk_by_index(minor);
      if (d == 0 || d->nsectors == 0 || d->claimed || disk_in_use(vm, 0, d)) {
        iput(ip);
        err = -EBUSY;
        goto fail;
      }
      d->claimed = 1;
      vm->disk = d;
      iput(ip); // the device node was only how the disk was named
    } else {
      if (disk_in_use(vm, ip, 0)) {
        iput(ip);
        err = -EBUSY;
        goto fail;
      }
      vm->disk_ip = ip;
    }
  }

  // a machine normally sees the kind of controller the host has; VMF_WANT_PLIC
  // asks for a plain PLIC instead, which is how a revision's starting point is
  // set up on an AIA host (all three models are ours, so this is honest).
  vm->vic_kind = (cfg->flags & VMF_WANT_PLIC)  ? VIC_PLIC
               : (cfg->flags & VMF_WANT_APLIC) ? VIC_APLIC
               : (cfg->flags & VMF_WANT_IMSIC) ? VIC_IMSIC
                                               : plat.intc_kind;
  // hardware guest interrupt files only when the machine is an IMSIC one *and*
  // this host has them; an IMSIC asked for on a host without is the software
  // model, which is what a machine preserving the shape it left with gets.
  vm->imsic_hw = (vm->vic_kind == VIC_IMSIC && plat.intc_kind == INTC_IMSIC &&
                  vimsic_geilen() > 0 && !(cfg->flags & VMF_SOFTIMSIC));

  // the memory map this machine gets: what was asked for, else the host's own
  // default (`guestmap=alt' on the host command line moves every guest's virtio
  // block up by a megabyte, which is how a host of a different shape is made)
  vm->mmio_shift = (cfg->flags & VMF_MMIO_SET) ? cfg->mmio_shift
                 : plat_bootarg("guestmap=alt") ? G_MMIO_ALT_SHIFT : 0;
  // and where its interrupt controller is (F2): the same rule
  vm->intc_shift = (cfg->flags & VMF_INTC_SET) ? cfg->intc_shift
                 : plat_bootarg("guestintc=alt") ? G_INTC_ALT_SHIFT : 0;
  // and how its interrupt lines are numbered (F3): the same rule again
  vm->irq_alt = (cfg->flags & VMF_IRQ_SET) ? (cfg->irq_alt != 0)
              : plat_bootarg("guestirq=alt") ? 1 : 0;
  // and which SBI extensions it is offered (F22): `nosbi=rfence' and friends
  // on the host command line withhold one each
  vm->sbi_hidden = (cfg->flags & VMF_SBI_SET) ? (cfg->sbi_hidden & SBIH_ALL) : sbi_hidden_default();
  // how it sends IPIs (F4) and where its timer is (F5): `guestipi=sbi' and
  // `guesttimer=sbi' on the host command line make hosts without a CLINT and
  // without Sstc
  vm->no_clint = (cfg->flags & VMF_IPI_SET) ? (cfg->no_clint != 0) : plat_bootarg("guestipi=sbi");
  vm->no_sstc = (cfg->flags & VMF_TIMER_SET) ? (cfg->no_sstc != 0) : plat_bootarg("guesttimer=sbi");
  // and the rate its clock is described as running at (F6): `guesttimebase=HZ'
  vm->timebase = (cfg->flags & VMF_TIMEBASE_SET) ? cfg->timebase : (uint32)plat_bootarg_num("guesttimebase");
  if (vm->timebase == HOST_TIMEBASE)
    vm->timebase = 0;
  // and how its harts are numbered (F9): `guesthart=4' makes a host whose
  // guests' harts are 4..; ids stay below NVCPU so a guest can index by them
  vm->hart_shift = (cfg->flags & VMF_HART_SET) ? cfg->hart_shift : (int)plat_bootarg_num("guesthart");
  if (vm->hart_shift < 0 || vm->hart_shift + nvcpu > NVCPU)
    vm->hart_shift = 0;
  // and the identity of its NIC (F19): `guestmac=alt' is a host that hands out
  // other addresses
  vm->mac_alt = (cfg->flags & VMF_MAC_SET) ? (cfg->mac_alt != 0) : plat_bootarg("guestmac=alt");
  // and whether it has an RTC (F16/F17): `guestrtc=off' is a host whose guests have none
  vm->no_rtc = (cfg->flags & VMF_RTC_SET) ? (cfg->no_rtc != 0) : plat_bootarg("guestrtc=off");
  // and what its console is (F18): a 16550, a virtio-console (-Q, as before),
  // or the SBI debug console; `guestcon=virtio|sbi' is a host of that kind
  vm->console_kind = (cfg->flags & VMF_CON_SET) ? cfg->console_kind
                   : (cfg->flags & VMF_CONQUEUE) ? CON_VIRTIO
                   : plat_bootarg("guestcon=virtio") ? CON_VIRTIO
                   : plat_bootarg("guestcon=sbi") ? CON_SBI : CON_UART;
  if (vm->console_kind < 0 || vm->console_kind > 2)
    vm->console_kind = CON_UART;
  // and how many of its harts are started (F7/F8): all, unless asked or the
  // host caps them (`guestcpus=N'); the rest are described `disabled'
  vm->online = (cfg->flags & VMF_CPUS_SET) ? cfg->online : nvcpu;
  {
    int cap = (int)plat_bootarg_num("guestcpus");
    if (!(cfg->flags & VMF_CPUS_SET) && cap > 0 && cap < vm->online)
      vm->online = cap;
  }
  if (vm->online < 1 || vm->online > nvcpu)
    vm->online = nvcpu;
  // F21: the cache-block size it is described with: this host's, unless asked
  vm->cboz_block = (cfg->flags & VMF_CBOZ_SET) ? cfg->cboz_block : plat.cboz_block;
  if (vm->cboz_block < 16 || vm->cboz_block > PGSIZE || (vm->cboz_block & (vm->cboz_block - 1)))
    vm->cboz_block = plat.cboz_block;
  vm->cboz_prev = vm->cboz_block;
  vm->ndev = 0;
  vuart_init(vm);
  vclint_init(vm);
  vic_init(vm);
  vvirtio_init(vm);
  vvnet_init(vm);
  vvcon_init(vm);
  vballoon_init(vm);
  vrtc_init(vm);
  vmachrev_init(vm);    // the anchor, for guests that cannot be told through SSIP
  if (vm->blk.bounce == 0)
    goto fail;

  if (!noload && vm_load(vm) < 0) {
    err = -ENOENT;
    goto fail;
  }

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
  return err;
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
  return gpa < dtb_end && gpa + n > vm->dtb_gpa - 2 * VM_DTB_MAX;   // the boot blob and both revision slots
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
  if (vm->arrival_timeline && !vm->t_resumed)
    vm->t_resumed = r_time();       // L15: the commit was read and the machine let go
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
  while (vm->vic_hold) {
    // the controller model is being rebuilt: stay out until it is done
    release(&vm->lock);
    yield();
    acquire(&vm->lock);
  }
  v->running_hart = cpuid();
  if (vm->arrival_timeline && !vm->t_first_entry) {
    vm->t_first_entry = r_time();   // L15: the guest's first instruction on this host
    uint64 t0 = vm->t_stream_end;
    if (!vm->t_resumed)
      vm->t_resumed = vm->t_first_entry;   // an arrival is started by its first entry, not through vm_resume
    printk("hyp: vm %d: arrival timeline: stream done +0, resumed +%ld us, first instruction +%ld us%s\n",
           vm->id, (long)((vm->t_resumed - t0) / 10), (long)((vm->t_first_entry - t0) / 10),
           vm->revise_acked != vm->revise_gen ? "; a revision is owed" : "");
    if (vm->revise_acked == vm->revise_gen)
      vm->arrival_timeline = 0;     // nothing more to add for this machine
  }
  if (v->ssip_req) {
    v->ssip_req = 0;
    v->vssip = 1;
  }
  if (v->vssip)
    hvip |= HVIP_VSSIP;
  // F5: without Sstc the timer interrupt is this hypervisor's to raise
  // (and F6: a scaled clock has no hardware comparator to use)
  if ((vm->no_sstc || vm_scaled_time(vm)) && v->vstimecmp != ~0UL &&
      (long)(r_time() + vm->htimedelta - v->vstimecmp) >= 0)
    hvip |= HVIP_VSTIP;
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
  // Defect 19. A guest with the AIA CSRs finds *which* interrupt is pending by
  // reading stopi, and for a VS-level guest with no guest interrupt file vstopi
  // shows the external interrupt only when hvictl names it (IID 9). The IMSIC
  // software model wrote hvictl at every entry; the PLIC and APLIC models
  // never did -- xv6 reads scause and never noticed -- so a Linux booted with
  // Smaia/Ssaia in its ISA string and then given a PLIC or an APLIC took
  // cause-9 traps with topi 0 forever (vicprobe5: the model's state was right
  // and the guest never claimed). Named here for every model; the IMSIC path's
  // own write agrees with it, and a guest file (vgein != 0) reports itself.
  if (plat.has_aia_csrs && v->vgein == 0)
    csr_write(CSR_HVICTL, (hvip & HVIP_VSEIP) ? HVICTL_VSEIP : 0);
  // F5: Sstc for this machine or not (STCE); A/D updating always. F6: a
  // machine with another timebase may not read `time' itself (TM off).
  // F21: cbo.zero in hardware only when the block described is the block the
  // host has (and no revision between two sizes is pending); else it traps
  int cboz_native = vm->cboz_block == plat.cboz_block &&
                    (vm->cboz_prev == vm->cboz_block || !vm_revising(vm));
  if (plat_bootarg("nocbozmodel"))
    cboz_native = 1;    // the control: no software model, the hazard shows
  csr_write(CSR_HENVCFG, HENVCFG_ADUE | HENVCFG_CBCFE | HENVCFG_CBIE | (cboz_native ? HENVCFG_CBZE : 0) |
                         ((vm->no_sstc || vm_scaled_time(vm)) ? 0 : HENVCFG_STCE));
  csr_write(CSR_HCOUNTEREN, vm_scaled_time(vm) ? 5 : 7);
  csr_write(CSR_HTIMEDELTA, vm->htimedelta);

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
  if (v->vs_force) {              // the guest ceded its vector unit while this vcpu ran: Off, at this exit's save
    v->vsstatus &= ~SSTATUS_VS_MASK;
    v->vs_force = 0;
  }
  v->vsie = csr_read(CSR_VSIE);
  v->vstvec = csr_read(CSR_VSTVEC);
  v->vsscratch = csr_read(CSR_VSSCRATCH);
  v->vsepc = csr_read(CSR_VSEPC);
  v->vscause = csr_read(CSR_VSCAUSE);
  v->vstval = csr_read(CSR_VSTVAL);
  v->vsatp = csr_read(CSR_VSATP);
  v->vstimecmp = csr_read(CSR_VSTIMECMP);
  v->vssip = (csr_read(CSR_HVIP) & HVIP_VSSIP) ? 1 : 0;
  // Has this guest used its vector unit? Nothing traps when it does -- the unit
  // is enabled and a vector instruction is just an instruction -- so there is no
  // exit to hook, which is why OBS_VECTOR_USED went unset until now and the
  // observation set said `vector' of a guest that had run vecbench.
  //
  // The hardware has already answered it, and the line above has just read
  // vsstatus back on an exit that was happening anyway -- so the cost is a mask
  // and a compare on a value already in a register.
  //
  // Dirty, specifically, and not "Clean or better", which is what this tested
  // first and why every guest looked like a vector user. A guest kernel that
  // supports vectors marks the unit Clean on every return to userspace for any
  // task that has a save area, and in xv6 that is every task (trap.c does
  // exactly this). So Clean says "this kernel was built with vectors", which is
  // not the question. Dirty says the register file has been *written* since it
  // was last marked clean, which is the question -- and the guest's own
  // vec_restore() runs under Initial and sets Clean afterwards, so the kernel
  // saving and restoring state does not raise it.
  //
  // This is the third time in this work that "is the unit in use?" has been
  // asked with the wrong signal: `vstate != 0' is every task, register contents
  // are zero for a program that computed zeros, and VS >= Clean is every kernel
  // that supports the unit.
  //
  // And Dirty is a *lower bound*, not an answer. It is a transient: the guest's
  // own trap handler saves the register file and sets the unit back to Clean, so
  // whether the host ever samples a Dirty depends on when its exits happen to
  // fall. The same guest running the same vecbench sets this on some runs and not
  // others. When it fires the guest has certainly used the unit; when it does not,
  // nothing follows. So it is reported, and no decision is made on it -- migration
  // legality asks the guest instead (see vmstate.c).
  if ((v->vsstatus & SSTATUS_VS_MASK) == SSTATUS_VS_DIRTY)
    vm_observe(vm, OBS_VECTOR_USED);
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
  while (!(v->vssip || v->ssip_req || vic_seip(v) || r_time() + v->vm->htimedelta >= v->vstimecmp ||
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
    int busy = 0, on = 0;
    for (int j = 0; j < vm->nvcpu; j++) {
      struct vcpu *v = &vm->vcpu[j];
      if (v->idle && now + vm->htimedelta >= v->vstimecmp)
        wakeup(v);
      else if ((vm->no_sstc || vm_scaled_time(vm)) && !v->idle && v->running_hart >= 0 &&
               v->vstimecmp != ~0UL && now + vm->htimedelta >= v->vstimecmp)
        vcpu_kick(v);     // F5/F6: out, and back in with VSTIP raised
      if (!v->idle && v->hsm_state == HSM_STARTED)
        busy++;
      if (v->running_hart >= 0)
        on++;
    }
    // L6: how often a machine's busy vcpus are actually on harts together
    if (busy >= 2) {
      vm->corun_busy++;
      if (on >= 2)
        vm->corun_both++;
    }
    // L1: an announcement is at-least-once. The SSIP that carries it can be
    // consumed by the guest's IPI handler in the same trap as a real IPI, and
    // a guest asked on arrival may be asked before its first instruction has
    // run; a repeat is harmless (the guest compares generations) and cheap
    // (one bit per vcpu every two seconds while unacknowledged). Two arrivals
    // in ~6000 never acknowledged and left no evidence; the repeat is either
    // the fix or, counted, the evidence.
    // ... and only while the guest has not started on it: a guest that has
    // read describe() is inside its own handler, and a second SSIP there is
    // a re-entry into its lock (m8x: snap-migrate-vlen stuck at the acquire,
    // "described yes, repeated 10 times").
    if (vm->revise_capable && vm->revise_acked != vm->revise_gen &&
        vm->revise_described != vm->revise_gen &&
        now - vm->revise_asked > 2 * HOST_TIMEBASE &&
        now - vm->revise_repeat_at > 2 * HOST_TIMEBASE) {
      acquire(&vm->lock);
      vm->revise_repeat_at = now;
      vm->revise_repeats++;
      for (int j = 0; j < vm->nvcpu; j++)
        vm->vcpu[j].ssip_req = 1;
      vmachrev_notify(vm);
      release(&vm->lock);
      for (int j = 0; j < vm->nvcpu; j++)
        vcpu_kick(&vm->vcpu[j]);
      if (now - vm->revise_asked > 20 * HOST_TIMEBASE && !vm->revise_wd_printed) {
        vm->revise_wd_printed = 1;
        printk("hyp: vm %d: revision %d not acknowledged for 20 s (described %s, repeated %d times; lines %s, held 0x%lx)\n",
               vm->id, vm->revise_gen, vm->revise_described == vm->revise_gen ? "yes" : "no",
               vm->revise_repeats, vm->irq_alt ? "alt" : "default",
               (unsigned long)(vm->vic_kind == VIC_PLIC ? vm->plic.level : vm->aplic.level));
        for (int j = 0; j < vm->nvcpu; j++) {
          struct vcpu *v = &vm->vcpu[j];
          printk("hyp: vm %d:   vcpu %d pc 0x%lx idle %d hart %d ssip_req %d vssip %d hsm %d\n",
                 vm->id, j, v->pc, v->idle, v->running_hart, v->ssip_req, v->vssip, v->hsm_state);
        }
      }
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
  uint64 now = r_time();
  for (int i = 0; i < NVM; i++) {
    struct vm *vm = vmtab[i];
    if (vm == 0 || vm->state != VM_RUNNING)
      continue;
    for (int j = 0; j < vm->nvcpu; j++) {
      struct vcpu *v = &vm->vcpu[j];
      if (!(v->idle || vm->no_sstc || vm_scaled_time(vm)) || v->hsm_state != HSM_STARTED ||
          v->vstimecmp == ~0UL)
        continue;
      // in host time: the guest's clock is the host's plus htimedelta
      uint64 d = v->vstimecmp - vm->htimedelta;
      // A deadline already passed is not a reason to set the timer: the tick
      // that found it woke or kicked the vcpu, and VSTIP goes in on its
      // entry. Re-arming for it set the host's timer 10 us out, again and
      // again, until the guest's handler had written a new deadline -- and
      // the guest, interrupted every 10 us, took seconds to get there (tb1:
      // `sleep 1' = 2.9 s, then 4.6 s, on a 1 MHz machine; Linux on an
      // SBI-timer host crawling through its boot).
      if ((long)(d - now) <= 0)
        continue;
      if (d < best)
        best = d;
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
