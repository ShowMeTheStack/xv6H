//
// setjmp and longjmp.
//
// the jump buffer holds exactly what the RISC-V calling convention says
// a function must preserve: ra, sp, s0-s11 and fs0-fs11. everything
// else is caller-saved, so the compiler has already spilled whatever it
// still needs. sigsetjmp adds the signal mask, saved through the system
// call, and the flag that says whether longjmp should put it back.
//
// the layout matches jmp_buf in <setjmp.h>:
//   0   ra
//   8   sp
//   16  s0 .. s11        (12 words)
//   112 fs0 .. fs11      (12 doubles)
//   208 the saved signal mask (8 bytes)
//   216 savemask
//
#include "kernel/syscall.h"

.global setjmp
.global _setjmp
.type setjmp, @function
setjmp:
_setjmp:
        sd ra, 0(a0)
        sd sp, 8(a0)
        sd s0, 16(a0)
        sd s1, 24(a0)
        sd s2, 32(a0)
        sd s3, 40(a0)
        sd s4, 48(a0)
        sd s5, 56(a0)
        sd s6, 64(a0)
        sd s7, 72(a0)
        sd s8, 80(a0)
        sd s9, 88(a0)
        sd s10, 96(a0)
        sd s11, 104(a0)
        fsd fs0, 112(a0)
        fsd fs1, 120(a0)
        fsd fs2, 128(a0)
        fsd fs3, 136(a0)
        fsd fs4, 144(a0)
        fsd fs5, 152(a0)
        fsd fs6, 160(a0)
        fsd fs7, 168(a0)
        fsd fs8, 176(a0)
        fsd fs9, 184(a0)
        fsd fs10, 192(a0)
        fsd fs11, 200(a0)
        sd zero, 216(a0)        // setjmp does not save the mask
        li a0, 0
        ret

// int __sigsetjmp(sigjmp_buf env, int savemask)
.global __sigsetjmp
.type __sigsetjmp, @function
__sigsetjmp:
        addi sp, sp, -32
        sd ra, 0(sp)
        sd a0, 8(sp)
        sd a1, 16(sp)
        call _setjmp            // a0 still points at the buffer
        ld a1, 16(sp)
        ld a0, 8(sp)
        sd a1, 216(a0)
        beqz a1, 1f
        // sigprocmask(SIG_SETMASK, 0, &env->mask) reads the mask
        addi a2, a0, 208
        li a1, 0
        li a0, 2                // SIG_SETMASK
        li a7, SYS_sigprocmask
        ecall
        ld a0, 8(sp)
1:
        ld ra, 0(sp)
        addi sp, sp, 32
        li a0, 0
        ret

.global longjmp
.global _longjmp
.global siglongjmp
.type longjmp, @function
longjmp:
_longjmp:
siglongjmp:
        ld a2, 216(a0)
        beqz a2, 1f
        // put the mask back before returning into the older frame
        addi sp, sp, -32
        sd a0, 0(sp)
        sd a1, 8(sp)
        addi a1, a0, 208
        li a2, 0
        li a0, 2                // SIG_SETMASK
        li a7, SYS_sigprocmask
        ecall
        ld a1, 8(sp)
        ld a0, 0(sp)
        addi sp, sp, 32
1:
        ld ra, 0(a0)
        ld sp, 8(a0)
        ld s0, 16(a0)
        ld s1, 24(a0)
        ld s2, 32(a0)
        ld s3, 40(a0)
        ld s4, 48(a0)
        ld s5, 56(a0)
        ld s6, 64(a0)
        ld s7, 72(a0)
        ld s8, 80(a0)
        ld s9, 88(a0)
        ld s10, 96(a0)
        ld s11, 104(a0)
        fld fs0, 112(a0)
        fld fs1, 120(a0)
        fld fs2, 128(a0)
        fld fs3, 136(a0)
        fld fs4, 144(a0)
        fld fs5, 152(a0)
        fld fs6, 160(a0)
        fld fs7, 168(a0)
        fld fs8, 176(a0)
        fld fs9, 184(a0)
        fld fs10, 192(a0)
        fld fs11, 200(a0)
        // longjmp(env, 0) must still look like a non-zero return
        mv a0, a1
        bnez a0, 2f
        li a0, 1
2:
        ret
