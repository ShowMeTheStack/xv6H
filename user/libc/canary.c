//
// stack canaries for a process.
//
// the compiler gives every function with an array or a taken address
// on its stack a word just below its saved return address, written on
// the way in and checked on the way out. A write past the end of a
// local buffer changes it, and the function does not return: it comes
// here, and the process dies with SIGABRT rather than jumping to
// whatever the overflow put where the return address was.
//
// the value comes from AT_RANDOM in the auxiliary vector, which the
// kernel fills from its random generator, so it differs from one
// process to the next. crt.c sets it before main and before anything
// else with a canary of its own has been entered: a function checks on
// the way out the global as it stands then, so changing it underneath
// a live call would look exactly like an overflow.
//
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

unsigned long __stack_chk_guard = 0xff0a0dff595f9e5cUL;

// no canary of its own: it changes the value that every canary is
// compared against, and a function that saved the old one on the way
// in and met the new one on the way out would look exactly like an
// overflow.
__attribute__((no_stack_protector)) void
__libc_canary_init(const void *random16)
{
  unsigned long v;

  if (random16 == 0)
    return;
  memcpy(&v, random16, sizeof(v));
  // a low byte of zero would make a string write that stops at the
  // canary look intact
  if ((v & 0xff) == 0)
    v |= 0x5a;
  if (v)
    __stack_chk_guard = v;
}

__attribute__((noreturn, no_stack_protector)) void
__stack_chk_fail(void)
{
  static const char msg[] = "*** stack smashing detected ***\n";

  write(2, msg, sizeof(msg) - 1);
  raise(SIGABRT);
  _exit(134);
}

// the compiler emits this one for -fstack-protector with a local
// address that the checker knows the name of
__attribute__((noreturn, no_stack_protector)) void
__stack_chk_fail_local(void)
{
  __stack_chk_fail();
}
