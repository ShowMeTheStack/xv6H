//
// makecontext, and the compile-time check that ucontext.S and
// <signal.h> agree about where a ucontext_t keeps things.
//
// getcontext, setcontext and swapcontext are in ucontext.S, because
// saving registers is not something C can be asked to do. what is left
// for C is laying out the new stack, which is where makecontext earns
// its keep: the context it builds has to start executing a function
// with its arguments in place and, when that function returns, go on
// to uc_link instead of returning to a caller that never existed.
//
// the trick is that a context is restored by loading ra and returning
// into it. so makecontext points ra at __start_context and hides what
// that trampoline needs in the callee-saved registers, which are
// restored along with everything else: the function in s0, this
// context in s1 (for uc_link), and the arguments in s2 through s7.
// six arguments is what those registers hold, which is why that is the
// limit; POSIX only promises what the ABI allows anyway.
//
#include <ucontext.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>

// the register numbering of uc_mcontext.regs, which holds x1 .. x31.
#define REG_RA 0
#define REG_SP 1
#define REG_S0 7  // x8
#define REG_S1 8  // x9
#define REG_S2 17 // x18, and s3 .. s7 follow it

// ucontext.S reaches into a ucontext_t with numbers, so the numbers
// are checked here: a change to the kernel's structure breaks the
// build rather than the program.
_Static_assert(offsetof(ucontext_t, uc_link) == 8, "ucontext.S: uc_link moved");
_Static_assert(offsetof(ucontext_t, uc_sigmask) == 40, "ucontext.S: uc_sigmask moved");
_Static_assert(offsetof(ucontext_t, uc_mcontext.regs) == 56, "ucontext.S: regs moved");
_Static_assert(offsetof(ucontext_t, uc_mcontext.fregs) == 304, "ucontext.S: fregs moved");

void __start_context(void);

void
makecontext(ucontext_t *uc, void (*func)(void), int argc, ...)
{
  va_list ap;

  if (uc == 0 || uc->uc_stack.ss_sp == 0 || func == 0)
    return; // POSIX gives makecontext no way to report anything

  // the new stack grows down from the top of what the caller supplied,
  // and the ABI wants the pointer 16-byte aligned at a call.
  unsigned long sp = (unsigned long)uc->uc_stack.ss_sp + uc->uc_stack.ss_size;
  sp &= ~15UL;

  unsigned long *regs = uc->uc_mcontext.regs;
  regs[REG_SP] = sp;
  regs[REG_RA] = (unsigned long)__start_context;
  regs[REG_S0] = (unsigned long)func;
  regs[REG_S1] = (unsigned long)uc;

  // the standard says the arguments are ints; they arrive in whole
  // registers, so reading them as words also does the right thing for
  // the pointers that callers pass in practice.
  va_start(ap, argc);
  for (int i = 0; i < argc && i < 6; i++)
    regs[REG_S2 + i] = va_arg(ap, unsigned long);
  va_end(ap);
}
