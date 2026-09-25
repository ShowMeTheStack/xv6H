//
// stack canaries.
//
// every function with an array or a taken address on its stack writes
// this word just below its saved return address on the way in, and
// checks it on the way out. A write past the end of a local buffer
// changes it, and the function does not return: it comes here instead,
// and the machine stops rather than jumping to whatever the overflow
// put where the return address was.
//
// the value is picked once per boot, in machine mode before anything
// else runs, from the time counter and the hart id. That has to happen
// before any function that carries a canary has been entered, and it
// has to happen exactly once for the whole machine: a function
// compares what it saved on entry against the global as it stands on
// exit, so changing the global while another hart is inside such a
// function looks exactly like an overflow. `start' is compiled without
// a canary of its own for the same reason.
//
// it is not the random generator: the generator needs the device tree,
// the page allocator and a seed, none of which exist this early. The
// time counter on a machine that has just been reset is a poor source
// of entropy and the mixing below does not improve it much; what it
// buys is that the value differs from one boot to the next, which is
// what stops an overflow from being written once and reused.
//
#include "types.h"
#include "riscv.h"
#include "defs.h"

uint64 __stack_chk_guard = 0x595f9e5ca3a1e7d5UL;

__attribute__((no_stack_protector)) void
canary_init(uint64 hartid)
{
  // once per machine, by whichever hart gets here first. Every hart
  // calls this before it runs anything that carries a canary, so the
  // value is settled before the first check; a hart that seeded it
  // again later would change it under the live frames of the harts
  // already running, and every one of those would report an overflow
  // on the way out. (That is what happened: a guest with two vcpus
  // died with "stack smashing detected" in the middle of its boot,
  // reliably enough to take guest-churn and guest-parallel with it.)
  static int chosen;
  static volatile int settled;
  if (__sync_lock_test_and_set(&chosen, 1)) {
    // somebody else is choosing it. Waiting for them is the whole of
    // this: a hart that went on with the value as it stands would save
    // the old one in the first frame it entered, and the winner's store
    // a moment later would make that frame's check fail -- "stack
    // smashing detected" in whatever the loser happened to be inside.
    // (It was hart 0 inside the device-tree parse, on a machine with
    // sixteen gigabytes, where QEMU takes long enough to set the memory
    // up that the harts arrive here in a different order.)
    while (!__atomic_load_n(&settled, __ATOMIC_ACQUIRE))
      ;
    return;
  }

  uint64 t = r_time();
  uint64 c = r_cycle();

  // a low byte of zero would make a string write that stops at the
  // canary look intact
  uint64 v = (t * 0x9e3779b97f4a7c15UL) ^ (c << 17) ^ (hartid * 0x2545f4914f6cdd1dUL);
  v ^= v >> 31;
  v *= 0xbf58476d1ce4e5b9UL;
  v ^= v >> 27;
  if ((v & 0xff) == 0)
    v |= 0x5a;
  if (v)
    __stack_chk_guard = v;
  __atomic_store_n(&settled, 1, __ATOMIC_RELEASE);
}

void __attribute__((noreturn, no_stack_protector))
__stack_chk_fail(void)
{
  panic("stack smashing detected");
}
