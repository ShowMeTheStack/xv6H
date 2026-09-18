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
#include "fs.h"
#include "file.h"
#include "elf.h"
#include "stat.h"
#include "platform.h"
#include "intc.h"
#include "hyp.h"

struct vm vms[NVM];
static struct spinlock vmslock;

void guestret(struct vcpu *v);
void guestvec(void);
void kernelvec(void);

_Static_assert(__builtin_offsetof(struct vcpu, hostctx) == 256, "vcpu layout");
_Static_assert(__builtin_offsetof(struct vcpu, host_tp) == 368, "vcpu layout");

// ---- guest memory pool: 2 MB chunks above plat.host_top ----

#define POOL_MAXCHUNKS 2048 // 4 GB
static struct {
  struct spinlock lock;
  uint64 base, end;
  int nchunks, nfree;
  char used[POOL_MAXCHUNKS];
} pool;

static void
hpool_init(void)
{
  initlock(&pool.lock, "hpool");
  pool.base = (plat.host_top + HCHUNK - 1) & ~(HCHUNK - 1);
  pool.end = plat.ram_end & ~(HCHUNK - 1);
  if (pool.end <= pool.base) {
    pool.nchunks = 0;
    return;
  }
  pool.nchunks = (pool.end - pool.base) / HCHUNK;
  if (pool.nchunks > POOL_MAXCHUNKS)
    pool.nchunks = POOL_MAXCHUNKS;
  pool.nfree = pool.nchunks;
}

static uint64
hpool_alloc(void)
{
  acquire(&pool.lock);
  for (int i = 0; i < pool.nchunks; i++) {
    if (!pool.used[i]) {
      pool.used[i] = 1;
      pool.nfree--;
      release(&pool.lock);
      uint64 pa = pool.base + (uint64)i * HCHUNK;
      memset((void *)pa, 0, HCHUNK);
      return pa;
    }
  }
  release(&pool.lock);
  return 0;
}

static void
hpool_free(uint64 pa)
{
  if (pa < pool.base || pa >= pool.end || (pa & (HCHUNK - 1)))
    panic("hpool_free");
  int i = (pa - pool.base) / HCHUNK;
  acquire(&pool.lock);
  if (!pool.used[i])
    panic("hpool_free: not used");
  pool.used[i] = 0;
  pool.nfree++;
  release(&pool.lock);
}

int
hpool_free_chunks(void)
{
  return pool.nfree;
}

// ---- init ----

void
hypinit(void)
{
  initlock(&vmslock, "vms");
  for (int i = 0; i < NVM; i++) {
    initlock(&vms[i].lock, "vm");
    vms[i].id = i;
    vms[i].state = VM_FREE;
  }
  if (!plat.has_h) {
    printk("hyp: no hypervisor extension; guests disabled\n");
    return;
  }
  hpool_init();
  hypinithart();
  printk("hyp: hypervisor extension present, guest pool %d MB (%d chunks), %d guest interrupt files/hart\n",
         pool.nchunks * 2, pool.nchunks, vimsic_geilen());
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

// ---- G-stage page tables (Sv39x4) ----

static uint64 *
pt_page(struct vm *vm)
{
  if (vm->ptnext + PGSIZE > vm->ptchunk + HCHUNK)
    panic("hyp: page table chunk exhausted");
  uint64 *p = (uint64 *)vm->ptnext;
  vm->ptnext += PGSIZE;
  return p;
}

// map one 2 MB guest-physical chunk to host physical hpa.
static void
gstage_map_chunk(struct vm *vm, uint64 gpa, uint64 hpa)
{
  uint64 vpn2 = (gpa >> 30) & 0x7ff; // 11 bits: the root has 2048 entries
  uint64 vpn1 = (gpa >> 21) & 0x1ff;
  uint64 *root = vm->hgatp_root;
  uint64 *l1;
  if (root[vpn2] & PTE_V) {
    l1 = (uint64 *)PTE2PA(root[vpn2]);
  } else {
    l1 = pt_page(vm);
    root[vpn2] = PA2PTE(l1) | PTE_V;
  }
  // G-stage leaf PTEs must have U set; preset A and D.
  l1[vpn1] = PA2PTE(hpa) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | (1L << 6) | (1L << 7);
}

int
vm_gpa2hpa(struct vm *vm, uint64 gpa, uint64 *hpa, uint64 *len)
{
  if (gpa < G_RAM_BASE || gpa >= G_RAM_BASE + vm->ram_size)
    return -1;
  uint64 off = gpa - G_RAM_BASE;
  int k = off / HCHUNK;
  uint64 in = off % HCHUNK;
  *hpa = vm->chunk[k] + in;
  *len = HCHUNK - in;
  return 0;
}

int
vm_copyin(struct vm *vm, void *dst, uint64 gpa, uint64 n)
{
  while (n > 0) {
    uint64 hpa, len;
    if (vm_gpa2hpa(vm, gpa, &hpa, &len) < 0)
      return -1;
    if (len > n)
      len = n;
    memmove(dst, (void *)hpa, len);
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
    if (vm_gpa2hpa(vm, gpa, &hpa, &len) < 0)
      return -1;
    if (len > n)
      len = n;
    memmove((void *)hpa, src, len);
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
  return &vms[id];
}

static void
vm_free_resources(struct vm *vm)
{
  for (int i = 0; i < vm->nchunks; i++)
    if (vm->chunk[i])
      hpool_free(vm->chunk[i]);
  vm->nchunks = 0;
  if (vm->ptchunk)
    hpool_free(vm->ptchunk);
  vm->ptchunk = 0;
  if (vm->blk.bounce)
    kfree(vm->blk.bounce);
  vm->blk.bounce = 0;
}

// release the disk image file; must not hold spinlocks (may sleep).
static void
vm_put_disk(struct inode *ip)
{
  if (ip) {
    begin_op();
    iput(ip);
    end_op();
  }
}

// is the file already the disk of another VM?
static int
disk_in_use(struct vm *self, struct inode *ip)
{
  for (int i = 0; i < NVM; i++) {
    struct vm *o = &vms[i];
    if (o == self || o->state == VM_FREE || o->disk_ip == 0)
      continue;
    if (o->disk_ip->dev == ip->dev && o->disk_ip->inum == ip->inum)
      return 1;
  }
  return 0;
}

static void
vcpu_reset(struct vcpu *v, struct vm *vm, int id)
{
  memset(v, 0, sizeof(*v));
  v->vm = vm;
  v->id = id;
  v->running_hart = -1;
  v->pc = vm->entry;
  v->regs[10] = id;          // a0 = hartid
  v->regs[11] = vm->dtb_gpa; // a1 = device tree
  v->priv = 1;               // VS mode
  v->in_mmode = 1;           // running the guest's machine-mode boot code
  v->vstimecmp = ~0L;
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
  v->scounteren = 0;
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
  if (mem_bytes < 8 * HCHUNK || (mem_bytes % HCHUNK) != 0 ||
      mem_bytes / HCHUNK > VM_MAXCHUNKS)
    return -1;
  if (cfg->kernel[0] == 0)
    return -1;

  acquire(&vmslock);
  struct vm *vm = 0;
  for (int i = 0; i < NVM; i++) {
    if (vms[i].state == VM_FREE) {
      vm = &vms[i];
      vm->state = VM_NEW;
      break;
    }
  }
  release(&vmslock);
  if (vm == 0)
    return -1;

  int id = vm->id;
  struct spinlock lk = vm->lock;
  memset(vm, 0, sizeof(*vm));
  vm->lock = lk;
  vm->id = id;
  vm->state = VM_NEW;
  vm->cfg = *cfg;
  vm->nvcpu = nvcpu;
  vm->ram_size = mem_bytes;
  vm->nchunks = mem_bytes / HCHUNK;

  vm->ptchunk = hpool_alloc();
  if (vm->ptchunk == 0)
    goto fail;
  vm->hgatp_root = (uint64 *)vm->ptchunk; // 16 KB, 2 MB aligned
  vm->ptnext = vm->ptchunk + 4 * PGSIZE;
  vm->hgatp = HGATP_MODE_SV39X4 | ((uint64)(id + 1) << HGATP_VMID_SHIFT) |
              (vm->ptchunk >> 12);

  for (int i = 0; i < vm->nchunks; i++) {
    vm->chunk[i] = hpool_alloc();
    if (vm->chunk[i] == 0)
      goto fail;
    gstage_map_chunk(vm, G_RAM_BASE + (uint64)i * HCHUNK, vm->chunk[i]);
  }

  // the guest device tree sits in the last 64 KB of guest RAM.
  vm->dtb_gpa = G_RAM_BASE + vm->ram_size - 0x10000;

  // the disk image: a regular file in the host file system.
  if (cfg->disk[0]) {
    begin_op();
    struct inode *ip = namei(cfg->disk);
    if (ip == 0) {
      end_op();
      goto fail;
    }
    ilock(ip);
    int ok = (ip->type == T_FILE && ip->size >= 512);
    iunlock(ip);
    if (!ok || disk_in_use(vm, ip)) {
      iput(ip);
      end_op();
      goto fail;
    }
    end_op();
    vm->disk_ip = ip;
  }

  vm->vic_kind = plat.intc_kind;
  vm->imsic_hw = (plat.intc_kind == INTC_IMSIC && vimsic_geilen() > 0 && !(cfg->flags & VMF_SOFTIMSIC));

  vm->ndev = 0;
  vuart_init(vm);
  vclint_init(vm);
  vic_init(vm);
  vvirtio_init(vm);
  if (vm->blk.bounce == 0)
    goto fail;

  if (vm_load(vm) < 0)
    goto fail;

  for (int i = 0; i < nvcpu; i++)
    vcpu_reset(&vm->vcpu[i], vm, i);

  return id;

fail:
  vm_free_resources(vm);
  {
    struct inode *ip = vm->disk_ip;
    vm->disk_ip = 0;
    vm_put_disk(ip);
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

  begin_op();
  if ((ip = namei(cfg->kernel)) == 0) {
    end_op();
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
  end_op();
  if (ret < 0)
    return -1;

  // device tree.
  char *dtb = kalloc();
  if (dtb == 0)
    return -1;
  int n = hyp_make_dtb(vm, dtb, PGSIZE);
  if (n <= 0 || vm_copyout(vm, vm->dtb_gpa, dtb, n) < 0)
    ret = -1;
  kfree(dtb);
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
  acquire(&vm->lock);
  if (vm->state == VM_FREE) {
    release(&vm->lock);
    return -1;
  }
  if (vm->vcpus_running == 0) {
    // nobody is inside vm_run: free right here.
    vm_free_resources(vm);
    struct inode *ip = vm->disk_ip;
    vm->disk_ip = 0;
    vm->state = VM_FREE;
    release(&vm->lock);
    vm_put_disk(ip);
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
  if (cause == EXC_ILLEGAL && v->injected <= 3)
    printk("hyp: vm %d vcpu %d: illegal instruction 0x%lx at guest pc 0x%lx passed to guest\n",
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
  v->scounteren = csr_read(0x106);
  v->senvcfg = csr_read(0x10a);
  // the guest touched the FPU (hardware marks HS sstatus.FS Dirty too).
  if ((r_sstatus() & SSTATUS_FS_MASK) == SSTATUS_FS_DIRTY) {
    v->fcsr = r_fcsr();
    fp_save(v->fregs);
  }
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
    struct vm *vm = &vms[i];
    if (vm->state != VM_RUNNING)
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
    struct vm *vm = &vms[i];
    if (vm->state != VM_RUNNING)
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
    inst = hyp_fetch(v, v->pc);
    break;
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
  if (vm->vcpus_running == 0) {
    vm_free_resources(vm);
    ip = vm->disk_ip;
    vm->disk_ip = 0;
    vm->state = VM_FREE;
  }
  release(&vm->lock);
  vm_put_disk(ip);
  return rc;
}

// statistics for tests: out[0] = free pool chunks, out[1] = total IPIs,
// out[2..] per-VM exit counts.
void
hyp_stats(uint64 *out, int n)
{
  if (n > 0)
    out[0] = pool.nfree;
  if (n > 1) {
    uint64 t = 0;
    for (int i = 0; i < NCPU; i++)
      t += ipi_count[i];
    out[1] = t;
  }
  for (int i = 0; i < NVM && 2 + i < n; i++) {
    uint64 e = 0;
    for (int j = 0; j < vms[i].nvcpu; j++)
      e += vms[i].vcpu[j].exits;
    out[2 + i] = e;
  }
}
