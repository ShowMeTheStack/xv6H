//
// xv6 hypervisor: definitions shared by hyp.c, hypvec.S, hypcsr.c,
// hypmmio.c and the virtual device models.
//

#include "vmm.h"

// ---- hypervisor-extension CSR numbers (numeric, so that the assembler
// ---- does not need to know about them) ----
#define CSR_HSTATUS    0x600
#define CSR_HEDELEG    0x602
#define CSR_HIDELEG    0x603
#define CSR_HIE        0x604
#define CSR_HTIMEDELTA 0x605
#define CSR_HCOUNTEREN 0x606
#define CSR_HGEIE      0x607
#define CSR_HENVCFG    0x60a
#define CSR_HTVAL      0x643
#define CSR_HIP        0x644
#define CSR_HVIP       0x645
#define CSR_HTINST     0x64a
#define CSR_HGATP      0x680
#define CSR_HGEIP      0xe12
#define CSR_HVICTL     0x609
#define CSR_VSSTATUS   0x200
#define CSR_VSIE       0x204
#define CSR_VSTVEC     0x205
#define CSR_VSSCRATCH  0x240
#define CSR_VSEPC      0x241
#define CSR_VSCAUSE    0x242
#define CSR_VSTVAL     0x243
#define CSR_VSIP       0x244
#define CSR_VSTIMECMP  0x24d
#define CSR_VSATP      0x280

#define csr_read(csr)                                     \
  ({                                                      \
    uint64 __v;                                           \
    asm volatile("csrr %0, %1" : "=r"(__v) : "i"(csr));   \
    __v;                                                  \
  })
#define csr_write(csr, val)                               \
  ({                                                      \
    uint64 __v = (uint64)(val);                           \
    asm volatile("csrw %0, %1" : : "i"(csr), "r"(__v));   \
  })
#define csr_set(csr, val)                                 \
  ({                                                      \
    uint64 __v = (uint64)(val);                           \
    asm volatile("csrs %0, %1" : : "i"(csr), "r"(__v));   \
  })
#define csr_clear(csr, val)                               \
  ({                                                      \
    uint64 __v = (uint64)(val);                           \
    asm volatile("csrc %0, %1" : : "i"(csr), "r"(__v));   \
  })

// hstatus bits
#define HSTATUS_VSBE  (1L << 5)
#define HSTATUS_GVA   (1L << 6)
#define HSTATUS_SPV   (1L << 7)
#define HSTATUS_SPVP  (1L << 8)
#define HSTATUS_HU    (1L << 9)
#define HSTATUS_VGEIN_SHIFT 12
#define HSTATUS_VGEIN_MASK  (0x3fL << 12)
#define HSTATUS_VTVM  (1L << 20)
#define HSTATUS_VTW   (1L << 21)
#define HSTATUS_VTSR  (1L << 22)

// hvip / hip / vsip bits
#define HVIP_VSSIP (1L << 2)
#define HVIP_VSTIP (1L << 6)
#define HVIP_VSEIP (1L << 10)
#define HIP_SGEIP  (1L << 12)

// hgatp
#define HGATP_MODE_SV39X4 (8L << 60)
#define HGATP_VMID_SHIFT  44

// henvcfg
#define HENVCFG_STCE (1L << 63)
#define HENVCFG_ADUE (1L << 61)
#define HENVCFG_CBZE (1L << 7)
#define HENVCFG_CBCFE (1L << 6)
#define HENVCFG_CBIE (3L << 4)

// exception causes seen from guests
#define EXC_ILLEGAL       2
#define EXC_ECALL_VS      10
#define EXC_INST_GPF      20
#define EXC_LOAD_GPF      21
#define EXC_VIRT_INST     22
#define EXC_STORE_GPF     23
#define EXC_INST_ACCESS   1
#define EXC_LOAD_ACCESS   5
#define EXC_STORE_ACCESS  7

// guest-physical layout presented to every VM (same as qemu virt).
#define G_RAM_BASE   0x80000000L
#define G_UART_BASE  0x10000000L
#define G_UART_IRQ   10
#define G_VIRTIO_BASE 0x10001000L
#define G_VIRTIO_IRQ 1
#define G_VNET_BASE  0x10002000L
#define G_VNET_IRQ   2
#define G_BALLOON_BASE 0x10003000L
#define G_BALLOON_IRQ  3
#define G_CON_BASE   0x10004000L
#define G_MMIO_ALT_SHIFT 0x100000L   // the alternative map: virtio block at 0x1010xxxx
// F2: where the interrupt controller sits is a fact of the machine too. The
// alternative puts PLIC/APLIC at 0x1c00_0000/0x1d00_0000 and the IMSICs at
// 0x3400_0000/0x3800_0000 (`guestintc=alt' on the host command line).
#define G_INTC_ALT_SHIFT 0x10000000L
#define G_CON_IRQ    4
#define G_MACHREV_BASE 0x10005000L   // the anchor: where a revision is announced (vmachrev.c); never moves
#define G_MACHREV_IRQ  5
#define G_RTC_BASE   0x101000L
#define G_RTC_IRQ    11
#define G_CLINT_BASE 0x02000000L
#define G_PLIC_BASE  0x0c000000L
#define G_PLIC_SIZE  0x600000L
#define G_APLIC_M    0x0c000000L
#define G_APLIC_S    0x0d000000L
#define G_IMSIC_M    0x24000000L
#define G_IMSIC_S    0x28000000L

#define HGATP_MODE_SV48X4 (9L << 60)
#define HCHUNK       (2L * 1024 * 1024) // guest RAM granule (G-stage megapage)
#define HCHUNK_ORDER 9                   // as a buddy order
#define HCHUNK_PAGES 512                 // 4 KB pages in a chunk

// a chunk table entry: a 2 MB block, or (with the low bit set) a
// pointer to a page of 512 frames.  A page is 4 KB aligned, so the
// low bit is free to say which.
#define GC_SPLIT 1
#define gc_split(c) ((c) & GC_SPLIT)
#define gc_pages(c) ((uint64 *)((c) & ~(uint64)GC_SPLIT))
#define VM_MAXMEM    (256UL << 30)       // 256 GiB per VM at most
#define VM_DTB_MAX   0x10000

enum vic_kind { VIC_PLIC = 0, VIC_APLIC = 1, VIC_IMSIC = 2 };

#define HSM_STARTED 0
#define HSM_STOPPED 1
#define FENCE_I    1
#define FENCE_VMA  2
#define FENCE_GVMA 4  // the G-stage mapping changed under a running vcpu

// ---- virtual devices ----

struct vm;
struct vcpu;

struct vdev {
  uint64 base, size;
  int (*read)(struct vm *, struct vdev *, uint64 off, int size, uint64 *val);
  int (*write)(struct vm *, struct vdev *, uint64 off, int size, uint64 val);
  void *priv;
  const char *name;      // for `vmm info': which device the exits went to
  int irq;               // the line it raises (-1: none, it is a controller)
  int irq0;              // the same in the default numbering (F3: vm_irq(vm, irq0) == irq)
  uint32 obs;            // the observation a first access to it implies
  uint64 loads, stores;  // and how many of each
};
#define VM_MAXDEV 16

#define CRING 8192
struct cring {
  char buf[CRING];
  uint r, w; // read/write indices (mod CRING)
};

struct vuart {
  uint8 ier, lcr, mcr, scr, fcr;
  uint8 dll, dlm;
  int thre_intr;   // a transmit-holding-empty interrupt is pending
};

struct vvirtio {
  uint32 device_features_sel, driver_features_sel;
  uint64 driver_features;
  uint32 queue_sel, queue_num, queue_ready;
  uint64 desc, avail, used; // guest physical addresses of queue 0
  uint16 last_avail;
  uint32 intr_status, status;
  uint64 capacity;          // sectors (0: no disk)
  struct sleeplock qlock;   // serializes queue processing
  char *bounce;             // one page for data transfers
  // the queue is worked by a thread of its own, so that a guest which
  // has asked for a block goes straight back to running while the
  // host reads it
  int wake;                 // the guest rang the doorbell
  int stop;                 // the VM is going: leave
  int running;              // the thread exists and has not left yet
  volatile int busy;        // a request is being carried out right now
  // what the guest has got wrong, counted rather than printed (see
  // vm_gripe): a guest can post a broken request a million times a
  // second, and a host that printed each one would drown its console.
  uint64 malformed;         // requests that made no sense
  uint64 desync;            // available indexes the ring cannot hold
};

struct vq_state {
  uint32 num, ready;
  uint64 desc, avail, used;
  uint16 last_avail;
};

struct vvnet {
  int present;
  uint8 mac[6];
  uint32 device_features_sel, driver_features_sel;
  uint32 queue_sel;
  struct vq_state q[2];     // 0: receive, 1: transmit
  uint32 intr_status, status;
  uint64 rx_frames, tx_frames, rx_dropped, tx_dropped;
};

// ---- what a guest has observed about its machine ----
//
// A hypervisor tells a guest what machine it is on once and can never take it
// back, which is why fleets mask CPU features down to a common floor and pin
// machines to instance families for life. The first half of doing better is
// knowing what this particular guest has actually looked at: a guest that has
// never asked whether it has a vector unit can be moved to a machine without
// one, whatever the fleet-wide floor says.
//
// Every bit below is set from a path that already traps to the host, so
// observing costs nothing that was not already being paid. The device tree is
// the exception -- it is read with ordinary loads -- and is handled separately.
#define OBS_MISA        (1u << 0)   // asked which extensions exist
#define OBS_MENVCFG     (1u << 1)   // asked which of them are enabled
#define OBS_MHARTID     (1u << 2)   // asked which hart it is
#define OBS_MVENDOR     (1u << 3)   // asked whose silicon this is
#define OBS_VECTOR_USED (1u << 4)   // executed a vector instruction
#define OBS_INTC        (1u << 5)   // touched its interrupt controller
#define OBS_TIMER       (1u << 6)   // read the time or armed a timer
#define OBS_RTC         (1u << 7)   // read the wall clock device
#define OBS_VIRTIO_BLK  (1u << 8)   // probed the disk
#define OBS_VIRTIO_NET  (1u << 9)   // probed the NIC
#define OBS_VIRTIO_CON  (1u << 10)  // probed the console queue
#define OBS_BALLOON     (1u << 11)
#define OBS_UART        (1u << 12)
#define OBS_DTB         (1u << 13)  // read its own device tree
#define OBS_COUNT 14

// virtio-console: the guest's console as a virtqueue rather than as a
// register it writes one byte at a time.
//
// A booting xv6 guest makes 2840 emulated device accesses and 2319 of them
// -- eighty-two per cent -- are the 16550: its driver reads the line status
// register to see whether the transmit register is free and then writes one
// character, so every character of output is two exits, and an exit is about
// sixteen microseconds here. The same output through a queue is one
// notification for as many characters as the guest cared to put in the
// descriptor.
//
// Queue 0 carries what the host types to the guest, queue 1 what the guest
// prints; both end up in the same two rings the 16550 uses (vm->cons_out and
// vm->cons_in), so `vmm attach' cannot tell which device a machine is using
// and a guest that never drives this one loses nothing.
struct vvcon {
  int present;
  uint32 device_features_sel, driver_features_sel;
  uint32 queue_sel;
  struct vq_state q[2];     // 0: to the guest (receive), 1: from it (transmit)
  uint32 intr_status, status;
  uint64 rx_bytes, tx_bytes;   // what crossed, and in how few exits
  uint64 notifies;
};

// virtio-balloon: the host says how much of the guest's memory it
// would like back, the guest hands over pages, and the host takes them
// away. Nothing is forced: a balloon is a request, and a guest that
// ignores it keeps its memory.
struct vballoon {
  int present;
  uint64 desync;            // as vvirtio.desync
  uint32 device_features_sel, driver_features_sel;
  uint64 driver_features;
  uint32 queue_sel;
  struct vq_state q[3];     // 0: inflate, 1: deflate, 2: statistics
  uint32 intr_status, status;
  uint32 target;            // pages the host is asking for (config num_pages)
  uint32 actual;            // pages the guest has handed over
  uint32 config_gen;
  uint64 inflated, deflated; // pages taken and given back, in total
  uint64 stats[8];          // what the guest last reported, if it does
  int stats_go;             // the statistics buffer may be taken now (the poller, once a second)
  int have_stats;
};

#define VIRTIO_BALLOON_F_MUST_TELL_HOST 0
#define VIRTIO_BALLOON_F_STATS_VQ       1
#define VIRTIO_BALLOON_F_DEFLATE_ON_OOM 2

#define VPLIC_NIRQ 32
struct vplic {
  uint32 prio[VPLIC_NIRQ];
  uint32 level;            // current input levels
  uint32 pending;
  uint32 inservice;
  uint32 enable[2 * NVCPU];
  uint32 threshold[2 * NVCPU];
};

#define VAPLIC_NSRC 64
struct vaplic {
  // both domains (M at 0x0c000000, S at 0x0d000000) share the sources;
  // only the S domain delivers.
  uint32 domaincfg[2];
  uint32 sourcecfg[VAPLIC_NSRC]; // S-domain view; M-domain writes delegate
  uint32 msourcecfg[VAPLIC_NSRC];
  uint32 target[VAPLIC_NSRC];
  uint32 mmsiaddrcfg, mmsiaddrcfgh, smsiaddrcfg, smsiaddrcfgh;
  uint64 level;                  // rectified input levels
  uint64 pending;
  uint64 enable;
  uint32 idelivery[NVCPU], iforce[NVCPU], ithreshold[NVCPU];
};

#define VIMSIC_NIDS 256
struct vimsic {
  // software model of one guest interrupt file (used when VGEIN=0).
  uint32 eidelivery, eithreshold;
  uint64 eip[VIMSIC_NIDS / 64];
  uint64 eie[VIMSIC_NIDS / 64];
  uint64 iselect;
  int hwfile;      // >0: hardware guest interrupt file index on hwhart
  int hwhart;
};

// ---- vcpu ----

// register save area layout is shared with hypvec.S: regs[] first,
// then the host context, then host tp.
// how many exits to the host a vcpu may take in a tenth of a second
// before it is made to give the processor up. An xv6 guest booting
// takes a few thousand exits in all; a guest reading an emulated
// register in a loop takes this many in every window.
#define EXIT_RATE_LIMIT 20000

// the kinds of exit a histogram counts. They are the causes worth
// telling apart rather than every cause the hardware has: what is being
// asked is "which of these is this guest paying for?".
#define XK_MMIO_LOAD  0
#define XK_MMIO_STORE 1
#define XK_INST_FAULT 2
#define XK_CSR        3
#define XK_ECALL      4
#define XK_ILLEGAL    5
#define XK_TIMER      6
#define XK_EXTERNAL   7
#define XK_OTHER      8
#define XK_KINDS      9

struct vmsnap {
  volatile int active;
  struct inode *ip;
  uint off;                // where the next page record goes in the file
  uint64 written;          // bytes of the MEM section written so far
  uint32 *done;            // one bit per guest page (0 while pre-copying)
  // pages written since this round began: by the guest, which faults, or
  // by the host on the guest's behalf -- a disk read or a received frame
  // lands in guest memory without any fault at all, which is why
  // vm_copyout marks them here.
  uint32 *dirty;
  volatile int precopy;    // a migration's rounds: mark, do not write out
  int npages;              // how many bits that is
  struct sleeplock lock;
  uint64 faulted;          // pages the guest's own writes wrote out
};

struct vcpu {
  uint64 regs[32];        // 0..255   guest x0..x31 (x0 unused)
  struct context hostctx; // 256..367 host ra, sp, s0..s11 (see proc.h)
  uint64 host_tp;         // 368

  struct vm *vm;
  int id;
  struct proc *p;         // host process running this vcpu (0 if none)
  volatile int running_hart; // hart currently executing the guest, or -1
  volatile int idle;      // sleeping in wfi
  int priv;               // 1 = VS, 0 = VU (for entry)
  uint64 pc;

  // VS CSRs, saved on exit and restored on entry.
  uint64 vsstatus, vsie, vstvec, vsscratch, vsepc, vscause, vstval, vsatp, vstimecmp;
  uint64 scounteren, senvcfg; // shared supervisor CSRs (no VS copies)
  uint64 vsiselect;           // AIA indirect-register selector (per hart)
  // floating-point state: f0..f31, fcsr.
  uint64 fregs[32];
  uint64 fcsr;
  // vector state: a page holding v0..v31, plus vstart/vtype/vl/vcsr.
  char *vstate;
  uint64 vcsrs[4];
  int vssip;              // virtual supervisor software interrupt pending
  volatile int ssip_req;  // vclint asked for VSSIP since the last entry
  int vgein;              // hardware guest interrupt file (0 = none)
  struct vimsic imsic;

  // SBI HSM state and remote fences
  int hsm_state;          // HSM_STARTED / HSM_STOPPED
  volatile int fence_req; // FENCE_I / FENCE_VMA / FENCE_GVMA on the next entry
  int last_hart;          // hart of the previous entry (-1: none)

  // emulated machine-mode state (the guest's own start() runs in VS mode).
  int in_mmode;
  uint64 m_mstatus, m_mepc, m_medeleg, m_mideleg, m_mie, m_mtvec, m_mscratch;
  uint64 m_menvcfg, m_mcounteren, m_pmpaddr0, m_pmpcfg0;

  // statistics
  uint64 exits, mmio, csr_emul, wfi_sleeps, injected;
  uint64 exit_hist[XK_KINDS];   // and what they were (XK_*)
  // the exit-rate throttle: how many exits this vcpu has taken since
  // the window opened, when it opened, and how often it has been made
  // to give the processor up for taking too many.
  uint64 exit_window_start, exit_window_count, exit_throttles;
  uint64 nudges;          // slices handed to a sibling vcpu
  uint64 last_cause, last_gpa;
};

// ---- vm ----

struct gendisk;

struct sched_group;

struct cgw;

struct vm {
  struct spinlock lock;
  // guest memory -- the chunk table and the G-stage page tables -- has
  // a lock of its own, and not because two locks are faster than one.
  // A device model writes into guest memory while it holds vm->lock,
  // and the page it writes into may be one the guest has posted but
  // never touched, so the write has to be able to back it. One lock
  // for both would be that write asking for a lock it already holds.
  // The order is vm->lock then memlock, never the other way.
  struct spinlock memlock;
  int id;
  volatile int state;
  // who it belongs to, and how many descriptors name it. A machine is
  // its creator's: another user may not run it, control it, read its
  // console or destroy it without CAP_SYS_ADMIN. `fdref' counts open
  // FD_VM files; an unnamed machine is destroyed when the last one is
  // closed, and a named one outlives its descriptors (vm_fdput).
  uint owner;
  int fdref;
  // complaints this machine has caused, counted rather than printed
  // every time (vm_gripe): a guest can fault or ask for something
  // impossible as fast as it can run.
  uint64 g_mmio, g_fault, g_sbi, g_filter;
  // the ports of this machine the world may reach, and how many frames
  // its switch port has turned away for not being asked for
  struct pport ports[NPPORT];
  int nports;
  struct spinlock portlock;
  // the gateway this machine is behind, if any. A machine behind one is
  // not on the switch's own segment: what it says reaches its gateway
  // and its neighbours behind the same gateway, and nothing else.
  struct cgw *gw;
  uint64 net_filtered;
  struct vmconfig cfg;    // how it was created
  uint64 entry;           // guest entry point
  struct inode *disk_ip;  // the disk image file, or 0
  struct gendisk *disk;   // a whole block device given to the guest instead, or 0
  int nvcpu;
  int vcpus_running;      // vcpus whose host process is inside vm_run
  // the scheduler group its vcpus belong to, so that the machine's
  // share of the processor is a property of the machine and not of
  // how many vcpus somebody gave it (kernel/sched.c)
  struct sched_group *sg;
  struct vcpu vcpu[NVCPU];

  uint64 ram_size;
  int nchunks;
  // one entry per 2 MB of guest RAM, 0 until touched.  An entry is
  // either the host physical address of a 2 MB block -- a G-stage
  // megapage -- or, with GC_SPLIT set, a pointer to a page holding
  // the 512 frames the chunk is backed by one page at a time.  See
  // gstage_split for why a chunk ever becomes the second kind.
  uint64 *chunk;
  uint64 resident;        // bytes of guest RAM currently backed
  int prefer_huge;        // back a chunk with one 2 MB block when one can be had
  uint64 mem_limit;       // host memory it may hold at once, in bytes (0: none)
  uint64 stalls;          // faults that waited for the limit to come down
  uint64 nsplit;          // chunks backed a page at a time
  uint64 pttables;        // G-stage table pages below the root
  uint64 *hgatp_root;     // 16 KB, 16 KB-aligned
  uint64 hgatp;

  int vic_kind;
  int vic_hold;           // vcpus stay out of the guest while the controller model is rebuilt
  int imsic_hw;           // use hardware guest interrupt files
  struct vuart uart;
  struct vvirtio blk;
  struct vvnet vnet;
  struct vvcon con;
  struct vballoon balloon;
  uint64 rtc_latch;       // virtual RTC: TIME_HIGH latched by a TIME_LOW read
  struct vplic plic;
  struct vaplic aplic;
  uint32 clint_msip[NVCPU];

  struct vdev dev[VM_MAXDEV];
  // rising edges per interrupt line, by irq number (see vic_set_level)
  uint64 irq_raised[VPLIC_NIRQ];
  // ---- machine revision (mutable.md) ----
  //
  // The description a guest was given is not final. `revise_gen' counts
  // revisions; the guest learns of one through a supervisor software interrupt
  // with no IPI reason behind it -- a channel chosen because it does not pass
  // through the interrupt controller, which is one of the things a revision may
  // be changing. `revise_acked' is the generation the guest has adopted, and
  // `revise_adopted' is the bitmask of what it managed (REV_* below).
  // the vector unit can be taken away from a running guest and given back
  // (mutable.md Phase 3). Revoking is masking V out of the misa this guest is
  // told about and out of its device tree, and forcing vsstatus.VS to Off so
  // that any vector instruction traps. A guest with live vector state in a task
  // refuses, which is the whole argument: what a machine may become depends on
  // what its guest is relying on at that moment.
  int vec_revoked;
  int vec_pending;   // a REV_VLEN revision is out and unanswered; a refusal flips vec_revoked back
  int vec_ceded;     // F11: the unit was given up for a move; the destination gives its own back
  // Lazy revelation: the machine is presented without a vector unit until the
  // guest executes a vector instruction, which traps here (illegal instruction,
  // not delegated) and is the one exact observation of *use* the host can have.
  // The unit is then granted and the instruction re-executed. A guest that never
  // tries keeps a compatibility set that includes every host without a unit.
  int lazy_v;

  // set the first time the guest asks the protocol's version: a guest that has
  // never asked does not speak it, and on arrival at a host of another shape is
  // given the description it left with rather than one it would never read.
  // where this machine's virtio devices sit relative to the default map
  // (G_VIRTIO_BASE and friends): the second axis a description can be revised
  // on. A host's own guests get the host's default; a machine arriving from a
  // host with a different map either keeps the one it left with (legacy) or is
  // revised to this host's and told (participating). Controllers, UART and RTC
  // are not moved: the UART is where earlycon expects it.
  uint64 mmio_shift;
  uint64 intc_shift;       // F2: where its interrupt controller's registers sit (+shift)
  int irq_alt;             // F3: its interrupt lines numbered the alternative way (guestirq=alt)
  // F15 on arrival: where the devices sat on the host it came from. Until the
  // guest has taken the new description its drivers may still touch the old
  // windows (Linux does, from another cpu, before its driver has moved them),
  // so those addresses are answered too while the revision is pending.
  int alias_on;
  uint64 alias_shift;
  int mac_told;            // F19: the NIC's identity changed on arrival; a guest with a driver is nudged to look (REV_DEVICES)
  int con_going;           // F18: the virtio-console left the description; its model answers until the guest has taken that
  int hart_alias_on;       // F9: the previous numbering still answers while the revision is pending (vm_vcpu_of_hart)
  int hart_alias_shift;
  uint32 sbi_hidden;       // F22: SBI extensions this machine is not offered (SBIH_*)
  int hart_shift;          // F9: the guest's harts are numbered id+shift (mhartid, cpus reg, a0 at boot)
  int mac_alt;             // F19: the NIC's identity (mac[3] 0x79 instead of 0x78), this host's policy
  int no_rtc;              // F16/F17: the machine has no RTC (the device that comes and goes)
  int console_kind;        // F18: CON_UART (16550), CON_VIRTIO (virtio-console), CON_SBI (DBCN)
  int online;              // F7/F8: vcpus [0, online) are started; the rest are described `disabled' and HSM-stopped
  int cboz_block;          // F21: the cache-block size described (bytes cbo.zero clears). The host's own: native
                           // (henvcfg.CBZE). Another: cbo.zero traps and is done here at the described size
  int cboz_prev;           // the size before a revision still pending: cleared at the larger of the two
  int machrev_line;        // the anchor device's line is up (a revision announced, not yet taken by the driver)
  // Where the description is. `dtb_gpa' is the blob the guest booted from, in
  // the last 64 KB of its RAM at birth; it is never written again, because a
  // guest may keep pointers into it (Linux's live tree does). A revision is
  // written to one of the two slots below it, alternately, and `desc_gpa' is
  // the slot the current description is in -- what `describe' answers. All
  // three are declared /reserved-memory (no-map) to the guest.
  uint64 desc_gpa;
  int no_clint;            // F4: no CLINT; the guest sends IPIs through SBI
  int no_sstc;             // F5: no Sstc; the guest's timer is the SBI timer, VSTIP injected
  uint32 timebase;         // F6: the timebase-frequency presented; 0 = the host's own (10 MHz).
                           // When it differs, `time' is trapped and scaled and the timer is SBI's.
  uint64 tb_x0, tb_g0;     // the clock's value is continuous across a rate change: guest time is
                           // tb_g0 + scale(X - tb_x0), X = host time + htimedelta, set at each change
  int revise_capable;
  uint32 revise_gen;
  uint32 revise_acked;
  uint32 revise_adopted;
  uint64 revise_asked;    // host tick when the notification went out
  uint32 revise_what;     // what the last revision said had changed (REV_*), for MACH fid 6
  uint64 revise_done;     // host tick when the ack came back

  // what this guest has observed about its machine (OBS_*), and the host tick
  // at which each fact was first observed. See vm_observe().
  uint32 obs;
  uint64 obs_first[OBS_COUNT];
  uint64 obs_t0;
  int ndev;

  struct cring cons_out, cons_in;
  int cons_attached;
  int cons_detach_req;    // a detach was requested: the next read returns -1

  uint64 dtb_gpa;

  // a snapshot in progress (kernel/vmstate.c). While one is, every page
  // the guest has not yet had written out is mapped read-only, and a
  // store to one is a page written to the file before the guest is let
  // on. `done' is one bit per guest page: set means the file has it (or
  // never needed it), clear means the guest's own write has to wait for
  // it. The lock covers the bitmap and the file's offset together,
  // because the two are one fact.
  struct vmsnap snap;

  // what this machine's guests add to the host's counter. Zero for a
  // machine that has always run here; on a restore it is the difference
  // between the counter the machine was saved at and the one it wakes up
  // to, so that the guest's clock goes on from where it stopped rather
  // than jumping forward by however long the file sat on a disk.
  uint64 htimedelta;
};

extern struct vm *vmtab[NVM]; // slot i's VM, or 0 until first used

// vballoon.c
void vballoon_init(struct vm *vm);
int vballoon_set(struct vm *vm, uint64 bytes); // ask for this much back
int vballoon_request(struct vm *vm, uint64 bytes); // the same, asked for by a person

// hyp.c
void hypinithart(void);
struct vm *vm_lookup(int id);
struct vm *vm_by_name(const char *name); // one the caller may reach, or 0
int vm_access(struct vm *vm);            // may this process touch it?
struct vm *vm_fdget(struct vm *vm);      // a descriptor takes a reference
void vm_fdput(struct vm *vm);            // and gives it back
int vm_count(void);
int vm_create(struct vmconfig *cfg);
// the same, without loading a kernel or writing a device tree: what a
// restore makes, because the machine's memory is about to be written
// from a file and an image loaded underneath it would show through
// wherever the guest had zeroes.
int vm_create_blank(struct vmconfig *cfg);
void vmstate_restored(struct vm *vm);
// a store to a page a snapshot has not written yet: the page goes to the
// file first, and the guest goes on afterwards. Called from the fault
// path, in the vcpu's own process, where sleeping is allowed.
void vmsnap_before_write(struct vm *vm, uint64 gpa);
int vm_snapshot(struct vm *vm, char *path);
int vm_clone_page(struct vm *dst, struct vm *src, uint64 gpa);
void vm_snap_protect(struct vm *vm, uint32 *done, uint32 *dirty, int npages);
void vm_snap_unprotect(struct vm *vm);
int vm_run(struct vm *vm, int vcpuid);
int vm_destroy(struct vm *vm);
int vm_pause(struct vm *vm);
int vm_resume(struct vm *vm);
int vm_gpa2hpa(struct vm *vm, uint64 gpa, uint64 *hpa, uint64 *len);
int vm_touch(struct vm *vm, uint64 gpa); // back the granule holding gpa, if it is guest RAM
int vm_split(struct vm *vm, uint64 gpa);   // make the chunk holding gpa page-granular
int vm_unback(struct vm *vm, uint64 gpa);  // take one page of guest RAM away
void vm_fence(struct vm *vm);              // every vcpu flushes its G-stage TLB
void vm_fence_sync(struct vm *vm);         // and wait until they have
int vm_reclaim(struct vm *vm, int tries);  // at the limit: ask the guest, then wait

// what same-page merging (ksm.c) needs from hyp.c
uint64 vm_protect_page(struct vm *vm, uint64 gpa);
void vm_unprotect_page(struct vm *vm, uint64 gpa, uint64 frame);
int vm_share_page(struct vm *vm, uint64 gpa, uint64 frame);
int vm_unshare(struct vm *vm, uint64 gpa);
int vm_copyin(struct vm *vm, void *dst, uint64 gpa, uint64 n);
int vm_copyout(struct vm *vm, uint64 gpa, void *src, uint64 n);
void vcpu_kick(struct vcpu *v);
void vcpu_inject(struct vcpu *v, uint64 cause, uint64 tval);
void vm_halt(struct vm *vm);
int vm_seip(struct vcpu *v);
void hyp_stats(uint64 *out, int n);
int vm_gripe(uint64 *count);  // should this complaint about a guest be printed?

// hypcsr.c
int hyp_illegal(struct vcpu *v, uint32 inst);
int hyp_virtinst(struct vcpu *v, uint32 inst);
int hyp_ecall(struct vcpu *v);           // hypsbi.c
int vcpu_hsm_start(struct vcpu *v, uint64 addr, uint64 opaque);
void vcpu_request_fence(struct vcpu *v, int what);
uint64 hyp_next_deadline(uint64 dflt);
uint32 hyp_fetch(struct vcpu *v, uint64 gva);

// hypmmio.c
int hyp_mmio(struct vcpu *v, uint64 gpa, int is_store, uint32 inst);
void vvcon_init(struct vm *vm);          // vvcon.c
void vvcon_rx(struct vm *vm);            // host input -> the guest's queue 0
int vvcon_active(struct vm *vm);         // is the guest driving it?
// What a revision can change, and what the guest reports adopting.
#define REV_DEVICES  (1u << 0)   // the device tree changed (devices, addresses)
#define REV_INTC     (1u << 1)   // the interrupt controller is a different kind
#define REV_VLEN     (1u << 2)   // the vector register file is a different width
#define REV_SBI      (1u << 3)   // the set of SBI extensions on offer changed (F22)
#define REV_TIMER    (1u << 4)   // the timer source changed: Sstc or the SBI timer (F5)
#define REV_MEM      (1u << 5)   // F14: host to host only -- the guest is asked (through its balloon) to be smaller
#define REV_MEMORY   (1u << 6)   // F13: the machine has more RAM than it had (the memory node grew)
// F18: the kinds of console a machine may be described with
#define CON_UART   0
#define CON_VIRTIO 1
#define CON_SBI    2
// F22: SBI extensions a machine can be described without. Bits of vm->sbi_hidden.
#define SBIH_TIME    1u
#define SBIH_IPI     2u
#define SBIH_RFENCE  4u
#define SBIH_HSM     8u
#define SBIH_SRST    16u
#define SBIH_DBCN    32u
#define SBIH_ALL     63u
#define REV_NONE     0u

int  vm_revise(struct vm *vm, uint32 what);   // hyp.c: ask a guest to look again
int  vm_revise_intc(struct vm *vm, int kind); // hyp.c: and give it a different controller
int  vm_revise_vector(struct vm *vm, int on);  // hyp.c: take the vector unit away, or give it back
int  vm_revise_intc_addr(struct vm *vm, uint64 shift);  // hyp.c: move the controller's registers (F2)
int  vm_irq(struct vm *vm, int irq);            // hyp.c: the line a device raises on this machine (F3)
int  vm_set_imsic_hw(struct vm *vm, int on);    // hyp.c: hardware guest file or software model (F23)
int  vm_revise_sbi(struct vm *vm, uint32 hidden); // hyp.c: change the SBI extensions on offer (F22)
int  vm_peek(struct vm *vm, void *dst, uint64 gpa, uint64 n);  // hyp.c: vm_copyin that backs nothing
int  vm_revise_ipi(struct vm *vm, int sbi);       // hyp.c: CLINT or SBI IPIs (F4)
void vclint_add(struct vm *vm);                   // vclint.c: register the CLINT model
int  vm_revise_timer(struct vm *vm, int sbi);     // hyp.c: Sstc or the SBI timer (F5)
int  vm_revise_timebase(struct vm *vm, uint32 hz); // hyp.c: another timebase-frequency (F6)
int  vm_revise_hartid(struct vm *vm, int shift);  // hyp.c: renumber the harts (F9)
int  vm_revise_mac(struct vm *vm, int alt);       // hyp.c: another NIC identity (F19)
int  vm_revise_rtc(struct vm *vm, int on);        // hyp.c: an RTC comes or goes (F16/F17)
int  vm_revise_console(struct vm *vm, int kind);  // hyp.c: another kind of console (F18)
int  vm_grow(struct vm *vm, uint64 bytes);        // hyp.c: more RAM for a running machine (F13)
int  vm_revise_cpus(struct vm *vm, int online);   // hyp.c: harts started or parked (F7/F8)
int  vm_revise_cboz(struct vm *vm, int bytes);    // hyp.c: another cache-block size (F21)
void vmachrev_init(struct vm *vm);                // vmachrev.c: the anchor device
void vmachrev_notify(struct vm *vm);              // vmachrev.c: raise its line (caller holds vm->lock)
extern uint64 hyp_cboz_exits;                     // cbo.zero instructions done in software
void vballoon_pin(struct vm *vm);                 // vballoon.c: the arriving balloon is what it is (F14)
void vvcon_add(struct vm *vm);                    // vvcon.c: register the virtio-console model
void vrtc_add(struct vm *vm);                     // vrtc.c: register the RTC model
void vvnet_mac_changed(struct vm *vm);            // vvnet.c: tell the guest through the device
// F9: a guest addresses its harts by hart id; the models index vcpus. -1: no such hart.
static inline int vm_vcpu_of_hart(struct vm *vm, long hart) {
  long i = hart - vm->hart_shift;
  if (i >= 0 && i < vm->nvcpu)
    return (int)i;
  // The old numbering, honoured until the guest has taken the new one. Until
  // then the guest's IPI masks, and the interrupt-controller context its
  // anchor line is enabled in, still carry the old numbers -- a Linux guest
  // renumbered on arrival never heard of the revision, because the line
  // announcing it was enabled in a context that had just moved (lxf9).
  if (vm->hart_alias_on && vm->revise_acked != vm->revise_gen) {
    i = hart - vm->hart_alias_shift;
    if (i >= 0 && i < vm->nvcpu)
      return (int)i;
  }
  return -1;
}
uint64 vm_guest_time(struct vm *vm);              // hyp.c: `time' as this machine sees it
uint64 vm_host_time_of(struct vm *vm, uint64 gt); // hyp.c: and back
uint64 vm_guest_time_of(struct vm *vm, uint64 x); // hyp.c: a host moment as the guest would read it
#define HOST_TIMEBASE 10000000UL
#define vm_scaled_time(vm) ((vm)->timebase != 0 && (vm)->timebase != HOST_TIMEBASE)
uint32 sbi_hidden_default(void);                // hyp.c: what the host command line withholds
uint64 hyp_guest_room_bytes(void);              // hyp.c: memory this host has for an arriving guest (F14)
int  vm_split_all(struct vm *vm);               // hyp.c: 4 KB pages everywhere (F24)
int  vm_revise_irqmap(struct vm *vm, int alt);  // hyp.c: renumber a running guest's lines (F3)
int  vic_levels_snapshot(struct vm *vm, uint64 *levels);   // vic.c
void vic_levels_restore(struct vm *vm, uint64 levels);     // vic.c
const char *vic_kindname(int kind);                         // vic.c
int  vic_held(uint64 levels);                              // vic.c
void vm_observe(struct vm *vm, uint32 what);   // hyp.c: record an observation
void vm_del_dev(struct vm *vm, const char *name);   // hypmmio.c
void vm_add_dev(struct vm *vm, const char *name, int irq, uint32 obs, uint64 base, uint64 size,
                int (*read)(struct vm *, struct vdev *, uint64, int, uint64 *),
                int (*write)(struct vm *, struct vdev *, uint64, int, uint64),
                void *priv);

// vic (virtual interrupt controller front end, in vic.c)
void vic_init(struct vm *vm);
void vic_set_level(struct vm *vm, int irq, int level); // caller holds vm->lock
int vic_seip(struct vcpu *v);                          // caller holds vm->lock
uint64 vic_vcpu_enter(struct vcpu *v);                 // per-entry setup; returns hvip bits
void vic_vcpu_exit(struct vcpu *v);
int vic_csr(struct vcpu *v, uint32 inst);              // IMSIC CSR emulation
void vic_msi(struct vm *vm, int vcpuid, int eiid);     // caller holds vm->lock

// vuart.c
void vuart_init(struct vm *vm);
int vuart_irq_level(struct vm *vm);
int vm_cons_read(struct vm *vm, int user, uint64 dst, int n);
int vm_cons_ready(struct vm *vm, int write);
int vm_cons_write(struct vm *vm, int user, uint64 src, int n);
void vm_cons_putc(struct vm *vm, int c);
void vm_cons_emit(struct vm *vm, const char *buf, int n);
int vm_cons_getc(struct vm *vm);

// vvirtio.c
void vvirtio_init(struct vm *vm);
void vvirtio_stop(struct vm *vm);
// wait until the disk thread has nothing in hand: what it has in hand is
// a write into guest memory, and a save that does not wait for it writes
// down a page the device is about to change.
int vvirtio_quiesce(struct vm *vm);

// vrtc.c
void vrtc_init(struct vm *vm);

// vvnet.c / bridge.c
struct mbuf;
void vvnet_init(struct vm *vm);
void vvnet_rx(struct vm *vm, struct mbuf *m);
int vm_port_add(struct vm *vm, uint8 proto, uint16 hport, uint16 gport);
int vm_port_rm(struct vm *vm, uint8 proto, uint16 hport);
int vm_port_lookup(struct vm *vm, uint8 proto, uint16 hport, uint16 *gport);
void bridgeinit(void);
void bridge_rx_uplink(struct mbuf *m);
int bridge_tx_host(struct mbuf *m);
void bridge_tx_guest(struct vm *from, struct mbuf *m);
extern uint64 bridge_guest_blocked;

// vclint.c
void vclint_init(struct vm *vm);

// vplic.c / vaplic.c / vimsic.c
void vplic_init(struct vm *vm);
void vplic_set_level(struct vm *vm, int irq, int level);
int vplic_seip(struct vcpu *v);
void vaplic_init(struct vm *vm, int msi);
void vaplic_set_level(struct vm *vm, int irq, int level);
int vaplic_seip(struct vcpu *v);
void vimsic_init(struct vm *vm);
int vimsic_seip(struct vcpu *v);
void vimsic_send(struct vcpu *v, int eiid);
int vimsic_csr(struct vcpu *v, uint32 inst);
uint64 vimsic_enter(struct vcpu *v);
void vimsic_hartinit(void);
int vimsic_geilen(void);
void vimsic_exit(struct vcpu *v);
void vimsic_sgei(void);

// fdtgen.c
int hyp_make_dtb(struct vm *vm, uint64 gpa, int max);

// cring helpers
static inline int
cring_empty(struct cring *r)
{
  return r->r == r->w;
}
static inline int
cring_full(struct cring *r)
{
  return r->w - r->r == CRING;
}
static inline void
cring_put(struct cring *r, char c)
{
  r->buf[r->w % CRING] = c;
  r->w++;
}
static inline char
cring_get(struct cring *r)
{
  char c = r->buf[r->r % CRING];
  r->r++;
  return c;
}
