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
#define VM_MAXMEM    (256UL << 30)       // 256 GiB per VM at most
#define VM_DTB_MAX   0x10000

enum vic_kind { VIC_PLIC = 0, VIC_APLIC = 1, VIC_IMSIC = 2 };

#define HSM_STARTED 0
#define HSM_STOPPED 1
#define FENCE_I   1
#define FENCE_VMA 2

// ---- virtual devices ----

struct vm;
struct vcpu;

struct vdev {
  uint64 base, size;
  int (*read)(struct vm *, struct vdev *, uint64 off, int size, uint64 *val);
  int (*write)(struct vm *, struct vdev *, uint64 off, int size, uint64 val);
  void *priv;
};
#define VM_MAXDEV 12

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
  volatile int fence_req; // FENCE_I / FENCE_VMA to execute on the next entry
  int last_hart;          // hart of the previous entry (-1: none)

  // emulated machine-mode state (the guest's own start() runs in VS mode).
  int in_mmode;
  uint64 m_mstatus, m_mepc, m_medeleg, m_mideleg, m_mie, m_mtvec, m_mscratch;
  uint64 m_menvcfg, m_mcounteren, m_pmpaddr0, m_pmpcfg0;

  // statistics
  uint64 exits, mmio, csr_emul, wfi_sleeps, injected;
  uint64 last_cause, last_gpa;
};

// ---- vm ----

struct gendisk;

struct vm {
  struct spinlock lock;
  int id;
  volatile int state;
  struct vmconfig cfg;    // how it was created
  uint64 entry;           // guest entry point
  struct inode *disk_ip;  // the disk image file, or 0
  struct gendisk *disk;   // a whole block device given to the guest instead, or 0
  int nvcpu;
  int vcpus_running;      // vcpus whose host process is inside vm_run
  struct vcpu vcpu[NVCPU];

  uint64 ram_size;
  int nchunks;
  uint64 *chunk;          // host physical address of each 2 MB chunk, 0 until touched
  uint64 resident;        // bytes of guest RAM currently backed
  uint64 ptchunk;         // 2 MB chunk holding the G-stage page tables
  uint64 ptnext;          // next free page in ptchunk
  uint64 *hgatp_root;     // 16 KB, 16 KB-aligned
  uint64 hgatp;

  int vic_kind;
  int imsic_hw;           // use hardware guest interrupt files
  struct vuart uart;
  struct vvirtio blk;
  struct vvnet vnet;
  uint64 rtc_latch;       // virtual RTC: TIME_HIGH latched by a TIME_LOW read
  struct vplic plic;
  struct vaplic aplic;
  uint32 clint_msip[NVCPU];

  struct vdev dev[VM_MAXDEV];
  int ndev;

  struct cring cons_out, cons_in;
  int cons_attached;
  int cons_detach_req;    // a detach was requested: the next read returns -1

  uint64 dtb_gpa;

};

extern struct vm *vmtab[NVM]; // slot i's VM, or 0 until first used

// hyp.c
void hypinithart(void);
struct vm *vm_lookup(int id);
int vm_count(void);
int vm_create(struct vmconfig *cfg);
int vm_run(struct vm *vm, int vcpuid);
int vm_destroy(struct vm *vm);
int vm_pause(struct vm *vm);
int vm_resume(struct vm *vm);
int vm_gpa2hpa(struct vm *vm, uint64 gpa, uint64 *hpa, uint64 *len);
int vm_touch(struct vm *vm, uint64 gpa); // back the chunk holding gpa, if it is guest RAM
int vm_copyin(struct vm *vm, void *dst, uint64 gpa, uint64 n);
int vm_copyout(struct vm *vm, uint64 gpa, void *src, uint64 n);
void vcpu_kick(struct vcpu *v);
void vcpu_inject(struct vcpu *v, uint64 cause, uint64 tval);
void vm_halt(struct vm *vm);
int vm_seip(struct vcpu *v);
void hyp_stats(uint64 *out, int n);

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
void vm_add_dev(struct vm *vm, uint64 base, uint64 size,
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
int vm_cons_write(struct vm *vm, int user, uint64 src, int n);
void vm_cons_putc(struct vm *vm, int c);
int vm_cons_getc(struct vm *vm);

// vvirtio.c
void vvirtio_init(struct vm *vm);

// vrtc.c
void vrtc_init(struct vm *vm);

// vvnet.c / bridge.c
struct mbuf;
void vvnet_init(struct vm *vm);
void vvnet_rx(struct vm *vm, struct mbuf *m);
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
