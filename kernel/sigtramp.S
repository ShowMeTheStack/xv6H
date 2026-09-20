        #
        # the page the kernel maps at SIGTRAMP in every address space:
        # a signal handler returns here (the kernel puts this address in
        # ra), and this calls sigreturn to restore the interrupted state.
        #
#include "syscall.h"

.globl sigtramp
.section sigtrampsec
.align 12
sigtramp:
        li a7, SYS_sigreturn
        ecall
        # sigreturn does not return; if it does, the process is broken.
1:      j 1b
