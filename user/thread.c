#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/mman.h"
#include "user/user.h"
#include "user/thread.h"
#include "user/libc/libc.h"

#define DEFAULT_STACK (64 * 1024)
#define WAKE_ALL 0x7fffffff

int
thread_create(thread_t *t, void (*fn)(void *), void *arg, uint64 stacksize)
{
  if (stacksize == 0)
    stacksize = DEFAULT_STACK;
  stacksize = (stacksize + PGSIZE - 1) & ~(uint64)(PGSIZE - 1);

  char *stack = mmap(0, stacksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED)
    return -1;

  // the top of the thread's own stack holds its thread-local storage
  // and, just below the thread pointer, its control block: errno and
  // the rest are private to the thread, and __thread variables live
  // above tp where the ABI says they must.
  uint64 top = ((uint64)stack + stacksize) & ~15UL;
  uint64 block = (top - __tls_size()) & ~15UL;
  void *tp = __tls_init((void *)block);

  t->stack = stack;
  t->stacksize = stacksize;
  t->tid = 0;

  uint64 flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS |
                 CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
  int tid = __clone(flags, (void *)block, &t->tid, tp, &t->tid, fn, arg);
  if (tid < 0) {
    munmap(stack, stacksize);
    t->stack = 0;
    return -1;
  }
  return tid;
}

// wait for the thread to leave and give its stack back. the kernel
// zeroes t->tid as the thread exits and wakes whoever waits on it.
int
thread_join(thread_t *t)
{
  int tid;

  while ((tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE)) != 0)
    futex((uint32 *)&t->tid, FUTEX_WAIT, (uint32)tid, 0, 0, 0);
  if (t->stack) {
    munmap(t->stack, t->stacksize);
    t->stack = 0;
  }
  return 0;
}

void
thread_exit(int code)
{
  exit(code); // exit() ends this thread; exit_group() ends the process
}

// ---- mutexes ----

void
mutex_init(mutex_t *m)
{
  __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
}

int
mutex_trylock(mutex_t *m)
{
  uint32 want = 0;
  return __atomic_compare_exchange_n(&m->state, &want, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : -1;
}

void
mutex_lock(mutex_t *m)
{
  uint32 want = 0;

  if (__atomic_compare_exchange_n(&m->state, &want, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return; // uncontended: no system call at all

  // contended: say so, then sleep until the holder says it is free
  uint32 c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    futex(&m->state, FUTEX_WAIT, 2, 0, 0, 0);
    c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  }
}

void
mutex_unlock(mutex_t *m)
{
  if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE) != 1) {
    __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
    futex(&m->state, FUTEX_WAKE, 1, 0, 0, 0);
  }
}

// ---- condition variables ----

void
cond_init(cond_t *c)
{
  __atomic_store_n(&c->seq, 0, __ATOMIC_RELEASE);
}

void
cond_wait(cond_t *c, mutex_t *m)
{
  uint32 seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);

  mutex_unlock(m);
  futex(&c->seq, FUTEX_WAIT, seq, 0, 0, 0); // a lost signal only wakes early
  mutex_lock(m);
}

void
cond_signal(cond_t *c)
{
  __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
  futex(&c->seq, FUTEX_WAKE, 1, 0, 0, 0);
}

void
cond_broadcast(cond_t *c)
{
  __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
  futex(&c->seq, FUTEX_WAKE, WAKE_ALL, 0, 0, 0);
}

// ---- barriers ----

void
barrier_init(barrier_t *b, int n)
{
  mutex_init(&b->m);
  cond_init(&b->c);
  b->count = 0;
  b->total = n;
  b->round = 0;
}

void
barrier_wait(barrier_t *b)
{
  mutex_lock(&b->m);
  int round = b->round;
  if (++b->count == b->total) {
    b->count = 0;
    b->round++;
    cond_broadcast(&b->c);
  } else {
    while (b->round == round)
      cond_wait(&b->c, &b->m);
  }
  mutex_unlock(&b->m);
}
