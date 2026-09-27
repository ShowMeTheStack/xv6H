//
// emulation of the guest's machine-mode CSRs and privileged
// instructions, virtual-instruction exceptions, and the tiny SBI.
//
// the guest kernel is the same xv6 binary as the host; its entry.S and
// start() believe they run in machine mode. every machine-mode CSR
// access and the final mret trap here as illegal instructions (cause 2),
// and we emulate just enough of machine mode for that boot code.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "hyp.h"

// fetch a 16- or 32-bit instruction at guest virtual address gva using
// the guest's own translation (hlvx.hu: VS-level, execute permission).
uint32
hyp_fetch(struct vcpu *v, uint64 gva)
{
  uint64 lo, hi;
  asm volatile("hlvx.hu %0, (%1)" : "=r"(lo) : "r"(gva));
  if ((lo & 3) != 3)
    return (uint32)lo; // compressed
  asm volatile("hlvx.hu %0, (%1)" : "=r"(hi) : "r"(gva + 2));
  return (uint32)(lo | (hi << 16));
}

#define CSR_MSTATUS    0x300
#define CSR_MISA       0x301
#define CSR_MEDELEG    0x302
#define CSR_MIDELEG    0x303
#define CSR_MIE        0x304
#define CSR_MTVEC      0x305
#define CSR_MCOUNTEREN 0x306
#define CSR_MENVCFG    0x30a
#define CSR_MSCRATCH   0x340
#define CSR_MEPC       0x341
#define CSR_MCAUSE     0x342
#define CSR_MTVAL      0x343
#define CSR_MIP        0x344
#define CSR_PMPCFG0    0x3a0
#define CSR_PMPADDR0   0x3b0
#define CSR_MVENDORID  0xf11
#define CSR_MARCHID    0xf12
#define CSR_MIMPID     0xf13
#define CSR_MHARTID    0xf14

static int
mcsr_read(struct vcpu *v, int csr, uint64 *val)
{
  // which of these a guest asks for is what decides where it can still be
  // migrated to, so the asking is recorded (see OBS_* in hyp.h)
  switch (csr) {
  case 0xc01:   // time: only trapped when this machine's clock is scaled (F6)
    *val = vm_guest_time(v->vm);
    return 0;
  case 0x14d:   // stimecmp on a machine without Sstc (F5/F6): the same deadline,
                // emulated, so the window between the host's change and the
                // guest's adoption costs a trap and not a panic
    *val = vm_guest_time_of(v->vm, v->vstimecmp);
    return 0;
  case CSR_MHARTID:
    vm_observe(v->vm, OBS_MHARTID);
    *val = v->id + v->vm->hart_shift;    // F9
    return 0;
  case CSR_MISA:
    vm_observe(v->vm, OBS_MISA);
    // bit 7 is H: no nested virtualization. bit 21 is V, and it is masked out
    // for a machine whose vector unit has been revoked -- which is how a guest
    // is told the unit is gone, because has_v is computed from misa
    // (platform.c). A guest that re-reads misa after a revision sees it.
    *val = plat.misa & ~(1L << 7) & ~(v->vm->vec_revoked ? (1L << 21) : 0);
    return 0;
  case CSR_MSTATUS:
    *val = (v->m_mstatus & ~(SSTATUS_FS_MASK | SSTATUS_VS_MASK)) |
           (v->vsstatus & (SSTATUS_FS_MASK | SSTATUS_VS_MASK));
    return 0;
  case CSR_MEDELEG:    *val = v->m_medeleg; return 0;
  case CSR_MIDELEG:    *val = v->m_mideleg; return 0;
  case CSR_MIE:        *val = v->m_mie; return 0;
  case CSR_MTVEC:      *val = v->m_mtvec; return 0;
  case CSR_MCOUNTEREN: *val = v->m_mcounteren; return 0;
  case CSR_MENVCFG:    vm_observe(v->vm, OBS_MENVCFG); *val = v->m_menvcfg; return 0;
  case CSR_MSCRATCH:   *val = v->m_mscratch; return 0;
  case CSR_MEPC:       *val = v->m_mepc; return 0;
  case CSR_PMPCFG0:    *val = v->m_pmpcfg0; return 0;
  case CSR_PMPADDR0:   *val = v->m_pmpaddr0; return 0;
  case CSR_MCAUSE:
  case CSR_MTVAL:
  case CSR_MIP:
  case CSR_MVENDORID:
  case CSR_MARCHID:
  case CSR_MIMPID:
    vm_observe(v->vm, OBS_MVENDOR); *val = 0;
    return 0;
  }
  return -1;
}

static int
mcsr_write(struct vcpu *v, int csr, uint64 val)
{
  switch (csr) {
  case 0x14d:   // stimecmp without Sstc (F5/F6): lands where an SBI set_timer would
    v->vstimecmp = vm_host_time_of(v->vm, val);
    timer_rearm();          // the host's timer to the guest's deadline (see hypsbi.c SBI_EXT_TIME)
    return 0;
  case CSR_MSTATUS:
    v->m_mstatus = val;
    // the FS and VS fields are the real ones: reflect them in vsstatus.
    v->vsstatus = (v->vsstatus & ~(SSTATUS_FS_MASK | SSTATUS_VS_MASK)) |
                  (val & (SSTATUS_FS_MASK | SSTATUS_VS_MASK));
    return 0;
  case CSR_MEDELEG:    v->m_medeleg = val; return 0;
  case CSR_MIDELEG:    v->m_mideleg = val; return 0;
  case CSR_MIE:        v->m_mie = val; return 0;
  case CSR_MTVEC:      v->m_mtvec = val; return 0;
  case CSR_MCOUNTEREN: v->m_mcounteren = val; return 0;
  case CSR_MENVCFG:    v->m_menvcfg = val; return 0;
  case CSR_MSCRATCH:   v->m_mscratch = val; return 0;
  case CSR_MEPC:       v->m_mepc = val; return 0;
  case CSR_PMPCFG0:    v->m_pmpcfg0 = val; return 0;
  case CSR_PMPADDR0:   v->m_pmpaddr0 = val; return 0;
  case CSR_MISA:
  case CSR_MCAUSE:
  case CSR_MTVAL:
  case CSR_MIP:
    return 0; // ignored
  }
  return -1;
}

// generic CSR read-modify-write emulation over rd/wr callbacks.
// returns 0 if handled, -1 if the CSR is unknown.
static int
csr_rmw(struct vcpu *v, uint32 inst, int (*rd)(struct vcpu *, int, uint64 *),
        int (*wr)(struct vcpu *, int, uint64))
{
  int funct3 = (inst >> 12) & 7;
  int csr = inst >> 20;
  int rs1 = (inst >> 15) & 31;
  int rdn = (inst >> 7) & 31;
  uint64 old, src, nw;
  int dowrite;

  if (rd(v, csr, &old) < 0)
    return -1;
  src = (funct3 & 4) ? (uint64)rs1 : v->regs[rs1];
  switch (funct3 & 3) {
  case 1: nw = src; dowrite = 1; break;                  // csrrw
  case 2: nw = old | src; dowrite = (rs1 != 0); break;   // csrrs
  case 3: nw = old & ~src; dowrite = (rs1 != 0); break;  // csrrc
  default: return -1;
  }
  if (dowrite && wr(v, csr, nw) < 0)
    return -1;
  if (rdn != 0)
    v->regs[rdn] = old;
  v->pc += 4;
  v->csr_emul++;
  return 0;
}

static void
emulate_mret(struct vcpu *v)
{
  uint64 ms = v->m_mstatus;
  int mpp = (ms >> 11) & 3;
  v->pc = v->m_mepc;
  v->priv = (mpp == 0) ? 0 : 1; // U -> VU, S or M -> VS
  v->in_mmode = (mpp == 3);
  // mret: MIE <- MPIE, MPIE <- 1, MPP <- U
  ms &= ~(3L << 11);
  if (ms & (1L << 7))
    ms |= (1L << 3);
  else
    ms &= ~(1L << 3);
  ms |= (1L << 7);
  v->m_mstatus = ms;
}

// illegal instruction from the guest: a machine-mode CSR access, mret,
// or a genuinely illegal instruction to hand back to the guest.
// OP-V, or a vector load/store: LOAD-FP/STORE-FP with a width the scalar FP
// instructions do not use (0, 5, 6, 7 are vector; 2, 3, 4 are flw/fld/flq).
static int
is_vector_inst(uint32 inst)
{
  uint32 op = inst & 0x7f, width = (inst >> 12) & 7;
  if (op == 0x57)
    return 1;
  if ((op == 0x07 || op == 0x27) && (width == 0 || width >= 5))
    return 1;
  return 0;
}

int
hyp_illegal(struct vcpu *v, uint32 inst)
{
  if ((inst & 0x7f) == 0x73) {
    int funct3 = (inst >> 12) & 7;
    if (funct3 == 0) {
      if (inst == 0x30200073) { // mret
        emulate_mret(v);
        v->csr_emul++;
        return 0;
      }
    } else {
      int csr = inst >> 20;
      // AIA indirect CSRs: qemu reports some VS-mode accesses with
      // VGEIN=0 as illegal rather than virtual instructions.
      // ... and stopi/vstopi (0xdb0/0xeb0), which do not exist at all on a host
      // without the AIA CSRs and are answered by the software model.
      if (csr == 0x150 || csr == 0x151 || csr == 0x15c || csr == 0x250 || csr == 0x251 || csr == 0x25c ||
          csr == 0xdb0 || csr == 0xeb0) {
        if (vic_csr(v, inst) == 0)
          return 0;
      } else if (csr_rmw(v, inst, mcsr_read, mcsr_write) == 0) {
        return 0;
      }
    }
  }
  // A vector instruction from a guest whose unit is hidden: this is the one
  // exact observation of use the host ever gets (Proposition 4 says sampling
  // cannot give it; a trap on first use can). Grant the unit, tell the guest,
  // and leave the pc where it is: the guest takes the revision on entry, adopts,
  // returns to the instruction, which now runs -- or traps again as a plain
  // illegal instruction into the guest if that task has no save area yet, which
  // the guest answers by allocating one (trap.c).
  if (is_vector_inst(inst) && v->vm->lazy_v && v->vm->vec_revoked && plat.has_v) {
    vm_observe(v->vm, OBS_VECTOR_USED);
    printk("hyp: vm %d: first vector instruction (0x%x): granting the unit\n",
           v->vm->id, inst);
    if (vm_revise_vector(v->vm, 1) == 0)
      return 0;                     // pc unchanged: re-execute after adoption
  }
  // not ours: the guest gets it as an illegal instruction exception.
  vcpu_inject(v, EXC_ILLEGAL, inst);
  return 0;
}

// virtual instruction exception: an instruction that would be legal in
// HS mode but not in VS mode (wfi is handled by the caller).
int
hyp_virtinst(struct vcpu *v, uint32 inst)
{
  // F21: cbo.zero, trapped because the block the guest is described with is
  // not the host's (henvcfg.CBZE off): clear the described block, in the
  // guest's own address space (hsv.d, VS-level translation)
  if ((inst & 0xfff07fff) == 0x0040200f) {
    struct vm *vm = v->vm;
    int n = vm->cboz_block;
    if (vm->revise_acked != vm->revise_gen && vm->cboz_prev > n)
      n = vm->cboz_prev;   // a revision between two sizes is pending: the larger
    uint64 gva = v->regs[(inst >> 15) & 31] & ~((uint64)n - 1);
    for (uint64 a = gva; a < gva + n; a += 8)
      asm volatile(".insn r 0x73, 0x4, 0x37, x0, %0, %1" ::"r"(a), "r"(0UL) : "memory");   // hsv.d x0, (a)
    __atomic_fetch_add(&hyp_cboz_exits, 1, __ATOMIC_RELAXED);
    v->pc += 4;
    return 0;
  }
  if ((inst & 0x7f) == 0x73 && ((inst >> 12) & 3) != 0) {
    int csr = inst >> 20;
    // F6: `time' read by a machine whose clock is described at another rate
    // (hcounteren.TM is off for it): answered at that rate, continuous
    if ((csr == 0xc01 || csr == 0x14d) && csr_rmw(v, inst, mcsr_read, mcsr_write) == 0)
      return 0;
    if (vic_csr(v, inst) == 0)
      return 0;
  }
  vcpu_inject(v, EXC_ILLEGAL, inst);
  return 0;
}

