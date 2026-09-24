//
// same-page merging: two guests running the same thing hold mostly the
// same bytes, and the host need only keep one copy.
//
// a thread walks the guest memory of every VM, hashes each 4 KB page,
// and when two pages hash alike compares them properly. Equal pages
// become one frame that both guests map read-only; the first write to
// either faults, and the writer is given a private copy (see
// vm_unshare). Nothing a guest can do is observable except through
// timing: it reads what it wrote, and what it did not write does not
// change under it.
//
// what makes it safe is the order. A page is write-protected before it
// is compared, and every vcpu of the VM is made to throw away its old
// translation before the comparison is believed; otherwise a guest
// could change a page between the comparison and the merge, and the
// two guests would share a frame that only one of them agrees with.
//
// only page-granular chunks can be merged: a megapage is one mapping
// of two megabytes, and sharing one page of it means splitting it
// first, which is what gstage_split is for. That is worth doing when
// the pages in it are shareable and a waste when they are not, so a
// chunk is split only once a page in it has actually matched.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "page.h"
#include "platform.h"
#include "hyp.h"

#define KSM_BUCKETS 1024
#define KSM_NODES   8192          // pages remembered at once
#define KSM_SCAN    512           // pages looked at in one pass

struct ksm_node {
  struct ksm_node *next;
  uint32 hash;
  int vmid;
  uint32 chunk;
  uint16 page;
  uint16 used;
};

static struct {
  struct spinlock lock;
  int on;
  struct ksm_node *bucket[KSM_BUCKETS];
  struct ksm_node nodes[KSM_NODES];
  int nextnode;
  uint32 chunk;   // where the scan is, the same index in every VM
  uint64 scanned, merged, shared, collisions;
} ksm;

void
ksm_init(void)
{
  initlock(&ksm.lock, "ksm");
}

// FNV-1a over the page. a hash is only a way of not comparing every
// page with every other one; equality is decided by memcmp.
static uint32
page_hash(const char *p)
{
  uint32 h = 2166136261u;

  for (int i = 0; i < PGSIZE; i += 8) {
    uint64 v = *(const uint64 *)(p + i);
    h = (h ^ (uint32)v) * 16777619u;
    h = (h ^ (uint32)(v >> 32)) * 16777619u;
  }
  return h;
}

// remember this page under its hash, replacing the oldest entry when
// the table is full. the table is a hint: an entry may name a page
// that has changed or a VM that has gone, and every use re-checks.
static void
remember(uint32 hash, int vmid, uint32 chunk, int page)
{
  struct ksm_node *n = &ksm.nodes[ksm.nextnode];

  if (n->used) {
    struct ksm_node **pp = &ksm.bucket[n->hash % KSM_BUCKETS];
    while (*pp && *pp != n)
      pp = &(*pp)->next;
    if (*pp)
      *pp = n->next;
  }
  ksm.nextnode = (ksm.nextnode + 1) % KSM_NODES;
  n->hash = hash;
  n->vmid = vmid;
  n->chunk = chunk;
  n->page = page;
  n->used = 1;
  n->next = ksm.bucket[hash % KSM_BUCKETS];
  ksm.bucket[hash % KSM_BUCKETS] = n;
}

// is this page still the one the table remembers, and is it worth
// looking at? a page of a VM that has gone, or one that is not backed,
// is simply forgotten.
static uint64
frame_of(struct vm *vm, uint32 k, int i)
{
  uint64 c;

  if (vm == 0 || k >= (uint32)vm->nchunks)
    return 0;
  c = vm->chunk[k];
  if (c == 0)
    return 0;
  // a chunk that is still one megapage has the page too; try_merge is
  // what splits it, and only once the page has actually matched
  return gc_split(c) ? gc_pages(c)[i] : c + (uint64)i * PGSIZE;
}

static uint64
gpa_of(uint32 k, int i)
{
  return G_RAM_BASE + (uint64)k * HCHUNK + (uint64)i * PGSIZE;
}

// two pages hashed alike. write-protect both, compare them properly,
// and if they are equal make the second one point at the first.
// returns 1 if they were merged.
static int
try_merge(struct vm *a, uint32 ka, int ia, struct vm *b, uint32 kb, int ib)
{
  uint64 fa, fb;
  int merged = 0;

  if (a == b && ka == kb && ia == ib)
    return 0;
  // in VM id order, so two scans can never take them the other way round
  if (a->id > b->id) {
    struct vm *tv = a; a = b; b = tv;
    uint32 tk = ka; ka = kb; kb = tk;
    int ti = ia; ia = ib; ib = ti;
  }

  fa = vm_protect_page(a, gpa_of(ka, ia));
  if (fa == 0)
    return 0;
  fb = vm_protect_page(b, gpa_of(kb, ib));
  if (fb == 0) {
    vm_unprotect_page(a, gpa_of(ka, ia), fa);
    return 0;
  }

  // both are write-protected and every vcpu of both has been made to
  // forget the old translation, so what they hold now is what they
  // will hold until one of them faults on a write
  if (fa != fb && memcmp((void *)fa, (void *)fb, PGSIZE) == 0) {
    if (vm_share_page(b, gpa_of(kb, ib), fa) == 0) {
      merged = 1;
      acquire(&ksm.lock);
      ksm.merged++;
      ksm.shared++;
      release(&ksm.lock);
    }
  } else {
    acquire(&ksm.lock);
    ksm.collisions++;
    release(&ksm.lock);
  }

  // b's page is either shared now or was never merged; either way the
  // reference this took goes back. a's stays read-only while it is
  // shared, which vm_unprotect_page knows.
  vm_unprotect_page(b, gpa_of(kb, ib), fb);
  vm_unprotect_page(a, gpa_of(ka, ia), fa);
  return merged;
}

// look at one chunk: hash every page that is backed, try the pages
// that hashed the same before, and remember this one.
//
// a chunk that is still a megapage is hashed through vm_copyin without
// being split: splitting two megabytes to share nothing would be a bad
// trade, and try_merge splits only once a page has actually matched.
static int
ksm_scan_chunk(struct vm *vm, uint32 k)
{
  static char scratch[PGSIZE] __attribute__((aligned(PGSIZE)));
  int looked = 0;

  // the table can go while this is walking it: a machine may be
  // destroyed at any moment, and vm_free_resources frees the chunks and
  // then the table itself. What is read here is re-read through
  // vm_copyin under the lock that protects it, which answers "not
  // backed" for a machine whose memory has gone; the two reads below are
  // hints, and are ordered so that a null table is never indexed.
  if (vm->chunk == 0 || k >= (uint32)vm->nchunks || vm->chunk[k] == 0)
    return 0;
  for (int i = 0; i < HCHUNK_PAGES; i++) {
    uint64 gpa = gpa_of(k, i);
    uint64 *tab = vm->chunk;
    uint64 c = tab ? tab[k] : 0;

    if (c == 0)
      break;
    if (gc_split(c) && gc_pages(c)[i] == 0)
      continue;
    if (vm_copyin(vm, scratch, gpa, PGSIZE) < 0)
      continue;
    looked++;
    uint32 h = page_hash(scratch);

    acquire(&ksm.lock);
    ksm.scanned++;
    struct ksm_node *n = ksm.bucket[h % KSM_BUCKETS];
    int cvm = -1;
    uint32 ck = 0;
    int ci = 0;
    while (n) {
      if (n->hash == h && !(n->vmid == vm->id && n->chunk == k && n->page == i)) {
        cvm = n->vmid;
        ck = n->chunk;
        ci = n->page;
        break;
      }
      n = n->next;
    }
    release(&ksm.lock);

    if (cvm >= 0) {
      struct vm *other = vm_lookup(cvm);
      if (other && other->state == VM_RUNNING && frame_of(other, ck, ci))
        try_merge(other, ck, ci, vm, k, i);
    }
    acquire(&ksm.lock);
    remember(h, vm->id, k, i);
    release(&ksm.lock);
  }
  return looked;
}

// one pass of the scan. returns how many pages were looked at.
//
// the order is chunk by chunk across every VM, not VM by VM. Two
// guests running the same thing put the same bytes at about the same
// guest-physical address, so the page that matches this one is most
// likely the one at the same place in another VM -- and a table that
// remembered a whole VM before it reached the next would have
// forgotten the first by then. This is the cheap half of what Linux's
// KSM gets from a content-indexed tree, and it is enough for the case
// that matters: several guests of the same image.
static int
scan_pass(void)
{
  int looked = 0;

  while (looked < KSM_SCAN) {
    int any = 0;
    for (int id = 0; id < NVM; id++) {
      struct vm *vm = vm_lookup(id);
      if (vm == 0 || vm->state != VM_RUNNING ||
          ksm.chunk >= (uint32)vm->nchunks)
        continue;
      any = 1;
      looked += ksm_scan_chunk(vm, ksm.chunk);
    }
    ksm.chunk++;
    if (!any) {
      ksm.chunk = 0;   // past the end of every VM: round again
      break;
    }
  }
  return looked;
}

static void
ksm_thread(void *arg)
{
  for (;;) {
    for (int i = 0; i < 5; i++) {   // twice a second
      acquire(&tickslock);
      sleep_prepare(&ticks);
      release(&tickslock);
      sleep();
    }
    if (!ksm.on)
      continue;
    scan_pass();
  }
}

int
ksm_set(int on)
{
  static int started;

  ksm.on = on != 0;
  if (ksm.on && !started) {
    started = 1;
    if (kthread_create(ksm_thread, 0, "ksmd") < 0) {
      ksm.on = 0;
      started = 0;
      return -1;
    }
  }
  return 0;
}

void
ksm_stats(uint64 *out, int n)
{
  acquire(&ksm.lock);
  if (n > 0) out[0] = ksm.on;
  if (n > 1) out[1] = ksm.scanned;
  if (n > 2) out[2] = ksm.merged;
  if (n > 3) out[3] = ksm.shared;
  release(&ksm.lock);
}
