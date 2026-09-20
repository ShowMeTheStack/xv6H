#ifndef _SETJMP_H
#define _SETJMP_H
#include <sys/cdefs.h>
#include "kernel/signal.h"

// ra, sp, s0-s11, fs0-fs11, then the saved mask and a flag
typedef struct {
  unsigned long regs[14];
  double fregs[12];
  sigset_t mask;
  int savemask;
} jmp_buf[1];
typedef jmp_buf sigjmp_buf;

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __noreturn;
int _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __noreturn;
int __sigsetjmp(sigjmp_buf env, int savemask);
#define sigsetjmp(env, save) __sigsetjmp(env, save)
void siglongjmp(sigjmp_buf env, int val) __noreturn;
#endif
