        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there,
        # with a0 = hartid and a1 = physical address of the
        # device tree blob (the xv6 hypervisor does the same
        # for its guests).
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        # (keep a0/a1 intact for start()).
        la sp, stack0
        li t0, 1024*4
        csrr t1, mhartid
        addi t1, t1, 1
        mul t0, t0, t1
        add sp, sp, t0
        # jump to start(hartid, fdt) in start.c
        csrr a0, mhartid
        call start
spin:
        j spin
