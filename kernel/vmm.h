#ifndef XV6_VMM_H
#define XV6_VMM_H
//
// hypervisor interface shared by the kernel and user programs (vmm).
//
//   vm_create(struct vmconfig *)      -> id, or -1
//   vm_run(id, vcpu)                  run a vcpu in this process until the VM stops
//   vm_destroy(id)                    halt and free
//   vm_ctl(id, op, arg)               VMCTL_PAUSE / VMCTL_RESUME / ...
//   vm_cons(id, op, buf, n)           VMCONS_READ / VMCONS_WRITE / VMCONS_DETACH
//   vm_info(id, struct vminfo *)      0 if the slot exists (state VM_FREE if empty)
//   vm_open(id, name, flags)          -> a descriptor for a machine
//
// A machine belongs to the user who created it: everything above
// refuses (-EPERM) for anybody else without CAP_SYS_ADMIN. A
// descriptor is the other way in -- whoever holds one may work the
// machine it names -- and it is what fork and SCM_RIGHTS pass around.
// An unnamed machine is destroyed when its last descriptor is closed;
// a named one outlives them, because its name is how it is found.
//   consraw(on)                       host console raw mode
//

#define VM_NAMELEN 16
#define VM_PATHLEN 64

// vmconfig.flags
#define VMF_SOFTIMSIC 1 // IMSIC mode: use the software interrupt file model
#define VMF_RAW       2 // kernel is a raw binary loaded at load_addr (also the entry)
#define VMF_HSM       4 // secondary vcpus start stopped, to be started via SBI HSM (Linux)
#define VMF_NOHUGE    8 // back guest RAM 4 KB at a time, never with megapages
// give the machine a virtio-console (0x10004000, irq 4) as well as the
// 16550. Not the default, and deliberately: a guest that has a driver for
// it pays one exit for a line of output instead of two for a character,
// but a guest that has a *different* driver for it may prefer it to the
// 16550 and be worse off. Linux does exactly that -- it binds hvc0 to this
// device and its boot goes from six seconds to past the test's patience --
// so a machine is given this device when its configuration asks for it.
#define VMF_CONQUEUE 16
// present a plain PLIC to this machine even when the host has the AIA. Needed
// to set up the *source* shape for a controller revision: a guest
// that starts as a PLIC guest and is given an IMSIC while it runs.
#define VMF_WANT_PLIC 32
// and the other two kinds, as software models wherever the host is. These are
// how a machine arriving from a host of another shape keeps the controller it
// left with when its guest cannot be told about a new one (vmstate.c).
#define VMF_WANT_APLIC 64
#define VMF_WANT_IMSIC 128
#define VMF_WANT_MASK (VMF_WANT_PLIC | VMF_WANT_APLIC | VMF_WANT_IMSIC)
#define VMF_MMIO_SET  256  // cfg.mmio_shift is a request, not the host's default
#define VMF_LAZYV     512  // present no vector unit until the guest tries to use one
#define VMF_INTC_SET 1024  // cfg.intc_shift is a request, not the host's default
#define VMF_VICSHIM  (1 << 23)  // describe the interrupt controller as xv6h,vic (Linux patch 0005: F1/F2 taken live)
#define VM_VICSHIM(vm) (((vm)->cfg.flags & VMF_VICSHIM) != 0)   // read from the cfg, which crosses with the machine (an arrival is built from it)
#define VMF_IRQ_SET  2048  // cfg.irq_alt is a request, not the host's default
#define VMF_SBI_SET  4096  // cfg.sbi_hidden is a request, not the host's default
#define VMF_IPI_SET  8192  // cfg.no_clint is a request, not the host's default
#define VMF_TIMER_SET 16384 // cfg.no_sstc is a request, not the host's default
#define VMF_TIMEBASE_SET 32768 // cfg.timebase is a request, not the host's default
#define VMF_HART_SET 65536  // cfg.hart_shift is a request, not the host's default
#define VMF_MAC_SET 131072  // cfg.mac_alt is a request, not the host's default
#define VMF_RTC_SET 262144  // cfg.no_rtc is a request, not the host's default
#define VMF_CON_SET 524288  // cfg.console_kind is a request, not the host's default
#define VMF_CPUS_SET 1048576 // cfg.online is a request, not the host's default
#define VMF_CBOZ_SET 2097152 // cfg.cboz_block is a request, not the host's own
#define VMF_FOREIGN 4194304  // a kernel of its own (-k): a guest whose console and hart bring-up are not xv6's
// the rules a foreign guest (Linux) needs: its console cannot move, its harts are its own to start
#define VM_FOREIGN(vm) ((vm)->cfg.flags & (VMF_FOREIGN | VMF_HSM))

// A machine's switch port is closed: what arrives from outside is
// delivered only when it belongs to a conversation the machine itself
// started, or when it is for a port the machine has published. A
// published port is named by the port outside and the port inside,
// which are on the same address and so are usually the same
// number; a gateway is what makes them differ.
#define NPPORT 8

#define NCTVOL 4          // volumes a container may be given
#define NCTENV 8          // and variables

struct pport {
  uint16 hport, gport;
  uint8 proto;             // IPPROTO_TCP (6) or IPPROTO_UDP (17)
};

struct vmconfig {
  char name[VM_NAMELEN];   // optional
  char kernel[VM_PATHLEN]; // ELF (any load address) or raw image, in the host file system
  char disk[VM_PATHLEN];   // disk image file in the host file system, "" for no disk
  int mem_mb;
  int mem_limit_mb;        // host memory this VM may hold at once (0: no limit)
  int nvcpu;
  int flags;
  uint64 mmio_shift;       // virtio block offset from the default map (VMF_MMIO_SET)
  uint64 intc_shift;       // interrupt controller offset from the default map (VMF_INTC_SET)
  int irq_alt;             // interrupt lines numbered the alternative way (VMF_IRQ_SET)
  uint32 sbi_hidden;       // SBI extensions withheld, SBIH_* (VMF_SBI_SET)
  int no_clint;            // no CLINT: IPIs through SBI (VMF_IPI_SET)
  int no_sstc;             // no Sstc: the SBI timer (VMF_TIMER_SET)
  uint32 timebase;         // timebase-frequency presented, Hz (VMF_TIMEBASE_SET)
  int hart_shift;          // hart ids presented as id+shift (VMF_HART_SET)
  int mac_alt;             // the NIC's alternative identity (VMF_MAC_SET)
  int no_rtc;              // no RTC device (VMF_RTC_SET)
  int console_kind;        // 0 uart, 1 virtio-console, 2 SBI debug console (VMF_CON_SET)
  int online;              // harts started, of nvcpu described (VMF_CPUS_SET)
  int cboz_block;          // cache-block size described (VMF_CBOZ_SET)
  uint64 load_addr;        // VMF_RAW: guest-physical load address
  char bootargs[128];      // kernel command line (device tree /chosen/bootargs)
  int net;                 // 0: no address configuration, 1: dhcp, 2: static
  uint32 ip, mask, gw, dns; // static configuration (host byte order)
  // what share of the host's processors this machine gets. `weight'
  // is on the same scale as a task's: 1024 is one nice-0 task's
  // worth, whatever the vcpu count. `cpu_max_us' per `cpu_period_us'
  // is a ceiling on top of that (0: none).
  int weight;
  int cpu_max_us, cpu_period_us;
  struct pport ports[NPPORT];
  int nports;
  char gwname[16];         // the gateway this machine is behind, by name
};

enum vmstate { VM_FREE = 0, VM_NEW, VM_RUNNING, VM_HALTING, VM_PAUSED };

// one line of /proc/vmm: the registry of machines on this host. Unlike
// vm_info, which answers for the machines the caller may reach, this
// is the whole table, and only a privileged reader gets it.
struct vmentry {
  int id, state, kind, nvcpu, fdref;
  unsigned int owner;      // the uid that created it
  unsigned long mem_mb, resident_mb;
  char name[VM_NAMELEN];
};

#define VMCTL_PAUSE  0
#define VMCTL_RESUME 1
// same-page merging is the host's, not a VM's: the id is ignored
#define VMCTL_KSM_OFF 2
#define VMCTL_KSM_ON  3
// vm_ctl(id, VMCTL_BALLOON, megabytes): ask the guest to give that
// much of its memory back. The guest answers in its own time, and
// vm_info reports how far it has got.
#define VMCTL_BALLOON 4
// vm_ctl(id, VMCTL_WEIGHT, w): the machine's share of the host's
// processors, on the same scale as a task's weight (1024 is what a
// task of nice 0 gets), whatever its vcpu count.
#define VMCTL_WEIGHT 5
// vm_ctl(id, VMCTL_CPUMAX, pct): a ceiling, in hundredths of one
// processor, over a period of a tenth of a second. 0 removes it.
#define VMCTL_CPUMAX 6
// vm_ctl(id, VMCTL_PORT_ADD | VMCTL_PORT_RM, VMCTL_PORT(proto, hport,
// gport)): publish a port of this machine, or stop publishing it. Only
// the host port and the protocol name a port to remove.
#define VMCTL_PORT_ADD 7
#define VMCTL_PORT_RM  8
#define VMCTL_PORT(proto, hport, gport) \
  (((unsigned long)(proto) << 32) | ((unsigned)(hport) << 16) | (unsigned)(gport))

// vm_ctl(id, VMCTL_REVISE, what): tell the guest its machine description has
// changed and it should look again. `what' is the REV_* mask of
// what the host intends to change; a guest that does not speak the revision
// protocol ignores the notification and keeps the description it booted with.
#define VMCTL_REVISE 9
// vm_ctl(id, VMCTL_REVISE_INTC, kind): give a running guest a different kind of
// interrupt controller (0 plic, 1 aplic, 2 imsic). -EAGAIN while the guest is
// inside an interrupt handler.
#define VMCTL_REVISE_INTC 10
// vm_ctl(id, VMCTL_REVISE_VEC, on): take the vector unit away from a running
// guest (0) or give it back (1). The guest may refuse to lose it while a task
// still holds vector state.
#define VMCTL_REVISE_VEC 11
// vm_ctl(id, VMCTL_REVISE_INTCMAP, shift): move a running guest's interrupt
// controller registers to another address (F2) and tell it
#define VMCTL_REVISE_INTCMAP 12
// vm_ctl(id, VMCTL_REVISE_IRQMAP, alt): renumber a running guest's interrupt lines (F3)
#define VMCTL_REVISE_IRQMAP 13
// vm_ctl(id, VMCTL_IMSIC_HW, on): deliver a running IMSIC guest's interrupts
// through a hardware guest interrupt file (1) or the software model (0). The
// guest cannot tell (F23); what changes is the exits per interrupt.
#define VMCTL_IMSIC_HW 14
// vm_ctl(id, VMCTL_SPLIT, 0): back every 2 MB block of the guest's memory with
// 4 KB pages instead (F24: memory backing, invisible to the guest)
#define VMCTL_SPLIT 15
// vm_ctl(id, VMCTL_REVISE_SBI, hidden): change the SBI extensions a running guest is offered (F22)
#define VMCTL_REVISE_SBI 16
#define VMCTL_REVISE_IPI 17     // arg: 1 = SBI IPIs (no CLINT), 0 = CLINT (F4)
#define VMCTL_REVISE_TIMER 18   // arg: 1 = SBI timer (no Sstc), 0 = Sstc (F5)
#define VMCTL_REVISE_TIMEBASE 19 // arg: Hz (F6)
#define VMCTL_REVISE_HARTID 20   // arg: shift (F9)
#define VMCTL_REVISE_MAC 21      // arg: 1 = alternative identity, 0 = default (F19)
#define VMCTL_REVISE_RTC 22      // arg: 1 = an RTC, 0 = none (F16/F17)
#define VMCTL_REVISE_CONSOLE 23  // arg: 0 uart, 1 virtio, 2 sbi (F18)
#define VMCTL_GROW 24            // arg: MB, the machine's new size (F13)
#define VMCTL_REVISE_CPUS 25     // arg: harts online (F7 up, F8 down)
#define VMCTL_REVISE_CBOZ 26     // arg: cache-block size in bytes (F21)
#define VMCTL_REVISE_MAP 27      // arg: the device windows' shift, 0 or 0x100000 (F15, live)
#define VMCTL_PEEK 28            // arg: a guest-physical address; returns the 32-bit word there (diagnosis)

#define VMCONS_READ   0
#define VMCONS_WRITE  1
#define VMCONS_DETACH 2

struct vminfo {
  int id, state, nvcpu, mem_mb, intc, imsic_hw;
  int revise_capable, revise_gen;  // speaks the revision protocol; generation
  uint32 revise_repeats;           // announcements repeated (a lost one, counted)
  uint32 revise_stale;             // acks refused as stale (defect 20)
  uint32 irq_quiet_held;           // F3: device interrupts held in the quiet window and raised on the new numbers at the ack
  uint32 aplic_ie, aplic_idel0, aplic_idel1, aplic_tgt10; uint64 aplic_en, aplic_pend, aplic_lvl;   // the APLIC model, for a guest that is not answering
  uint64 corun_busy, corun_both;   // ticks with two vcpus busy; of those, both on harts
  uint64 lookup_direct, lookup_direct_cyc, lookup_alias, lookup_alias_cyc; // device lookups and their cost
  int resident_mb;        // guest RAM actually backed by host memory
  int split_chunks;       // 2 MB chunks backed a page at a time
  int table_pages;        // G-stage page table pages below the root
  int balloon_target_mb;  // how much the host has asked the guest to give back
  int balloon_actual_mb;  // how much the guest has given back so far
  int mem_limit_mb;       // the limit, if it has one
  int weight;             // its share of the processors
  int cpu_max_us, cpu_period_us;  // and its ceiling, if it has one
  uint64 throttles;       // how often the ceiling has stopped it
  int frozen;
  struct vmconfig cfg;
  uint64 entry;
  uint64 disk_sectors;
  uint64 exits, mmio, csr, wfi, injected;
  uint64 exit_throttles, nudges;  // the exit-rate throttle, and the sibling nudge
  uint64 cboz_soft;               // F21: cbo.zero done in software, all machines
  uint64 host_time, htimedelta, vstimecmp1;   // the clocks: guest time is host_time + htimedelta
  // which device the emulated accesses went to, which is what says what is
  // worth building next
  int ndev;
  struct { char name[12]; uint64 loads, stores, irqs; uint64 base; } dev[12];
  // what the guest has observed about its machine, and when (host ticks since
  // the machine was created). See OBS_* in hyp.h.
  uint32 obs;
  uint64 obs_first[14];
  uint64 obs_now;        // ticks since creation, so `when' has a scale
  // its switch port: what it publishes, and what was turned away
  struct pport ports[NPPORT];
  int nports;
  uint64 net_filtered;
  // vcpu 0, for debugging
  uint64 pc, last_cause, last_gpa;
  int running_hart, idle, mmode;
  uint64 vsatp, vstvec, vsepc, vscause, vsstatus, vstval, sp, ra, vsie, vstimecmp;
  uint64 scounteren, senvcfg, hcounteren;
  // vcpu 1 (if any): pc, last cause, idle, hsm state, exits, injected VSEIP state
  uint64 pc1, cause1, exits1;
  // what vcpu 0's exits were: one count per kind (XK_* in kernel/hyp.h,
  // and the names in user/vmm.c). A histogram says which exit is worth
  // making cheaper; a total says only how busy the guest is.
  uint64 exit_hist[9];
  int idle1, hsm1, hart1;
  uint64 eie0_0, eip0_0, eie0_1, eip0_1;
  int deliv0, deliv1, thr0, thr1;
  uint64 hvip0;                   // the hvip bits vcpu0's last entry injected
  uint64 a0_0, a0_1, ra1;         // a0 of each vcpu and vcpu1's ra (a lock's address while spinning)
  uint32 csr_id[16], csr_n[16];   // CSR exits by CSR, the first sixteen seen
  int topid0;                     // the software IMSIC's top pending id for vcpu0
};

#endif
