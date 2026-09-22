#ifndef XV6_VMM_H
#define XV6_VMM_H
//
// hypervisor interface shared by the kernel and user programs (vmm).
//
//   vm_create(struct vmconfig *)      -> id, or -1
//   vm_run(id, vcpu)                  run a vcpu in this process until the VM stops
//   vm_destroy(id)                    halt and free
//   vm_ctl(id, op)                    VMCTL_PAUSE / VMCTL_RESUME
//   vm_cons(id, op, buf, n)           VMCONS_READ / VMCONS_WRITE / VMCONS_DETACH
//   vm_info(id, struct vminfo *)      0 if the slot exists (state VM_FREE if empty)
//   consraw(on)                       host console raw mode
//

#define VM_NAMELEN 16
#define VM_PATHLEN 64

// vmconfig.flags
#define VMF_SOFTIMSIC 1 // IMSIC mode: use the software interrupt file model
#define VMF_RAW       2 // kernel is a raw binary loaded at load_addr (also the entry)
#define VMF_HSM       4 // secondary vcpus start stopped, to be started via SBI HSM (Linux)
#define VMF_NOHUGE    8 // back guest RAM 4 KB at a time, never with megapages

struct vmconfig {
  char name[VM_NAMELEN];   // optional
  char kernel[VM_PATHLEN]; // ELF (any load address) or raw image, in the host file system
  char disk[VM_PATHLEN];   // disk image file in the host file system, "" for no disk
  int mem_mb;
  int mem_limit_mb;        // host memory this VM may hold at once (0: no limit)
  int nvcpu;
  int flags;
  uint64 load_addr;        // VMF_RAW: guest-physical load address
  char bootargs[128];      // kernel command line (device tree /chosen/bootargs)
  int net;                 // 0: no address configuration, 1: dhcp, 2: static
  uint32 ip, mask, gw, dns; // static configuration (host byte order)
};

enum vmstate { VM_FREE = 0, VM_NEW, VM_RUNNING, VM_HALTING, VM_PAUSED };

#define VMCTL_PAUSE  0
#define VMCTL_RESUME 1
// vm_ctl(id, VMCTL_BALLOON + megabytes): ask the guest to give that
// much of its memory back. The guest answers in its own time, and
// vm_info reports how far it has got.
#define VMCTL_BALLOON 0x10000
// same-page merging is the host's, not a VM's: the id is ignored
#define VMCTL_KSM_OFF 2
#define VMCTL_KSM_ON  3

#define VMCONS_READ   0
#define VMCONS_WRITE  1
#define VMCONS_DETACH 2

struct vminfo {
  int id, state, nvcpu, mem_mb, intc, imsic_hw;
  int resident_mb;        // guest RAM actually backed by host memory
  int split_chunks;       // 2 MB chunks backed a page at a time
  int table_pages;        // G-stage page table pages below the root
  int balloon_target_mb;  // how much the host has asked the guest to give back
  int balloon_actual_mb;  // how much the guest has given back so far
  int mem_limit_mb;       // the limit, if it has one
  struct vmconfig cfg;
  uint64 entry;
  uint64 disk_sectors;
  uint64 exits, mmio, csr, wfi, injected;
  // vcpu 0, for debugging
  uint64 pc, last_cause, last_gpa;
  int running_hart, idle, mmode;
  uint64 vsatp, vstvec, vsepc, vscause, vsstatus, vstval, sp, ra, vsie, vstimecmp;
  uint64 scounteren, senvcfg, hcounteren;
  // vcpu 1 (if any): pc, last cause, idle, hsm state, exits, injected VSEIP state
  uint64 pc1, cause1, exits1;
  int idle1, hsm1, hart1;
  uint64 eie0_0, eip0_0, eie0_1, eip0_1;
  int deliv0, deliv1, thr0, thr1;
};

#endif
