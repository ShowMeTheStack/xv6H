        #
        # low-level code to handle traps from user space into
        # the kernel, and returns from kernel to user.
        #
        # the kernel maps the page holding this code
        # at the same virtual address (TRAMPOLINE)
        # in user and kernel space so that it continues
        # to work when it switches page tables.
        # kernel.ld causes this code to start at 
        # a page boundary.
        #

#include "riscv.h"
#include "memlayout.h"

.section trampsec
.globl trampoline
.globl usertrap
trampoline:
.align 4
.globl uservec
uservec:    
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #

        # save user a0 in sscratch so
        # a0 can be used to get at TRAPFRAME.
        csrw sscratch, a0

        # each process has a separate p->trapframe memory area,
        # but it's mapped to the same virtual address
        # (TRAPFRAME) in every process's user page table.
        li a0, TRAPFRAME
        
        # save the user registers in TRAPFRAME
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# save the user a0 in p->trapframe->a0
        csrr t0, sscratch
        sd t0, 112(a0)

        # floating-point registers: save them only if the process used
        # the FPU since they were last restored (sstatus.FS == Dirty).
        csrr t0, sstatus
        srli t0, t0, 13
        andi t0, t0, 3
        li t1, 3
        bne t0, t1, fp_saved
        fsd f0, 288(a0)
        fsd f1, 296(a0)
        fsd f2, 304(a0)
        fsd f3, 312(a0)
        fsd f4, 320(a0)
        fsd f5, 328(a0)
        fsd f6, 336(a0)
        fsd f7, 344(a0)
        fsd f8, 352(a0)
        fsd f9, 360(a0)
        fsd f10, 368(a0)
        fsd f11, 376(a0)
        fsd f12, 384(a0)
        fsd f13, 392(a0)
        fsd f14, 400(a0)
        fsd f15, 408(a0)
        fsd f16, 416(a0)
        fsd f17, 424(a0)
        fsd f18, 432(a0)
        fsd f19, 440(a0)
        fsd f20, 448(a0)
        fsd f21, 456(a0)
        fsd f22, 464(a0)
        fsd f23, 472(a0)
        fsd f24, 480(a0)
        fsd f25, 488(a0)
        fsd f26, 496(a0)
        fsd f27, 504(a0)
        fsd f28, 512(a0)
        fsd f29, 520(a0)
        fsd f30, 528(a0)
        fsd f31, 536(a0)
        csrr t1, fcsr
        sd t1, 544(a0)
fp_saved:

        # initialize kernel stack pointer, from p->trapframe->kernel_sp
        ld sp, 8(a0)

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, 32(a0)

        # load the address of usertrap(), from p->trapframe->kernel_trap
        ld t0, 16(a0)

        # fetch the kernel page table address, from p->trapframe->kernel_satp.
        ld t1, 0(a0)

        # wait for any previous memory operations to complete, so that
        # they use the user page table.
        sfence.vma zero, zero

        # install the kernel page table.
        csrw satp, t1

        # flush now-stale user entries from the TLB.
        sfence.vma zero, zero

        # call usertrap()
        jalr t0

.globl userret
userret:
        # usertrap() returns here, with user satp in a0.
        # return from kernel to user.

        # flush icache, in case this is the first time
        # we're running this proc on this hart.
        fence.i

        # switch to the user page table.
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        li a0, TRAPFRAME

        # restore the floating-point state (zeros for a process that never
        # used the FPU), then mark it Clean so an untouched FPU is not saved
        # again at the next trap.
        li t0, 0x6000
        csrc sstatus, t0
        li t0, 0x2000
        csrs sstatus, t0
        fld f0, 288(a0)
        fld f1, 296(a0)
        fld f2, 304(a0)
        fld f3, 312(a0)
        fld f4, 320(a0)
        fld f5, 328(a0)
        fld f6, 336(a0)
        fld f7, 344(a0)
        fld f8, 352(a0)
        fld f9, 360(a0)
        fld f10, 368(a0)
        fld f11, 376(a0)
        fld f12, 384(a0)
        fld f13, 392(a0)
        fld f14, 400(a0)
        fld f15, 408(a0)
        fld f16, 416(a0)
        fld f17, 424(a0)
        fld f18, 432(a0)
        fld f19, 440(a0)
        fld f20, 448(a0)
        fld f21, 456(a0)
        fld f22, 464(a0)
        fld f23, 472(a0)
        fld f24, 480(a0)
        fld f25, 488(a0)
        fld f26, 496(a0)
        fld f27, 504(a0)
        fld f28, 512(a0)
        fld f29, 520(a0)
        fld f30, 528(a0)
        fld f31, 536(a0)
        ld t1, 544(a0)
        csrw fcsr, t1
        li t0, 0x6000
        csrc sstatus, t0
        li t0, 0x4000
        csrs sstatus, t0

        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0
        ld a0, 112(a0)
        
        # return to user mode and user pc.
        # prepare_return() sets up sstatus and sepc.
        sret
