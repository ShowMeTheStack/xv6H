// IMSIC definitions (RISC-V AIA spec, chapter 3).
// shared by the host driver (imsic.c) and the guest model (vimsic.c).

// indirect register numbers, selected through siselect / vsiselect.
#define IMSIC_EIDELIVERY  0x70
#define IMSIC_EITHRESHOLD 0x72
#define IMSIC_EIP0        0x80 // eip0..eip63 (only even ones on RV64)
#define IMSIC_EIE0        0xC0 // eie0..eie63

// CSR numbers.
#define CSR_SISELECT  0x150
#define CSR_SIREG     0x151
#define CSR_STOPEI    0x15c
#define CSR_STOPI     0xdb0
#define CSR_VSISELECT 0x250
#define CSR_VSIREG    0x251
#define CSR_VSTOPEI   0x25c
#define CSR_VSTOPI    0xeb0

// memory-mapped interrupt file registers (one 4 KB page per file).
#define IMSIC_SETEIPNUM_LE 0x000
#define IMSIC_SETEIPNUM_BE 0x004

#define IMSIC_MAX_IDS 256
