        # entry for the hostile test guest: a0 = hartid, a1 = dtb.
        # only hart 0 runs; others park.
.section .text.entry
.globl _entry
_entry:
        bnez a0, park
        la sp, stacktop
        call badmain
park:   wfi
        j park

        # trap handler: skip the faulting instruction (all 4 bytes,
        # the payload is built without compressed instructions).
.globl badtrap
.align 4
badtrap:
        csrr t0, sepc
        addi t0, t0, 4
        csrw sepc, t0
        sret

.section .bss
.align 4
        .space 4096
stacktop:
