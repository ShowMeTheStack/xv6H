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
        li t1, 0x8000000000000003
        bne t0, t1, mvec_unexpected

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

.section .bss
.align 4
.globl mvec_fault
mvec_fault:
        .space 32
