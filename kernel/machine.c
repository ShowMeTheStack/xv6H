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

static uint32 revision_seen;      // the generation this kernel has adopted
static int revision_supported = -1;   // -1 unknown, 0 no, 1 yes
uint64 machine_revisions;         // how many we have been through (for /sys)

// Does the thing underneath us speak the revision protocol? Asked once.
static int
supported(void)
{
  if (revision_supported < 0) {
    if (!plat.is_guest) {
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
reread(uint64 dtb_gpa)
{
  struct platform live = plat;      // keep the one everybody else is using
  uint32 changed = 0;

  platform_init(dtb_gpa);           // fills `plat' from the new blob
  struct platform fresh = plat;
  plat = live;                      // and put the live one straight back

  if (fresh.intc_kind != live.intc_kind)
    changed |= REV_INTC;
  if (fresh.vlenb != live.vlenb || fresh.has_v != live.has_v)
    changed |= REV_VLEN;
  if (fresh.nvirtio != live.nvirtio || fresh.ram_size != live.ram_size ||
      fresh.ncpu != live.ncpu || fresh.rtc_base != live.rtc_base)
    changed |= REV_DEVICES;
  return changed;
}

// Called from the supervisor-software-interrupt path when no IPI reason was
// pending -- see kernel/trap.c. Cheap when there is nothing to do: one ecall,
// and only on a software interrupt nobody asked for.
void
machine_revise_check(void)
{
  if (!supported())
    return;

  struct sbiret p = sbi_call(SBI_EXT_MACHINE, MACH_PENDING, 0);
  if (p.err || (uint32)p.val == revision_seen)
    return;

  uint32 gen = (uint32)p.val;
  struct sbiret d = sbi_call(SBI_EXT_MACHINE, MACH_DESCRIBE, 0);
  if (d.err) {
    revision_seen = gen;            // cannot read it; do not ask again
    return;
  }

  uint32 changed = reread((uint64)d.val);
  revision_seen = gen;
  machine_revisions++;
  printk("machine: revision %d: %s%s%s%s\n", gen,
         (changed & REV_INTC) ? "interrupt-controller " : "",
         (changed & REV_VLEN) ? "vector-width " : "",
         (changed & REV_DEVICES) ? "devices " : "",
         changed ? "" : "(no change)");

  // Adopt nothing yet, and say so honestly: the host learns exactly which
  // parts of a revision a guest could take, which is what makes a refusal
  // something the host can plan around rather than a silent failure.
  sbi_call(SBI_EXT_MACHINE, MACH_ACK, REV_NONE);
}
