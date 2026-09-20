#include "kernel/syscall.h"

//
// int __clone(uint64 flags, void *stack, int *ptid, void *tls, int *ctid,
//             void (*fn)(void *), void *arg);
//
// the parent gets the new thread's id back; the child starts on its own
// stack in fn(arg) and never returns here, so its exit is the thread's.
//
.hidden __syscall_ret
.global __clone
__clone:
        addi a1, a1, -16        # fn and arg, waiting on the child's stack
        sd a5, 0(a1)
        sd a6, 8(a1)
        li a7, SYS_clone
        ecall
        beqz a0, 1f             # a0 == 0: the child, on its own stack
        bltz a0, 2f             # a0 < 0: the call failed
        ret                     # the parent: a0 is the new thread's id
2:
        j __syscall_ret         # -errno becomes -1 with errno set
1:
        ld a1, 0(sp)            # the child, on the stack it was given
        ld a0, 8(sp)
        addi sp, sp, 16
        jalr a1
        li a7, SYS_exit         # fn returned: this thread is done
        ecall
3:      j 3b
