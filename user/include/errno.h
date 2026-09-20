#ifndef _ERRNO_H
#define _ERRNO_H
#include <sys/cdefs.h>
#include "kernel/errno.h" // the numbers, shared with the kernel

// every thread has a control block, and errno lives in it, so two
// threads never clobber each other's error.
//
// the thread pointer points at the thread's static TLS block, as the
// RISC-V ABI says it must, and the control block sits immediately
// below it: __thread variables are at positive offsets from tp and the
// library's own fields at negative ones. errno is the last word before
// tp, which is why the system call stubs can write it in one
// instruction (see user/usys.pl).
struct __tcb {
  void *self;   // tp - 24: the thread's own structure
  long tid;     // tp - 16
  long err;     // tp - 8: errno is its low half
};
#define TCB_ERRNO_OFF (-8)   // must match user/usys.pl
#define TCB_SELF_OFF  (-24)

int *__errno_location(void);
#define errno (*__errno_location())
#endif
