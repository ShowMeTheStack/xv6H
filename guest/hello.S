        # entry for the "hello" test guest: a0 = hartid, a1 = dtb address.
        # every hart gets its own 4 KB stack and calls hello_main.
.section .text.entry
.globl _start
_start:
        la sp, stacks
        li t0, 4096
        addi t1, a0, 1
        mul t0, t0, t1
        add sp, sp, t0
        la t0, hello_trap
        csrw stvec, t0
        # enable the FPU and (if present) the vector unit for C code
        li t0, (1 << 13) | (1 << 9)
        csrs sstatus, t0
        call hello_main
1:      wfi
        j 1b

        # traps: skip the instruction (4-byte encodings only).
.align 4
.globl hello_trap
hello_trap:
        csrr t0, sepc
        addi t0, t0, 4
        csrw sepc, t0
        sret

.section .bss
.align 4
stacks: .space 4096 * 9
