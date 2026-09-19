        #
        # floating-point register save/restore for the hypervisor:
        #   fp_save(uint64 *area)      f0..f31 (8 bytes each)
        #   fp_restore(uint64 *area)
        # sstatus.FS must be on.
        #
.globl fp_save
fp_save:
        fsd f0, 0(a0)
        fsd f1, 8(a0)
        fsd f2, 16(a0)
        fsd f3, 24(a0)
        fsd f4, 32(a0)
        fsd f5, 40(a0)
        fsd f6, 48(a0)
        fsd f7, 56(a0)
        fsd f8, 64(a0)
        fsd f9, 72(a0)
        fsd f10, 80(a0)
        fsd f11, 88(a0)
        fsd f12, 96(a0)
        fsd f13, 104(a0)
        fsd f14, 112(a0)
        fsd f15, 120(a0)
        fsd f16, 128(a0)
        fsd f17, 136(a0)
        fsd f18, 144(a0)
        fsd f19, 152(a0)
        fsd f20, 160(a0)
        fsd f21, 168(a0)
        fsd f22, 176(a0)
        fsd f23, 184(a0)
        fsd f24, 192(a0)
        fsd f25, 200(a0)
        fsd f26, 208(a0)
        fsd f27, 216(a0)
        fsd f28, 224(a0)
        fsd f29, 232(a0)
        fsd f30, 240(a0)
        fsd f31, 248(a0)
        ret

.globl fp_restore
fp_restore:
        fld f0, 0(a0)
        fld f1, 8(a0)
        fld f2, 16(a0)
        fld f3, 24(a0)
        fld f4, 32(a0)
        fld f5, 40(a0)
        fld f6, 48(a0)
        fld f7, 56(a0)
        fld f8, 64(a0)
        fld f9, 72(a0)
        fld f10, 80(a0)
        fld f11, 88(a0)
        fld f12, 96(a0)
        fld f13, 104(a0)
        fld f14, 112(a0)
        fld f15, 120(a0)
        fld f16, 128(a0)
        fld f17, 136(a0)
        fld f18, 144(a0)
        fld f19, 152(a0)
        fld f20, 160(a0)
        fld f21, 168(a0)
        fld f22, 176(a0)
        fld f23, 184(a0)
        fld f24, 192(a0)
        fld f25, 200(a0)
        fld f26, 208(a0)
        fld f27, 216(a0)
        fld f28, 224(a0)
        fld f29, 232(a0)
        fld f30, 240(a0)
        fld f31, 248(a0)
        ret

        #
        # vector register save/restore (V extension):
        #   vec_save(char *regs, uint64 csrs[4])
        #   vec_restore(char *regs, uint64 csrs[4])
        # regs holds v0..v31 (32 * vlenb bytes); csrs = vstart, vtype, vl, vcsr.
        # sstatus.VS must be on.
        #
        .option push
        .option arch, +v
.globl vec_save
vec_save:
        csrr t0, vstart
        csrr t1, vtype
        csrr t2, vl
        csrr t3, vcsr
        sd t0, 0(a1)
        sd t1, 8(a1)
        sd t2, 16(a1)
        sd t3, 24(a1)
        vsetvli t4, x0, e8, m8, ta, ma
        vse8.v v0, (a0)
        add a0, a0, t4
        vse8.v v8, (a0)
        add a0, a0, t4
        vse8.v v16, (a0)
        add a0, a0, t4
        vse8.v v24, (a0)
        ret

.globl vec_restore
vec_restore:
        vsetvli t4, x0, e8, m8, ta, ma
        vle8.v v0, (a0)
        add a0, a0, t4
        vle8.v v8, (a0)
        add a0, a0, t4
        vle8.v v16, (a0)
        add a0, a0, t4
        vle8.v v24, (a0)
        ld t0, 0(a1)
        ld t1, 8(a1)
        ld t2, 16(a1)
        ld t3, 24(a1)
        vsetvl x0, t2, t1
        csrw vstart, t0
        csrw vcsr, t3
        ret
        .option pop
