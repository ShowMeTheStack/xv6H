        #
        # machine-mode trap vector.
        #
        # xv6 delegates everything it can to supervisor mode, so the only
        # trap that arrives here in normal operation is a machine software
        # interrupt (MSIP), which supervisor code on another hart raised by
        # writing our CLINT msip register. we turn it into a supervisor
        # software interrupt (SSIP) and return. this is how xv6 does
        # inter-processor interrupts without firmware.
        #
        # mscratch holds the top of this hart's small machine-mode stack.
        #
.globl mvec
.align 4
mvec:
        csrrw sp, mscratch, sp
        addi sp, sp, -32
        sd t0, 0(sp)
        sd t1, 8(sp)
        sd t2, 16(sp)

        csrr t0, mcause
        li t1, 0x8000000000000007
        beq t0, t1, mvec_mti
        li t1, 9
        beq t0, t1, mvec_ecall
        li t1, 0x8000000000000003
        bne t0, t1, mvec_full

        # clear our msip: *(uint32*)(clint_msip_base + 4*mhartid) = 0
        la t1, clint_msip_base
        ld t1, 0(t1)
        csrr t0, mhartid
        slli t0, t0, 2
        add t1, t1, t0
        sw zero, 0(t1)

        # raise SSIP for supervisor mode.
        li t0, 2
        csrs mip, t0

        ld t0, 0(sp)
        ld t1, 8(sp)
        ld t2, 16(sp)
        addi sp, sp, 32
        csrrw sp, mscratch, sp
        mret

# A host without Sstc (Rocket on the ZedBoard). The supervisor's timer is the
# machine's: S asks for a deadline with the SBI's TIME call (machine.c
# machine_sbi_set_timer, medeleg leaves ecall-from-S here, start.c), which
# goes into this hart's mtimecmp with STIP lowered; the machine timer, when it
# fires, raises STIP and masks itself until the next call.
mvec_mti:
        li t0, 0x80                     # MTIE
        csrc mie, t0
        li t0, 0x20                     # STIP
        csrs mip, t0
        j mvec_out

mvec_ecall:
        li t1, 0x54494D45               # TIME; anything else is mtrap's to report
        bne a7, t1, mvec_full
        la t1, clint_msip_base
        ld t1, 0(t1)
        li t2, 0x4000                   # mtimecmp[hart] = clint + 0x4000 + 8 * hart
        add t1, t1, t2
        csrr t0, mhartid
        slli t0, t0, 3
        add t1, t1, t0
        sd a0, 0(t1)
        li t0, 0x20                     # STIP down: the old deadline is answered
        csrc mip, t0
        li t0, 0x80                     # MTIE up
        csrs mie, t0
        csrr t0, mepc
        addi t0, t0, 4
        csrw mepc, t0
        li a0, 0                        # sbiret: no error
        li a1, 0
mvec_out:
        ld t0, 0(sp)
        ld t1, 8(sp)
        ld t2, 16(sp)
        addi sp, sp, 32
        csrrw sp, mscratch, sp
        mret

# Everything else (mtrap.c): misaligned loads and stores performed, anything
# unexpected reported. A full frame, x1..x31 at 8*i, x2 the interrupted sp.
mvec_full:
        ld t0, 0(sp)
        ld t1, 8(sp)
        ld t2, 16(sp)
        addi sp, sp, 32-256
        sd x1, 8(sp)
        sd x3, 24(sp)
        sd x4, 32(sp)
        sd x5, 40(sp)
        sd x6, 48(sp)
        sd x7, 56(sp)
        sd x8, 64(sp)
        sd x9, 72(sp)
        sd x10, 80(sp)
        sd x11, 88(sp)
        sd x12, 96(sp)
        sd x13, 104(sp)
        sd x14, 112(sp)
        sd x15, 120(sp)
        sd x16, 128(sp)
        sd x17, 136(sp)
        sd x18, 144(sp)
        sd x19, 152(sp)
        sd x20, 160(sp)
        sd x21, 168(sp)
        sd x22, 176(sp)
        sd x23, 184(sp)
        sd x24, 192(sp)
        sd x25, 200(sp)
        sd x26, 208(sp)
        sd x27, 216(sp)
        sd x28, 224(sp)
        sd x29, 232(sp)
        sd x30, 240(sp)
        sd x31, 248(sp)
        csrr t0, mscratch
        sd t0, 16(sp)
        sd zero, 0(sp)
        mv a0, sp
        call mtrap
        ld t0, 16(sp)
        csrw mscratch, t0               # the interrupted sp (a load may have set it)
        ld x1, 8(sp)
        ld x3, 24(sp)
        ld x4, 32(sp)
        ld x5, 40(sp)
        ld x6, 48(sp)
        ld x7, 56(sp)
        ld x8, 64(sp)
        ld x9, 72(sp)
        ld x10, 80(sp)
        ld x11, 88(sp)
        ld x12, 96(sp)
        ld x13, 104(sp)
        ld x14, 112(sp)
        ld x15, 120(sp)
        ld x16, 128(sp)
        ld x17, 136(sp)
        ld x18, 144(sp)
        ld x19, 152(sp)
        ld x20, 160(sp)
        ld x21, 168(sp)
        ld x22, 176(sp)
        ld x23, 184(sp)
        ld x24, 192(sp)
        ld x25, 200(sp)
        ld x26, 208(sp)
        ld x27, 216(sp)
        ld x28, 224(sp)
        ld x29, 232(sp)
        ld x30, 240(sp)
        ld x31, 248(sp)
        addi sp, sp, 256
        csrrw sp, mscratch, sp
        mret

# One byte (or halfword) through MPRV|MXR, so the access is the faulting mode's,
# behind a trap guard: a trap during it records its cause in *fault and skips
# the access (the guard is entered from M, so MPP is M and its own store is
# physical). Uncompressed, so the skip is four bytes. t5 holds the fault word.
.option push
.option norvc
.align 2
mprv_guard:
        li t3, 0xA0000
        csrc mstatus, t3
        csrr t3, mcause
        sd t3, 0(t5)
        csrr t3, mepc
        addi t3, t3, 4
        csrw mepc, t3
        mret

.globl mprv_lbu
mprv_lbu:                               # (addr, *fault)
        mv t5, a1
        sd zero, 0(t5)
        la t0, mprv_guard
        csrrw t1, mtvec, t0
        li t2, 0xA0000                  # MPRV | MXR
        csrs mstatus, t2
        lbu a0, 0(a0)
        csrc mstatus, t2
        csrw mtvec, t1
        ret

.globl mprv_lhu
mprv_lhu:                               # (addr, *fault): a 2-aligned halfword of an instruction
        mv t5, a1
        sd zero, 0(t5)
        la t0, mprv_guard
        csrrw t1, mtvec, t0
        li t2, 0xA0000
        csrs mstatus, t2
        lhu a0, 0(a0)
        csrc mstatus, t2
        csrw mtvec, t1
        ret

.globl mprv_sb
mprv_sb:                                # (addr, val, *fault)
        mv t5, a2
        sd zero, 0(t5)
        la t0, mprv_guard
        csrrw t1, mtvec, t0
        li t2, 0xA0000
        csrs mstatus, t2
        sb a1, 0(a0)
        csrc mstatus, t2
        csrw mtvec, t1
        ret

# does a misaligned load trap on this hart? 1 if it does (Rocket), 0 if the
# hart performs it (QEMU)
.globl mprobe_misaligned_traps
mprobe_misaligned_traps:
        la t0, mprobe_trap
        csrrw t1, mtvec, t0
        li a0, 1
        addi t2, sp, -15
        ld t2, 0(t2)
        csrw mtvec, t1
        xori a0, a0, 1
        ret
.option pop

mvec_unexpected:
        # record cause/epc/tval for a debugger and stop this hart.
        la t1, mvec_fault
        sd t0, 0(t1)
        csrr t0, mepc
        sd t0, 8(t1)
        csrr t0, mtval
        sd t0, 16(t1)
        li t0, 1
        sd t0, 24(t1)
1:      wfi
        j 1b

# Does this hart have the CSR? A machine-mode read behind a one-instruction
# trap: 1 if the read went through, 0 if it trapped. Called from
# platform_init, in machine mode, before mtvec is mvec. The reads are csrr,
# never compressed, so the trap skips exactly four bytes.
.align 2
mprobe_trap:
        li a0, 0
        csrr t0, mepc
        addi t0, t0, 4
        csrw mepc, t0
        mret

.globl mprobe_time
mprobe_time:
        la t0, mprobe_trap
        csrrw t1, mtvec, t0
        li a0, 1
        csrr t2, time
        csrw mtvec, t1
        ret

.globl mprobe_cycle
mprobe_cycle:
        la t0, mprobe_trap
        csrrw t1, mtvec, t0
        li a0, 1
        csrr t2, cycle
        csrw mtvec, t1
        ret

.globl mprobe_stimecmp
mprobe_stimecmp:
        la t0, mprobe_trap
        csrrw t1, mtvec, t0
        li a0, 1
        csrr t2, 0x14d
        csrw mtvec, t1
        ret

.globl mprobe_htimedelta
mprobe_htimedelta:
        la t0, mprobe_trap
        csrrw t1, mtvec, t0
        li a0, 1
        csrr t2, 0x605
        csrw mtvec, t1
        ret

.section .bss
.align 4
.globl mvec_fault
mvec_fault:
        .space 32
