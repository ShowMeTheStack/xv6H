        #
        # guest entry and exit.
        #
        # guestret(struct vcpu *v): save the host's callee-saved registers
        # in v->hostctx (like swtch), load the guest's registers from
        # v->regs, and sret into the guest (V=1). hyp.c has already set up
        # sepc, sstatus, hstatus and the VS CSRs, and sscratch = v.
        #
        # guestvec: traps while V=1 come here (hyp.c points stvec at it).
        # save the guest registers into v->regs, restore the host's
        # callee-saved registers and return from the guestret() call.
        #
        # struct vcpu layout (hyp.h): regs[32] at 0, hostctx at 256
        # (ra, sp, s0..s11), host_tp at 368.
        #

#define HC 256
#define HOST_TP 368

.globl guestret
guestret:
        sd ra, HC+0(a0)
        sd sp, HC+8(a0)
        sd s0, HC+16(a0)
        sd s1, HC+24(a0)
        sd s2, HC+32(a0)
        sd s3, HC+40(a0)
        sd s4, HC+48(a0)
        sd s5, HC+56(a0)
        sd s6, HC+64(a0)
        sd s7, HC+72(a0)
        sd s8, HC+80(a0)
        sd s9, HC+88(a0)
        sd s10, HC+96(a0)
        sd s11, HC+104(a0)
        sd tp, HOST_TP(a0)

        csrw sscratch, a0

        ld x1, 8(a0)
        ld x2, 16(a0)
        ld x3, 24(a0)
        ld x4, 32(a0)
        ld x5, 40(a0)
        ld x6, 48(a0)
        ld x7, 56(a0)
        ld x8, 64(a0)
        ld x9, 72(a0)
        ld x11, 88(a0)
        ld x12, 96(a0)
        ld x13, 104(a0)
        ld x14, 112(a0)
        ld x15, 120(a0)
        ld x16, 128(a0)
        ld x17, 136(a0)
        ld x18, 144(a0)
        ld x19, 152(a0)
        ld x20, 160(a0)
        ld x21, 168(a0)
        ld x22, 176(a0)
        ld x23, 184(a0)
        ld x24, 192(a0)
        ld x25, 200(a0)
        ld x26, 208(a0)
        ld x27, 216(a0)
        ld x28, 224(a0)
        ld x29, 232(a0)
        ld x30, 240(a0)
        ld x31, 248(a0)
        ld x10, 80(a0)
        sret

.globl guestvec
.align 4
guestvec:
        # a0 <- vcpu, sscratch <- guest a0
        csrrw a0, sscratch, a0

        sd x1, 8(a0)
        sd x2, 16(a0)
        sd x3, 24(a0)
        sd x4, 32(a0)
        sd x5, 40(a0)
        sd x6, 48(a0)
        sd x7, 56(a0)
        sd x8, 64(a0)
        sd x9, 72(a0)
        sd x11, 88(a0)
        sd x12, 96(a0)
        sd x13, 104(a0)
        sd x14, 112(a0)
        sd x15, 120(a0)
        sd x16, 128(a0)
        sd x17, 136(a0)
        sd x18, 144(a0)
        sd x19, 152(a0)
        sd x20, 160(a0)
        sd x21, 168(a0)
        sd x22, 176(a0)
        sd x23, 184(a0)
        sd x24, 192(a0)
        sd x25, 200(a0)
        sd x26, 208(a0)
        sd x27, 216(a0)
        sd x28, 224(a0)
        sd x29, 232(a0)
        sd x30, 240(a0)
        sd x31, 248(a0)

        # guest a0 was parked in sscratch
        csrr t0, sscratch
        sd t0, 80(a0)
        csrw sscratch, a0

        ld ra, HC+0(a0)
        ld sp, HC+8(a0)
        ld s0, HC+16(a0)
        ld s1, HC+24(a0)
        ld s2, HC+32(a0)
        ld s3, HC+40(a0)
        ld s4, HC+48(a0)
        ld s5, HC+56(a0)
        ld s6, HC+64(a0)
        ld s7, HC+72(a0)
        ld s8, HC+80(a0)
        ld s9, HC+88(a0)
        ld s10, HC+96(a0)
        ld s11, HC+104(a0)
        ld tp, HOST_TP(a0)

        # return from guestret() into hyp.c
        ret
