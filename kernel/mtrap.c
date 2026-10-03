//
// machine mode's own traps, on hardware that leaves things to it (Rocket on
// the ZedBoard). Two jobs:
//
//   - misaligned loads and stores. Rocket raises an address-misaligned
//     exception where QEMU simply performs the access, so nothing in xv6H
//     (or in an xv6 guest, or a Linux guest's user space) was written to
//     expect one. They are performed here, a byte at a time, for every mode
//     below this one: through mstatus.MPRV, so the access is translated as the
//     faulting mode's would have been (MPV for a guest: its two-stage
//     translation), with MXR so the instruction itself can be read. An access
//     that faults is handed down as the fault it would have been, to whoever
//     takes it (VS when hedeleg says so, else HS), as OpenSBI does.
//
//   - anything else that reaches machine mode unexpectedly is said on the
//     console, cause, pc and tval, before the hart parks: it used to park
//     silently, and on the board the record was in a write-back cache.
//
// Runs on the machine stack (mscratch), with the MMU off for its own accesses.
//
#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"

#define CSR_VSSTATUS_ 0x200
#define CSR_VSTVEC_   0x205
#define CSR_VSEPC_    0x241
#define CSR_VSCAUSE_  0x242
#define CSR_VSTVAL_   0x243
#define CSR_HSTATUS_  0x600
#define CSR_HEDELEG_  0x602
#define CSR_HTVAL_    0x643
#define CSR_HTINST_   0x64a
#define MSTATUS_MPV   (1UL << 39)
#define MSTATUS_GVA   (1UL << 38)
#define HSTATUS_SPV   (1UL << 7)
#define HSTATUS_SPVP  (1UL << 8)
#define HSTATUS_GVA   (1UL << 6)

#define csrr(csr) ({ uint64 __v; asm volatile("csrr %0, " #csr : "=r"(__v)); __v; })
#define csrw(csr, v) asm volatile("csrw " #csr ", %0" ::"r"((uint64)(v)))
#define r_mcause() csrr(mcause)
#define r_mepc()   csrr(mepc)
#define r_mtval()  csrr(mtval)
#define w_scause(v) csrw(scause, v)
#define w_stval(v)  csrw(stval, v)

// mvec.S: one byte through MPRV, behind a trap guard; *fault is the cause, or 0
uint64 mprv_lbu(uint64 addr, uint64 *fault);
void mprv_sb(uint64 addr, uint64 val, uint64 *fault);
uint64 mprv_lhu(uint64 addr, uint64 *fault);

uint64 mtrap_misaligned;        // how many were performed (for the record)

static void
mputc(int c)
{
  volatile uint8 *u = (volatile uint8 *)plat.uart_base;
  if (!u)
    return;
  while ((u[5] & (1 << 5)) == 0)
    ;
  u[0] = c;
}

static void
mputs(const char *s)
{
  while (*s)
    mputc(*s++);
}

static void
mputx(uint64 v)
{
  mputs("0x");
  for (int i = 60; i >= 0; i -= 4)
    mputc("0123456789abcdef"[(v >> i) & 15]);
}

// the trap handed down to the mode that would have taken it had it been delegated
static uint64 redirects;
static void mputs(const char *s);
static void mputx(uint64 v);

static void
redirect(uint64 cause, uint64 tval, uint64 epc, uint64 mstatus)
{
  int from_v = plat.has_h && (mstatus & MSTATUS_MPV);
  int from_s = ((mstatus >> 11) & 3) == 1;

  if (redirects++ < 8) {          // said, the first few: a fault in a performed access is rare
    mputs("\nmachine: handing down cause ");
    mputx(cause);
    mputs(" tval ");
    mputx(tval);
    mputs(" epc ");
    mputx(epc);
    mputs(" from mode ");
    mputx((mstatus >> 11) & 3);
    mputs(from_v ? " (guest)\n" : "\n");
  }
  if (((mstatus >> 11) & 3) == 3) {
    // machine mode's own access faulted: there is nobody below to take it
    mputs("machine: a performed access of machine mode itself faulted\n");
    for (;;)
      asm volatile("wfi");
  }

  if (from_v && ((csrr(0x602) >> cause) & 1)) {
    // the guest's own: into VS
    uint64 vs = csrr(0x200);
    vs = (vs & ~(SSTATUS_SPP | SSTATUS_SPIE)) | (from_s ? SSTATUS_SPP : 0) | ((vs & SSTATUS_SIE) ? SSTATUS_SPIE : 0);
    vs &= ~SSTATUS_SIE;
    csrw(0x200, vs);
    csrw(0x242, cause);
    csrw(0x243, tval);
    csrw(0x241, epc);
    w_mepc(csrr(0x205) & ~3UL);
    w_mstatus((mstatus & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_S);   // MPV stays 1
    return;
  }
  // the hypervisor's (or, without H, the kernel's): into HS
  if (plat.has_h) {
    uint64 hs = csrr(0x600) & ~(HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_GVA);
    if (from_v)
      hs |= HSTATUS_SPV | (from_s ? HSTATUS_SPVP : 0) | HSTATUS_GVA;
    csrw(0x600, hs);
    csrw(0x643, 0);
    csrw(0x64a, 0);
  }
  uint64 s = r_sstatus();
  s = (s & ~(SSTATUS_SPP | SSTATUS_SPIE)) | ((from_s || from_v) ? SSTATUS_SPP : 0) | ((s & SSTATUS_SIE) ? SSTATUS_SPIE : 0);
  s &= ~SSTATUS_SIE;
  w_sstatus(s);
  w_scause(cause);
  w_stval(tval);
  w_sepc(epc);
  w_mepc(r_stvec() & ~3UL);
  w_mstatus((mstatus & ~(MSTATUS_MPP_MASK | MSTATUS_MPV)) | MSTATUS_MPP_S);
}

// the floating-point registers by number
#define FREG_CASES(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
  X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
static uint64
freg_get(int n, int dbl)
{
  uint64 v = 0;
#define G(i) case i: if (dbl) asm volatile("fmv.x.d %0, f" #i : "=r"(v)); else asm volatile("fmv.x.w %0, f" #i : "=r"(v)); break;
  switch (n) { FREG_CASES(G) }
#undef G
  return dbl ? v : (v & 0xffffffffUL);
}
static void
freg_set(int n, uint64 v, int dbl)
{
#define S(i) case i: if (dbl) asm volatile("fmv.d.x f" #i ", %0" ::"r"(v)); else asm volatile("fmv.w.x f" #i ", %0" ::"r"(v)); break;
  switch (n) { FREG_CASES(S) }
#undef S
}

// 0: performed; 1: not a load or store this knows
static int
misaligned(uint64 *r, uint64 cause, uint64 epc, uint64 tval, uint64 mstatus)
{
  uint64 fault = 0;
  uint32 inst = mprv_lhu(epc, &fault);
  if (!fault && (inst & 3) == 3)
    inst |= (uint32)mprv_lhu(epc + 2, &fault) << 16;
  if (fault) {
    w_mstatus(mstatus);
    redirect(fault, epc, epc, mstatus);
    return 0;
  }
  int len = (inst & 3) == 3 ? 4 : 2;
  int load, n, sign = 0, fp = 0, reg;
  uint32 f3 = (inst >> 12) & 7, op = inst & 0x7f;
  if (len == 4) {
    if (op == 0x03 && f3 != 7) {               // LB LH LW LD LBU LHU LWU
      load = 1; n = 1 << (f3 & 3); sign = !(f3 & 4); reg = (inst >> 7) & 31;
    } else if (op == 0x23 && f3 < 4) {         // SB SH SW SD
      load = 0; n = 1 << f3; reg = (inst >> 20) & 31;
    } else if (op == 0x07 && (f3 == 2 || f3 == 3)) {   // FLW FLD
      load = 1; n = f3 == 2 ? 4 : 8; fp = 1; reg = (inst >> 7) & 31;
    } else if (op == 0x27 && (f3 == 2 || f3 == 3)) {   // FSW FSD
      load = 0; n = f3 == 2 ? 4 : 8; fp = 1; reg = (inst >> 20) & 31;
    } else {
      return 1;
    }
  } else {
    uint32 q = inst & 3, c3 = (inst >> 13) & 7;
    int rdp = 8 + ((inst >> 2) & 7);
    if (q == 0 && c3 == 1)      { load = 1; n = 8; fp = 1; reg = rdp; }               // C.FLD
    else if (q == 0 && c3 == 2) { load = 1; n = 4; sign = 1; reg = rdp; }             // C.LW
    else if (q == 0 && c3 == 3) { load = 1; n = 8; reg = rdp; }                       // C.LD
    else if (q == 0 && c3 == 5) { load = 0; n = 8; fp = 1; reg = rdp; }               // C.FSD
    else if (q == 0 && c3 == 6) { load = 0; n = 4; reg = rdp; }                       // C.SW
    else if (q == 0 && c3 == 7) { load = 0; n = 8; reg = rdp; }                       // C.SD
    else if (q == 2 && c3 == 1) { load = 1; n = 8; fp = 1; reg = (inst >> 7) & 31; }  // C.FLDSP
    else if (q == 2 && c3 == 2) { load = 1; n = 4; sign = 1; reg = (inst >> 7) & 31; }// C.LWSP
    else if (q == 2 && c3 == 3) { load = 1; n = 8; reg = (inst >> 7) & 31; }          // C.LDSP
    else if (q == 2 && c3 == 5) { load = 0; n = 8; fp = 1; reg = (inst >> 2) & 31; }  // C.FSDSP
    else if (q == 2 && c3 == 6) { load = 0; n = 4; reg = (inst >> 2) & 31; }          // C.SWSP
    else if (q == 2 && c3 == 7) { load = 0; n = 8; reg = (inst >> 2) & 31; }          // C.SDSP
    else return 1;
  }
  if (load != (cause == 4))
    return 1;
  if (load) {
    uint64 v = 0;
    for (int i = 0; i < n; i++) {
      v |= mprv_lbu(tval + i, &fault) << (8 * i);
      if (fault) {
        w_mstatus(mstatus);
        redirect(fault, tval + i, epc, mstatus);
        return 0;
      }
    }
    if (sign && n < 8 && (v >> (8 * n - 1)) & 1)
      v |= ~0UL << (8 * n);
    if (fp)
      freg_set(reg, v, n == 8);
    else if (reg != 0)
      r[reg] = v;
  } else {
    uint64 v = fp ? freg_get(reg, n == 8) : r[reg];
    for (int i = 0; i < n; i++) {
      mprv_sb(tval + i, (v >> (8 * i)) & 0xff, &fault);
      if (fault) {
        w_mstatus(mstatus);
        redirect(fault, tval + i, epc, mstatus);
        return 0;
      }
    }
  }
  w_mstatus(mstatus);           // the byte accesses' guard traps disturbed MPP/MPV
  w_mepc(epc + len);
  mtrap_misaligned++;
  return 0;
}

__attribute__((no_stack_protector)) void
mtrap(uint64 *r)
{
  uint64 cause = r_mcause(), epc = r_mepc(), tval = r_mtval(), mstatus = r_mstatus();

  if ((cause == 4 || cause == 6) && misaligned(r, cause, epc, tval, mstatus) == 0)
    return;
  mputs("\nmachine: unexpected trap: mcause ");
  mputx(cause);
  mputs(" mepc ");
  mputx(epc);
  mputs(" mtval ");
  mputx(tval);
  mputs(" mstatus ");
  mputx(mstatus);
  mputs(" a7 ");
  mputx(r[17]);
  mputs("\n");
  for (;;)
    asm volatile("wfi");
}
