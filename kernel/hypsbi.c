//
// the SBI (Supervisor Binary Interface) that guests see: what firmware
// such as OpenSBI would provide underneath a kernel. Linux depends on
// it; xv6 guests never call it.
//
// implemented: Base, TIME, IPI, RFENCE, HSM, SRST, DBCN and the legacy
// console/shutdown calls (SBI v2.0 calling convention: a7 = extension,
// a6 = function, a0/a1 = error/value on return).
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
#include "hyp.h"

#define SBI_SUCCESS               0
#define SBI_ERR_FAILED           -1
#define SBI_ERR_NOT_SUPPORTED    -2
#define SBI_ERR_INVALID_PARAM    -3
#define SBI_ERR_DENIED           -4
#define SBI_ERR_INVALID_ADDRESS  -5
#define SBI_ERR_ALREADY_AVAILABLE -6
#define SBI_ERR_ALREADY_STARTED  -7
#define SBI_ERR_ALREADY_STOPPED  -8

#define SBI_EXT_LEGACY_SET_TIMER 0x00
#define SBI_EXT_LEGACY_PUTCHAR   0x01
#define SBI_EXT_LEGACY_GETCHAR   0x02
#define SBI_EXT_LEGACY_SHUTDOWN  0x08
#define SBI_EXT_BASE   0x10
#define SBI_EXT_TIME   0x54494D45
#define SBI_EXT_IPI    0x735049
#define SBI_EXT_RFENCE 0x52464E43
#define SBI_EXT_HSM    0x48534D
#define SBI_EXT_SRST   0x53525354
#define SBI_EXT_DBCN   0x4442434E
// "MACH" -- machine revision (mutable.md). Not a registered SBI extension id;
// it sits in the experimental space until it is one.
#define SBI_EXT_MACHINE 0x4D414348

static int sbi_hidden(struct vm *vm, uint64 ext);   // F22, below

struct sbiret {
  long err, val;
};

// call fn for every vcpu in an SBI hart mask. base == -1 means all.
static int
for_each_hart(struct vm *vm, uint64 mask, uint64 base, void (*fn)(struct vcpu *, uint64), uint64 arg)
{
  if (base == (uint64)-1) {
    for (int i = 0; i < vm->nvcpu; i++)
      fn(&vm->vcpu[i], arg);
    return 0;
  }
  // F9: the mask names hart ids; the vcpus are id - shift
  if (base >= (uint64)(vm->nvcpu + vm->hart_shift))
    return -1;
  for (int b = 0; b < 64; b++) {
    if (!(mask & (1UL << b)))
      continue;
    int i = vm_vcpu_of_hart(vm, base + b);
    if (i >= 0)
      fn(&vm->vcpu[i], arg);
  }
  return 0;
}

static void
send_ipi(struct vcpu *v, uint64 arg)
{
  v->ssip_req = 1;
  vcpu_kick(v);
}

static void
req_fence(struct vcpu *v, uint64 what)
{
  vcpu_request_fence(v, (int)what);
}

static struct sbiret
sbi_base(struct vcpu *v, uint64 fid, uint64 a0)
{
  struct sbiret r = {SBI_SUCCESS, 0};
  switch (fid) {
  case 0: r.val = 0x02000000; break; // spec version 2.0
  case 1: r.val = 0x58; break;       // implementation id (not one of the registered ones)
  case 2: r.val = 1; break;          // implementation version
  case 3:                            // probe_extension
    if (sbi_hidden(v->vm, a0)) {     // F22: withheld from this machine
      r.val = 0;
      break;
    }
    switch (a0) {
    case SBI_EXT_BASE: case SBI_EXT_TIME: case SBI_EXT_IPI: case SBI_EXT_RFENCE:
    case SBI_EXT_HSM: case SBI_EXT_SRST: case SBI_EXT_DBCN:
    case SBI_EXT_MACHINE:
    case SBI_EXT_LEGACY_SET_TIMER: case SBI_EXT_LEGACY_PUTCHAR: case SBI_EXT_LEGACY_GETCHAR:
    case SBI_EXT_LEGACY_SHUTDOWN:
      r.val = 1;
      break;
    default:
      r.val = 0;
    }
    break;
  case 4: case 5: case 6: r.val = 0; break; // mvendorid, marchid, mimpid
  default: r.err = SBI_ERR_NOT_SUPPORTED;
  }
  return r;
}

// Machine revision. A guest that implements this can be told its machine
// description has changed and re-enumerate; a guest that does not simply never
// calls it, never acks, and keeps the description it booted with.
//
//   fid 0  version()            -> protocol version (1)
//   fid 1  pending()            -> revision generation; 0 means never revised
//   fid 2  describe()           -> a0 = guest-physical address of the device tree
//                                  a1 = its length
//   fid 3  ack(adopted)         -> the guest reports what it managed (REV_*)
//   fid 4  dtb_max              -> the room the description may take
//   fid 5  request(what)        -> the guest asks for a fact it was described
//                                  without (a vector unit it was shown none of):
//                                  the description negotiated the other way
//   fid 6  what                 -> what the last revision said had changed (REV_*):
//                                  the part the tree cannot say (the SBI set)
// F22: is this extension withheld from this machine?
static int
sbi_hidden(struct vm *vm, uint64 ext)
{
  uint32 h = vm->sbi_hidden;
  switch (ext) {
  case SBI_EXT_TIME:   return (h & SBIH_TIME) != 0;
  case SBI_EXT_IPI:    return (h & SBIH_IPI) != 0;
  case SBI_EXT_RFENCE: return (h & SBIH_RFENCE) != 0;
  case SBI_EXT_HSM:    return (h & SBIH_HSM) != 0;
  case SBI_EXT_SRST:   return (h & SBIH_SRST) != 0;
  case SBI_EXT_DBCN:   return (h & SBIH_DBCN) != 0;
  }
  return 0;
}

static struct sbiret
sbi_machine(struct vcpu *v, uint64 fid, uint64 a0, uint64 a1, uint64 a2)
{
  struct vm *vm = v->vm;
  struct sbiret r = {SBI_SUCCESS, 0};

  switch (fid) {
  case 0:
    r.val = 1;
    vm->revise_capable = 1;   // it asked, so it speaks: the legacy tier is off
    break;
  case 1:
    r.val = vm->revise_gen;
    break;
  case 2:
    // the guest asks where its description is; a1 is returned in the second
    // register, which the caller reads as sbiret.val after a second call --
    // simpler to hand back the address here and the length on fid 4.
    r.val = vm->desc_gpa;   // the current slot (hyp.h)
    vm->revise_described = vm->revise_gen;   // L1: the watchdog's evidence
    if (vm->irq_alias_on && vm->revise_acked != vm->revise_gen)
      vm->irq_quiet = 1;    // a renumbering is being taken: device lines held until the ack (vic.c)
    break;
  case 3:
    acquire(&vm->lock);
    // defect 20 (found by the model, L22): an ack names the generation it
    // acknowledges (a1; 0 from a guest that predates this). Without it the ack
    // of an older revision ended a newer one: the guest read generation g, the
    // machine crossed mid-revision, the destination asked g+1 and re-announced,
    // and the guest's ack of g -- arriving after -- ended g+1's alias with the
    // guest still on g's description. A stale ack is refused and the
    // announcement stands: the guest reads again and acknowledges what it read.
    if (a1 && (uint32)a1 != vm->revise_gen) {
      vm->revise_stale++;
      printk("hyp: vm %d: stale ack of revision %d (the machine is on %d): refused\n",
             vm->id, (int)a1, vm->revise_gen);
      release(&vm->lock);
      r.err = SBI_ERR_INVALID_PARAM;
      break;
    }
    vm->revise_acked = vm->revise_gen;
    vm_del_ghosts(vm);            // the old controller's window has answered its last
    vm_irq_alias_end(vm);         // F3: what the devices hold moves to the new numbers
    vm->cboz_prev = vm->cboz_block;   // F21: the old stride is gone
    if (vm->con_going) {              // F18: the guest has let the virtio-console go; now the model may
      vm->con_going = 0;
      vm->con.present = 0;
      vm_del_dev(vm, "virtio-con");
    }
    vm->revise_adopted = (uint32)a0;
    vm->revise_done = r_time();
    if (vm->vec_pending) {
      // A revocation the guest declined is not a revocation: the description
      // must say what the guest is actually holding, or the next move carries a
      // machine "without a unit" to a host without one while a task inside is
      // using it (busy2: the guest arrived and executed nothing but faults).
      vm->vec_pending = 0;
      if (!(a0 & REV_VLEN)) {
        vm->vec_revoked = !vm->vec_revoked;     // the description goes back to what the guest holds; VS untouched
        printk("hyp: vm %d: the guest declined: its vector unit stays %s\n", vm->id,
               vm->vec_revoked ? "absent" : "present");
      } else if (vm->vec_revoked) {
        // ceded: from here the unit is gone -- VS Off on every vcpu, so a vector
        // instruction traps (illegal) instead of touching registers the
        // description no longer has; a vcpu on a hart takes it at its exit
        for (int i = 0; i < vm->nvcpu; i++) {
          if (vm->vcpu[i].running_hart >= 0)
            vm->vcpu[i].vs_force = 1;
          else
            vm->vcpu[i].vsstatus &= ~SSTATUS_VS_MASK;
        }
      }
    }
    release(&vm->lock);
    if (vm->arrival_timeline && vm->t_first_entry) {
      uint64 t0 = vm->t_stream_end;
      printk("hyp: vm %d: arrival timeline: revision asked +%ld us, adopted +%ld us (of which the guest's %ld us)\n",
             vm->id, (long)((vm->revise_asked - t0) / 10), (long)((vm->revise_done - t0) / 10),
             (long)((vm->revise_done - vm->revise_asked) / 10));
      vm->arrival_timeline = 0;
    }
    printk("hyp: vm %d: revision %d adopted (0x%lx) after %ld us\n", vm->id,
           vm->revise_acked, a0,
           (long)((vm->revise_done - vm->revise_asked) / 10));
    break;
  case 4:
    r.val = VM_DTB_MAX;
    break;
  case 6:
    r.val = vm->revise_what;
    break;
  case 5:
    // Lazy revelation, from the guest's side: a machine presented without a
    // vector unit (-L) is asked for one the moment a program asks its kernel
    // whether there is one. A guest that believes its description never
    // executes a vector instruction, so the first-use trap (hypcsr.c) alone
    // would never fire for a well-behaved program; the question is the
    // observation. Granted iff this host has a unit and it was only hidden.
    // a request is a revision of the machine, which is not free: one every
    // tenth of a second per machine, and the rest are refused (L21: a guest
    // asking by the thousand is a guest asking the host to spend its time)
    if (r_time() - vm->request_at < HOST_TIMEBASE / 10) {
      vm->requests_refused++;
      r.err = SBI_ERR_FAILED;
      break;
    }
    vm->request_at = r_time();
    if ((a0 & REV_VLEN) && !vm->vec_revoked && plat.has_v) {
      // already granted -- the guest's first vector instruction trapped here
      // and the unit was given on the spot (hypcsr.c), and the guest's own
      // handler asks a moment later, before its revision thread has taken the
      // grant (lxvs4: "refused (-2)", SIGILL, on a unit that was on its way).
      // The request is idempotent: yes, it is yours; the revision is coming.
      r.val = REV_VLEN;
      break;
    }
    if ((a0 & REV_VLEN) && vm->lazy_v && vm->vec_revoked && plat.has_v) {
      vm_observe(vm, OBS_VECTOR_USED);
      printk("hyp: vm %d: the guest asks for a vector unit: granting it\n", vm->id);
      vm->revise_hold++;
      int rv = vm_revise_vector(vm, 1);
      vm->revise_hold--;
      if (rv == 0)
        r.val = REV_VLEN;
      else
        r.err = SBI_ERR_FAILED;
    } else {
      r.err = SBI_ERR_NOT_SUPPORTED;
    }
    break;
  default:
    r.err = SBI_ERR_NOT_SUPPORTED;
  }
  return r;
}

static struct sbiret
sbi_hsm(struct vcpu *v, uint64 fid, uint64 a0, uint64 a1, uint64 a2)
{
  struct vm *vm = v->vm;
  struct sbiret r = {SBI_SUCCESS, 0};
  switch (fid) {
  case 0: { // hart_start(hartid, start_addr, opaque)
    int t = vm_vcpu_of_hart(vm, a0);     // F9: a hart id
    if (t < 0) {
      if (vm_gripe(&vm->g_sbi))
        printk("hyp: vm %d: hsm hart_start(%ld): no such vcpu\n", vm->id, a0);
      r.err = SBI_ERR_INVALID_PARAM;
      break;
    }
    a0 = t;
    if (a1 < G_RAM_BASE || a1 >= G_RAM_BASE + vm->ram_size) {
      if (vm_gripe(&vm->g_sbi))
        printk("hyp: vm %d: hsm hart_start(%ld): bad address 0x%lx\n", vm->id, a0, a1);
      r.err = SBI_ERR_INVALID_ADDRESS;
      break;
    }
    acquire(&vm->lock);
    int rc = vcpu_hsm_start(&vm->vcpu[a0], a1, a2);
    release(&vm->lock);
    if (rc < 0) {
      if (vm_gripe(&vm->g_sbi))
        printk("hyp: vm %d: hsm hart_start(%ld) refused: vcpu state %d\n", vm->id, a0,
               vm->vcpu[a0].hsm_state);
      r.err = SBI_ERR_ALREADY_AVAILABLE;
    }
    break;
  }
  case 1: // hart_stop: this vcpu parks until started again
    acquire(&vm->lock);
    v->hsm_state = HSM_STOPPED;
    release(&vm->lock);
    break;
  case 2: { // hart_get_status(hartid)
    int t = vm_vcpu_of_hart(vm, a0);     // F9
    if (t < 0)
      r.err = SBI_ERR_INVALID_PARAM;
    else
      r.val = vm->vcpu[t].hsm_state == HSM_STARTED ? 0 : 1; // STARTED=0, STOPPED=1
    break;
  }
  default:
    r.err = SBI_ERR_NOT_SUPPORTED;
  }
  return r;
}

// DBCN console_write: copy num_bytes from guest physical base to the console.
static struct sbiret
sbi_dbcn_write(struct vcpu *v, uint64 num, uint64 lo, uint64 hi)
{
  struct sbiret r = {SBI_SUCCESS, 0};
  uint64 gpa = lo | (hi << 32);
  char buf[64];
  uint64 done = 0;
  while (done < num) {
    uint64 n = num - done;
    if (n > sizeof(buf))
      n = sizeof(buf);
    if (vm_copyin(v->vm, buf, gpa + done, n) < 0) {
      r.err = SBI_ERR_INVALID_ADDRESS;
      break;
    }
    for (uint64 i = 0; i < n; i++)
      vm_cons_putc(v->vm, buf[i]);
    done += n;
  }
  r.val = done;
  return r;
}

static struct sbiret
sbi_dbcn_read(struct vcpu *v, uint64 num, uint64 lo, uint64 hi)
{
  struct sbiret r = {SBI_SUCCESS, 0};
  uint64 gpa = lo | (hi << 32);
  uint64 done = 0;
  while (done < num) {
    int c = vm_cons_getc(v->vm);
    if (c < 0)
      break;
    char ch = c;
    if (vm_copyout(v->vm, gpa + done, &ch, 1) < 0) {
      r.err = SBI_ERR_INVALID_ADDRESS;
      break;
    }
    done++;
  }
  r.val = done;
  if (done && plat_bootarg("condbg"))
    printk("hyp: vm %d: dbcn read %ld byte(s) to 0x%lx\n", v->vm->id, (long)done, gpa);
  return r;
}

int
hyp_ecall(struct vcpu *v)
{
  struct vm *vm = v->vm;
  uint64 ext = v->regs[17], fid = v->regs[16];
  uint64 a0 = v->regs[10], a1 = v->regs[11], a2 = v->regs[12];
  struct sbiret r = {SBI_SUCCESS, 0};
  int legacy = 0;

  if (sbi_hidden(vm, ext)) {
    // F22: an extension this machine was described without behaves as one
    // the firmware does not have -- not as one that is broken
    v->regs[10] = SBI_ERR_NOT_SUPPORTED;
    v->regs[11] = 0;
    v->pc += 4;
    return 0;
  }
  switch (ext) {
  case SBI_EXT_BASE:
    r = sbi_base(v, fid, a0);
    break;
  case SBI_EXT_TIME:
    if (fid == 0) {
      v->vstimecmp = vm_host_time_of(vm, a0); // in host units; loaded into the hart on the next entry (F6 scales)
      if (plat_bootarg("tracetimer"))
        printk("hyp: vm %d vcpu %d: set_timer 0x%lx (host 0x%lx, now 0x%lx, tb %u)\n", vm->id, v->id,
               a0, v->vstimecmp, r_time() + vm->htimedelta, vm->timebase);
      // and the host's timer to it: a guest without Sstc gets its VSTIP from
      // this hypervisor, at the deadline -- not at the next host tick, which
      // is up to ten milliseconds later and made a Linux boot with its
      // thousands of millisecond sleeps take over ninety seconds (lx8)
      timer_rearm();
    } else
      r.err = SBI_ERR_NOT_SUPPORTED;
    break;
  case SBI_EXT_IPI:
    if (fid == 0) {
      acquire(&vm->lock);
      if (for_each_hart(vm, a0, a1, send_ipi, 0) < 0)
        r.err = SBI_ERR_INVALID_PARAM;
      release(&vm->lock);
    } else {
      r.err = SBI_ERR_NOT_SUPPORTED;
    }
    break;
  case SBI_EXT_RFENCE:
    // 0 fence_i, 1 sfence_vma, 2 sfence_vma_asid; the hfence variants
    // (3..6) are for nested hypervisors and are not supported.
    if (fid <= 2) {
      acquire(&vm->lock);
      if (for_each_hart(vm, a0, a1, req_fence, fid == 0 ? FENCE_I : FENCE_VMA) < 0)
        r.err = SBI_ERR_INVALID_PARAM;
      release(&vm->lock);
    } else {
      r.err = SBI_ERR_NOT_SUPPORTED;
    }
    break;
  case SBI_EXT_HSM:
    r = sbi_hsm(v, fid, a0, a1, a2);
    break;
  case SBI_EXT_MACHINE:
    r = sbi_machine(v, fid, a0, a1, a2);
    break;
  case SBI_EXT_SRST:
    if (fid == 0) {
      vm_halt(vm); // shutdown, cold reboot and warm reboot all stop the VM
    } else {
      r.err = SBI_ERR_NOT_SUPPORTED;
    }
    break;
  case SBI_EXT_DBCN:
    if (fid == 0)
      r = sbi_dbcn_write(v, a0, a1, a2);
    else if (fid == 1)
      r = sbi_dbcn_read(v, a0, a1, a2);
    else if (fid == 2)
      vm_cons_putc(vm, (int)a0);
    else
      r.err = SBI_ERR_NOT_SUPPORTED;
    break;
  // legacy extensions return only a0.
  case SBI_EXT_LEGACY_SET_TIMER:
    legacy = 1;
    v->vstimecmp = a0;
    break;
  case SBI_EXT_LEGACY_PUTCHAR:
    legacy = 1;
    vm_cons_putc(vm, (int)a0);
    break;
  case SBI_EXT_LEGACY_GETCHAR:
    legacy = 1;
    r.err = vm_cons_getc(vm); // -1 if none
    break;
  case SBI_EXT_LEGACY_SHUTDOWN:
    legacy = 1;
    vm_halt(vm);
    break;
  default:
    r.err = SBI_ERR_NOT_SUPPORTED;
    break;
  }
  v->regs[10] = r.err;
  if (!legacy)
    v->regs[11] = r.val;
  v->pc += 4;
  return 0;
}
