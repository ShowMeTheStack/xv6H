//
// getcontext, setcontext and swapcontext for RISC-V lp64d.
//
// a context holds what the calling convention says a function must
// preserve, exactly as the jump buffer in setjmp.S does: ra, sp,
// s0-s11 and fs0-fs11, plus the signal mask. everything else is
// caller-saved, so whoever called getcontext has already spilled
// anything it still wanted. the whole of uc_mcontext is not saved:
// only a signal frame, which the kernel writes, has the rest.
//
// the registers go at their architectural places inside
// uc_mcontext.regs, which holds x1 through x31, so a context this file
// wrote and a context a signal delivered can be read the same way.
// ucontext.c checks these offsets against the structure.
//
//   8    uc_link
//   40   uc_sigmask
//   56   ra   (regs[0], x1)          64   sp   (regs[1], x2)
//   112  s0   (regs[7], x8)          120  s1   (regs[8], x9)
//   192  s2 .. s11 (regs[17..26], x18 .. x27)
//   368  fs0 (fregs[8], f8)          376  fs1  (fregs[9], f9)
//   448  fs2 .. fs11 (fregs[18..27], f18 .. f27)
//
#include "kernel/syscall.h"

#define UC_LINK 8
#define UC_MASK 40
#define UC_RA   56
#define UC_SP   64
#define UC_S0   112
#define UC_S1   120
#define UC_S2   192
#define UC_S3   200
#define UC_S4   208
#define UC_S5   216
#define UC_S6   224
#define UC_S7   232
#define UC_S8   240
#define UC_S9   248
#define UC_S10  256
#define UC_S11  264
#define UC_FS0  368
#define UC_FS1  376
#define UC_FS2  448
#define UC_FS3  456
#define UC_FS4  464
#define UC_FS5  472
#define UC_FS6  480
#define UC_FS7  488
#define UC_FS8  496
#define UC_FS9  504
#define UC_FS10 512
#define UC_FS11 520

#define SIG_SETMASK 2

// int getcontext(ucontext_t *uc)
.global getcontext
.type getcontext, @function
getcontext:
        sd ra, UC_RA(a0)
        sd sp, UC_SP(a0)
        sd s0, UC_S0(a0)
        sd s1, UC_S1(a0)
        sd s2, UC_S2(a0)
        sd s3, UC_S3(a0)
        sd s4, UC_S4(a0)
        sd s5, UC_S5(a0)
        sd s6, UC_S6(a0)
        sd s7, UC_S7(a0)
        sd s8, UC_S8(a0)
        sd s9, UC_S9(a0)
        sd s10, UC_S10(a0)
        sd s11, UC_S11(a0)
        fsd fs0, UC_FS0(a0)
        fsd fs1, UC_FS1(a0)
        fsd fs2, UC_FS2(a0)
        fsd fs3, UC_FS3(a0)
        fsd fs4, UC_FS4(a0)
        fsd fs5, UC_FS5(a0)
        fsd fs6, UC_FS6(a0)
        fsd fs7, UC_FS7(a0)
        fsd fs8, UC_FS8(a0)
        fsd fs9, UC_FS9(a0)
        fsd fs10, UC_FS10(a0)
        fsd fs11, UC_FS11(a0)
        // sigprocmask(SIG_SETMASK, 0, &uc->uc_sigmask) reads the mask
        addi sp, sp, -16
        sd ra, 0(sp)
        addi a2, a0, UC_MASK
        li a1, 0
        li a0, SIG_SETMASK
        li a7, SYS_sigprocmask
        ecall
        ld ra, 0(sp)
        addi sp, sp, 16
        li a0, 0
        ret

// int setcontext(const ucontext_t *uc); it does not return here, but
// wherever the context's ra points, and it makes that look like a
// return of zero from the getcontext that saved it.
.global setcontext
.type setcontext, @function
setcontext:
        // the two jumps below go to this local label rather than to
        // the global one: inside a shared library a jump to a symbol
        // that another object could replace has to go through the
        // procedure linkage table, and a short jump cannot.
.Lsetcontext:
        // put the mask back before the registers, while this stack
        // frame is still the one we are standing on.
        addi sp, sp, -16
        sd a0, 0(sp)
        addi a1, a0, UC_MASK
        li a2, 0
        li a0, SIG_SETMASK
        li a7, SYS_sigprocmask
        ecall
        ld a0, 0(sp)
        addi sp, sp, 16
        ld ra, UC_RA(a0)
        ld s0, UC_S0(a0)
        ld s1, UC_S1(a0)
        ld s2, UC_S2(a0)
        ld s3, UC_S3(a0)
        ld s4, UC_S4(a0)
        ld s5, UC_S5(a0)
        ld s6, UC_S6(a0)
        ld s7, UC_S7(a0)
        ld s8, UC_S8(a0)
        ld s9, UC_S9(a0)
        ld s10, UC_S10(a0)
        ld s11, UC_S11(a0)
        fld fs0, UC_FS0(a0)
        fld fs1, UC_FS1(a0)
        fld fs2, UC_FS2(a0)
        fld fs3, UC_FS3(a0)
        fld fs4, UC_FS4(a0)
        fld fs5, UC_FS5(a0)
        fld fs6, UC_FS6(a0)
        fld fs7, UC_FS7(a0)
        fld fs8, UC_FS8(a0)
        fld fs9, UC_FS9(a0)
        fld fs10, UC_FS10(a0)
        fld fs11, UC_FS11(a0)
        ld sp, UC_SP(a0)        // last: a0 is still needed above
        li a0, 0
        ret

// int swapcontext(ucontext_t *ouc, const ucontext_t *uc)
//
// the saving half is getcontext's, written out again rather than
// called, because a call would save a return address inside this
// function and the resumed context would then go on to switch away a
// second time.
.global swapcontext
.type swapcontext, @function
swapcontext:
        sd ra, UC_RA(a0)
        sd sp, UC_SP(a0)
        sd s0, UC_S0(a0)
        sd s1, UC_S1(a0)
        sd s2, UC_S2(a0)
        sd s3, UC_S3(a0)
        sd s4, UC_S4(a0)
        sd s5, UC_S5(a0)
        sd s6, UC_S6(a0)
        sd s7, UC_S7(a0)
        sd s8, UC_S8(a0)
        sd s9, UC_S9(a0)
        sd s10, UC_S10(a0)
        sd s11, UC_S11(a0)
        fsd fs0, UC_FS0(a0)
        fsd fs1, UC_FS1(a0)
        fsd fs2, UC_FS2(a0)
        fsd fs3, UC_FS3(a0)
        fsd fs4, UC_FS4(a0)
        fsd fs5, UC_FS5(a0)
        fsd fs6, UC_FS6(a0)
        fsd fs7, UC_FS7(a0)
        fsd fs8, UC_FS8(a0)
        fsd fs9, UC_FS9(a0)
        fsd fs10, UC_FS10(a0)
        fsd fs11, UC_FS11(a0)
        addi sp, sp, -16
        sd ra, 0(sp)
        sd a1, 8(sp)
        addi a2, a0, UC_MASK
        li a1, 0
        li a0, SIG_SETMASK
        li a7, SYS_sigprocmask
        ecall
        ld ra, 0(sp)
        ld a0, 8(sp)            // the context to go to
        addi sp, sp, 16
        j .Lsetcontext

// where a context made by makecontext begins. makecontext left the
// function in s0, the ucontext_t in s1 and up to six arguments in
// s2-s7, all of which setcontext has just restored; this moves the
// arguments into place, calls the function, and follows uc_link when
// it returns.
.global __start_context
.type __start_context, @function
__start_context:
        mv a0, s2
        mv a1, s3
        mv a2, s4
        mv a3, s5
        mv a4, s6
        mv a5, s7
        jalr s0
        ld a0, UC_LINK(s1)
        beqz a0, 1f
        call .Lsetcontext       // and never comes back
1:
        li a0, 0
        call exit               // a null uc_link ends the program
