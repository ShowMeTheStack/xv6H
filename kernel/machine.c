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
#define MACH_REQUEST  5
#define MACH_WHAT     6
#define MACH_RATE_NOW 7     // F6: serve the described rate from now (at the re-anchor)

// what a revision can change, and what we report adopting (must match hyp.h)
#define REV_DEVICES  (1u << 0)
#define REV_INTC     (1u << 1)
#define REV_VLEN     (1u << 2)
#define REV_SBI      (1u << 3)
#define REV_TIMER    (1u << 4)
#define REV_MEMORY   (1u << 6)
#define REV_NONE     0u

struct sbiret {
  long err;
  long val;
};

// An ecall from VS mode traps to the hypervisor. On bare metal there is no
// firmware underneath this kernel at all (it boots with -bios none and is its
// own machine mode), so this must never be called unless we are a guest.
static struct sbiret
sbi_call3(uint64 ext, uint64 fid, uint64 a0, uint64 a1, uint64 a2)
{
  register uint64 r_a0 asm("a0") = a0;
  register uint64 r_a6 asm("a6") = fid;
  register uint64 r_a7 asm("a7") = ext;
  register uint64 r_a1 asm("a1") = a1;
  register uint64 r_a2 asm("a2") = a2;

  asm volatile("ecall"
               : "+r"(r_a0), "+r"(r_a1)
               : "r"(r_a2), "r"(r_a6), "r"(r_a7)
               : "memory");
  struct sbiret ret = {(long)r_a0, (long)r_a1};
  return ret;
}

// F18: the SBI debug console, when the description names it as the console.
// The kernel's addresses are physical addresses here (the direct map).
//
// Through a static buffer: the SBI takes a *physical* address, and this
// kernel's statics are identity-mapped while a kernel stack is not -- the
// shell's prompt came from consolewrite's stack buffer and the host answered
// INVALID_ADDRESS to every byte of it (con_probe2).
static struct spinlock dbcn_lock;
static char dbcn_buf[256];
static int dbcn_lock_ready;

void
machine_sbi_dbcn_write(const char *s, int n)
{
  extern volatile int panicking;
  if (!dbcn_lock_ready) {
    initlock(&dbcn_lock, "dbcn");
    dbcn_lock_ready = 1;
  }
  while (n > 0) {
    int k = n > (int)sizeof(dbcn_buf) ? (int)sizeof(dbcn_buf) : n;
    if (panicking == 0)
      acquire(&dbcn_lock);
    memmove(dbcn_buf, s, k);
    sbi_call3(0x4442434E, 0, k, (uint64)dbcn_buf, 0);
    if (panicking == 0)
      release(&dbcn_lock);
    s += k;
    n -= k;
  }
}

int
machine_sbi_dbcn_read(char *buf, int n)
{
  struct sbiret r = sbi_call3(0x4442434E, 1, n, (uint64)buf, 0);
  return r.err == 0 ? (int)r.val : 0;
}

static struct sbiret
sbi_call2(uint64 ext, uint64 fid, uint64 a0, uint64 a1)
{
  register uint64 r_a0 asm("a0") = a0;
  register uint64 r_a6 asm("a6") = fid;
  register uint64 r_a7 asm("a7") = ext;
  register uint64 r_a1 asm("a1") = a1;

  asm volatile("ecall"
               : "+r"(r_a0), "+r"(r_a1)
               : "r"(r_a6), "r"(r_a7)
               : "memory");
  struct sbiret ret = {(long)r_a0, (long)r_a1};
  return ret;
}

static struct sbiret
sbi_call(uint64 ext, uint64 fid, uint64 a0)
{
  return sbi_call2(ext, fid, a0, 0);
}

// F5, F4: the firmware's timer and IPIs, for a machine described without Sstc
// or without a CLINT. Both are the SBI's oldest services (TIME, IPI).
void
machine_sbi_set_timer(uint64 when)
{
  sbi_call(0x54494D45, 0, when);
}

void
machine_sbi_ipi(int hart)
{
  sbi_call2(0x735049, 0, 1UL << hart, 0);
}

// F8: this hart leaves the machine. SBI HSM stop does not return; a hart
// started again (F7) enters the kernel afresh at _entry.
void
machine_hart_park(void)
{
  intr_off();
  printk("machine: hart %d parked\n", cpuid());
  sbi_call(0x48534D, 1, 0);
  for (;;)
    asm volatile("wfi");
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
static void sbi_reprobe(void);        // F22, below
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
  //
  // And never into `plat' itself: the other harts keep running on the live
  // description while this one reads the new one. It used to parse into
  // `plat' with a save and a restore around it, and a hart that read the
  // description in that window -- a tick, an IPI -- found a machine with
  // nothing in it: with two harts, a hang on one arrival in two (tmr3/tmr4).
  memset(out, 0, sizeof(*out));
  out->is_guest = live.is_guest;    // not in the tree; known from boot
  platform_parse_into(out, dtb_gpa);
  struct platform fresh = *out;

  if (fresh.intc_kind != live.intc_kind)
    changed |= REV_INTC;
  // the same kind somewhere else is a change of the same class (F2): the
  // driver is bound to registers at an address, and the address is in the
  // page tables as well as in the description
  else if (fresh.plic_base != live.plic_base || fresh.aplic_s_base != live.aplic_s_base ||
           fresh.aplic_m_base != live.aplic_m_base || fresh.imsic_s_base != live.imsic_s_base)
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
      fresh.ncpu != live.ncpu || fresh.rtc_base != live.rtc_base ||
      fresh.clint_base != live.clint_base)           // F4: the CLINT came or went
    changed |= REV_DEVICES;
  for (int i = 0; i < fresh.ncpu && i < live.ncpu && i < NCPU; i++)
    if (fresh.hartid[i] != live.hartid[i])           // F9: the harts were renumbered
      changed |= REV_DEVICES;
  if (fresh.console_kind != live.console_kind)       // F18
    changed |= REV_DEVICES;
  if (fresh.cboz_block != live.cboz_block)           // F21: the cache block cbo.zero clears
    changed |= REV_DEVICES;
  if (fresh.ram_size != live.ram_size)               // F13
    changed |= REV_MEMORY;
  if (fresh.has_sstc != live.has_sstc)               // F5
    changed |= REV_TIMER;
  if (fresh.timebase != live.timebase)               // F6
    changed |= REV_TIMER;
  for (int i = 0; i < fresh.nvirtio && i < live.nvirtio; i++)
    if (fresh.virtio_base[i] != live.virtio_base[i])
      changed |= REV_DEVICES;   // a device moved: the layout axis
  // F3: the same device on another line is a change of the same class
  if (fresh.uart_irq != live.uart_irq || fresh.rtc_irq != live.rtc_irq)
    changed |= REV_DEVICES;
  for (int i = 0; i < fresh.nvirtio && i < live.nvirtio; i++)
    if (fresh.virtio_irq[i] != live.virtio_irq[i])
      changed |= REV_DEVICES;
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
  // only a guest has an SBI to probe: this kernel *is* the M-mode under a host
  // (an ecall from here would land in its own mvec.S, and sbi1/inv4 hung every
  // host at boot when this ran unconditionally)
  if (plat.is_guest)
    sbi_reprobe();
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
// F22: which SBI extensions the firmware under this kernel offers. Probed once
// at boot and again when a revision says the set changed (REV_SBI) -- the tree
// does not describe it, so the host says so through MACH fid 6. Exposed as
// /sys/machine/sbi; consulted by whatever picks between an SBI service and a
// device (the timer and the IPIs, when a machine is revised to lack Sstc or a
// CLINT: M2).
static const uint64 sbi_ext_ids[] = {0x54494D45, 0x735049, 0x52464E43, 0x48534D, 0x53525354, 0x4442434E};
static const char *sbi_ext_names[] = {"time", "ipi", "rfence", "hsm", "srst", "dbcn"};
static uint32 sbi_present;      // SBIH_* bits, present

static void
sbi_reprobe(void)
{
  uint32 p = 0;
  for (int i = 0; i < 6; i++) {
    struct sbiret r = sbi_call(0x10, 3, sbi_ext_ids[i]);   // base: probe_extension
    if (r.err == 0 && r.val != 0)
      p |= 1u << i;
  }
  sbi_present = p;
}

uint32
machine_sbi_present(void)
{
  return sbi_present;
}

int
machine_sbi_names(char *buf, int n)
{
  int k = 0;
  for (int i = 0; i < 6; i++) {
    if (!(sbi_present & (1u << i)))
      continue;
    int l = strlen(sbi_ext_names[i]);
    if (k + l + 2 > n)
      break;
    if (k)
      buf[k++] = ' ';
    memmove(buf + k, sbi_ext_names[i], l);
    k += l;
  }
  buf[k] = 0;
  return k;
}

// Ask the host for a fact this machine was described without -- a vector unit
// a lazily-revealed machine has not been shown. The host grants it as a
// revision, which is taken here and now rather than on the next interrupt, so
// the caller's answer (hwinfo) is the description as it is after the question.
// A refusal is remembered so that no program's every hwinfo costs an ecall.
int
machine_request(uint32 what)
{
  static uint32 refused;
  if (!supported() || (what & refused) == what)
    return -1;
  struct sbiret r = sbi_call(SBI_EXT_MACHINE, MACH_REQUEST, what);
  if (r.err) {
    refused |= what;
    return -1;
  }
  push_off();                   // as the IPI path runs it: nothing delivered meanwhile
  machine_revise_check();
  pop_off();
  return 0;
}

// Does this hart owe the per-hart part of a revision another hart has
// already taken? A memory compare, for the paths that run too often to ask
// the host: the announcement is one SSIP bit shared with ordinary IPIs, and
// a hart whose announcement coincided with a TLB flush handled the flush and
// dropped the announcement, while the host had already stopped repeating it
// because the first hart had read the description (defect 21, found by the
// model: the hart kept the old controller and timer state after the ack).
int
machine_revise_owed(void)
{
  return supported() && revision_hart[cpuid()] != revision_global;
}

// ... and when it does owe one, say so: the announcement was lost to an IPI
// that arrived with it, or never came, and this hart is catching up from the
// tick or from an IPI's trap instead. Counted, and printed, so that a test
// can show the case occurring and being caught.
static int revise_late;
static uint32 revision_applying;       // the generation a hart is applying right now (defect 33)
static uint32 revision_yielded[NCPU];  // a hart that stood aside from the global part and takes its own part later
void
machine_revise_late(const char *how)
{
  int hart = cpuid();
  uint32 want = revision_global;
  if (revision_yielded[hart] == want) {
    // not lost: this hart stood aside while another applied the global part (defect 33)
    revision_yielded[hart] = 0;
    machine_revise_check();
    return;
  }
  machine_revise_check();
  revise_late++;
  printk("machine: hart %d took revision %d late, from the %s (%d so far)\n",
         hart, want, how, revise_late);
}

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
    tick_reanchor();          // F6: at the rate the description now has
    timer_rearm();            // F5: through whichever timer the description now has
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
    tick_reanchor();          // F6: at the rate the description now has
    timer_rearm();            // F5: through whichever timer the description now has
    revision_hart[hart] = gen;
    return;
  }
  if (revision_applying == gen) {
    // another hart is in the middle of the global part. This hart's own part is
    // owed (machine_revise_owed) and is taken at its next software interrupt or
    // tick, quietly -- not by spinning here on rev_lock while the other hart
    // applies and prints its line. With both vcpus on one host hart that spin
    // kept the lock holder preempted until the host's stretched tick: 88 ms to
    // acknowledge instead of 8 (defect 33).
    revision_yielded[hart] = gen;
    release(&rev_lock);
    return;
  }
  revision_applying = gen;
  release(&rev_lock);          // the apply and its line run without the lock

  uint32 changed = reread((uint64)d.val, &fresh);
  // the part of a description that is not in the tree: what the host says
  struct sbiret w = sbi_call(SBI_EXT_MACHINE, MACH_WHAT, 0);
  if (w.err == 0 && (w.val & REV_SBI))
    changed |= REV_SBI;
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
    printk("machine: interrupt controller %s at 0x%lx\n",
           plat.intc_kind == INTC_PLIC ? "plic" : plat.intc_kind == INTC_APLIC ? "aplic" : "aplic-msi+imsic",
           plat.intc_kind == INTC_PLIC ? plat.plic_base : plat.aplic_s_base);
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
    // F3: the lines. The dispatch (trap.c) and the drivers key on the numbers
    // in `plat', so the numbers are data; what has to be *redone* is the
    // controller's source configuration, which intc_init() rebuilds for the
    // numbers it finds -- unless a new controller was just brought up above,
    // which did that already against the fresh numbers.
    int renumbered = fresh.uart_irq != plat.uart_irq || fresh.rtc_irq != plat.rtc_irq;
    for (int i = 0; i < fresh.nvirtio && i < plat.nvirtio; i++)
      if (fresh.virtio_irq[i] != plat.virtio_irq[i])
        renumbered = 1;
    if (renumbered) {
      plat.uart_irq = fresh.uart_irq;
      plat.rtc_irq = fresh.rtc_irq;
      for (int i = 0; i < fresh.nvirtio && i < plat.nvirtio; i++)
        plat.virtio_irq[i] = fresh.virtio_irq[i];
      if (!(changed & REV_INTC)) {
        intc_init();
        intc_inithart();
      }
      printk("machine: interrupt lines renumbered: uart %d, virtio", plat.uart_irq);
      for (int i = 0; i < plat.nvirtio; i++)
        printk(" %d", plat.virtio_irq[i]);
      printk("\n");
    }
    // F9: the harts were renumbered. Logical cpu i is still cpu i (tp); what
    // changes is the hart id its per-hart controller registers are found
    // under, so every hart rebinds them (intc_inithart here and in the
    // per-hart path of every other hart as it takes the revision).
    int renamed = 0;
    for (int i = 0; i < fresh.ncpu && i < plat.ncpu && i < NCPU; i++)
      if (fresh.hartid[i] != plat.hartid[i])
        renamed = 1;
    if (renamed) {
      for (int i = 0; i < NCPU; i++)
        plat.hartid[i] = fresh.hartid[i];
      // the IMSIC's files are per hart *id*: the window mapped at boot covered
      // the ids the machine had then (two files for two harts); with ids 4 and
      // 5 the IPI to hart 5 is a store to a file 20 KB past it -- a guest page
      // fault and a panic (hart_probe). Every file this machine could ever be
      // given an id for is mapped, once, here.
      if (plat.intc_kind == INTC_IMSIC && plat.imsic_s_base)
        map_device(plat.imsic_s_base, 0x1000L * NCPU, 1);
      if (!(changed & REV_INTC) && !renumbered) {
        intc_init();
        intc_inithart();
      }
      printk("machine: harts renumbered:");
      for (int i = 0; i < plat.ncpu; i++)
        printk(" %d", plat.hartid[i]);
      printk("\n");
    }
    // F16: virtio devices that came (a virtio-console, F18), or went. The list
    // is taken as the tree gives it; new windows are mapped and their lines
    // enabled; a console device that went is let go of.
    if (fresh.nvirtio != plat.nvirtio) {
      for (int i = 0; i < fresh.nvirtio && i < PLAT_MAXVIRTIO; i++) {
        int had = 0;
        for (int j = 0; j < plat.nvirtio; j++)
          if (plat.virtio_base[j] == fresh.virtio_base[i])
            had = 1;
        if (!had)
          map_device(fresh.virtio_base[i], 0x1000, 1);
      }
      if (fresh.nvirtio < plat.nvirtio)
        virtio_con_detach();       // the only device that leaves a running machine here
      plat.nvirtio = fresh.nvirtio;
      for (int i = 0; i < PLAT_MAXVIRTIO; i++) {
        plat.virtio_base[i] = fresh.virtio_base[i];
        plat.virtio_irq[i] = fresh.virtio_irq[i];
      }
      if (!(changed & REV_INTC)) {
        intc_init();               // the new device's line, enabled
        intc_inithart();
      }
      printk("machine: %d virtio device(s) now\n", plat.nvirtio);
    }
    // F18: the console is another kind now
    if (fresh.console_kind != plat.console_kind)
      console_switch(fresh.console_kind);
    // F21: the cache block cbo.zero clears; the next page is zeroed at the new stride
    if (fresh.cboz_block != plat.cboz_block) {
      printk("machine: cache block %d -> %d bytes\n", plat.cboz_block, fresh.cboz_block);
      plat.cboz_block = fresh.cboz_block;
    }
    // F7/F8: harts came or went. Up: the tables grow now, the harts arrive
    // (started by the host after this acknowledgement) and boot themselves
    // in. Down: the harts beyond the count drain and park themselves.
    if (fresh.ncpu != plat.ncpu) {
      int old = plat.ncpu;
      for (int i = 0; i < NCPU; i++)
        plat.hartid[i] = fresh.hartid[i];
      plat.ncpu = fresh.ncpu;
      if (fresh.ncpu > old) {
        for (int i = old; i < fresh.ncpu && i < NCPU; i++)
          cpus[i].offline = 0;
        printk("machine: cpus %d -> %d\n", old, fresh.ncpu);
      } else {
        for (int i = fresh.ncpu; i < old && i < NCPU; i++) {
          cpus[i].offline = 1;
          sched_kick(i);
        }
        printk("machine: cpus %d -> %d (parking %d hart(s))\n", old, fresh.ncpu, old - fresh.ncpu);
      }
    }
    // F16/F17: the RTC came or went
    if (fresh.rtc_base != plat.rtc_base) {
      if (fresh.rtc_base)
        map_device(fresh.rtc_base, 0x1000, 1);
      plat.rtc_base = fresh.rtc_base;
      rtc_rebind();
    }
    // F4: the CLINT came or went. IPIs in flight are pending VSSIPs on their
    // targets either way; only the way the next one is sent changes.
    if (fresh.clint_base != plat.clint_base) {
      extern uint64 clint_msip_base;
      if (fresh.clint_base)
        map_device(fresh.clint_base, 0x10000, 1);
      plat.clint_base = fresh.clint_base;
      clint_msip_base = fresh.clint_base;
      printk("machine: ipis via %s\n", plat.clint_base ? "clint" : "sbi");
    }
    adopted |= REV_DEVICES;
  }

  if (changed & REV_MEMORY) {
    // F13: the machine has more memory. The new range gets a page database of
    // its own and joins the free lists; nothing that exists moves.
    if (fresh.ram_end > plat.ram_end) {
      uint64 b = plat.ram_end, e = fresh.ram_end;
      if (kalloc_grow(b, e) == 0) {
        plat.ram_end = e;
        plat.ram_size = fresh.ram_size;
        printk("machine: memory %ld -> %ld MB (%ld MB added at 0x%lx)\n",
               (long)((b - plat.ram_base) >> 20), (long)(plat.ram_size >> 20), (long)((e - b) >> 20), b);
        adopted |= REV_MEMORY;
      } else {
        printk("machine: memory %ld -> %ld MB offered; could not take it\n",
               (long)(plat.ram_size >> 20), (long)(fresh.ram_size >> 20));
      }
    }
  }

  if (changed & REV_TIMER) {
    // F5: the timer moved between stimecmp and the SBI timer. What is armed
    // stays armed as a value; it is re-armed through the other mechanism here
    // and on every other hart as it takes the revision (below).
    plat.has_sstc = fresh.has_sstc;
    if (fresh.timebase != plat.timebase) {
      // F6: the counter runs at another rate now. The tick interval is
      // derived from it (tickinit); the wall clock and the scheduler read
      // the rate each time they convert, and the count itself is continuous
      // (the host scales the whole count, not a delta).
      // the host serves the new rate from the call; the clock is re-anchored at once after it,
      // with this hart's interrupts off between, so nothing here reads it in between
      int was = intr_get();
      intr_off();
      sbi_call(SBI_EXT_MACHINE, MACH_RATE_NOW, 0);
      clock_reanchor();          // the ns so far stay; the rate from here is new
      plat.timebase = fresh.timebase;
      if (was)
        intr_on();
      tickinit();
      tick_reanchor();           // and the next tick is one new interval away
      printk("machine: timebase %lu Hz\n", (unsigned long)plat.timebase);
    }
    timer_rearm();
    printk("machine: timer via %s\n", plat.has_sstc ? "stimecmp" : "sbi");
    adopted |= REV_TIMER;
  }

  if (changed & REV_SBI) {
    char names[64];
    sbi_reprobe();
    machine_sbi_names(names, sizeof(names));
    printk("machine: sbi extensions: %s\n", names);
    adopted |= REV_SBI;
  }

  if (changed & REV_VLEN) {
    if (fresh.has_v) {
      plat.has_v = 1;
      plat.misa = fresh.misa;
      // vlenb is readable only with the unit enabled; the host no longer
      // turns VS on for a guest (defect 15: Linux read that as live state), so
      // this kernel does, for the read (vreg2: kerneltrap on csrr vlenb)
      uint64 ss = r_sstatus();
      w_sstatus((ss & ~SSTATUS_VS_MASK) | SSTATUS_VS_INITIAL);
      plat.vlenb = r_vlenb();
      w_sstatus(ss);
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

  acquire(&rev_lock);
  revision_global = gen;
  revision_hart[hart] = gen;
  release(&rev_lock);

  // What could not be taken is reported rather than hidden: the host learns
  // exactly which parts of a revision this guest managed, so a refusal is
  // something it can plan around instead of a silent failure.
  sbi_call2(SBI_EXT_MACHINE, MACH_ACK, adopted, gen);   // named: a stale ack is refused (defect 20)
}
