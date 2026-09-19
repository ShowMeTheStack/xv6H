// hwinfo(): what this kernel is running on (hwinfo system call).
struct hwinfo {
  uint64 misa;   // ISA bits: bit 5 F, bit 3 D, bit 21 V, bit 7 H, ...
  int ncpu;
  int has_h;     // hypervisor extension
  int has_v;     // vector extension
  uint64 vlenb;  // bytes per vector register
  int is_guest;  // running as an xv6H guest
  int guest_id;
};
