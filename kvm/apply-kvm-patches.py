#!/usr/bin/env python3
# (runs on the build box): the KVM changes for revisable machine descriptions, applied to
# ~/xv6h-kvm/linux-6.18.54 idempotently. Each change is one function below; `git diff` against
# the pristine tree (kept in ~/xv6h-kvm/linux-6.18.54.orig) is the patch series we report.
import os, re, sys
T = os.path.expanduser(os.environ.get("KT", "~/xv6h-kvm/linux-6.18.54"))   # KT: another tree
def sub(rel, pairs):
    p = os.path.join(T, rel); s = open(p).read(); o = s
    for a, b in pairs:
        if b in s: continue                     # already applied
        n = s.count(a); assert n == 1, (rel, a[:60], n)
        s = s.replace(a, b)
    if s != o: open(p, "w").write(s); print("patched", rel)

# 1. the MACH extension (0x4D414348) is forwarded to userspace, like the experimental and vendor
#    ranges; disabled by default, enabled by a VMM that implements it (KVM_REG_RISCV_SBI_EXT).
sub("arch/riscv/include/uapi/asm/kvm.h", [(
"\tKVM_RISCV_SBI_EXT_FWFT,\n\tKVM_RISCV_SBI_EXT_MAX,",
"\tKVM_RISCV_SBI_EXT_FWFT,\n\tKVM_RISCV_SBI_EXT_MACH,\t/* revisable machine descriptions: forwarded to the VMM */\n\tKVM_RISCV_SBI_EXT_MAX,")])
sub("arch/riscv/include/asm/kvm_vcpu_sbi.h", [(
"extern const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_vendor;",
"extern const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_vendor;\nextern const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_mach;")])
sub("arch/riscv/kvm/vcpu_sbi_base.c", [(
"const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_vendor = {\n\t.extid_start = SBI_EXT_VENDOR_START,\n\t.extid_end = SBI_EXT_VENDOR_END,\n\t.handler = kvm_sbi_ext_forward_handler,\n};",
"const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_vendor = {\n\t.extid_start = SBI_EXT_VENDOR_START,\n\t.extid_end = SBI_EXT_VENDOR_END,\n\t.handler = kvm_sbi_ext_forward_handler,\n};\n\n"
"/*\n * The machine-description revision extension (\"MACH\"): every call is the VMM's, which owns\n * the description, its slots and the generation. Off until the VMM enables it.\n */\n"
"#define SBI_EXT_MACH 0x4D414348\n"
"const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_mach = {\n\t.extid_start = SBI_EXT_MACH,\n\t.extid_end = SBI_EXT_MACH,\n\t.default_disabled = true,\n\t.handler = kvm_sbi_ext_forward_handler,\n};")])
sub("arch/riscv/kvm/vcpu_sbi.c", [(
"\t{\n\t\t.ext_idx = KVM_RISCV_SBI_EXT_VENDOR,\n\t\t.ext_ptr = &vcpu_sbi_ext_vendor,\n\t},\n};",
"\t{\n\t\t.ext_idx = KVM_RISCV_SBI_EXT_VENDOR,\n\t\t.ext_ptr = &vcpu_sbi_ext_vendor,\n\t},\n\t{\n\t\t.ext_idx = KVM_RISCV_SBI_EXT_MACH,\n\t\t.ext_ptr = &vcpu_sbi_ext_mach,\n\t},\n};")])
# 2. F6: the guest's timebase revisable under a running guest (the timer's `frequency' ONE_REG,
#    read-only until now, is writable). The counter is continuous: anchored (x0, g0) where it
#    is and run at the new rate from there. A scaled guest reads `time' by trap (hcounteren.TM
#    off; emulated in vcpu_insn.c), and has no hardware comparator (vstimecmp counts at the
#    host's rate), so its deadlines go through the hrtimer in its own units; back at the host's
#    rate the offset is folded into htimedelta and Sstc is used again. stimecmp is emulated as
#    well for a scaled guest that has not yet left Sstc (the window before its ack).
sub("arch/riscv/include/asm/kvm_vcpu_timer.h", [
 ("\t/* Time delta value */\n\tu64 time_delta;\n};",
  "\t/* Time delta value */\n\tu64 time_delta;\n"
  "\t/* F6: the guest's rate (0: the host's) and the anchor of the last change */\n"
  "\tu64 scale_hz;\n\tu64 x0, g0;\n\tspinlock_t scale_lock;\n};"),
 ("bool kvm_riscv_vcpu_timer_pending(struct kvm_vcpu *vcpu);",
  "bool kvm_riscv_vcpu_timer_pending(struct kvm_vcpu *vcpu);\n"
  "u64 kvm_riscv_vcpu_timer_guest_time(struct kvm_vcpu *vcpu);\n"
  "bool kvm_riscv_vcpu_timer_scaled(struct kvm_vcpu *vcpu);")])
sub("arch/riscv/kvm/vcpu_timer.c", [
 ("#include <linux/uaccess.h>\n", "#include <linux/uaccess.h>\n#include <linux/math64.h>\n"),
 ("static u64 kvm_riscv_current_cycles(struct kvm_guest_timer *gt)\n{\n\treturn get_cycles64() + gt->time_delta;\n}",
  "static u64 kvm_riscv_host_cycles(struct kvm_guest_timer *gt)\n{\n\treturn get_cycles64() + gt->time_delta;\n}\n\n"
  "/* the guest's counter: the host's plus the delta, or at its own rate from the anchor (F6) */\n"
  "static u64 kvm_riscv_current_cycles(struct kvm_guest_timer *gt)\n{\n"
  "\tu64 x = kvm_riscv_host_cycles(gt), hz = READ_ONCE(gt->scale_hz);\n\n"
  "\tif (!hz)\n\t\treturn x;\n"
  "\treturn gt->g0 + mul_u64_u64_div_u64(x - gt->x0, hz, riscv_timebase);\n}\n\n"
  "u64 kvm_riscv_vcpu_timer_guest_time(struct kvm_vcpu *vcpu)\n{\n"
  "\treturn kvm_riscv_current_cycles(&vcpu->kvm->arch.timer);\n}\n\n"
  "bool kvm_riscv_vcpu_timer_scaled(struct kvm_vcpu *vcpu)\n{\n"
  "\treturn READ_ONCE(vcpu->kvm->arch.timer.scale_hz) != 0;\n}\n\n"
  "/* Sstc's comparator serves this vcpu: available, and the clock at the host's rate */\n"
  "static bool kvm_riscv_vcpu_use_sstc(struct kvm_vcpu *vcpu)\n{\n"
  "\treturn vcpu->arch.timer.sstc_enabled && !kvm_riscv_vcpu_timer_scaled(vcpu);\n}"),
 ("\tdelta_ns = (cycles_delta * gt->nsec_mult) >> gt->nsec_shift;\n\tlocal_irq_restore(flags);",
  "\tif (READ_ONCE(gt->scale_hz))\t/* the guest's cycles in the host's */\n"
  "\t\tcycles_delta = mul_u64_u64_div_u64(cycles_delta, riscv_timebase, gt->scale_hz);\n"
  "\tdelta_ns = (cycles_delta * gt->nsec_mult) >> gt->nsec_shift;\n\tlocal_irq_restore(flags);"),
 ("\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\n\treturn t->timer_next_event(vcpu, ncycles);",
  "\tif (kvm_riscv_vcpu_use_sstc(vcpu))\n\t\treturn kvm_riscv_vcpu_update_vstimecmp(vcpu, ncycles);\n"
  "\treturn kvm_riscv_vcpu_update_hrtimer(vcpu, ncycles);"),
 ("\tt->next_set = false;\n\tkvm_vcpu_kick(vcpu);\n\n\treturn HRTIMER_NORESTART;",
  "\tt->next_set = false;\n\tif (kvm_riscv_vcpu_timer_scaled(vcpu))\n"
  "\t\tkvm_riscv_vcpu_set_interrupt(vcpu, IRQ_VS_TIMER);\t/* no comparator: the timer is ours */\n"
  "\telse\n\t\tkvm_vcpu_kick(vcpu);\n\n\treturn HRTIMER_NORESTART;"),
 ("\tcase KVM_REG_RISCV_TIMER_REG(frequency):\n\t\treg_val = riscv_timebase;",
  "\tcase KVM_REG_RISCV_TIMER_REG(frequency):\n\t\treg_val = gt->scale_hz ? gt->scale_hz : riscv_timebase;"),
 ("\tcase KVM_REG_RISCV_TIMER_REG(frequency):\n\t\tif (reg_val != riscv_timebase)\n\t\t\treturn -EINVAL;\n\t\tbreak;\n\tcase KVM_REG_RISCV_TIMER_REG(time):\n\t\tgt->time_delta = reg_val - get_cycles64();",
  "\tcase KVM_REG_RISCV_TIMER_REG(frequency):\n\t\tret = kvm_riscv_vcpu_timer_rescale(vcpu, reg_val);\n\t\tbreak;\n"
  "\tcase KVM_REG_RISCV_TIMER_REG(time):\n\t\tif (gt->scale_hz) {\n\t\t\tgt->x0 = kvm_riscv_host_cycles(gt);\n\t\t\tgt->g0 = reg_val;\n\t\t} else {\n"
  "\t\t\tgt->time_delta = reg_val - get_cycles64();\n\t\t}"),
 ("int kvm_riscv_vcpu_set_reg_timer(struct kvm_vcpu *vcpu,\n\t\t\t\t const struct kvm_one_reg *reg)\n{",
  "/*\n * F6: a new rate for the guest's counter, set on each vcpu from its own thread. The first call\n"
  " * moves the VM's clock (continuously); every call moves that vcpu's deadline to the mode its\n"
  " * clock now has: the hrtimer, in the guest's units, while scaled; vstimecmp at the host's rate.\n */\n"
  "static int kvm_riscv_vcpu_timer_rescale(struct kvm_vcpu *vcpu, u64 hz)\n{\n"
  "\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\tstruct kvm_guest_timer *gt = &vcpu->kvm->arch.timer;\n"
  "\tu64 want = hz == riscv_timebase ? 0 : hz, g, x;\n\tunsigned long flags;\n\n"
  "\tif (!hz)\n\t\treturn -EINVAL;\n"
  "\tspin_lock_irqsave(&gt->scale_lock, flags);\n"
  "\tif (want != gt->scale_hz) {\n\t\tg = kvm_riscv_current_cycles(gt);\n\t\tx = kvm_riscv_host_cycles(gt);\n"
  "\t\tif (!want) {\n\t\t\tgt->time_delta += g - x;\t/* the host's rate again, from where the guest is */\n"
  "\t\t} else {\n\t\t\tgt->x0 = x;\n\t\t\tgt->g0 = g;\n\t\t}\n\t\tWRITE_ONCE(gt->scale_hz, want);\n\t}\n"
  "\tspin_unlock_irqrestore(&gt->scale_lock, flags);\n\n"
  "\tif (!t->init_done)\n\t\treturn 0;\n"
  "\tif (kvm_riscv_vcpu_use_sstc(vcpu)) {\n\t\t/* the deadline goes back into vstimecmp at the next restore */\n"
  "\t\thrtimer_cancel(&t->hrt);\n\t\tt->next_set = false;\n\t\tkvm_riscv_vcpu_unset_interrupt(vcpu, IRQ_VS_TIMER);\n"
  "\t} else if (t->next_cycles != -1ULL) {\n\t\tkvm_riscv_vcpu_update_hrtimer(vcpu, t->next_cycles);\n\t}\n\treturn 0;\n}\n\n"
  "int kvm_riscv_vcpu_set_reg_timer(struct kvm_vcpu *vcpu,\n\t\t\t\t const struct kvm_one_reg *reg)\n{"),
 ("\tkvm_riscv_vcpu_update_timedelta(vcpu);\n\n\tif (!t->sstc_enabled)\n\t\treturn;",
  "\tkvm_riscv_vcpu_update_timedelta(vcpu);\n"
  "\t/* F6: a scaled guest reads `time' by trap, at its own rate (vcpu_insn.c) */\n"
  "\tncsr_write(CSR_HCOUNTEREN, kvm_riscv_vcpu_timer_scaled(vcpu) ? 0 : 0x02);\n\n"
  "\tif (!kvm_riscv_vcpu_use_sstc(vcpu)) {\n\t\tif (t->sstc_enabled)\n\t\t\tncsr_write(CSR_VSTIMECMP, -1UL);\t/* no hardware deadline while scaled */\n\t\treturn;\n\t}"),
 ("void kvm_riscv_vcpu_timer_sync(struct kvm_vcpu *vcpu)\n{\n\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\n\tif (!t->sstc_enabled)\n\t\treturn;",
  "void kvm_riscv_vcpu_timer_sync(struct kvm_vcpu *vcpu)\n{\n\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\n\tif (!kvm_riscv_vcpu_use_sstc(vcpu))\n\t\treturn;"),
 ("void kvm_riscv_vcpu_timer_save(struct kvm_vcpu *vcpu)\n{\n\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\n\tif (!t->sstc_enabled)\n\t\treturn;",
  "void kvm_riscv_vcpu_timer_save(struct kvm_vcpu *vcpu)\n{\n\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\n\tif (!kvm_riscv_vcpu_use_sstc(vcpu))\n\t\treturn;"),
 ("\triscv_cs_get_mult_shift(&gt->nsec_mult, &gt->nsec_shift);\n\tgt->time_delta = -get_cycles64();",
  "\triscv_cs_get_mult_shift(&gt->nsec_mult, &gt->nsec_shift);\n\tgt->time_delta = -get_cycles64();\n\tspin_lock_init(&gt->scale_lock);"),
])
sub("arch/riscv/kvm/vcpu_insn.c", [
 ("static const struct csr_func csr_funcs[] = {\n\tKVM_RISCV_VCPU_AIA_CSR_FUNCS",
  "/* F6: `time' of a guest whose counter runs at another rate (hcounteren.TM off for it) */\n"
  "static int time_csr_rmw(struct kvm_vcpu *vcpu, unsigned int csr_num,\n\t\t\tunsigned long *val, unsigned long new_val,\n\t\t\tunsigned long wr_mask)\n{\n"
  "\tif (wr_mask)\n\t\treturn KVM_INSN_ILLEGAL_TRAP;\n\t*val = kvm_riscv_vcpu_timer_guest_time(vcpu);\n\treturn KVM_INSN_CONTINUE_NEXT_SEPC;\n}\n\n"
  "/* ... and its stimecmp whenever the access traps for a guest that has Sstc: henvcfg.STCE is off\n"
  " * while scaled, and stays off on a vcpu until it next enters after the rate is the host's again\n"
  " * (the guest re-rates every cpu's device inside one stop_machine, from vcpus that have not yet\n"
  " * re-entered). The same deadline in its own units: the window costs a trap and not a panic. */\n"
  "static int stimecmp_csr_rmw(struct kvm_vcpu *vcpu, unsigned int csr_num,\n\t\t\t    unsigned long *val, unsigned long new_val,\n\t\t\t    unsigned long wr_mask)\n{\n"
  "\tstruct kvm_vcpu_timer *t = &vcpu->arch.timer;\n\n"
  "\tif (!riscv_isa_extension_available(vcpu->arch.isa, SSTC))\n\t\treturn KVM_INSN_ILLEGAL_TRAP;\n"
  "\t*val = t->next_cycles;\n\tif (wr_mask)\n\t\tkvm_riscv_vcpu_timer_next_event(vcpu, (t->next_cycles & ~wr_mask) | (new_val & wr_mask));\n"
  "\treturn KVM_INSN_CONTINUE_NEXT_SEPC;\n}\n\n"
  "static const struct csr_func csr_funcs[] = {\n\tKVM_RISCV_VCPU_AIA_CSR_FUNCS\n"
  "\t{ .base = CSR_TIME, .count = 1, .func = time_csr_rmw },\n"
  "\t{ .base = CSR_STIMECMP, .count = 1, .func = stimecmp_csr_rmw },"),
])
# henvcfg: no Sstc for a scaled guest (its stimecmp traps and is emulated above)
sub("arch/riscv/kvm/vcpu.c", [
 ("\t\tnacl_csr_write(nsh, CSR_HENVCFG, cfg->henvcfg);\n",
  "\t\tnacl_csr_write(nsh, CSR_HENVCFG, cfg->henvcfg & ~(kvm_riscv_vcpu_timer_scaled(vcpu) ? ENVCFG_STCE : 0));\n"),
 ("\t\tcsr_write(CSR_HENVCFG, cfg->henvcfg);\n",
  "\t\tcsr_write(CSR_HENVCFG, cfg->henvcfg & ~(kvm_riscv_vcpu_timer_scaled(vcpu) ? ENVCFG_STCE : 0));\n"),
])

# 3. F10: the vector unit revisable under a running guest. The V extension's ISA register may be
#    changed after the vcpu has run (today: -EBUSY). Off: the vcpu's sstatus.VS is left Off on
#    every entry, so a vector instruction in the guest is illegal (with V=1 both vsstatus.VS and
#    the hypervisor's sstatus.VS must be on), and its state is no longer saved; the VMM turns it
#    off only after the guest has ceded the unit (its ack). On: the unit comes back clean
#    (VS Initial). F11, another width, is not offered: the hardware's VLEN is fixed.
sub("arch/riscv/kvm/vcpu_onereg.c", [
 ("\t\tkvm_riscv_vcpu_fp_reset(vcpu);\n\t} else {\n\t\treturn -EBUSY;\n\t}\n\n\treturn 0;\n}",
  "\t\tkvm_riscv_vcpu_fp_reset(vcpu);\n\t} else if (reg_num == KVM_RISCV_ISA_EXT_V && riscv_isa_extension_available(NULL, v)) {\n"
  "\t\tstruct kvm_cpu_context *cntx = &vcpu->arch.guest_context;\n\n"
  "\t\tif (reg_val) {\n\t\t\tset_bit(guest_ext, vcpu->arch.isa);\n"
  "\t\t\tcntx->sstatus = (cntx->sstatus & ~SR_VS) | SR_VS_INITIAL;\t/* back, clean */\n"
  "\t\t} else {\n\t\t\tclear_bit(guest_ext, vcpu->arch.isa);\n"
  "\t\t\tcntx->sstatus &= ~SR_VS;\t/* Off: a vector instruction is illegal in the guest */\n\t\t}\n"
  "\t} else {\n\t\treturn -EBUSY;\n\t}\n\n\treturn 0;\n}"),
])

print("kvm patches applied")

# 4. the in-kernel AIA reconfigured under a running guest: the VMM may
#    move every vCPU's IMSIC from its hardware guest file to the software file and back, with
#    nothing told to the guest (as F23 under xv6H). KVM already moves a vCPU's IMSIC state between
#    a hardware file and the software one when no hardware file is free (AUTO mode); this lets
#    the VMM ask for it. Only in AUTO mode: HWACCEL has no software file, EMUL no hardware one.
sub("arch/riscv/include/uapi/asm/kvm.h", [(
"#define KVM_DEV_RISCV_AIA_CTRL_INIT\t\t0\n",
"#define KVM_DEV_RISCV_AIA_CTRL_INIT\t\t0\n#define KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW\t\t1\t/* u32: 1 software files, 0 hardware */\n")])
sub("arch/riscv/include/asm/kvm_aia.h", [(
"\t/* Internal state of APLIC */\n\tvoid\t\t*aplic_state;\n};",
"\t/* Internal state of APLIC */\n\tvoid\t\t*aplic_state;\n\n\t/* The VMM asked for software IMSIC files (KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW) */\n\tbool\t\timsic_sw;\n};")])
sub("arch/riscv/kvm/aia_imsic.c", [(
"\t/* Do nothing if we are continuing on same CPU */\n\tif (old_vsfile_cpu == vcpu->cpu)\n\t\treturn 1;",
"\t/*\n\t * The VMM asked for the software file: give the hardware file back,\n\t * its state moved to the software file, and run without one. When it\n\t * asks for hardware again, the code below allocates a file as it does\n\t * when a vCPU moves to another CPU.\n\t */\n\tif (READ_ONCE(kvm->arch.aia.imsic_sw)) {\n\t\tif (old_vsfile_cpu >= 0)\n\t\t\tkvm_riscv_vcpu_aia_imsic_release(vcpu);\n\t\tgoto done;\n\t}\n\n\t/* Do nothing if we are continuing on same CPU */\n\tif (old_vsfile_cpu == vcpu->cpu)\n\t\treturn 1;")])
sub("arch/riscv/kvm/aia_device.c", [(
"\tcase KVM_DEV_RISCV_AIA_GRP_CTRL:\n\t\tswitch (type) {\n\t\tcase KVM_DEV_RISCV_AIA_CTRL_INIT:\n\t\t\tmutex_lock(&dev->kvm->lock);\n\t\t\tr = aia_init(dev->kvm);\n\t\t\tmutex_unlock(&dev->kvm->lock);\n\t\t\tbreak;\n\t\t}",
"\tcase KVM_DEV_RISCV_AIA_GRP_CTRL:\n\t\tswitch (type) {\n\t\tcase KVM_DEV_RISCV_AIA_CTRL_INIT:\n\t\t\tmutex_lock(&dev->kvm->lock);\n\t\t\tr = aia_init(dev->kvm);\n\t\t\tmutex_unlock(&dev->kvm->lock);\n\t\t\tbreak;\n\t\tcase KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW: {\n\t\t\tstruct kvm_vcpu *vcpu;\n\t\t\tunsigned long i;\n\n\t\t\tif (copy_from_user(&nr, uaddr, sizeof(nr)))\n\t\t\t\treturn -EFAULT;\n\t\t\tif (!dev->kvm->arch.aia.initialized ||\n\t\t\t    dev->kvm->arch.aia.mode != KVM_DEV_RISCV_AIA_MODE_AUTO) {\n\t\t\t\tr = -EINVAL;\n\t\t\t\tbreak;\n\t\t\t}\n\t\t\tWRITE_ONCE(dev->kvm->arch.aia.imsic_sw, !!nr);\n\t\t\t/* every vCPU re-evaluates its file before it next enters */\n\t\t\tkvm_for_each_vcpu(i, vcpu, dev->kvm)\n\t\t\t\tkvm_vcpu_kick(vcpu);\n\t\t\tr = 0;\n\t\t\tbreak;\n\t\t}\n\t\t}"),
("\tcase KVM_DEV_RISCV_AIA_GRP_CTRL:\n\t\tswitch (attr->attr) {\n\t\tcase KVM_DEV_RISCV_AIA_CTRL_INIT:\n\t\t\treturn 0;\n\t\t}",
 "\tcase KVM_DEV_RISCV_AIA_GRP_CTRL:\n\t\tswitch (attr->attr) {\n\t\tcase KVM_DEV_RISCV_AIA_CTRL_INIT:\n\t\tcase KVM_DEV_RISCV_AIA_CTRL_IMSIC_SW:\n\t\t\treturn 0;\n\t\t}")])
