        # entry for the "revguest" test guest: a0 = hartid, a1 = dtb address.
        # one hart only; a real trap vector that saves and restores the
        # registers so that C can take interrupts.
.section .text.entry
.globl _start
_start:
        la sp, stacks
        li t0, 8192
        add sp, sp, t0
        la t0, rev_vec
        csrw stvec, t0
        li t0, (1 << 13)          # FS: floating point on for C
        csrs sstatus, t0
        call rev_main
1:      wfi
        j 1b

.align 4
.globl rev_vec
rev_vec:
        addi sp, sp, -256
        sd ra, 0(sp)
        sd t0, 8(sp)
        sd t1, 16(sp)
        sd t2, 24(sp)
        sd t3, 32(sp)
        sd t4, 40(sp)
        sd t5, 48(sp)
        sd t6, 56(sp)
        sd a0, 64(sp)
        sd a1, 72(sp)
        sd a2, 80(sp)
        sd a3, 88(sp)
        sd a4, 96(sp)
        sd a5, 104(sp)
        sd a6, 112(sp)
        sd a7, 120(sp)
        sd s0, 128(sp)
        sd s1, 136(sp)
        sd s2, 144(sp)
        sd s3, 152(sp)
        sd s4, 160(sp)
        sd s5, 168(sp)
        sd s6, 176(sp)
        sd s7, 184(sp)
        sd s8, 192(sp)
        sd s9, 200(sp)
        sd s10, 208(sp)
        sd s11, 216(sp)
        sd gp, 224(sp)
        sd tp, 232(sp)
        call rev_trap
        ld ra, 0(sp)
        ld t0, 8(sp)
        ld t1, 16(sp)
        ld t2, 24(sp)
        ld t3, 32(sp)
        ld t4, 40(sp)
        ld t5, 48(sp)
        ld t6, 56(sp)
        ld a0, 64(sp)
        ld a1, 72(sp)
        ld a2, 80(sp)
        ld a3, 88(sp)
        ld a4, 96(sp)
        ld a5, 104(sp)
        ld a6, 112(sp)
        ld a7, 120(sp)
        ld s0, 128(sp)
        ld s1, 136(sp)
        ld s2, 144(sp)
        ld s3, 152(sp)
        ld s4, 160(sp)
        ld s5, 168(sp)
        ld s6, 176(sp)
        ld s7, 184(sp)
        ld s8, 192(sp)
        ld s9, 200(sp)
        ld s10, 208(sp)
        ld s11, 216(sp)
        ld gp, 224(sp)
        ld tp, 232(sp)
        addi sp, sp, 256
        sret

.section .bss
.align 4
stacks:
        .space 8192
