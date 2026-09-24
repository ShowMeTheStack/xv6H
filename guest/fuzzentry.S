        # entry for the fuzzing guest: a0 = hartid, a1 = dtb.
        #
        # every hart runs the fuzzer, on a stack of its own: a device
        # model that is right on one vcpu and wrong on two is a
        # hypervisor bug of the kind that only shows up when two
        # guests' vcpus are in the same emulated device at once.
.section .text.entry
.globl _entry
_entry:
        la sp, stacks
        li t0, 16384
        addi t1, a0, 1
        mul t0, t0, t1
        add sp, sp, t0
        la t0, fuzz_trap
        csrw stvec, t0
        call fuzz_main
park:   wfi
        j park

        # traps: count them and step over the instruction that caused
        # one. Every instruction the payload executes -- including the
        # ones it generates -- is four bytes, so the step is four.
        #
        # a trap can land in the middle of compiled C, so the handler
        # saves what it uses. It runs on the interrupted stack: nothing
        # here ever leaves sp in a state where that is not safe.
.globl fuzz_trap
.align 4
fuzz_trap:
        addi sp, sp, -16
        sd t0, 0(sp)
        sd t1, 8(sp)
        la t0, fuzz_traps
        ld t1, 0(t0)
        addi t1, t1, 1
        sd t1, 0(t0)
        # an interrupt, not an exception: sepc is the instruction that
        # was interrupted, and stepping over it would skip an
        # instruction of the payload itself -- a loop counter, say, so
        # that the run never ends. A round may have written nonsense
        # into sie or sstatus and let one in; take the interrupts away
        # again and return to the instruction that was interrupted.
        csrr t0, scause
        bgez t0, 1f
        csrw sie, zero
        li t1, 2
        csrc sstatus, t1
        j 2f
1:
        csrr t0, sepc
        addi t0, t0, 4
        csrw sepc, t0
2:
        # the round just run may have written 0 to stvec, in which case
        # the next trap would go to address 0: put the handler back
        # every time rather than hope it survived.
        la t0, fuzz_trap
        csrw stvec, t0
        ld t0, 0(sp)
        ld t1, 8(sp)
        addi sp, sp, 16
        sret

.section .bss
.align 16
.globl stacks
stacks:
        .space 16384 * 8   # one stack per hart, up to NVCPU of them
