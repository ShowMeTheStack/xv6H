//
// description of the machine we are running on, filled in from the
// device tree that qemu (or the xv6 hypervisor) hands us in a1 at boot.
//

#define PLAT_MAXVIRTIO 8

enum intc_kind {
  INTC_PLIC = 0,  // classic PLIC (qemu -M virt,aia=none)
  INTC_APLIC = 1, // APLIC in direct mode (aia=aplic)
  INTC_IMSIC = 2, // APLIC in MSI mode + IMSIC (aia=aplic-imsic)
};

struct platform {
  volatile int ready;   // set once hart 0 has filled this in

  uint64 ram_base;      // physical RAM start (0x80000000 on qemu virt)
  uint64 ram_size;      // from the device tree
  uint64 ram_end;       // ram_base + ram_size
  uint64 resv_top;      // bytes at the top of RAM not to touch: the machine's description and its revision slots
  uint64 resv_base, resv_size;   // as the tree says it (reserved-memory/xv6h-description)
  int mmu_max;          // widest paging mode the hardware offers: 39, 48 or 57 (device tree mmu-type)
  int mmu_levels;       // levels of every page table: 4 (Sv48) or 3 (Sv39)
  uint64 maxva;         // one past the highest virtual address: 1<<47 or 1<<38
  char bootargs[128];   // /chosen/bootargs of this machine (the host's own command line)
  int ncpu;             // harts described by the device tree

  int intc_kind;        // enum intc_kind
  uint64 plic_base, plic_size;
  uint64 aplic_m_base, aplic_s_base, aplic_size;
  int aplic_nsrc;
  uint64 imsic_m_base, imsic_s_base, imsic_s_size;
  int imsic_guest_bits; // riscv,guest-index-bits (S-file stride = 4096 << bits)
  int imsic_nids;       // riscv,num-ids

  uint64 clint_base;    // 0: no CLINT, IPIs go through SBI (F4)
  int has_sstc;         // the timer is stimecmp (Sstc); else the SBI timer (F5)
  int cboz_block;       // F21: bytes cbo.zero clears (riscv,cboz-block-size); 0 without Zicboz
  int hartid[NCPU];     // F9: the hart id of logical cpu i (tp), from the tree's cpu nodes
  int console_kind;     // F18: 0 the 16550, 1 a virtio-console, 2 the SBI debug console (chosen/stdout-path)

  uint64 rtc_base;      // goldfish RTC (0: none)
  int rtc_irq;
  uint64 timebase;      // ticks of the time counter per second
  uint64 tb_t0, tb_ns0; // the monotonic clock's anchor at the last rate change (F6)

  uint64 uart_base;
  int uart_irq;

  int nvirtio;
  uint64 virtio_base[PLAT_MAXVIRTIO];
  int virtio_irq[PLAT_MAXVIRTIO];

  uint32 net_ip, net_mask, net_gw, net_dns; // static network config (0: use DHCP)
  // a machine behind a gateway object: the network on the far side of
  // it, and the two addresses on that network it may not talk to.
  // outer_mask is 0 on a machine that is not behind one.
  uint32 outer_net, outer_mask, outer_gw, outer_dns;
  uint64 misa;          // read in machine mode at boot
  int has_h;            // misa reports the hypervisor extension
  int has_v;            // misa reports the vector extension
  // the AIA CSRs -- hvictl, vsiselect/vsireg/vstopei -- exist on this CPU. Not in
  // misa (Ssaia is not a letter), and no trap-safe way to probe a CSR here, so
  // it is inferred from the machine: qemu's virt gives the CPU those CSRs with
  // aia=aplic-imsic and a host booted on aia=none took an illegal instruction on
  // hvictl. Conservative: only an IMSIC host is assumed to have them.
  int has_aia_csrs;
  uint64 vlenb;         // bytes per vector register (0 without V)
  int is_guest;         // device tree carries the "xv6h,guest" marker
  int guest_id;         // from the xv6h,vmid property, if a guest
  uint64 fdt_addr;      // where the original blob was
  uint32 fdt_size;
};

extern struct platform plat;
extern char fdt_copy[];
#define FDT_COPY_MAX (64 * 1024)

void platform_init(uint64 fdt_pa);
int plat_bootarg(const char *word);
void platform_parse_into(struct platform *out, uint64 fdt_pa);   // a tree into a struct other than plat
int plat_cpu_of_hart(uint64 hartid);        // F9: logical index of a hart id, or -1
uint64 plat_bootarg_num(const char *key);   // the number after `key=', or 0
int platform_virtio_by_irq(int irq);
