//
// The guest's side of machine revision (see mutable.md).
//
// A hypervisor tells a guest what machine it is on exactly once, and every
// painful thing about heterogeneous virtualization follows from that: fleets
// mask CPU features down to a common floor, and machines are pinned to an
// instance family for life. This is the guest half of taking it back.
//
// The host bumps a revision generation, rebuilds the device tree where this
// guest already has it, and pokes us with a supervisor software interrupt that
// carries no IPI reason. That channel is deliberate: a revision may be changing
// the *interrupt controller*, so a notification which travelled through the
// interrupt controller could not survive its own delivery. A software interrupt
// with nothing behind it is something this kernel never otherwise sees, so the
// check below costs nothing on any normal path.
//
// A guest that does not implement this never calls, never acks, and is never
// revised. Nothing breaks: it keeps the description it booted with, which is
// still a true description of a machine it can run on.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "intc.h"

#define SBI_EXT_MACHINE 0x4D414348   // "MACH"
#define MACH_VERSION  0
#define MACH_PENDING  1
#define MACH_DESCRIBE 2
#define MACH_ACK      3
#define MACH_DTB_MAX  4

// what a revision can change, and what we report adopting (must match hyp.h)
#define REV_DEVICES  (1u << 0)
#define REV_INTC     (1u << 1)
#define REV_VLEN     (1u << 2)
#define REV_NONE     0u

struct sbiret {
  long err;
  long val;
};

// An ecall from VS mode traps to the hypervisor. On bare metal there is no
// firmware underneath this kernel at all (it boots with -bios none and is its
// own machine mode), so this must never be called unless we are a guest.
static struct sbiret
sbi_call(uint64 ext, uint64 fid, uint64 a0)
{
  register uint64 r_a0 asm("a0") = a0;
  register uint64 r_a6 asm("a6") = fid;
  register uint64 r_a7 asm("a7") = ext;
  register uint64 r_a1 asm("a1") = 0;

  asm volatile("ecall"
               : "+r"(r_a0), "+r"(r_a1)
               : "r"(r_a6), "r"(r_a7)
               : "memory");
  struct sbiret ret = {(long)r_a0, (long)r_a1};
  return ret;
}

// ---- coordinating a revision across harts ----
//
// Every hart is poked, because each has per-hart controller state to bring up.
// One of them does the global part -- re-read the description, decide what can
// be adopted, change the live `plat' -- and the rest wait for that to finish and
// then do only their own. The lock is what makes "one of them" true; the
// generation numbers are what make a hart that arrives late do the right thing
// rather than redo the global part.
//
// Per-hart work is asynchronous on purpose: the host's ack means the
// *description* has been adopted, which is what it needs to know. A source
// whose hart has not re-initialised yet is level-triggered and simply waits,
// which is why this is safe rather than merely convenient.
static struct spinlock rev_lock;
static uint32 revision_global;    // generation whose global part is complete
static uint32 revision_hart[NCPU];// generation each hart has applied locally
static uint32 revision_seen;      // deprecated by the two above; kept for /sys
static int revision_supported = -1;   // -1 unknown, 0 no, 1 yes
uint64 machine_revisions;         // how many we have been through (for /sys)

// Does the thing underneath us speak the revision protocol? Asked once.
static int
supported(void)
{
  if (revision_supported < 0) {
    if (!plat.is_guest || plat_bootarg("norevise")) {
      // not a guest, or asked to behave like one that predates the protocol
      // (`norevise' on the guest command line): never asks, never acks, and the
      // host treats it as legacy. This is how the legacy tier is tested without
      // a second guest OS.
      revision_supported = 0;
    } else {
      struct sbiret r = sbi_call(SBI_EXT_MACHINE, MACH_VERSION, 0);
      revision_supported = (r.err == 0 && r.val >= 1);
    }
  }
  return revision_supported;
}

// Re-read the machine description and say what changed.
//
// Parsed into a shadow of `plat' rather than over the top of it: this runs in
// interrupt context with the rest of the kernel using the live copy, and a
// half-updated platform description is worse than an out-of-date one. What is
// adopted for real is decided per subsystem, and for now that is nothing --
// this is the round trip, not yet the reconfiguration.
static uint32
reread(uint64 dtb_gpa, struct platform *out)
{
  struct platform live = plat;      // keep the one everybody else is using
  uint32 changed = 0;

  // Parse into an *empty* description, not over the live one. platform_init()
  // fills whatever `plat' already holds, and add_virtio() appends: re-reading a
  // tree in which the four virtio devices had moved produced eight entries,
  // sorted by address, whose first four were the old addresses. The comparison
  // below then saw "the device count changed" and "nothing moved", the drivers
  // stayed on the old windows, and the guest died on its first store to one
  // (vscause 7, vstval 0x10002050 -- the old virtio-net notify register).
  memset(&plat, 0, sizeof(plat));
  plat.is_guest = live.is_guest;    // not in the tree; known from boot
  platform_init(dtb_gpa);           // fills `plat' from the new blob
  struct platform fresh = plat;
  plat = live;                      // and put the live one straight back
  *out = fresh;

  if (fresh.intc_kind != live.intc_kind)
    changed |= REV_INTC;
  // misa is not in the device tree: has_v is computed from it (platform.c), so
  // a vector unit that has come or gone is only visible by reading the CSR
  // again. That read traps to the host, which is where the masking happens.
  uint64 misa = r_misa();
  out->misa = misa;
  out->has_v = (misa >> 21) & 1;
  out->vlenb = out->has_v ? live.vlenb : 0;
  if (out->has_v != live.has_v)
    changed |= REV_VLEN;
  if (fresh.nvirtio != live.nvirtio || fresh.ram_size != live.ram_size ||
      fresh.ncpu != live.ncpu || fresh.rtc_base != live.rtc_base)
    changed |= REV_DEVICES;
  for (int i = 0; i < fresh.nvirtio && i < live.nvirtio; i++)
    if (fresh.virtio_base[i] != live.virtio_base[i])
      changed |= REV_DEVICES;   // a device moved: the layout axis
  return changed;
}

// Called once from main() on hart 0, before the other harts are running.
//
// The lock cannot be initialised lazily on first use: two harts reaching that
// test together both run initlock, one of them re-initialising the lock while
// the other holds it. With one vcpu it never happened; with four it hung a guest
// on fork-heavy work, and it took a bisect against a clean tree to believe it
// was mine.
void
machine_init(void)
{
  initlock(&rev_lock, "machrev");
  // and say so, once, at boot: asking the protocol's version is how the host
  // learns that this guest can be told about a new description. Asked lazily --
  // on the first revision check, which was how it worked -- a guest that had
  // never been revised looked exactly like one that predates the protocol, and a
  // destination preserved its controller instead of revising it. The capability
  // has to be announced before it is needed, which means here.
  supported();
}

// Map a device's registers into the kernel table if they are not there already.
//
// Asked rather than assumed, because the controllers overlap: a PLIC and an
// APLIC's M domain share 0x0c000000, so a guest moving from one to the other
// finds half of what it needs already mapped and kvmmap_rt panics on a remap.
static void
map_device(uint64 base, uint64 size, int wanted)
{
  if (!wanted || base == 0 || size == 0)
    return;
  for (uint64 off = 0; off < size; off += PGSIZE)
    if (!kvm_mapped(base + off))
      kvmmap_rt(base + off, base + off, PGSIZE, PTE_R | PTE_W);
}

// Called from the supervisor-software-interrupt path when no IPI reason was
// pending -- see kernel/trap.c. Cheap when there is nothing to do: one ecall,
// and only on a software interrupt nobody asked for.
void
machine_revise_check(void)
{
  if (!supported())
    return;

  int hart = cpuid();
  struct sbiret p = sbi_call(SBI_EXT_MACHINE, MACH_PENDING, 0);
  if (p.err)
    return;
  uint32 gen = (uint32)p.val;
  if (gen == revision_hart[hart])
    return;                     // this hart is already up to date

  // the global part happens once; whichever hart gets here first does it
  acquire(&rev_lock);
  if (revision_global == gen) {
    release(&rev_lock);
    // somebody else has already changed the description: this hart only has to
    // bring its own controller state up against it
    intc_inithart();
    revision_hart[hart] = gen;
    return;
  }
  release(&rev_lock);
  struct sbiret d = sbi_call(SBI_EXT_MACHINE, MACH_DESCRIBE, 0);
  if (d.err) {
    revision_seen = gen;            // cannot read it; do not ask again
    return;
  }

  struct platform fresh;
  acquire(&rev_lock);
  if (revision_global == gen) {
    // lost the race while fetching the description
    release(&rev_lock);
    intc_inithart();
    revision_hart[hart] = gen;
    return;
  }

  uint32 changed = reread((uint64)d.val, &fresh);
  revision_seen = gen;
  machine_revisions++;

  uint32 adopted = REV_NONE;

  // ---- adopting a different interrupt controller ----
  //
  // This is the part a feature mask cannot express: PLIC, APLIC and
  // APLIC-MSI+IMSIC are three programming models, and this kernel is holding a
  // driver for one of them. Adoption is: take the new controller's identity and
  // addresses into the live platform description, then bring the driver up
  // against it from scratch. intc_init() re-enables the sources this kernel
  // cares about, which is why the host does not have to translate our
  // configuration -- we rebuild it here.
  //
  // Interrupts are off (this is a trap handler), so nothing is delivered while
  // the live description is inconsistent.
  if (changed & REV_INTC) {
    // The registers of the incoming controller have to be *mapped* before the
    // driver can touch them, and this kernel's page table was built at boot for
    // the controller it had then (see kvminit). This is the part of a machine
    // description that is not data: it is baked into the page tables, and a
    // guest that is handed a different controller faults on its first register
    // write -- `scause=0xf stval=0x0d000000', which is the APLIC's S domain,
    // unmapped because this guest booted with a PLIC.
    //
    // kvmmap_rt panics on a remap, so only what is not already there is added;
    // nothing is unmapped, because the outgoing controller's registers are
    // harmless to leave mapped and unmapping them would need a TLB shootdown
    // on every hart for no gain.
    map_device(fresh.plic_base, fresh.plic_size, fresh.intc_kind == INTC_PLIC);
    map_device(fresh.aplic_m_base, fresh.aplic_size,
               fresh.intc_kind != INTC_PLIC);
    map_device(fresh.aplic_s_base, fresh.aplic_size,
               fresh.intc_kind != INTC_PLIC);
    map_device(fresh.imsic_s_base, fresh.imsic_s_size,
               fresh.intc_kind == INTC_IMSIC);

    plat.intc_kind = fresh.intc_kind;
    plat.plic_base = fresh.plic_base;
    plat.plic_size = fresh.plic_size;
    plat.aplic_m_base = fresh.aplic_m_base;
    plat.aplic_s_base = fresh.aplic_s_base;
    plat.aplic_size = fresh.aplic_size;
    plat.aplic_nsrc = fresh.aplic_nsrc;
    plat.imsic_m_base = fresh.imsic_m_base;
    plat.imsic_s_base = fresh.imsic_s_base;
    plat.imsic_s_size = fresh.imsic_s_size;
    plat.imsic_guest_bits = fresh.imsic_guest_bits;
    plat.imsic_nids = fresh.imsic_nids;
    intc_init();
    intc_inithart();
    adopted |= REV_INTC;
  }

  // ---- adopting a vector unit that has come or gone ----
  //
  // Gaining one is always safe: nothing is relying on a unit that was not there.
  // Losing one is only safe if nothing is relying on it *now* -- a task holding
  // vector state has thirty-two registers of it with nowhere to go on a machine
  // that has no vector unit. So the guest looks, and refuses if it finds one.
  // The refusal is reported, and the host can ask again later: the same guest
  // becomes revisable the moment those tasks exit or their state is dropped.
  if (changed & REV_DEVICES) {
    // A device moved. Its virtqueues live in *our* memory and the host's model
    // keeps all its state; only the register window changed address. So the
    // work is: map the new window, and tell each driver where its registers
    // are now. Nothing is renegotiated, nothing is lost, no request in flight is
    // disturbed -- the same reasoning as the controller: what must be carried is
    // what the other side owns, and here the other side owns nothing we cannot
    // simply point at again. A uniform shift keeps the sorted order platform.c
    // built, so devices pair up by index.
    //
    // Nothing is printed until every device has moved, and the reason is worth
    // a sentence: the console this kernel prints on is a virtio device, and it
    // is one of the ones moving. A line written after the first device moved but
    // before the console did would go to the console's *old* window, which the
    // host has just stopped answering.
    uint64 moved_from[PLAT_MAXVIRTIO], moved_to[PLAT_MAXVIRTIO];
    int nmoved = 0;
    for (int i = 0; i < fresh.nvirtio && i < plat.nvirtio; i++) {
      uint64 old = plat.virtio_base[i], nw = fresh.virtio_base[i];
      if (old == nw)
        continue;
      map_device(nw, 0x1000, 1);
      virtio_disk_rebase(old, nw);
      virtio_net_rebase(old, nw);
      virtio_balloon_rebase(old, nw);
      virtio_con_rebase(old, nw);
      plat.virtio_base[i] = nw;
      if (nmoved < PLAT_MAXVIRTIO) {
        moved_from[nmoved] = old;
        moved_to[nmoved++] = nw;
      }
    }
    for (int i = 0; i < nmoved; i++)
      printk("machine: virtio device moved 0x%lx -> 0x%lx\n", moved_from[i], moved_to[i]);
    adopted |= REV_DEVICES;
  }

  if (changed & REV_VLEN) {
    if (fresh.has_v) {
      plat.has_v = 1;
      plat.misa = fresh.misa;
      plat.vlenb = r_vlenb();
      adopted |= REV_VLEN;
    } else {
      // Which tasks are actually *relying* on the unit? Not the ones with a
      // vstate buffer -- this kernel allocates one for every process created
      // while the machine had vectors (proc.c), so that would be all of them
      // and the answer would always be no. What matters is whether the saved
      // register file holds anything: a task that has never executed a vector
      // instruction has the zeroed page it was given.
      //
      // Conservative in the safe direction: a task that used vectors and left
      // zeros behind is counted as a user and merely delays the revocation.
      // Which tasks are actually relying on the unit? Not the ones holding a
      // vstate buffer -- this kernel gives one to every process created while
      // the machine had vectors (proc.c), so that would be all of them. And not
      // the ones whose saved registers are non-zero either: an axpy over zeroed
      // arrays leaves zeroed registers, and that heuristic said "nobody is using
      // it" while vecbench was running. What is exact is whether a task has ever
      // left the unit dirty, which trap.c now records.
      int users = 0;
      proc_iter_start();
      for (struct proc *q = proc_iter_next(0); q; q = proc_iter_next(q))
        if (q->vstate && q->vec_used)
          users++;
      proc_iter_end();
      if (users == 0) {
        // Losing the unit is not only a change of description: every task that
        // was given a vstate buffer has one, and the return-to-user path
        // restores from it unconditionally (kernel/trap.c) -- with vector
        // instructions, which now trap because the unit is gone. So the buffers
        // go first, and only then does the description change. Without this the
        // guest revokes successfully and then wedges on its next return to
        // user, which is exactly what it did the first time.
        proc_iter_start();
        for (struct proc *q = proc_iter_next(0); q; q = proc_iter_next(q)) {
          if (q->vstate == 0)
            continue;
          char *old_state = q->vstate;
          q->vstate = 0;
          for (int k = 0; k < 4; k++)
            q->trapframe->vcsrs[k] = 0;
          kfree(old_state);
        }
        proc_iter_end();
        plat.has_v = 0;
        plat.misa = fresh.misa;
        plat.vlenb = 0;
        adopted |= REV_VLEN;
      } else {
        printk("machine: revision %d: keeping the vector unit, %d task(s) "
               "still hold state in it\n", gen, users);
      }
    }
  }

  printk("machine: revision %d: %s%s%s%s(adopted 0x%x)\n", gen,
         (changed & REV_INTC) ? "interrupt-controller " : "",
         (changed & REV_VLEN) ? "vector-width " : "",
         (changed & REV_DEVICES) ? "devices " : "",
         changed ? "" : "no-change ", adopted);

  revision_global = gen;
  revision_hart[hart] = gen;
  release(&rev_lock);

  // What could not be taken is reported rather than hidden: the host learns
  // exactly which parts of a revision this guest managed, so a refusal is
  // something it can plan around instead of a silent failure.
  sbi_call(SBI_EXT_MACHINE, MACH_ACK, adopted);
}
