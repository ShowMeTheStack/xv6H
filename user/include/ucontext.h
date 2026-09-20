#ifndef _UCONTEXT_H
#define _UCONTEXT_H
#include <sys/cdefs.h>
// ucontext_t and mcontext_t come from the kernel's header: a signal
// handler is handed one of these, and a context saved by getcontext
// must be the same shape as one the kernel built, or the two halves of
// the interface would not fit together.
#include <signal.h>

int getcontext(ucontext_t *uc);
int setcontext(const ucontext_t *uc);
void makecontext(ucontext_t *uc, void (*func)(void), int argc, ...);
int swapcontext(ucontext_t *ouc, const ucontext_t *uc);
#endif
