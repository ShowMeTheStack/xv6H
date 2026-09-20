#ifndef _FENV_H
#define _FENV_H
#include <sys/cdefs.h>

// the RISC-V floating point control and status register packs the five
// sticky exception flags into fflags (fcsr bits 4..0) and the rounding mode
// into frm (fcsr bits 7..5), and both are also addressable as CSRs of their
// own. the exception macros below are the hardware bit positions, so a
// fexcept_t is simply a copy of fflags.
#define FE_INEXACT   0x01
#define FE_UNDERFLOW 0x02
#define FE_OVERFLOW  0x04
#define FE_DIVBYZERO 0x08
#define FE_INVALID   0x10
#define FE_ALL_EXCEPT 0x1f

// the frm encodings; RMM (round to nearest, ties away from zero) has no
// name in C, so it is not offered here.
#define FE_TONEAREST  0
#define FE_TOWARDZERO 1
#define FE_DOWNWARD   2
#define FE_UPWARD     3

typedef unsigned int fexcept_t;
typedef unsigned int fenv_t;  // a whole fcsr: rounding mode and flags

// the environment the hardware comes up in: nearest, nothing raised
#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int);
int fegetexceptflag(fexcept_t *, int);
int feraiseexcept(int);
int fesetexceptflag(const fexcept_t *, int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t *);
int feholdexcept(fenv_t *);
int fesetenv(const fenv_t *);
int feupdateenv(const fenv_t *);
#endif
