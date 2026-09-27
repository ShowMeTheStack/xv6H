//
// save and restore a machine (§9).
//
// A saved machine is a file the *user* opened: `vmm save' creates it and
// hands the descriptor over, and the kernel fills it. That way the file
// belongs to whoever asked for it rather than to root, and this code does
// not need a second copy of what open(2) does.
//
// What is written is the guest's own state and nothing of the host's. The
// registers and the VS CSRs of every vcpu, the emulated devices'
// registers, the guest's clock, and the pages of guest memory that are
// backed and not all zeroes -- each named by its guest physical address,
// so that a restore can put it wherever the host has room. What is *not*
// written is everything the host makes again: the G-stage page tables,
// the chunk table, the host processes that run the vcpus, the scheduler
// group, the switch port, the disk's inode. A restored machine is a new
// machine of the host's, running a guest that cannot tell.
//
// A save happens with the machine paused and drained: its vcpus are
// stopped at the boundary between one entry to the guest and the next
// (kernel/sched.c's freeze_point), where the state in `struct vcpu' is
// the state the guest has -- inside an entry it is in the hardware's CSRs
// instead, and reading it from here would be reading yesterday's copy.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "defs.h"
#include "errno.h"
#include "platform.h"
#include "vmm.h"
#include "hyp.h"
#include "vmstate.h"

// one vcpu, as the file holds it. The host's parts of struct vcpu are not
// here: which hart it last ran on, which host process is inside vm_run,
// how many exits it has taken. A hardware guest interrupt file is the
// host's too -- what is saved is the software model, and a restored vcpu
// asks for a file of its own when it next enters.
struct vmstate_vcpu {
  uint64 regs[32];
  uint64 pc;
  uint64 vsstatus, vsie, vstvec, vsscratch, vsepc, vscause, vstval, vsatp;
  uint64 vstimecmp;
  uint64 scounteren, senvcfg, vsiselect;
  uint64 fregs[32];
  uint64 fcsr;
  uint64 vcsrs[4];
  uint64 m_mstatus, m_mepc, m_medeleg, m_mideleg, m_mie, m_mtvec, m_mscratch;
  uint64 m_menvcfg, m_mcounteren, m_pmpaddr0, m_pmpcfg0;
  struct vimsic imsic;
  int priv;
  int hsm_state;
  int in_mmode;
  int vssip;
  int has_vstate;          // a page of vector registers follows
  int pad;
};

// and the devices. Every one of these is plain data; the parts of the
// models that are the host's -- a sleeplock, a bounce page, the thread
// that works the block queue -- are left out and made again.
struct vmstate_dev {
  struct vuart uart;
  struct vplic plic;
  struct vaplic aplic;
  struct vvnet vnet;
  struct vballoon balloon;
  uint32 clint_msip[NVCPU];
  uint64 rtc_latch;
  // the block device's queue and registers, without the host's parts
  struct vq_state blk_q;
  uint64 blk_features, blk_capacity;
  uint32 blk_status, blk_intr_status, blk_queue_sel;
  uint32 blk_dfs, blk_ffs;
  uint64 dtb_gpa;
  uint64 entry;
};

// the configuration, and the shape of the machine the file describes.
//
// `vic_kind' and `obs' are what make a machine movable to a host of a different
// shape. The kind says what the guest is *currently* bound to, so a destination
// that gives it something else knows a revision is owed; `obs' says what the
// guest has actually looked at, which is what decides whether this destination
// is allowed to have it at all. A guest that has never executed a vector
// instruction may land on a machine with no vector unit; one that has may not.
struct vmstate_cfg {
  struct vmconfig cfg;
  uint64 ram_size;
  int nvcpu;
  int vic_kind;
  int imsic_hw;
  int prefer_huge;
  uint64 mem_limit;
  uint32 obs;             // OBS_* : what the guest has observed about itself
  uint32 vlenb;           // the vector register width it was given (0: none)
  // The revision generation belongs to the *machine*, not to the host, and this
  // is why. The guest remembers the last generation it applied, and that memory
  // travels with it. The host's counter, if it started from zero on every host,
  // would hand a migrated guest "revision 1" for the second time -- and the
  // guest, which already applied a revision 1, would correctly conclude it was
  // up to date and ignore it. A guest that had been revised once on its way
  // could then never be revised again on arrival. Carried, so the sequence
  // continues across hosts.
  uint32 revise_gen;
  uint64 mmio_shift;      // where its virtio block sat (the layout axis)
  // bit 0: the guest has given the vector unit up (vm->vec_revoked). It matters
  // for legality and it is not the same question as `has it ever used one'. A
  // guest that used the unit and then *accepted* a revocation is holding no
  // state in it -- the guest itself refused the revocation if any task was, and
  // this bit is only set because it agreed. So it may move to a host with no
  // unit, even though `vector' is in its observation set for the rest of its
  // life. That is the negotiation being worth something: a machine that could
  // not move becomes one that can, because the guest gave something up.
#define VMSTATE_F_VEC_REVOKED 1u
  // bit 1: the guest has spoken the revision protocol. Without it, a machine
  // arriving at a host of another shape keeps the description it left with --
  // the controller it was bound to, as a software model -- because a guest that
  // has never asked about revisions would never read a new one and would be left
  // deaf. That is the legacy tier: correct anywhere, paying emulation exits. A
  // guest that has asked is revised to the host's native controller instead.
#define VMSTATE_F_CAPABLE 2u
  uint32 flags2;
};

_Static_assert(sizeof(struct vmstate_dev) <= PGSIZE,
               "the device record no longer fits in a page");

// what save and restore need somewhere other than the stack. A kernel
// stack is 32 KB, sixty-four vcpus of saved state are sixty-four
// kilobytes, and two console rings are sixteen more: everything here is
// one page, allocated for the length of the call.
struct vmstate_work {
  struct vmstate_cfg cfg;
  struct vmstate_time tm;
  struct vmstate_vcpu vs;
  struct vmstate_page ph;
};

_Static_assert(sizeof(struct vmstate_work) <= PGSIZE,
               "the working record no longer fits in a page");

// ---- writing ----
//
// one offset, carried through. Each of these returns 0 or -errno; the
// first failure ends the save, and what has been written is left for the
// caller to unlink.
// where the sections go: an inode at an offset, or something already open
// (a socket). The offset is kept either way, because the header carries
// the length and a save to a file still says how long it is.
struct sink {
  struct inode *ip;
  struct file *f;
  uint off;
};

static int
put(struct sink *s, const void *buf, uint n)
{
  int r;

  if (s->f) {
    // a connection: what is written is written, and there is no seeking
    // back. The socket layer is called rather than filewrite, because
    // what is being handed over is a kernel address and filewrite's is a
    // user one.
    if (s->f->type != FD_SOCK || s->f->sock == 0)
      return -EINVAL;
    r = sockwrite(s->f->sock, 0, (uint64)buf, (int)n);
  } else {
    ilock(s->ip);
    r = writei(s->ip, 0, (uint64)buf, s->off, n);
    iunlock(s->ip);
  }
  if (r != (int)n)
    return -EIO;
  s->off += n;
  return 0;
}

static int
put_sec(struct sink *s, const char *name, const void *buf, uint64 len)
{
  struct vmstate_sec sec;

  memset(&sec, 0, sizeof(sec));
  safestrcpy(sec.name, name, sizeof(sec.name));
  sec.len = len;
  int r = put(s, &sec, sizeof(sec));
  if (r < 0 || len == 0)
    return r;
  return put(s, buf, (uint)len);
}

// What the other end has, page by page: the checksum of the page as it
// was sent. The last round is a comparison against this rather than a
// list of pages somebody remembered to mark -- see precopy_verify.
static uint32
pcs(const char *p)
{
  const uint32 *q = (const uint32 *)p;
  uint32 h = 2166136261u;

  for (int i = 0; i < (int)(PGSIZE / 4); i++)
    h = (h ^ q[i]) * 16777619u;
  return h ? h : 1;
}

// The same hash, with the accessed and dirty bits taken out of anything that
// looks like a page-table entry: for every 8-byte word whose bit 0 (V) is set,
// bits 6 (A) and 7 (D) are cleared before hashing.
//
// This exists to answer one question and it is the question the whole paper
// turns on. A page whose raw checksum has changed but whose masked checksum has
// not changed *only in the A/D bits of PTE-shaped words* -- which is the
// signature of the MMU maintaining the guest's page tables behind the
// hypervisor's back, and not of any writer this kernel could have hooked.
//
// Only computed when `admeasure=1' is on the host's command line: it is a
// second pass over every page of every round, which a migration should not pay
// for a measurement.
#define PTE_V_BIT 1UL
#define PTE_AD_BITS 0xc0UL       // bit 6 (A) and bit 7 (D)

static uint32
pcs_masked(const char *p)
{
  const uint64 *w = (const uint64 *)p;
  uint32 h = 2166136261u;

  for (int i = 0; i < (int)(PGSIZE / 8); i++) {
    uint64 v = w[i];
    if (v & PTE_V_BIT)
      v &= ~PTE_AD_BITS;
    uint32 lo = (uint32)v, hi = (uint32)(v >> 32);
    h = (h ^ lo) * 16777619u;
    h = (h ^ hi) * 16777619u;
  }
  return h ? h : 1;
}

// admeasure=1 on the host's command line (see procinit_early for the pattern).
static int admeasure = -1;

static int
ad_measuring(void)
{
  if (admeasure < 0) {
    const char *s = plat.bootargs;
    admeasure = 0;
    while (*s) {
      while (*s == ' ')
        s++;
      if (memcmp(s, "admeasure=", 10) == 0 && s[10] == '1')
        admeasure = 1;
      while (*s && *s != ' ')
        s++;
    }
  }
  return admeasure;
}

// norevise=1 on the host's command line: take a machine of another shape and
// say nothing. This exists to be *measured against*. The claim of this work is
// that a guest which is told its machine changed shape keeps running, and a
// claim like that is worth nothing without the control -- what happens to the
// same guest, on the same two hosts, over the same wire, when the revision is
// withheld. Same pattern as admeasure=1 above; default off, so an ordinary host
// always tells the truth.
static int norevise = -1;

static int
revise_muted(void)
{
  if (norevise < 0)
    norevise = plat_bootarg("norevise=1");
  return norevise;
}

// is this page worth writing down?
static int
page_is_zero(const char *p)
{
  const uint64 *q = (const uint64 *)p;

  for (int i = 0; i < (int)(PGSIZE / sizeof(uint64)); i++)
    if (q[i])
      return 0;
  return 1;
}

static void
save_vcpu(struct vcpu *v, struct vmstate_vcpu *o)
{
  memset(o, 0, sizeof(*o));
  memmove(o->regs, v->regs, sizeof(o->regs));
  o->pc = v->pc;
  o->vsstatus = v->vsstatus;
  o->vsie = v->vsie;
  o->vstvec = v->vstvec;
  o->vsscratch = v->vsscratch;
  o->vsepc = v->vsepc;
  o->vscause = v->vscause;
  o->vstval = v->vstval;
  o->vsatp = v->vsatp;
  o->vstimecmp = v->vstimecmp;
  o->scounteren = v->scounteren;
  o->senvcfg = v->senvcfg;
  o->vsiselect = v->vsiselect;
  memmove(o->fregs, v->fregs, sizeof(o->fregs));
  o->fcsr = v->fcsr;
  memmove(o->vcsrs, v->vcsrs, sizeof(o->vcsrs));
  o->m_mstatus = v->m_mstatus;
  o->m_mepc = v->m_mepc;
  o->m_medeleg = v->m_medeleg;
  o->m_mideleg = v->m_mideleg;
  o->m_mie = v->m_mie;
  o->m_mtvec = v->m_mtvec;
  o->m_mscratch = v->m_mscratch;
  o->m_menvcfg = v->m_menvcfg;
  o->m_mcounteren = v->m_mcounteren;
  o->m_pmpaddr0 = v->m_pmpaddr0;
  o->m_pmpcfg0 = v->m_pmpcfg0;
  o->imsic = v->imsic;
  o->imsic.hwfile = 0;       // the host's: asked for again on entry
  o->imsic.hwhart = -1;
  o->priv = v->priv;
  o->hsm_state = v->hsm_state;
  o->in_mmode = v->in_mmode;
  o->vssip = v->vssip;
  o->has_vstate = v->vstate != 0;
}

static void
restore_vcpu(struct vcpu *v, const struct vmstate_vcpu *o)
{
  memmove(v->regs, o->regs, sizeof(v->regs));
  v->pc = o->pc;
  v->vsstatus = o->vsstatus;
  v->vsie = o->vsie;
  v->vstvec = o->vstvec;
  v->vsscratch = o->vsscratch;
  v->vsepc = o->vsepc;
  v->vscause = o->vscause;
  v->vstval = o->vstval;
  v->vsatp = o->vsatp;
  v->vstimecmp = o->vstimecmp;
  v->scounteren = o->scounteren;
  v->senvcfg = o->senvcfg;
  v->vsiselect = o->vsiselect;
  memmove(v->fregs, o->fregs, sizeof(v->fregs));
  v->fcsr = o->fcsr;
  memmove(v->vcsrs, o->vcsrs, sizeof(v->vcsrs));
  v->m_mstatus = o->m_mstatus;
  v->m_mepc = o->m_mepc;
  v->m_medeleg = o->m_medeleg;
  v->m_mideleg = o->m_mideleg;
  v->m_mie = o->m_mie;
  v->m_mtvec = o->m_mtvec;
  v->m_mscratch = o->m_mscratch;
  v->m_menvcfg = o->m_menvcfg;
  v->m_mcounteren = o->m_mcounteren;
  v->m_pmpaddr0 = o->m_pmpaddr0;
  v->m_pmpcfg0 = o->m_pmpcfg0;
  int file = v->imsic.hwfile, hart = v->imsic.hwhart;
  v->imsic = o->imsic;
  v->imsic.hwfile = file;    // whatever this host has given it
  v->imsic.hwhart = hart;
  v->priv = o->priv;
  v->hsm_state = o->hsm_state;
  v->in_mmode = o->in_mmode;
  v->vssip = o->vssip;
}

static void
save_dev(struct vm *vm, struct vmstate_dev *d)
{
  memset(d, 0, sizeof(*d));
  d->uart = vm->uart;
  d->plic = vm->plic;
  d->aplic = vm->aplic;
  d->vnet = vm->vnet;
  d->balloon = vm->balloon;
  for (int i = 0; i < NVCPU; i++)
    d->clint_msip[i] = vm->clint_msip[i];
  d->rtc_latch = vm->rtc_latch;
  d->blk_q.num = vm->blk.queue_num;
  d->blk_q.ready = vm->blk.queue_ready;
  d->blk_q.desc = vm->blk.desc;
  d->blk_q.avail = vm->blk.avail;
  d->blk_q.used = vm->blk.used;
  d->blk_q.last_avail = vm->blk.last_avail;
  d->blk_features = vm->blk.driver_features;
  d->blk_capacity = vm->blk.capacity;
  d->blk_status = vm->blk.status;
  d->blk_intr_status = vm->blk.intr_status;
  d->blk_queue_sel = vm->blk.queue_sel;
  d->blk_dfs = vm->blk.device_features_sel;
  d->blk_ffs = vm->blk.driver_features_sel;
  d->dtb_gpa = vm->dtb_gpa;
  d->entry = vm->entry;
}

static void
restore_dev(struct vm *vm, const struct vmstate_dev *d, int src_kind)
{
  vm->uart = d->uart;
  vm->plic = d->plic;
  vm->aplic = d->aplic;
  // A machine that arrives on a host of a different shape gets that host's
  // interrupt controller, not the one it left. The register state just copied
  // in belongs to a model this machine no longer has, and translating it would
  // be copying something the guest is about to overwrite anyway: it
  // re-enumerates when it adopts the new description, and re-enables the
  // sources it cares about itself.
  //
  // What cannot be re-derived by the guest is which lines the *host's* devices
  // are currently holding down, because that is the host's state and not the
  // guest's. Every virtual device here is level-triggered, so that set is a
  // faithful summary of what the devices want, independent of any controller --
  // which is the whole reason a controller can be exchanged under a running
  // guest at all. Take it from whichever model was live on the source and
  // re-assert it into the one this host has built.
  if (src_kind != vm->vic_kind && !revise_muted()) {
    uint64 levels = (src_kind == VIC_PLIC) ? d->plic.level : d->aplic.level;
    acquire(&vm->lock);
    vic_levels_restore(vm, levels);
    release(&vm->lock);
    printk("hyp: vm %d: arrived as %s, this host gives %s: %d line(s) still held\n",
           vm->id, vic_kindname(src_kind), vic_kindname(vm->vic_kind),
           vic_held(levels));
  }
  // the network device keeps the hardware address this host gave it: the
  // switch knows the machine by that, and a saved one may be restored
  // beside the machine it was saved from.
  uint8 mac[6];
  memmove(mac, vm->vnet.mac, sizeof(mac));
  int present = vm->vnet.present;
  vm->vnet = d->vnet;
  memmove(vm->vnet.mac, mac, sizeof(mac));
  vm->vnet.present = present;
  int bpresent = vm->balloon.present;
  vm->balloon = d->balloon;
  vm->balloon.present = bpresent;
  for (int i = 0; i < NVCPU; i++)
    vm->clint_msip[i] = d->clint_msip[i];
  vm->rtc_latch = d->rtc_latch;
  vm->blk.queue_num = d->blk_q.num;
  vm->blk.queue_ready = d->blk_q.ready;
  vm->blk.desc = d->blk_q.desc;
  vm->blk.avail = d->blk_q.avail;
  vm->blk.used = d->blk_q.used;
  vm->blk.last_avail = d->blk_q.last_avail;
  vm->blk.driver_features = d->blk_features;
  vm->blk.status = d->blk_status;
  vm->blk.intr_status = d->blk_intr_status;
  vm->blk.queue_sel = d->blk_queue_sel;
  vm->blk.device_features_sel = d->blk_dfs;
  vm->blk.driver_features_sel = d->blk_ffs;
  vm->dtb_gpa = d->dtb_gpa;
  vm->entry = d->entry;
}

// the memory: every backed page that is not all zeroes, named by its
// guest physical address. A 128 MB guest that has touched twelve writes
// twelve, which is what makes saving a machine cheap enough to do often.
// `only', when it is there, is the set of pages this call carries -- a
// pre-copy round sends what has been written since the last one and
// nothing else. Without it every backed, non-zero page goes.
//
// `drop_zero' is what makes a saved machine small: a page of zeroes is
// not written, because the machine it is restored into starts with
// nothing but zeroes. That reasoning holds exactly once. In a pre-copy
// round after the first, the other end already has a copy of the page,
// and a page the guest has *zeroed* since then is a change like any
// other -- dropping it would leave yesterday's bytes over there. (This
// is what a migrated guest's "panic: sched locks" was: a page holding
// the guest's own lock counters, zeroed and then not sent.)
// `sums', when it is there, is updated with what this call sent: one
// checksum per page, which is what the other end now has. With `verify'
// the call ignores `only' and sends every page whose checksum says the
// other end has something else -- see precopy_verify.
// What the verifying last round found, which is the only place this system
// can see the thing that makes write-protection unsound here.
//
// `marked' is the set of pages somebody remembered: the guest's own stores
// (caught by write-protecting the G-stage) and the host's writes on its
// behalf (caught in vm_copyout). A page whose checksum has changed and
// which is *not* in that set changed for a third reason -- the hardware
// setting accessed and dirty bits in the guest's own page tables, through
// a mapping this kernel made read-only, with no fault arriving. Counting
// those separately turns a sentence in a comment into a number, and it is
// the number that says whether a list of dirty pages could ever have been
// enough.
struct lastround {
  uint32 *marked;   // pages somebody marked (may be 0: then no attribution)
  uint32 *msums;    // masked checksums, when admeasure=1 (else 0)
  int nsent;        // pages the comparison sent
  int nunmarked;    // of those, pages nobody had marked
  int nadonly;      // of those, pages differing only in PTE A/D bits
  int nunmarked_adonly;  // and both at once -- the number the paper turns on
};

static int
save_mem(struct vm *vm, struct sink *s, uint64 *nbytes, uint32 *only,
         int drop_zero, uint32 *sums, int verify, struct lastround *lr)
{
  char *page = kalloc();
  uint64 written = 0;
  int sent = 0;
  struct vmstate_sec sec;

  if (page == 0)
    return -ENOMEM;
  // the header's length is zero and the pages end with a sentinel: a
  // section written down a socket cannot be written backwards, and there
  // may be several of them.
  memset(&sec, 0, sizeof(sec));
  safestrcpy(sec.name, "MEM", sizeof(sec.name));
  int r = put(s, &sec, sizeof(sec));
  if (r < 0)
    goto out;

  for (int k = 0; k < vm->nchunks; k++) {
    for (int i = 0; i < HCHUNK_PAGES; i++) {
      uint64 gpa = G_RAM_BASE + (uint64)k * HCHUNK + (uint64)i * PGSIZE;
      int pidx = k * HCHUNK_PAGES + i;
      if (!verify && only && !vmsnap_bit(only, pidx))
        continue;
      if (vm_copyin(vm, page, gpa, PGSIZE) < 0) {
        if (sums)
          sums[pidx] = 0;               // not backed: zeroes over there
        if (lr && lr->msums)
          lr->msums[pidx] = 0;
        continue;
      }
      uint32 now = page_is_zero(page) ? 0 : pcs(page);
      if (verify && sums && now == sums[pidx])
        continue;                       // the other end has this already
      if (verify && lr) {
        int unmarked = lr->marked && !vmsnap_bit(lr->marked, pidx);
        // A/D-only means: the raw checksum moved (we are here) but the
        // checksum that ignores A and D in PTE-shaped words did not. That
        // is the signature of the MMU writing the guest's page tables
        // through a mapping this kernel made read-only.
        int adonly = 0;
        if (lr->msums) {
          uint32 mnow = pcs_masked(page);
          adonly = (mnow == lr->msums[pidx]);
        }
        if (unmarked)
          lr->nunmarked++;
        if (adonly)
          lr->nadonly++;
        if (unmarked && adonly)
          lr->nunmarked_adonly++;
      }
      if (drop_zero && page_is_zero(page)) {
        if (sums)
          sums[pidx] = 0;               // left out, so zeroes over there
        if (lr && lr->msums)
          lr->msums[pidx] = 0;          // 0 means "unknown": never reads as A/D-only
        continue;
      }
      if (sums)
        sums[pidx] = now;
      if (lr && lr->msums)
        lr->msums[pidx] = pcs_masked(page);
      sent++;
      struct vmstate_page ph = {.gpa = gpa};
      if ((r = put(s, &ph, sizeof(ph))) < 0)
        goto out;
      if ((r = put(s, page, PGSIZE)) < 0)
        goto out;
      written += sizeof(ph) + PGSIZE;
      // a line every four megabytes: enough to see that a long save is
      // moving, not enough to bury the console in it
      if ((written & ((1 << 22) - 1)) < PGSIZE)
        printk("vmstate: sent %ld MB\n", (long)(written >> 20));
    }
  }
  {
    struct vmstate_page end = {.gpa = VMSTATE_END};
    if ((r = put(s, &end, sizeof(end))) < 0)
      goto out;
    written += sizeof(end);
  }
  *nbytes = written;
  if (lr)
    lr->nsent = sent;
out:
  kfree(page);
  return r;
}

// ---- the calls ----

// every vcpu out of the guest, or -EBUSY. The pause is the caller's.
static int
drain(struct vm *vm)
{
  // the disk thread first: it writes guest memory on the guest's behalf
  // and does not stop because the vcpus have.
  if (vvirtio_quiesce(vm) < 0)
    return -EBUSY;
  for (int tries = 0; tries < 500; tries++) {
    int inside = 0;
    acquire(&vm->lock);
    for (int i = 0; i < vm->nvcpu; i++)
      if (vm->vcpu[i].running_hart >= 0)
        inside++;
    release(&vm->lock);
    if (inside == 0)
      return 0;
    // a tick at a time: the vcpus are on their way out of the guest, and
    // what this is waiting for is the scheduler running them one last
    // time (kernel/sched.c's freeze_point)
    extern struct spinlock tickslock;
    extern uint ticks;
    acquire(&tickslock);
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
  }
  return -EBUSY;
}

// the body of a save, with the sink already set up. A file and a socket
// differ in two places (the header's length, and how the pages end) and
// nowhere else.
static int save_to(struct vm *vm, struct sink *s);

// ---- pre-copy (§9) ----
//
// A stop-and-copy stops the guest for as long as its memory takes to
// cross. Pre-copy sends the memory while the guest runs: every page in
// the first round, and in each round after it only what has been written
// since the one before -- by the guest, which faults on a page made
// read-only, or by the host on the guest's behalf, which does not fault
// and is marked in vm_copyout. When what is left is small enough the
// machine stops, and only that last handful crosses with it stopped.
//
// Five rounds and sixty-four pages are the limits: a guest that dirties
// memory faster than the wire carries it will never converge, and
// stopping it is then the shorter outage. What the rounds buy is printed,
// because the interesting number is how long the guest was stopped.
#define PRECOPY_ROUNDS 5
#define PRECOPY_FLOOR 64

struct precopy {
  uint32 *dirty;           // written since this round began
  uint32 *send;            // what this round carries
  uint32 *sum;             // what the other end has, per page
  uint32 *msum;            // and the same with A/D bits masked out (admeasure=1)
  int npages, order, mapbytes, sumorder;
};

static int
precopy_begin(struct vm *vm, struct precopy *pc)
{
  struct vmsnap *sn = &vm->snap;

  pc->npages = (int)(vm->ram_size / PGSIZE);
  pc->mapbytes = (pc->npages + 31) / 32 * 4;
  pc->order = 0;
  while (((uint64)PGSIZE << pc->order) < (uint64)pc->mapbytes)
    pc->order++;
  pc->sumorder = 0;
  while (((uint64)PGSIZE << pc->sumorder) < (uint64)pc->npages * 4)
    pc->sumorder++;
  pc->dirty = kalloc_order(pc->order);
  pc->send = kalloc_order(pc->order);
  pc->sum = kalloc_order(pc->sumorder);
  if (pc->sum)
    memset(pc->sum, 0, pc->npages * 4);   // it has nothing yet
  pc->msum = 0;
  if (ad_measuring() && pc->sum) {
    pc->msum = kalloc_order(pc->sumorder);
    if (pc->msum)
      memset(pc->msum, 0, pc->npages * 4);
    // a missing msum is not fatal: the measurement is skipped, the
    // migration is not
  }
  if (pc->dirty == 0 || pc->send == 0 || pc->sum == 0) {
    if (pc->dirty)
      kfree_order(pc->dirty, pc->order);
    if (pc->send)
      kfree_order(pc->send, pc->order);
    if (pc->sum)
      kfree_order(pc->sum, pc->sumorder);
    if (pc->msum)
      kfree_order(pc->msum, pc->sumorder);
    pc->dirty = pc->send = pc->sum = pc->msum = 0;
    return -ENOMEM;
  }
  memset(pc->dirty, 0, pc->mapbytes);
  memset(pc->send, 0xff, pc->mapbytes);    // the first round is all of it
  acquiresleep(&sn->lock);
  if (sn->active) {
    releasesleep(&sn->lock);
    kfree_order(pc->dirty, pc->order);
    kfree_order(pc->send, pc->order);
    kfree_order(pc->sum, pc->sumorder);
    pc->dirty = pc->send = pc->sum = 0;
    return -EBUSY;                         // a snapshot is already running
  }
  sn->done = 0;                            // not this mode's business
  sn->dirty = pc->dirty;
  sn->npages = pc->npages;
  sn->faulted = 0;
  sn->precopy = 1;
  __sync_synchronize();
  sn->active = 1;
  releasesleep(&sn->lock);
  return 0;
}

static void
precopy_end(struct vm *vm, struct precopy *pc)
{
  struct vmsnap *sn = &vm->snap;

  if (pc->dirty == 0)
    return;
  acquiresleep(&sn->lock);
  sn->active = 0;
  sn->precopy = 0;
  sn->dirty = 0;
  releasesleep(&sn->lock);
  vm_snap_unprotect(vm);
  kfree_order(pc->dirty, pc->order);
  kfree_order(pc->send, pc->order);
  kfree_order(pc->sum, pc->sumorder);
  if (pc->msum)
    kfree_order(pc->msum, pc->sumorder);
  pc->dirty = pc->send = pc->sum = pc->msum = 0;
}

// take the dirty set as this round's work and start counting again. The
// exchange is one word at a time and atomic: a page written between one
// word and the next is in the next round's set, which costs a page sent
// twice and never loses one.
static void
precopy_take(struct precopy *pc)
{
  for (int i = 0; i < pc->mapbytes / 4; i++)
    pc->send[i] = __sync_lock_test_and_set(&pc->dirty[i], 0);
}

int
vm_save_file(struct vm *vm, struct file *f)
{
  struct sink s;

  if (f == 0 || f->type != FD_SOCK)
    return -EINVAL;
  if (vm->disk && vm->disk_ip == 0)
    return -ENOTSUP;
  memset(&s, 0, sizeof(s));
  s.f = f;
  return save_to(vm, &s);
}

// the sections, in order, into whatever the sink is
static int
save_to(struct vm *vm, struct sink *s)
{
  struct vmstate_hdr hdr;
  struct vmstate_work *w = (struct vmstate_work *)kalloc();
  struct vmstate_dev *dev = (struct vmstate_dev *)kalloc();
  struct precopy pc;
  int was_running = 0, r = 0;

  memset(&pc, 0, sizeof(pc));
  if (w == 0 || dev == 0) {
    r = -ENOMEM;
    goto out;
  }

  // the header and the machine's shape can go before anything stops: a
  // running machine does not change either.
  memset(&hdr, 0, sizeof(hdr));
  memmove(hdr.magic, VMSTATE_MAGIC, 8);
  hdr.version = VMSTATE_VERSION;
  hdr.saved_time = r_time();
  if ((r = put(s, &hdr, sizeof(hdr))) < 0)
    goto out;

  memset(&w->cfg, 0, sizeof(w->cfg));
  w->cfg.cfg = vm->cfg;
  w->cfg.ram_size = vm->ram_size;
  w->cfg.nvcpu = vm->nvcpu;
  w->cfg.vic_kind = vm->vic_kind;
  w->cfg.imsic_hw = vm->imsic_hw;
  w->cfg.prefer_huge = vm->prefer_huge;
  w->cfg.mem_limit = vm->mem_limit;
  w->cfg.obs = vm->obs;
  w->cfg.vlenb = (uint32)(vm->vec_revoked ? 0 : plat.vlenb);
  w->cfg.revise_gen = vm->revise_gen;
  w->cfg.mmio_shift = vm->mmio_shift;
  w->cfg.flags2 = (vm->vec_revoked ? VMSTATE_F_VEC_REVOKED : 0) |
                  (vm->revise_capable ? VMSTATE_F_CAPABLE : 0);
  if ((r = put_sec(s, "CFG", &w->cfg, sizeof(w->cfg))) < 0)
    goto out;

  // and now, if this is a migration of a machine that is running, the
  // memory goes over while it keeps running (§9). A save to a file does
  // not do this: the file is written once, and a page written twice into
  // it would only make it longer.
  if (s->f && vm->state == VM_RUNNING && precopy_begin(vm, &pc) == 0) {
    for (int round = 1; round <= PRECOPY_ROUNDS; round++) {
      uint64 mem = 0;
      // everything read-only again first, so that a write during the
      // sweep is a write this round knows about
      vm_snap_protect(vm, 0, pc.dirty, pc.npages);
      if (round > 1)
        precopy_take(&pc);
      // the first round is every page there is, so a page of zeroes can
      // be left out as in a plain save; from the second round on it
      // cannot (see save_mem).
      // an lr with no `marked' and no counters: it is here only so that the
      // masked checksums are recorded as pages go over (admeasure=1)
      struct lastround rlr;
      memset(&rlr, 0, sizeof(rlr));
      rlr.msums = pc.msum;
      if ((r = save_mem(vm, s, &mem, pc.send, round == 1, pc.sum, 0, &rlr)) < 0) {
        precopy_end(vm, &pc);
        goto out;
      }
      int left = vmsnap_count(pc.dirty, pc.npages);
      printk("hyp: vm %d: pre-copy round %d: %ld KB, %d pages left\n",
             vm->id, round, (long)(mem / 1024), left);
      if (left <= PRECOPY_FLOOR)
        break;
    }
  }

  // paused and drained for the rest of it, and put back the way it was
  // found afterwards. With a pre-copy behind it this is the whole of the
  // guest's outage: the state, and whatever it wrote in the last round.
  was_running = (vm->state == VM_RUNNING);
  if (was_running)
    vm_pause(vm);
  if ((r = drain(vm)) < 0)
    goto out;

  memset(&w->tm, 0, sizeof(w->tm));
  w->tm.saved_time = hdr.saved_time;
  w->tm.htimedelta = vm->htimedelta;
  if ((r = put_sec(s, "TIME", &w->tm, sizeof(w->tm))) < 0)
    goto out;

  // the vcpus, one at a time: sixty-four of them at once is twice a
  // kernel stack. The vector registers, if this machine has any, are the
  // page after the structure.
  for (int i = 0; i < vm->nvcpu; i++) {
    save_vcpu(&vm->vcpu[i], &w->vs);
    uint64 len = sizeof(w->vs) + (w->vs.has_vstate ? PGSIZE : 0);
    struct vmstate_sec sec;
    memset(&sec, 0, sizeof(sec));
    safestrcpy(sec.name, "VCPU", sizeof(sec.name));
    sec.len = len;
    if ((r = put(s, &sec, sizeof(sec))) < 0)
      goto out;
    if ((r = put(s, &w->vs, sizeof(w->vs))) < 0)
      goto out;
    if (w->vs.has_vstate && (r = put(s, vm->vcpu[i].vstate, PGSIZE)) < 0)
      goto out;
  }

  save_dev(vm, dev);
  if ((r = put_sec(s, "DEV", dev, sizeof(*dev))) < 0)
    goto out;
  // the console rings are eight kilobytes each and are written from where
  // they live rather than through a copy
  if ((r = put_sec(s, "CONSO", &vm->cons_out, sizeof(vm->cons_out))) < 0)
    goto out;
  if ((r = put_sec(s, "CONSI", &vm->cons_in, sizeof(vm->cons_in))) < 0)
    goto out;

  uint64 mem = 0;
  if (pc.sum) {
    // The last round, with the machine stopped, is a comparison rather
    // than a list: every page whose checksum differs from what the other
    // end was sent goes again.
    //
    // The last round is a comparison and not a list, and the history of
    // why is worth keeping because the first explanation was wrong.
    //
    // Pages did arrive at the far end stale -- 2 to 7 of them in a
    // migration of a 48 MB guest -- and the guest panicked in its own
    // kernel a moment after it started running. The explanation written
    // here for some time was that the *hardware* writes guest memory
    // without a fault arriving, setting accessed and dirty bits in the
    // guest's own page tables through a mapping this kernel had made
    // read-only. That was plausible, it was never measured, and it was
    // false.
    //
    // What was actually happening: vm_unshare() handed write permission
    // back to a page in the middle of a snapshot, so the first write to it
    // faulted and was recorded and every write after that was invisible.
    // With that fixed, this round finds *nothing* -- zero pages, under a
    // tmpfs writer and under a fork-heavy workload that churns page tables
    // as hard as a 48 MB guest can. The MMU's implicit writes do take a
    // guest-page fault here.
    //
    // So the comparison stays, as insurance rather than as a repair: it
    // costs four bytes a page and one hash per page, and it is what found
    // that bug when six migration tests could not. A dirty set that is
    // wrong is indistinguishable from one that is right until something
    // checks it. See journal.md and `admeasure=1' for the measurements.
    struct lastround lr;
    memset(&lr, 0, sizeof(lr));
    lr.marked = pc.dirty;     // everything anyone remembered, not yet sent
    lr.msums = pc.msum;       // 0 unless admeasure=1
    r = save_mem(vm, s, &mem, 0, 0, pc.sum, 1, &lr);
    printk("hyp: vm %d: stopped for %d page(s), %ld KB; %ld written by the "
           "guest under the copy; %d page(s) nobody had marked\n",
           vm->id, lr.nsent, (long)(mem / 1024), (long)vm->snap.faulted,
           lr.nunmarked);
    if (pc.msum)
      printk("hyp: vm %d: admeasure: sent %d, unmarked %d, A/D-only %d, "
             "unmarked+A/D-only %d\n", vm->id, lr.nsent, lr.nunmarked,
             lr.nadonly, lr.nunmarked_adonly);
    precopy_end(vm, &pc);
    if (r < 0)
      goto out;
  } else if ((r = save_mem(vm, s, &mem, 0, 1, 0, 0, 0)) < 0) {
    goto out;
  }
  // and the end of the sections, which a connection needs: MEM is no
  // longer the last of them.
  if ((r = put_sec(s, "END", 0, 0)) < 0)
    goto out;
  // and the header again for a file, which can say how long it is; a
  // connection has already sent it and ends with END instead.
  if (s->ip) {
    hdr.len = s->off - sizeof(hdr);
    ilock(s->ip);
    r = writei(s->ip, 0, (uint64)&hdr, 0, sizeof(hdr)) == sizeof(hdr) ? 0 : -EIO;
    iunlock(s->ip);
  }
out:
  precopy_end(vm, &pc);          // a no-op unless a round failed
  // A machine that has been handed over does not run again on this side:
  // the other host has its state, and an instruction executed here would
  // diverge from the guest that is now going on over there -- and would
  // write to the disk image the other end is about to open. The caller
  // destroys it (sys_vm_migrate). A save to a file, or a migration that
  // failed, goes back to running.
  if (was_running && !(s->f && r == 0))
    vm_resume(vm);
  if (w)
    kfree(w);
  if (dev)
    kfree(dev);
  return r;
}

// ---- the acknowledgement ----
//
// A migration used to be: send everything, and if the sending worked, destroy
// the machine here. The destination can still refuse *after* it has read the
// stream -- it may have no vector unit for a save area, no room for the memory,
// or no free slot -- and the machine was then gone from both ends. A guest lost
// because two hosts disagreed about whether the move was legal is the worst
// outcome this code can produce, and it was reachable.
//
// So the destination says one byte when it knows, and the source waits for it
// before giving anything up. 'Y' means the machine is safe over there. Anything
// else, including the connection closing, means it is not, and the source keeps
// running it.
int
vmstate_ack_put(struct file *f, int ok)
{
  char c = ok ? 'Y' : 'N';

  if (f == 0 || f->type != FD_SOCK || f->sock == 0)
    return -EINVAL;
  if (sockwrite(f->sock, 0, (uint64)&c, 1) != 1)
    return -EIO;
  return 0;
}

// and the source's side of it: 0 when the far end said yes.
//
// Bounded, because an unbounded wait here would be a worse bug than the one this
// fixes. A destination that crashes closes the connection and sockread says so at
// once; one that *hangs* says nothing at all, and a source blocked forever on it
// is a machine that is neither running nor migrated. So the wait has a deadline,
// and running out of it is a no: the guest stays here, which is the safe answer
// whichever way the far end eventually goes -- a destination that has not said yes
// has not started it.
// Thirty seconds at TICK_HZ 100. Deliberately generous: the destination restores
// the machine as it reads it, so by the time the source has finished writing the
// far end is nearly done and the answer comes in milliseconds. A deadline tight
// enough to be tripped by a loaded destination would invent the very failure this
// is here to prevent -- a machine kept here that the far end was about to accept.
// Like FREE_TICKS in user/vmm.c: not a real wait, just long enough that reaching
// it means something is genuinely wrong.
#define ACK_TICKS 3000

static uint
now_ticks(void)
{
  acquire(&tickslock);
  uint t = ticks;
  release(&tickslock);
  return t;
}

int
vmstate_ack_get(struct file *f)
{
  char c = 0;

  if (f == 0 || f->type != FD_SOCK || f->sock == 0)
    return -EINVAL;
  uint deadline = now_ticks() + ACK_TICKS;
  for (;;) {
    int ready = sockready(f->sock, 0);
    if (ready < 0)
      return -EIO;             // it closed without answering
    if (ready > 0)
      break;
    if (killed(myproc()))
      return -EINTR;
    if (now_ticks() >= deadline)
      return -ETIMEDOUT;       // it said nothing: the machine stays here
    // a tick at a time, on the channel the timer wakes; registered before
    // tickslock is dropped, so a tick in between is not missed
    acquire(&tickslock);
    sleep_prepare((void *)&ticks);
    release(&tickslock);
    sleep();
  }
  if (sockread(f->sock, 0, (uint64)&c, 1) != 1)
    return -EIO;
  return c == 'Y' ? 0 : -ECONNRESET;
}

int
vm_save(struct vm *vm, char *path)
{
  struct inode *ip = namei(path);
  struct sink s;
  int r;

  if (ip == 0)
    return -ENOENT;
  if (ip->type != T_FILE) {
    iput(ip);
    return -EINVAL;
  }
  // a machine given a whole block device cannot be saved: what a save
  // names is the image file it will be restored beside, and a device is
  // not one. Saying so is better than writing a file that cannot be
  // restored (§9).
  if (vm->disk && vm->disk_ip == 0) {
    iput(ip);
    return -ENOTSUP;
  }
  memset(&s, 0, sizeof(s));
  s.ip = ip;
  r = save_to(vm, &s);
  iput(ip);
  return r;
}

// ---- a snapshot, with the machine running (§9) ----
//
// The difference between this and a save is where the guest is while it
// happens. A save stops it; a snapshot lets it run, and the file still
// has to be the machine as it was at one moment -- the moment the
// snapshot began.
//
// What makes that possible is the G-stage. At the start, with the machine
// paused for as long as it takes to write down its registers and its
// devices, every page of guest memory that is backed is made read-only
// and marked as still owed to the file. Then the machine runs again. This
// process walks the pages and writes them; and a guest that writes to a
// page before the walk reaches it takes a store fault, and the fault
// hands that page to the file before letting the write through
// (vmsnap_before_write). Either way what the file gets is the page as it
// was when the snapshot began, and nobody waits for longer than one page.

// the page index of a guest physical address, and back
static int
snap_index(struct vm *vm, uint64 gpa)
{
  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return -1;
  return (int)((gpa - G_RAM_BASE) / PGSIZE);
}

// write one page into the file, under the lock that owns the offset.
// The caller has the page's contents; the bitmap says whether anybody
// still wants them.
static int
snap_put_page(struct vm *vm, uint64 gpa, const char *page)
{
  struct vmsnap *sn = &vm->snap;
  int idx = snap_index(vm, gpa);
  struct vmstate_page ph;
  int r = 0;

  if (idx < 0 || idx >= sn->npages)
    return 0;
  acquiresleep(&sn->lock);
  if (!sn->active || vmsnap_bit(sn->done, idx)) {
    releasesleep(&sn->lock);
    return 0;               // somebody else has written it
  }
  ph.gpa = gpa;
  ilock(sn->ip);
  if (writei(sn->ip, 0, (uint64)&ph, sn->off, sizeof(ph)) != sizeof(ph) ||
      writei(sn->ip, 0, (uint64)page, sn->off + sizeof(ph), PGSIZE) != PGSIZE)
    r = -EIO;
  iunlock(sn->ip);
  if (r == 0) {
    sn->off += sizeof(ph) + PGSIZE;
    sn->written += sizeof(ph) + PGSIZE;
    vmsnap_set(sn->done, idx);
  }
  releasesleep(&sn->lock);
  return r;
}

void
vmsnap_before_write(struct vm *vm, uint64 gpa)
{
  struct vmsnap *sn = &vm->snap;
  int idx = snap_index(vm, gpa & ~(uint64)(PGSIZE - 1));
  char *page;

  if (!sn->active || idx < 0 || idx >= sn->npages)
    return;
  if (sn->precopy) {
    // a migration's pre-copy: the page is the guest's to write, and the
    // copy this round sent is stale, so it goes again next round. The
    // guest is not held up for it.
    vmsnap_mark(sn->dirty, idx);
    sn->faulted++;
    return;
  }
  if (vmsnap_bit(sn->done, idx))
    return;                 // the file has it already: nothing to wait for
  if ((page = kalloc()) == 0)
    return;                 // no room to copy: the snapshot will be short
  if (vm_copyin(vm, page, gpa & ~(uint64)(PGSIZE - 1), PGSIZE) == 0) {
    if (snap_put_page(vm, gpa & ~(uint64)(PGSIZE - 1), page) == 0)
      sn->faulted++;
  }
  kfree(page);
}

int
vm_snapshot(struct vm *vm, char *path)
{
  struct inode *ip = namei(path);
  struct vmsnap *sn = &vm->snap;
  struct vmstate_hdr hdr;
  struct vmstate_work *w = 0;
  struct vmstate_dev *dev = 0;
  char *page = 0;
  struct sink s;
  int was_running = 0, r = 0;
  uint secoff = 0;

  if (ip == 0)
    return -ENOENT;
  if (ip->type != T_FILE) {
    iput(ip);
    return -EINVAL;
  }
  if (vm->disk && vm->disk_ip == 0) {
    // §9: a machine given a raw device cannot be snapshotted until the
    // device is an overlay, and saying so is the answer
    iput(ip);
    return -ENOTSUP;
  }
  acquiresleep(&sn->lock);
  if (sn->active) {
    releasesleep(&sn->lock);
    iput(ip);
    return -EBUSY;
  }
  releasesleep(&sn->lock);

  w = (struct vmstate_work *)kalloc();
  dev = (struct vmstate_dev *)kalloc();
  page = kalloc();
  int npages = (int)(vm->ram_size / PGSIZE);
  int mapbytes = (npages + 31) / 32 * 4;
  int order = 0;
  while (((uint64)PGSIZE << order) < (uint64)mapbytes)
    order++;
  uint32 *done = kalloc_order(order);
  // and one for the pages the *host* writes under the walk: a block the
  // disk reads in, a frame that arrives. Those do not fault, so nothing
  // would otherwise notice them, and the page in the file would be the
  // page as it was before the device filled it (vm_copyout marks them).
  uint32 *hostdirty = kalloc_order(order);
  if (w == 0 || dev == 0 || page == 0 || done == 0 || hostdirty == 0) {
    r = -ENOMEM;
    goto out;
  }
  memset(done, 0, mapbytes);
  memset(hostdirty, 0, mapbytes);

  // the registers and the devices, with the machine held still: a few
  // milliseconds, not the length of the file.
  was_running = (vm->state == VM_RUNNING);
  if (was_running)
    vm_pause(vm);
  if ((r = drain(vm)) < 0)
    goto out;

  // the whole sink, not just the two fields this path uses: it has one
  // for a connection as well now, and a stack full of yesterday's bytes
  // in it would send a live snapshot down something that is not there.
  memset(&s, 0, sizeof(s));
  s.ip = ip;
  memset(&hdr, 0, sizeof(hdr));
  memmove(hdr.magic, VMSTATE_MAGIC, 8);
  hdr.version = VMSTATE_VERSION;
  hdr.saved_time = r_time();
  hdr.flags = 1;             // taken live
  if ((r = put(&s, &hdr, sizeof(hdr))) < 0)
    goto resume;

  memset(&w->cfg, 0, sizeof(w->cfg));
  w->cfg.cfg = vm->cfg;
  w->cfg.ram_size = vm->ram_size;
  w->cfg.nvcpu = vm->nvcpu;
  w->cfg.vic_kind = vm->vic_kind;
  w->cfg.imsic_hw = vm->imsic_hw;
  w->cfg.prefer_huge = vm->prefer_huge;
  w->cfg.mem_limit = vm->mem_limit;
  w->cfg.obs = vm->obs;
  w->cfg.vlenb = (uint32)(vm->vec_revoked ? 0 : plat.vlenb);
  w->cfg.revise_gen = vm->revise_gen;
  w->cfg.mmio_shift = vm->mmio_shift;
  w->cfg.flags2 = (vm->vec_revoked ? VMSTATE_F_VEC_REVOKED : 0) |
                  (vm->revise_capable ? VMSTATE_F_CAPABLE : 0);
  if ((r = put_sec(&s, "CFG", &w->cfg, sizeof(w->cfg))) < 0)
    goto resume;
  memset(&w->tm, 0, sizeof(w->tm));
  w->tm.saved_time = hdr.saved_time;
  w->tm.htimedelta = vm->htimedelta;
  if ((r = put_sec(&s, "TIME", &w->tm, sizeof(w->tm))) < 0)
    goto resume;
  for (int i = 0; i < vm->nvcpu; i++) {
    save_vcpu(&vm->vcpu[i], &w->vs);
    uint64 len = sizeof(w->vs) + (w->vs.has_vstate ? PGSIZE : 0);
    struct vmstate_sec sec;
    memset(&sec, 0, sizeof(sec));
    safestrcpy(sec.name, "VCPU", sizeof(sec.name));
    sec.len = len;
    if ((r = put(&s, &sec, sizeof(sec))) < 0)
      goto resume;
    if ((r = put(&s, &w->vs, sizeof(w->vs))) < 0)
      goto resume;
    if (w->vs.has_vstate && (r = put(&s, vm->vcpu[i].vstate, PGSIZE)) < 0)
      goto resume;
  }
  save_dev(vm, dev);
  if ((r = put_sec(&s, "DEV", dev, sizeof(*dev))) < 0)
    goto resume;
  if ((r = put_sec(&s, "CONSO", &vm->cons_out, sizeof(vm->cons_out))) < 0)
    goto resume;
  if ((r = put_sec(&s, "CONSI", &vm->cons_in, sizeof(vm->cons_in))) < 0)
    goto resume;

  // the MEM header, with a length to be filled in, and then the machine
  // goes back to running while its pages are written
  {
    struct vmstate_sec sec;
    memset(&sec, 0, sizeof(sec));
    safestrcpy(sec.name, "MEM", sizeof(sec.name));
    secoff = s.off;
    if ((r = put(&s, &sec, sizeof(sec))) < 0)
      goto resume;
  }
  sn->ip = ip;
  sn->off = s.off;
  sn->written = 0;
  sn->done = done;
  sn->dirty = hostdirty;
  sn->npages = npages;
  sn->faulted = 0;
  vm_snap_protect(vm, done, 0, npages);
  __sync_synchronize();
  sn->active = 1;

resume:
  if (was_running)
    vm_resume(vm);
  if (r < 0 || !sn->active)
    goto out;

  // and now the walk, with the guest running underneath it
  for (int idx = 0; idx < npages && r == 0; idx++) {
    if (vmsnap_bit(sn->done, idx))
      continue;
    uint64 gpa = G_RAM_BASE + (uint64)idx * PGSIZE;
    if (vm_copyin(vm, page, gpa, PGSIZE) < 0) {
      acquiresleep(&sn->lock);
      vmsnap_set(sn->done, idx);
      releasesleep(&sn->lock);
      continue;
    }
    if (page_is_zero(page)) {
      acquiresleep(&sn->lock);
      vmsnap_set(sn->done, idx);
      releasesleep(&sn->lock);
      continue;
    }
    r = snap_put_page(vm, gpa, page);
    // one page at a time, and the processor given up now and then: a
    // snapshot is not worth starving the machine it is taken from
    if ((idx & 63) == 0)
      yield();
  }

  // the machine's pages are its own again, and the section's length is
  // what was written
  acquiresleep(&sn->lock);
  sn->active = 0;
  uint64 written = sn->written;
  uint off = sn->off;
  uint64 faulted = sn->faulted;
  sn->done = 0;
  sn->dirty = 0;
  sn->ip = 0;
  releasesleep(&sn->lock);
  vm_snap_unprotect(vm);

  // the pages the host wrote into while the walk was going on. There are
  // a few of them at most, so the machine is stopped for them: a page
  // written by a device after it was copied is a page the file has wrong,
  // and the alternative to stopping is never knowing when to stop.
  if (r == 0) {
    int hostpages = vmsnap_count(hostdirty, npages);
    if (hostpages > 0) {
      int running = (vm->state == VM_RUNNING);
      if (running)
        vm_pause(vm);
      if (drain(vm) == 0) {
        struct sink fin;
        memset(&fin, 0, sizeof(fin));
        fin.ip = ip;
        fin.off = off;
        for (int idx = 0; idx < npages && r == 0; idx++) {
          if (!vmsnap_bit(hostdirty, idx))
            continue;
          uint64 gpa = G_RAM_BASE + (uint64)idx * PGSIZE;
          if (vm_copyin(vm, page, gpa, PGSIZE) < 0)
            continue;
          struct vmstate_page ph = {.gpa = gpa};
          if ((r = put(&fin, &ph, sizeof(ph))) < 0)
            break;
          if ((r = put(&fin, page, PGSIZE)) < 0)
            break;
          written += sizeof(ph) + PGSIZE;
        }
        off = fin.off;
      }
      if (running)
        vm_resume(vm);
      printk("hyp: vm %d: snapshot: %d page(s) the host wrote under it, "
             "taken with the machine stopped\n", vm->id, hostpages);
    }
  }
  if (r == 0) {
    struct vmstate_sec sec;
    // the sentinel that ends every MEM section, here as much as in a save
    // written down a socket: a restore reads pages until it meets one,
    // and a live snapshot that left it out was a file that could be
    // written and not read back.
    struct vmstate_page end = {.gpa = VMSTATE_END};
    ilock(ip);
    if (writei(ip, 0, (uint64)&end, off, sizeof(end)) != sizeof(end))
      r = -EIO;
    iunlock(ip);
    written += sizeof(end);
    off += sizeof(end);
    memset(&sec, 0, sizeof(sec));
    safestrcpy(sec.name, "MEM", sizeof(sec.name));
    sec.len = written;
    // the section that ends the sections, and then the two lengths
    struct vmstate_sec end2;
    memset(&end2, 0, sizeof(end2));
    safestrcpy(end2.name, "END", sizeof(end2.name));
    ilock(ip);
    if (writei(ip, 0, (uint64)&end2, off, sizeof(end2)) != sizeof(end2))
      r = -EIO;
    iunlock(ip);
    off += sizeof(end2);
    hdr.len = off - sizeof(hdr);
    ilock(ip);
    if (writei(ip, 0, (uint64)&sec, secoff, sizeof(sec)) != sizeof(sec) ||
        writei(ip, 0, (uint64)&hdr, 0, sizeof(hdr)) != sizeof(hdr))
      r = -EIO;
    iunlock(ip);
  }
  if (r == 0 && faulted)
    printk("hyp: vm %d: snapshot: %ld pages written by the guest's own "
           "writes\n", vm->id, (long)faulted);
out:
  if (done)
    kfree_order(done, order);
  if (hostdirty)
    kfree_order(hostdirty, order);
  if (w)
    kfree(w);
  if (dev)
    kfree(dev);
  if (page)
    kfree(page);
  iput(ip);
  return r;
}

// ---- cloning (§9) ----
//
// The same machine twice, without a copy of its memory and without a file
// in between: the clone's state is the original's state, written into the
// clone rather than into a file, and every page is shared read-only until
// one of them writes. What the clone does not share is its disk, which is
// the caller's to provide -- an overlay over the original's image is what
// `losetup -b' makes, and a machine whose disk is the same file open twice
// is a machine with two kernels writing one file system.
int
vm_clone(struct vm *src, const char *name, const char *disk, int *idp)
{
  struct vmstate_work *w = 0;
  struct vmstate_dev *dev = 0;
  struct vm *dst = 0;
  int was_running = 0, id = -1, r = 0;

  if (src->disk && src->disk_ip == 0)
    return -ENOTSUP;        // a raw device is not something to clone
  w = (struct vmstate_work *)kalloc();
  dev = (struct vmstate_dev *)kalloc();
  if (w == 0 || dev == 0) {
    r = -ENOMEM;
    goto out;
  }
  was_running = (src->state == VM_RUNNING);
  if (was_running)
    vm_pause(src);
  if ((r = drain(src)) < 0)
    goto out;

  memset(&w->cfg, 0, sizeof(w->cfg));
  w->cfg.cfg = src->cfg;
  if (name && name[0])
    safestrcpy(w->cfg.cfg.name, name, VM_NAMELEN);
  else
    w->cfg.cfg.name[0] = 0;
  if (disk && disk[0])
    safestrcpy(w->cfg.cfg.disk, disk, VM_PATHLEN);
  else
    w->cfg.cfg.disk[0] = 0;
  id = vm_create_blank(&w->cfg.cfg);
  if (id < 0) {
    r = -ENOSPC;
    goto out;
  }
  if ((dst = vm_lookup(id)) == 0) {
    r = -ENOENT;
    goto out;
  }
  dst->mem_limit = src->mem_limit;
  dst->prefer_huge = src->prefer_huge;

  // the memory: every page the original has, in both machines, read-only
  for (int k = 0; k < src->nchunks && r == 0; k++) {
    acquire(&src->memlock);
    int backed = src->chunk && src->chunk[k] != 0;
    release(&src->memlock);
    if (!backed)
      continue;
    for (int i = 0; i < HCHUNK_PAGES; i++) {
      uint64 gpa = G_RAM_BASE + (uint64)k * HCHUNK + (uint64)i * PGSIZE;
      if (vm_clone_page(dst, src, gpa) < 0) {
        r = -ENOMEM;
        break;
      }
    }
    if ((k & 3) == 0)
      yield();              // a clone is not worth starving the machine
  }
  if (r < 0)
    goto out;

  // and the state: the same registers, the same devices, the same clock
  for (int i = 0; i < src->nvcpu; i++) {
    save_vcpu(&src->vcpu[i], &w->vs);
    restore_vcpu(&dst->vcpu[i], &w->vs);
    if (w->vs.has_vstate && src->vcpu[i].vstate && dst->vcpu[i].vstate)
      memmove(dst->vcpu[i].vstate, src->vcpu[i].vstate, PGSIZE);
  }
  save_dev(src, dev);
  restore_dev(dst, dev, src->vic_kind);
  dst->revise_gen = src->revise_gen;
  dst->revise_acked = src->revise_gen;
  dst->cons_out = src->cons_out;
  dst->cons_in = src->cons_in;
  dst->htimedelta = src->htimedelta;
  vmstate_restored(dst);
  if (idp)
    *idp = id;
  r = 0;
out:
  if (was_running)
    vm_resume(src);
  if (r < 0 && dst)
    vm_destroy(dst);
  if (w)
    kfree(w);
  if (dev)
    kfree(dev);
  return r;
}

// ---- reading ----

struct src {
  struct inode *ip;
  struct file *f;
  uint off;
  uint len;
};

static int
get(struct src *s, void *buf, uint n)
{
  int r;

  if (s->f) {
    // a connection hands over what has arrived, which may be less than
    // was asked for: a section is read until it is whole or the other end
    // has gone.
    uint got = 0;
    if (s->f->type != FD_SOCK || s->f->sock == 0)
      return -EINVAL;
    while (got < n) {
      r = sockread(s->f->sock, 0, (uint64)buf + got, (int)(n - got));
      if (r <= 0)
        return -EIO;
      got += r;
    }
    s->off += n;
    return 0;
  }
  ilock(s->ip);
  r = readi(s->ip, 0, (uint64)buf, s->off, n);
  iunlock(s->ip);
  if (r != (int)n)
    return -EIO;
  s->off += n;
  return 0;
}

// the body of a restore, with the source already set up.
static int restore_from(struct src *s, const char *name, int *idp);

int
vm_restore_file(struct file *f, const char *name, int *idp)
{
  struct src s;

  if (f == 0 || f->type != FD_SOCK)
    return -EINVAL;
  memset(&s, 0, sizeof(s));
  s.f = f;
  return restore_from(&s, name, idp);
}

int
vm_restore(char *path, const char *name, int *idp)
{
  struct inode *ip = namei(path);
  struct src s;
  int r;

  if (ip == 0)
    return -ENOENT;
  memset(&s, 0, sizeof(s));
  s.ip = ip;
  r = restore_from(&s, name, idp);
  iput(ip);
  return r;
}

static int
restore_from(struct src *s, const char *name, int *idp)
{
  struct vmstate_hdr hdr;
  struct vmstate_work *w = 0;
  struct vmstate_dev *dev = 0;
  char *page = 0;
  int nvs = 0, r, id = -1;
  uint64 got = 0;
  struct vm *vm = 0;
  int src_kind = -1;        // the controller the machine was bound to when it left
  uint64 src_shift = 0;     // and where its devices sat
  // a file too short to hold a header is not a saved machine, and saying
  // "invalid argument" is the answer; "input/output error" would be a
  // complaint about the disk, which is not what is wrong.
  if (get(s, &hdr, sizeof(hdr)) < 0 ||
      memcmp(hdr.magic, VMSTATE_MAGIC, 8) != 0) {
    r = -EINVAL;
    goto out;
  }
  if (hdr.version != VMSTATE_VERSION) {
    // a file from a kernel built from other structures is not a machine
    // this one can continue, and guessing would be worse than refusing
    r = -ENOTSUP;
    goto out;
  }
  w = (struct vmstate_work *)kalloc();
  dev = (struct vmstate_dev *)kalloc();
  page = kalloc();
  if (w == 0 || dev == 0 || page == 0) {
    r = -ENOMEM;
    goto out;
  }
  memset(w, 0, sizeof(*w));
  memset(dev, 0, sizeof(*dev));

  // the sections in the order they were written: CFG first, so that
  // everything after it goes into a machine that exists.
  // how far the sections go. A save to a file says in its header; a
  // machine that came down a connection ends with its memory, and the MEM
  // section moves this to where the sentinel was.
  uint end = hdr.len ? sizeof(hdr) + (uint)hdr.len : (uint)-1;
  while (s->off < end) {
    struct vmstate_sec sec;
    if ((r = get(s, &sec, sizeof(sec))) < 0)
      goto out;
    uint next = s->off + (uint)sec.len;
    if (strncmp(sec.name, "CFG", sizeof(sec.name)) == 0) {
      if (sec.len != sizeof(w->cfg)) {
        r = -EINVAL;
        goto out;
      }
      if ((r = get(s, &w->cfg, sizeof(w->cfg))) < 0)
        goto out;
      if (name && name[0])
        safestrcpy(w->cfg.cfg.name, name, VM_NAMELEN);
      // ---- is this host allowed to have this machine? ----
      //
      // Not "is this host identical to the one it left", which is the question a
      // compatibility set asks and the reason a fleet is pinned to an instance
      // family for life. The question is whether this host can honour what the
      // guest currently *has*.
      //
      // A vector unit the guest still has cannot be taken away by arriving
      // somewhere without one: the register file is state, thirty-two registers
      // per task that has touched it, and a host with no unit has nowhere to put
      // it. So the machine is refused -- and the refusal is not the end of it.
      // The way to make this move legal is to ask the guest to give the unit up
      // *before* moving, which it will do if no task is relying on it
      // (vm_revise_vector, and the guest answers). A guest that has agreed
      // arrives with VMSTATE_F_VEC_REVOKED set and is taken.
      //
      // Deliberately NOT keyed on OBS_VECTOR_USED, and the reason is a result in
      // itself: the host cannot observe vector use reliably or cheaply.
      // vsstatus.VS going Dirty is the only free signal, and it is a transient --
      // the guest's own trap handler saves the register file and sets it back to
      // Clean, so whether the host ever samples a Dirty depends on when its exits
      // happen to fall. Measured: the same guest running the same vecbench showed
      // `vector' in its observation set on some runs and not others. The exact
      // answer lives in the guest, which knows which tasks hold state, so the
      // decision is a negotiation rather than an observation.
      if (!(w->cfg.flags2 & VMSTATE_F_VEC_REVOKED) && w->cfg.vlenb > 0 &&
          (!plat.has_v || plat.vlenb < (uint64)w->cfg.vlenb)) {
        printk("hyp: refusing a machine with a %d-byte vector unit: this host has "
               "%s. Ask the guest to give it up first\n", w->cfg.vlenb,
               plat.has_v ? "narrower ones" : "none");
        r = -ENOTSUP;
        goto out;
      }
      src_kind = w->cfg.vic_kind;
      // ---- which tier is this machine? ----
      //
      // A guest that has spoken the revision protocol will read a new description
      // and rebind, so it is built with this host's native controller and told.
      // One that has not -- an unmodified guest -- would never read it, so the
      // shape it left with is kept: the same controller kind, as a software model
      // on this host. Correct on any host, at the price of emulation exits for
      // every interrupt; and the exits are exactly what a participating guest
      // saves, which is the whole argument for speaking the protocol.
      int native = plat.intc_kind == INTC_PLIC ? VIC_PLIC
                 : plat.intc_kind == INTC_APLIC ? VIC_APLIC : VIC_IMSIC;
      uint64 native_shift = plat_bootarg("guestmap=alt") ? G_MMIO_ALT_SHIFT : 0;
      w->cfg.cfg.flags &= ~(VMF_WANT_MASK | VMF_MMIO_SET);
      // the layout axis, same rule: a guest that cannot be told keeps the map
      // it left with; one that can is given this host's and told
      if (!(w->cfg.flags2 & VMSTATE_F_CAPABLE) && w->cfg.mmio_shift != native_shift) {
        w->cfg.cfg.flags |= VMF_MMIO_SET;
        w->cfg.cfg.mmio_shift = w->cfg.mmio_shift;
        printk("hyp: a machine arriving with its devices at +0x%lx does not speak "
               "revision: keeping that map\n", w->cfg.mmio_shift);
      }
      src_shift = w->cfg.mmio_shift;
      if (!(w->cfg.flags2 & VMSTATE_F_CAPABLE) && src_kind != native) {
        w->cfg.cfg.flags |= src_kind == VIC_PLIC  ? VMF_WANT_PLIC
                          : src_kind == VIC_APLIC ? VMF_WANT_APLIC : VMF_WANT_IMSIC;
        printk("hyp: a machine arriving as %s does not speak revision: keeping "
               "%s on this %s host\n", vic_kindname(src_kind),
               vic_kindname(src_kind), vic_kindname(native));
      }
      id = vm_create_blank(&w->cfg.cfg);
      if (id < 0) {
        r = -ENOSPC;
        goto out;
      }
      if ((vm = vm_lookup(id)) == 0) {
        r = -ENOENT;
        goto out;
      }
      vm->mem_limit = w->cfg.mem_limit;
      // go on from the generation the guest last saw, not from zero, and count
      // it as settled: nothing is pending until this host asks for something.
      vm->revise_gen = w->cfg.revise_gen;
      vm->revise_acked = w->cfg.revise_gen;
      // a machine that gave the unit up stays without it here, whatever this
      // host has: the guest is running code that was told there is none.
      vm->vec_revoked = (w->cfg.flags2 & VMSTATE_F_VEC_REVOKED) ? 1 : 0;
      vm->revise_capable = (w->cfg.flags2 & VMSTATE_F_CAPABLE) ? 1 : 0;
    } else if (strncmp(sec.name, "TIME", sizeof(sec.name)) == 0) {
      if ((r = get(s, &w->tm, sizeof(w->tm))) < 0)
        goto out;
    } else if (strncmp(sec.name, "VCPU", sizeof(sec.name)) == 0) {
      if (vm == 0 || nvs >= vm->nvcpu) {
        r = -EINVAL;
        goto out;
      }
      if ((r = get(s, &w->vs, sizeof(w->vs))) < 0)
        goto out;
      restore_vcpu(&vm->vcpu[nvs], &w->vs);
      if (w->vs.has_vstate) {
        // straight into the vcpu's own page: the machine has one for
        // every vcpu when the host supports vectors.
        //
        // A host without a vector unit has no page to put it in, and used to
        // refuse the machine here with EINVAL -- after the source had already
        // handed it over, which lost the guest. It is now read and dropped,
        // and that is sound rather than merely convenient: the legality test in
        // the CFG section has already established that this guest never wrote
        // its vector registers (OBS_VECTOR_USED is not in its observation set),
        // so the page being dropped contains nothing the guest has looked at.
        // A guest that *had* used the unit never reaches this point.
        if (vm->vcpu[nvs].vstate == 0) {
          if ((r = get(s, page, PGSIZE)) < 0)
            goto out;
        } else if ((r = get(s, vm->vcpu[nvs].vstate, PGSIZE)) < 0) {
          goto out;
        }
      }
      nvs++;
    } else if (strncmp(sec.name, "DEV", sizeof(sec.name)) == 0) {
      if (vm == 0 || sec.len != sizeof(*dev)) {
        r = -EINVAL;
        goto out;
      }
      if ((r = get(s, dev, sizeof(*dev))) < 0)
        goto out;
      restore_dev(vm, dev, src_kind);
    } else if (strncmp(sec.name, "CONSO", sizeof(sec.name)) == 0) {
      if (vm == 0 || sec.len != sizeof(vm->cons_out)) {
        r = -EINVAL;
        goto out;
      }
      if ((r = get(s, &vm->cons_out, sizeof(vm->cons_out))) < 0)
        goto out;
    } else if (strncmp(sec.name, "CONSI", sizeof(sec.name)) == 0) {
      if (vm == 0 || sec.len != sizeof(vm->cons_in)) {
        r = -EINVAL;
        goto out;
      }
      if ((r = get(s, &vm->cons_in, sizeof(vm->cons_in))) < 0)
        goto out;
    } else if (strncmp(sec.name, "MEM", sizeof(sec.name)) == 0) {
      if (vm == 0) {
        r = -EINVAL;      // MEM before CFG: not a file this wrote
        goto out;
      }
      // pages until the sentinel: the section has no length, because a
      // connection cannot be written backwards and a file written from
      // one has to read the same way
      for (;;) {
        if ((r = get(s, &w->ph, sizeof(w->ph))) < 0)
          goto out;
        if (w->ph.gpa == VMSTATE_END)
          break;
        if ((r = get(s, page, PGSIZE)) < 0)
          goto out;
        if (vm_copyout(vm, w->ph.gpa, page, PGSIZE) < 0) {
          r = -ENOMEM;    // the host has no room for the guest it had
          goto out;
        }
        got += PGSIZE;
        if ((got & ((1 << 22) - 1)) < PGSIZE)
          printk("vmstate: took %ld MB\n", (long)(got >> 20));
      }
      next = s->off;       // where the pages actually ended
    } else if (strncmp(sec.name, "END", sizeof(sec.name)) == 0) {
      // the sections are over. A file says in its header as well; a
      // connection has only this.
      break;
    }
    s->off = next;         // whatever it was, skip the rest of it
  }

  if (vm == 0 || nvs != vm->nvcpu) {
    r = -EINVAL;
    goto out;
  }
  // the guest's clock goes on from where it stopped rather than jumping
  // to the host's: htimedelta is what the guest adds to the host's
  // counter, and the difference between then and now is what it lost.
  vm->htimedelta = w->tm.htimedelta - (r_time() - w->tm.saved_time);
  // A machine that has landed on a host of a different shape owes its guest a
  // new description. It is asked for here, with no vcpu running yet: the request
  // is a flag and a pending VSSIP per vcpu, so the guest meets it on its very
  // first instruction on this host rather than running for a while against a
  // machine that is no longer the one underneath it.
  //
  // This has to be after the memory, not before: vm_revise() writes the new
  // device tree into guest RAM, and the MEM section would have put the old one
  // back over it.
  uint32 owed = 0;
  if (src_kind >= 0 && src_kind != vm->vic_kind)
    owed |= REV_INTC | REV_DEVICES;
  if (src_kind >= 0 && src_shift != vm->mmio_shift) {
    owed |= REV_DEVICES;
    printk("hyp: vm %d: arrived with devices at +0x%lx, this host puts them at "
           "+0x%lx\n", vm->id, src_shift, vm->mmio_shift);
  }
  if (owed && revise_muted())
    printk("hyp: vm %d: arrived needing revision 0x%x and norevise=1: not telling it\n",
           vm->id, owed);
  else if (owed && vm_revise(vm, owed) < 0) {
    printk("hyp: vm %d: arrived needing a new description and could not be told\n",
           vm->id);
    r = -EINVAL;
    goto out;
  }
  vmstate_restored(vm);
  if (idp)
    *idp = id;
  r = 0;
out:
  if (w)
    kfree(w);
  if (dev)
    kfree(dev);
  if (page)
    kfree(page);
  if (r < 0 && vm)
    vm_destroy(vm);
  return r;
}
