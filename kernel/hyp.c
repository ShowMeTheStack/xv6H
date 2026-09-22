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
void
vm_unprotect_page(struct vm *vm, uint64 gpa, uint64 frame)
{
  uint64 off = (gpa - G_RAM_BASE) & ~(uint64)(PGSIZE - 1);
  int k = off / HCHUNK, i = (off % HCHUNK) / PGSIZE;

  acquire(&vm->memlock);
  uint64 c = vm->chunk[k];
  if (c && gc_split(c) && gc_pages(c)[i] == frame) {
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

// a store to a page this VM maps read-only. if it is shared, the VM is
// given a copy of its own; if it was only write-protected while it was
// being compared, it becomes writable again. returns 1 when something
// was done, 0 when the page was already writable (another hart got
// there first), -1 when it is not a page this can help with.
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
        *pte = PA2PTE(fresh) | GPTE_LEAF;
        old = frame;
        fresh = 0;
        r = 1;
      }
    } else {
      // nobody else has it: writing again is all it takes
      pg->flags &= ~(uint16)PG_KSM;
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
int
vm_create(struct vmconfig *cfg)
{
  if (!plat.has_h)
    return -1;
  int nvcpu = cfg->nvcpu;
  uint64 mem_bytes = (uint64)cfg->mem_mb * 1024 * 1024;
  if (nvcpu < 1 || nvcpu > NVCPU)
    return -1;
  if (mem_bytes < 8 * HCHUNK || (mem_bytes % HCHUNK) != 0 || mem_bytes > VM_MAXMEM)
    return -1;
  if (cfg->kernel[0] == 0)
    return -1;

  // a slot: a VM object is allocated the first time its slot is used and
  // kept for reuse (struct vm is large: vcpus, interrupt controller
  // state, console rings).
  acquire(&vmslock);
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
      nv->id = i;
      nv->state = VM_FREE;
      vmtab[i] = nv;
    }
    if (vmtab[i]->state == VM_FREE) {
      vm = vmtab[i];
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
  vm->cfg = *cfg;
  vm->nvcpu = nvcpu;
  vm->ram_size = mem_bytes;
  vm->nchunks = mem_bytes / HCHUNK;
  vm->resident = 0;
  vm->prefer_huge = (cfg->flags & VMF_NOHUGE) == 0;
  vm->mem_limit = (uint64)cfg->mem_limit_mb * 1024 * 1024;
  if (vm->mem_limit > mem_bytes)
    vm->mem_limit = 0;   // a limit above the memory it has is no limit

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
  vballoon_init(vm);
  vrtc_init(vm);
  if (vm->blk.bounce == 0)
    goto fail;

  if (vm_load(vm) < 0)
    goto fail;

  for (int i = 0; i < nvcpu; i++) {
    if (plat.has_v && (vm->vcpu[i].vstate = kalloc()) == 0)
      goto fail;
    vcpu_reset(&vm->vcpu[i], vm, i);
  }

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
  release(&vm->lock);
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
  if (vm->vcpus_running == 0) {
    // nobody is inside vm_run: free right here.
    vm_free_resources(vm);
    struct inode *ip = vm->disk_ip;
    struct gendisk *d = vm->disk;
    vm->disk_ip = 0;
    vm->disk = 0;
    vm->state = VM_FREE;
    wakeup(&vm->cons_out);
    release(&vm->lock);
    vm_put_disk(ip);
    vm_put_device(d);
    return 0;
  }
  vm->state = VM_HALTING;
  wakeup((void *)&vm->state);
  for (int i = 0; i < vm->nvcpu; i++)
    vcpu_kick(&vm->vcpu[i]);
  release(&vm->lock);
  return 0;
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

  int hart = cpuid();
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
}

// the guest executed wfi: sleep until something is pending for it.
static void
vcpu_idle(struct vcpu *v)
{
  struct vm *vm = v->vm;
  acquire(&vm->lock);
  while (!(v->vssip || v->ssip_req || vic_seip(v) || r_time() >= v->vstimecmp ||
           vm->state != VM_RUNNING || killed(v->p))) {
    v->idle = 1;
    v->wfi_sleeps++;
    sleep_prepare(v);
    release(&vm->lock);
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

static int
handle_exit(struct vcpu *v)
{
  uint64 scause = r_scause();
  uint64 stval = r_stval();
  uint64 htval = csr_read(CSR_HTVAL);
  v->last_cause = scause;
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
    if (t == 0) {
      // the page is there. a store that still faulted is a store to a
      // page mapped read-only because it is shared, and the writer
      // gets a copy of its own (see vm_unshare).
      if (scause == EXC_STORE_GPF)
        vm_unshare(v->vm, gpa);
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
    printk("hyp: vm %d vcpu %d: instruction fetch from unmapped gpa 0x%lx (pc 0x%lx)\n",
           v->vm->id, v->id, (htval << 2) | (stval & 3), v->pc);
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
  vm->vcpus_running++;
  vm->state = VM_RUNNING;
  release(&vm->lock);

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
  acquire(&vm->lock);
  v->p = 0;
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
    wakeup(&vm->cons_out); // an attached console reader sees the end
    release(&vm->lock);
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
