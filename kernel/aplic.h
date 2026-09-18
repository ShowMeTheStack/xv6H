// APLIC register layout (RISC-V AIA spec, chapter 4).
// shared by the host driver (aplic.c) and the guest model (vaplic.c).

#define APLIC_DOMAINCFG      0x0000
#define APLIC_DOMAINCFG_BE   (1 << 0)
#define APLIC_DOMAINCFG_DM   (1 << 2)
#define APLIC_DOMAINCFG_IE   (1 << 8)
#define APLIC_DOMAINCFG_RO80 (0x80 << 24)

#define APLIC_SOURCECFG(i)   (0x0004 + ((i) - 1) * 4)
#define APLIC_SOURCECFG_D    (1 << 10)
#define APLIC_SM_INACTIVE    0
#define APLIC_SM_DETACHED    1
#define APLIC_SM_EDGE1       4
#define APLIC_SM_EDGE0       5
#define APLIC_SM_LEVEL1      6
#define APLIC_SM_LEVEL0      7

#define APLIC_MMSIADDRCFG    0x1BC0
#define APLIC_MMSIADDRCFGH   0x1BC4
#define APLIC_SMSIADDRCFG    0x1BC8
#define APLIC_SMSIADDRCFGH   0x1BCC

#define APLIC_SETIP          0x1C00 // 32 words
#define APLIC_SETIPNUM       0x1CDC
#define APLIC_IN_CLRIP       0x1D00 // 32 words
#define APLIC_CLRIPNUM       0x1DDC
#define APLIC_SETIE          0x1E00 // 32 words
#define APLIC_SETIENUM       0x1EDC
#define APLIC_CLRIE          0x1F00 // 32 words
#define APLIC_CLRIENUM       0x1FDC
#define APLIC_SETIPNUM_LE    0x2000
#define APLIC_SETIPNUM_BE    0x2004
#define APLIC_GENMSI         0x3000
#define APLIC_TARGET(i)      (0x3004 + ((i) - 1) * 4)

#define APLIC_IDC(hart)      (0x4000 + (hart) * 32)
#define APLIC_IDC_IDELIVERY  0x00
#define APLIC_IDC_IFORCE     0x04
#define APLIC_IDC_ITHRESHOLD 0x08
#define APLIC_IDC_TOPI       0x18
#define APLIC_IDC_CLAIMI     0x1C

#define APLIC_MMIO_SIZE      0x8000
#define APLIC_MAX_SOURCES    96 // qemu virt
