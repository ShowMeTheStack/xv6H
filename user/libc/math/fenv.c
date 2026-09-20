// the floating point environment, which on RISC-V is one register.
//
// fcsr holds the five sticky exception flags in its low bits and the
// rounding mode just above them, and the hardware also exposes those two
// fields on their own as the fflags and frm CSRs. all three are
// unprivileged, so user code reaches them with ordinary csr instructions
// and none of this needs the kernel.
//
// the exception macros in fenv.h are the hardware bit positions and the
// rounding macros are the hardware mode numbers, which is why nothing here
// has to translate between what C asks for and what the register holds.
//
// RISC-V never traps on a floating point exception: raising one only sets
// its flag. feraiseexcept therefore sets the bits directly instead of
// contriving an operation that would produce them.

#include <fenv.h>

static unsigned int
getflags(void)
{
  unsigned int f;

  asm volatile("csrr %0, fflags" : "=r"(f));
  return f;
}

int
feclearexcept(int excepts)
{
  unsigned int m = (unsigned int)excepts & FE_ALL_EXCEPT;

  asm volatile("csrc fflags, %0" ::"r"(m));
  return 0;
}

int
feraiseexcept(int excepts)
{
  unsigned int m = (unsigned int)excepts & FE_ALL_EXCEPT;

  asm volatile("csrs fflags, %0" ::"r"(m));
  return 0;
}

int
fetestexcept(int excepts)
{
  return (int)(getflags() & (unsigned int)excepts & FE_ALL_EXCEPT);
}

int
fegetexceptflag(fexcept_t *flagp, int excepts)
{
  *flagp = getflags() & (unsigned int)excepts & FE_ALL_EXCEPT;
  return 0;
}

int
fesetexceptflag(const fexcept_t *flagp, int excepts)
{
  unsigned int m = (unsigned int)excepts & FE_ALL_EXCEPT;

  // only the selected flags move, and each takes the value it has in the
  // saved set rather than simply being raised
  feclearexcept((int)m);
  feraiseexcept((int)(*flagp & m));
  return 0;
}

int
fegetround(void)
{
  unsigned int r;

  asm volatile("csrr %0, frm" : "=r"(r));
  return (int)r;
}

int
fesetround(int round)
{
  unsigned int r = (unsigned int)round;

  if (round < 0 || round > FE_UPWARD)
    return -1;
  asm volatile("csrw frm, %0" ::"r"(r));
  return 0;
}

int
fegetenv(fenv_t *envp)
{
  unsigned int f;

  asm volatile("csrr %0, fcsr" : "=r"(f));
  *envp = f;
  return 0;
}

int
fesetenv(const fenv_t *envp)
{
  // the default environment is round to nearest with nothing raised, which
  // is a zero fcsr
  unsigned int f = envp == FE_DFL_ENV ? 0 : *envp;

  asm volatile("csrw fcsr, %0" ::"r"(f));
  return 0;
}

int
feholdexcept(fenv_t *envp)
{
  fegetenv(envp);
  feclearexcept(FE_ALL_EXCEPT);
  return 0;
}

int
feupdateenv(const fenv_t *envp)
{
  unsigned int held = getflags();

  // whatever was raised while the old environment was held stays raised in
  // the new one
  fesetenv(envp);
  feraiseexcept((int)held);
  return 0;
}
