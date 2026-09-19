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

  uint64 clint_base;

  uint64 rtc_base;      // goldfish RTC (0: none)
  int rtc_irq;
  uint64 timebase;      // ticks of the time counter per second

  uint64 uart_base;
  int uart_irq;

  int nvirtio;
  uint64 virtio_base[PLAT_MAXVIRTIO];
  int virtio_irq[PLAT_MAXVIRTIO];

  uint32 net_ip, net_mask, net_gw, net_dns; // static network config (0: use DHCP)
  uint64 misa;          // read in machine mode at boot
  int has_h;            // misa reports the hypervisor extension
  int has_v;            // misa reports the vector extension
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
int platform_virtio_by_irq(int irq);
