//
// <pthread.h> and <semaphore.h>.
//
// a thread is a clone that shares everything, with a control block at
// the top of its own stack; tp points at that block, so a thread finds
// itself without a system call. every lock here is the same shape: an
// atomic word that costs nothing when it is uncontended, and a futex
// wait on that word when it is not.
//
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include "kernel/riscv.h"
#include "kernel/thread.h"
#include "user/thread.h"
#include "libc.h"
#include "libc.h"

#define WAKE_ALL 0x7fffffff

// what a thread needs to know about itself, beyond the control block
// that sits below its thread pointer.
struct pthread {
  void *(*fn)(void *);
  void *arg;
  void *ret;
  int tid;                 // the kernel zeroes it when the thread exits
  int detached;
  int cancelstate, canceltype, cancelrequested;
  void *stack;
  size_t stacksize;
  struct __ptcleanup *cleanup;
  void *tls[PTHREAD_KEYS_MAX];
  struct pthread *next;    // every thread, so pthread_kill can find one
  char name[16];
};

static struct pthread main_thread;
static struct pthread *threads = &main_thread;
static mutex_t threads_lock = MUTEX_INIT;
static void (*key_dtor[PTHREAD_KEYS_MAX])(void *);
static unsigned key_used;
static mutex_t key_lock = MUTEX_INIT;

// the thread's own structure, which its control block points at. the
// first thread's block was laid out by crt.c before this library knew
// anything about threads, so the first call here adopts it.
static struct pthread *
self(void)
{
  char *tp;

  asm volatile("mv %0, tp" : "=r"(tp));
  struct __tcb *tcb = (struct __tcb *)(tp - sizeof(struct __tcb));
  if (tcb->self == (void *)tcb) {
    main_thread.tid = (int)tcb->tid;
    tcb->self = &main_thread;
  }
  return tcb->self;
}

pthread_t
pthread_self(void)
{
  return (pthread_t)self();
}

int
pthread_equal(pthread_t a, pthread_t b)
{
  return a == b;
}

// ---- creating and ending threads ----

static void
run_thread(void *arg)
{
  struct pthread *p = arg;

  p->ret = p->fn(p->arg);
  pthread_exit(p->ret);
}

int
pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
  size_t stacksize = attr && attr->stacksize ? attr->stacksize : 128 * 1024;
  int detached = attr ? attr->detachstate == PTHREAD_CREATE_DETACHED : 0;

  stacksize = (stacksize + PGSIZE - 1) & ~(size_t)(PGSIZE - 1);
  if (stacksize < PTHREAD_STACK_MIN)
    stacksize = PTHREAD_STACK_MIN;

  char *stack = mmap(0, stacksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED)
    return EAGAIN;

  // the top of the stack holds, from the top down, the thread-local
  // storage, the control block the thread pointer sits above, and the
  // thread's own structure.
  uint64 top = ((uint64)stack + stacksize) & ~15UL;
  uint64 block = (top - __tls_size()) & ~15UL;
  void *tp = __tls_init((void *)block);
  struct pthread *p = (struct pthread *)((block - sizeof(struct pthread)) & ~15UL);
  memset(p, 0, sizeof(*p));
  ((struct __tcb *)((char *)tp - sizeof(struct __tcb)))->self = p;
  p->fn = fn;
  p->arg = arg;
  p->stack = stack;
  p->stacksize = stacksize;
  p->detached = detached;
  p->cancelstate = PTHREAD_CANCEL_ENABLE;
  p->canceltype = PTHREAD_CANCEL_DEFERRED;

  mutex_lock(&threads_lock);
  p->next = threads;
  threads = p;
  mutex_unlock(&threads_lock);

  uint64 flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS |
                 CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
  int tid = __clone(flags, (void *)p, &p->tid, tp, &p->tid, run_thread, p);
  if (tid < 0) {
    mutex_lock(&threads_lock);
    struct pthread **pp = &threads;
    while (*pp && *pp != p)
      pp = &(*pp)->next;
    if (*pp)
      *pp = p->next;
    mutex_unlock(&threads_lock);
    munmap(stack, stacksize);
    return EAGAIN;
  }
  *t = (pthread_t)p;
  return 0;
}

// the last thing a thread does: run its cleanup handlers and the
// destructors of its thread-specific data, then leave. the stack is
// freed by whoever joins it, because the thread is still standing on
// it until the kernel takes it away.
void
pthread_exit(void *ret)
{
  struct pthread *p = self();

  while (p->cleanup) {
    struct __ptcleanup *c = p->cleanup;
    p->cleanup = c->next;
    c->fn(c->arg);
  }
  for (int round = 0; round < 4; round++) {
    int any = 0;
    for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
      void *v = p->tls[k];
      if (v && key_dtor[k]) {
        p->tls[k] = 0;
        key_dtor[k](v);
        any = 1;
      }
    }
    if (!any)
      break;
  }
  p->ret = ret;

  if (p == &main_thread) {
    // the last thing the first thread does is end the process, once
    // every other thread has finished.
    exit(0);
  }
  if (p->detached) {
    mutex_lock(&threads_lock);
    struct pthread **pp = &threads;
    while (*pp && *pp != p)
      pp = &(*pp)->next;
    if (*pp)
      *pp = p->next;
    mutex_unlock(&threads_lock);
    // the mapping cannot be freed from the thread standing on it; the
    // kernel drops it when the address space goes, and a detached
    // thread's stack is reused only after that.
  }
  _exit(0); // ends this thread, not the process
}

int
pthread_join(pthread_t t, void **ret)
{
  struct pthread *p = (struct pthread *)t;
  int tid;

  if (p == self())
    return EDEADLK;
  if (p->detached)
    return EINVAL;
  while ((tid = __atomic_load_n(&p->tid, __ATOMIC_ACQUIRE)) != 0)
    futex((uint32 *)&p->tid, FUTEX_WAIT, (uint32)tid, 0, 0, 0);
  if (ret)
    *ret = p->ret;

  mutex_lock(&threads_lock);
  struct pthread **pp = &threads;
  while (*pp && *pp != p)
    pp = &(*pp)->next;
  if (*pp)
    *pp = p->next;
  mutex_unlock(&threads_lock);

  void *stack = p->stack;
  size_t size = p->stacksize;
  if (stack)
    munmap(stack, size); // the control block lives on it: nothing may touch p now
  return 0;
}

int
pthread_tryjoin_np(pthread_t t, void **ret)
{
  struct pthread *p = (struct pthread *)t;

  if (__atomic_load_n(&p->tid, __ATOMIC_ACQUIRE) != 0)
    return EBUSY;
  return pthread_join(t, ret);
}

int
pthread_detach(pthread_t t)
{
  struct pthread *p = (struct pthread *)t;

  p->detached = 1;
  return 0;
}

int
pthread_kill(pthread_t t, int sig)
{
  struct pthread *p = (struct pthread *)t;
  int tid = __atomic_load_n(&p->tid, __ATOMIC_ACQUIRE);

  if (tid == 0)
    return ESRCH;
  return tkill(tid, sig) < 0 ? errno : 0;
}

int
pthread_setname_np(pthread_t t, const char *name)
{
  struct pthread *p = (struct pthread *)t;

  strlcpy(p->name, name, sizeof(p->name));
  return 0;
}

int
pthread_getname_np(pthread_t t, char *name, size_t n)
{
  struct pthread *p = (struct pthread *)t;

  strlcpy(name, p->name, n);
  return 0;
}

int
pthread_getattr_np(pthread_t t, pthread_attr_t *attr)
{
  struct pthread *p = (struct pthread *)t;

  memset(attr, 0, sizeof(*attr));
  attr->stackaddr = p->stack;
  attr->stacksize = p->stacksize;
  attr->detachstate = p->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
  return 0;
}

// ---- attributes ----

int
pthread_attr_init(pthread_attr_t *a)
{
  memset(a, 0, sizeof(*a));
  a->stacksize = 128 * 1024;
  a->guardsize = PGSIZE;
  return 0;
}

int
pthread_attr_destroy(pthread_attr_t *a)
{
  (void)a;
  return 0;
}

int
pthread_attr_setstacksize(pthread_attr_t *a, size_t n)
{
  if (n < PTHREAD_STACK_MIN)
    return EINVAL;
  a->stacksize = n;
  return 0;
}

int
pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n)
{
  *n = a->stacksize;
  return 0;
}

int
pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n)
{
  a->stackaddr = addr;
  a->stacksize = n;
  return 0;
}

int
pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n)
{
  *addr = a->stackaddr;
  *n = a->stacksize;
  return 0;
}

int
pthread_attr_setguardsize(pthread_attr_t *a, size_t n)
{
  a->guardsize = n;
  return 0;
}

int
pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n)
{
  *n = a->guardsize;
  return 0;
}

int
pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
  a->detachstate = state;
  return 0;
}

int
pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
  *state = a->detachstate;
  return 0;
}

int
pthread_attr_setschedpolicy(pthread_attr_t *a, int policy)
{
  a->schedpolicy = policy;
  return 0;
}

int
pthread_attr_getschedpolicy(const pthread_attr_t *a, int *policy)
{
  *policy = a->schedpolicy;
  return 0;
}

int
pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p)
{
  a->schedparam = *p;
  return 0;
}

int
pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p)
{
  *p = a->schedparam;
  return 0;
}

int
pthread_attr_setinheritsched(pthread_attr_t *a, int inherit)
{
  a->inheritsched = inherit;
  return 0;
}

int
pthread_attr_getinheritsched(const pthread_attr_t *a, int *inherit)
{
  *inherit = a->inheritsched;
  return 0;
}

int
pthread_attr_setscope(pthread_attr_t *a, int scope)
{
  a->scope = scope;
  return 0;
}

int
pthread_attr_getscope(const pthread_attr_t *a, int *scope)
{
  *scope = a->scope;
  return 0;
}

// ---- mutexes ----

int
pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
  m->state = 0;
  m->type = a ? a->type : PTHREAD_MUTEX_NORMAL;
  m->count = 0;
  m->owner = 0;
  return 0;
}

int
pthread_mutex_destroy(pthread_mutex_t *m)
{
  (void)m;
  return 0;
}

int
pthread_mutex_trylock(pthread_mutex_t *m)
{
  pthread_t me = pthread_self();
  unsigned want = 0;

  if (m->type != PTHREAD_MUTEX_NORMAL && m->owner == me) {
    if (m->type == PTHREAD_MUTEX_RECURSIVE) {
      m->count++;
      return 0;
    }
    return EDEADLK;
  }
  if (__atomic_compare_exchange_n(&m->state, &want, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    m->owner = me;
    m->count = 1;
    return 0;
  }
  return EBUSY;
}

int
pthread_mutex_lock(pthread_mutex_t *m)
{
  pthread_t me = pthread_self();
  unsigned want = 0;

  if (m->type != PTHREAD_MUTEX_NORMAL && m->owner == me) {
    if (m->type == PTHREAD_MUTEX_RECURSIVE) {
      m->count++;
      return 0;
    }
    return EDEADLK;
  }
  if (__atomic_compare_exchange_n(&m->state, &want, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    m->owner = me;
    m->count = 1;
    return 0;
  }
  unsigned c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    futex(&m->state, FUTEX_WAIT, 2, 0, 0, 0);
    c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  }
  m->owner = me;
  m->count = 1;
  return 0;
}

int
pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs)
{
  // try, then wait in slices, because the futex timeout is relative
  // and the caller gave an absolute deadline.
  for (;;) {
    if (pthread_mutex_trylock(m) == 0)
      return 0;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long secs = abs->tv_sec - now.tv_sec;
    long nsecs = abs->tv_nsec - now.tv_nsec;
    if (nsecs < 0) {
      nsecs += 1000000000L;
      secs--;
    }
    if (secs < 0)
      return ETIMEDOUT;
    struct timespec wait = {secs, nsecs};
    unsigned c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    if (c == 0) {
      m->owner = pthread_self();
      m->count = 1;
      return 0;
    }
    if (futex(&m->state, FUTEX_WAIT, 2, &wait, 0, 0) < 0 && errno == ETIMEDOUT)
      return ETIMEDOUT;
  }
}

int
pthread_mutex_unlock(pthread_mutex_t *m)
{
  if (m->type == PTHREAD_MUTEX_RECURSIVE && m->count > 1) {
    m->count--;
    return 0;
  }
  if (m->type == PTHREAD_MUTEX_ERRORCHECK && m->owner != pthread_self())
    return EPERM;
  m->owner = 0;
  m->count = 0;
  if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE) != 1) {
    __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
    futex(&m->state, FUTEX_WAKE, 1, 0, 0, 0);
  }
  return 0;
}

int
pthread_mutexattr_init(pthread_mutexattr_t *a)
{
  memset(a, 0, sizeof(*a));
  return 0;
}

int
pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
  (void)a;
  return 0;
}

int
pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
  if (type < 0 || type > PTHREAD_MUTEX_ERRORCHECK)
    return EINVAL;
  a->type = type;
  return 0;
}

int
pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
  *type = a->type;
  return 0;
}

int
pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared)
{
  a->pshared = shared;
  return 0;
}

int
pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *shared)
{
  *shared = a->pshared;
  return 0;
}

int
pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int robust)
{
  a->robust = robust;
  return 0;
}

int
pthread_mutexattr_getrobust(const pthread_mutexattr_t *a, int *robust)
{
  *robust = a->robust;
  return 0;
}

int
pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p)
{
  a->protocol = p;
  return 0;
}

int
pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *p)
{
  *p = a->protocol;
  return 0;
}

// ---- condition variables ----

int
pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
  c->seq = 0;
  c->waiters = 0;
  c->m = 0;
  c->clock = a ? a->clock : CLOCK_REALTIME;
  return 0;
}

int
pthread_cond_destroy(pthread_cond_t *c)
{
  (void)c;
  return 0;
}

int
pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
  unsigned seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);

  c->m = m;
  __atomic_fetch_add(&c->waiters, 1, __ATOMIC_RELAXED);
  pthread_mutex_unlock(m);
  futex(&c->seq, FUTEX_WAIT, seq, 0, 0, 0);
  __atomic_fetch_sub(&c->waiters, 1, __ATOMIC_RELAXED);
  pthread_mutex_lock(m);
  return 0;
}

int
pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs)
{
  unsigned seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
  struct timespec now, wait;

  clock_gettime(c->clock, &now);
  wait.tv_sec = abs->tv_sec - now.tv_sec;
  wait.tv_nsec = abs->tv_nsec - now.tv_nsec;
  if (wait.tv_nsec < 0) {
    wait.tv_nsec += 1000000000L;
    wait.tv_sec--;
  }
  if (wait.tv_sec < 0)
    return ETIMEDOUT;

  c->m = m;
  __atomic_fetch_add(&c->waiters, 1, __ATOMIC_RELAXED);
  pthread_mutex_unlock(m);
  int r = futex(&c->seq, FUTEX_WAIT, seq, &wait, 0, 0);
  int e = errno;
  __atomic_fetch_sub(&c->waiters, 1, __ATOMIC_RELAXED);
  pthread_mutex_lock(m);
  return (r < 0 && e == ETIMEDOUT) ? ETIMEDOUT : 0;
}

int
pthread_cond_signal(pthread_cond_t *c)
{
  __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
  futex(&c->seq, FUTEX_WAKE, 1, 0, 0, 0);
  return 0;
}

int
pthread_cond_broadcast(pthread_cond_t *c)
{
  __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
  futex(&c->seq, FUTEX_WAKE, WAKE_ALL, 0, 0, 0);
  return 0;
}

int
pthread_condattr_init(pthread_condattr_t *a)
{
  a->pshared = 0;
  a->clock = CLOCK_REALTIME;
  return 0;
}

int
pthread_condattr_destroy(pthread_condattr_t *a)
{
  (void)a;
  return 0;
}

int
pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clk)
{
  a->clock = clk;
  return 0;
}

int
pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clk)
{
  *clk = a->clock;
  return 0;
}

int
pthread_condattr_setpshared(pthread_condattr_t *a, int shared)
{
  a->pshared = shared;
  return 0;
}

int
pthread_condattr_getpshared(const pthread_condattr_t *a, int *shared)
{
  *shared = a->pshared;
  return 0;
}

// ---- read-write locks ----
//
// the state word holds the number of readers, with the top bit set
// while a writer holds it. writers wait on the word itself.

#define RW_WRITER 0x80000000u

int
pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a)
{
  (void)a;
  l->state = 0;
  l->waiters = 0;
  return 0;
}

int
pthread_rwlock_destroy(pthread_rwlock_t *l)
{
  (void)l;
  return 0;
}

int
pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
  unsigned s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);

  if (s & RW_WRITER)
    return EBUSY;
  return __atomic_compare_exchange_n(&l->state, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
             ? 0
             : EBUSY;
}

int
pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
  for (;;) {
    unsigned s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    if (s & RW_WRITER) {
      __atomic_fetch_add(&l->waiters, 1, __ATOMIC_RELAXED);
      futex(&l->state, FUTEX_WAIT, s, 0, 0, 0);
      __atomic_fetch_sub(&l->waiters, 1, __ATOMIC_RELAXED);
      continue;
    }
    if (__atomic_compare_exchange_n(&l->state, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return 0;
  }
}

int
pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
  unsigned want = 0;

  return __atomic_compare_exchange_n(&l->state, &want, RW_WRITER, 0, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)
             ? 0
             : EBUSY;
}

int
pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
  for (;;) {
    unsigned want = 0;
    if (__atomic_compare_exchange_n(&l->state, &want, RW_WRITER, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
      return 0;
    unsigned s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    if (s == 0)
      continue;
    __atomic_fetch_add(&l->waiters, 1, __ATOMIC_RELAXED);
    futex(&l->state, FUTEX_WAIT, s, 0, 0, 0);
    __atomic_fetch_sub(&l->waiters, 1, __ATOMIC_RELAXED);
  }
}

int
pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *abs)
{
  (void)abs;
  return pthread_rwlock_rdlock(l);
}

int
pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *abs)
{
  (void)abs;
  return pthread_rwlock_wrlock(l);
}

int
pthread_rwlock_unlock(pthread_rwlock_t *l)
{
  unsigned s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);

  if (s & RW_WRITER)
    __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
  else
    __atomic_fetch_sub(&l->state, 1, __ATOMIC_RELEASE);
  if (__atomic_load_n(&l->waiters, __ATOMIC_RELAXED))
    futex(&l->state, FUTEX_WAKE, WAKE_ALL, 0, 0, 0);
  return 0;
}

// ---- spin locks ----

int
pthread_spin_init(pthread_spinlock_t *l, int pshared)
{
  (void)pshared;
  l->locked = 0;
  return 0;
}

int
pthread_spin_destroy(pthread_spinlock_t *l)
{
  (void)l;
  return 0;
}

int
pthread_spin_lock(pthread_spinlock_t *l)
{
  while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
    sched_yield(); // one hart may be all there is: do not spin blindly
  return 0;
}

int
pthread_spin_trylock(pthread_spinlock_t *l)
{
  return __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int
pthread_spin_unlock(pthread_spinlock_t *l)
{
  __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
  return 0;
}

// ---- barriers ----

int
pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned int n)
{
  (void)a;
  if (n == 0)
    return EINVAL;
  pthread_mutex_init(&b->m, 0);
  pthread_cond_init(&b->c, 0);
  b->count = 0;
  b->total = n;
  b->round = 0;
  return 0;
}

int
pthread_barrier_destroy(pthread_barrier_t *b)
{
  (void)b;
  return 0;
}

int
pthread_barrier_wait(pthread_barrier_t *b)
{
  pthread_mutex_lock(&b->m);
  unsigned round = b->round;
  if (++b->count == b->total) {
    b->count = 0;
    b->round++;
    pthread_cond_broadcast(&b->c);
    pthread_mutex_unlock(&b->m);
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }
  while (b->round == round)
    pthread_cond_wait(&b->c, &b->m);
  pthread_mutex_unlock(&b->m);
  return 0;
}

int
pthread_barrierattr_init(pthread_barrierattr_t *a)
{
  a->pshared = 0;
  return 0;
}

int
pthread_barrierattr_destroy(pthread_barrierattr_t *a)
{
  (void)a;
  return 0;
}

// ---- once, and thread-specific data ----

int
pthread_once(pthread_once_t *once, void (*fn)(void))
{
  unsigned want = 0;

  // 0 not run, 1 running, 2 done
  if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) == 2)
    return 0;
  if (__atomic_compare_exchange_n(&once->done, &want, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    fn();
    __atomic_store_n(&once->done, 2, __ATOMIC_RELEASE);
    futex(&once->done, FUTEX_WAKE, WAKE_ALL, 0, 0, 0);
    return 0;
  }
  while (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) != 2)
    futex(&once->done, FUTEX_WAIT, 1, 0, 0, 0);
  return 0;
}

int
pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
  mutex_lock(&key_lock);
  for (int i = 0; i < PTHREAD_KEYS_MAX; i++) {
    if (!(key_used & (1u << (i % 32))) || i >= 32) {
      if (i < 32 && (key_used & (1u << i)))
        continue;
      if (i < 32)
        key_used |= 1u << i;
      key_dtor[i] = dtor;
      *key = i;
      mutex_unlock(&key_lock);
      return 0;
    }
  }
  mutex_unlock(&key_lock);
  return EAGAIN;
}

int
pthread_key_delete(pthread_key_t key)
{
  if (key >= PTHREAD_KEYS_MAX)
    return EINVAL;
  mutex_lock(&key_lock);
  if (key < 32)
    key_used &= ~(1u << key);
  key_dtor[key] = 0;
  mutex_unlock(&key_lock);
  return 0;
}

void *
pthread_getspecific(pthread_key_t key)
{
  if (key >= PTHREAD_KEYS_MAX)
    return 0;
  struct pthread *p = (struct pthread *)pthread_self();
  return p->tls[key];
}

int
pthread_setspecific(pthread_key_t key, const void *value)
{
  if (key >= PTHREAD_KEYS_MAX)
    return EINVAL;
  struct pthread *p = (struct pthread *)pthread_self();
  p->tls[key] = (void *)value;
  return 0;
}

// ---- cancellation ----
//
// cancellation is a request the thread honours at a cancellation
// point. asynchronous cancellation is accepted and treated as
// deferred, which is allowed to be the whole of it.

int
pthread_setcancelstate(int state, int *old)
{
  struct pthread *p = (struct pthread *)pthread_self();

  if (old)
    *old = p->cancelstate;
  p->cancelstate = state;
  return 0;
}

int
pthread_setcanceltype(int type, int *old)
{
  struct pthread *p = (struct pthread *)pthread_self();

  if (old)
    *old = p->canceltype;
  p->canceltype = type;
  return 0;
}

int
pthread_cancel(pthread_t t)
{
  struct pthread *p = (struct pthread *)t;

  p->cancelrequested = 1;
  int tid = __atomic_load_n(&p->tid, __ATOMIC_ACQUIRE);
  if (tid)
    tkill(tid, SIGCONT); // wake it out of a blocking call
  return 0;
}

void
pthread_testcancel(void)
{
  struct pthread *p = (struct pthread *)pthread_self();

  if (p->cancelrequested && p->cancelstate == PTHREAD_CANCEL_ENABLE)
    pthread_exit(PTHREAD_CANCELED);
}

void
__pthread_cleanup_push(struct __ptcleanup *c, void (*fn)(void *), void *arg)
{
  struct pthread *p = (struct pthread *)pthread_self();

  c->fn = fn;
  c->arg = arg;
  c->next = p->cleanup;
  p->cleanup = c;
}

void
__pthread_cleanup_pop(struct __ptcleanup *c, int run)
{
  struct pthread *p = (struct pthread *)pthread_self();

  if (p->cleanup == c)
    p->cleanup = c->next;
  if (run)
    c->fn(c->arg);
}

int
pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
  (void)prepare;
  (void)parent;
  (void)child;
  return 0; // fork from a threaded process leaves one thread: nothing to do
}

int
pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p)
{
  (void)t;
  (void)policy;
  (void)p;
  return 0;
}

int
pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p)
{
  (void)t;
  *policy = SCHED_OTHER;
  p->sched_priority = 0;
  return 0;
}

int
pthread_setschedprio(pthread_t t, int prio)
{
  (void)t;
  (void)prio;
  return 0;
}

int
pthread_getconcurrency(void)
{
  return 0;
}

int
pthread_setconcurrency(int n)
{
  (void)n;
  return 0;
}

int
pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
  return sigprocmask(how, set, old) < 0 ? errno : 0;
}

// ---- semaphores ----

int
sem_init(sem_t *s, int pshared, unsigned int value)
{
  (void)pshared;
  s->value = value;
  s->waiters = 0;
  return 0;
}

int
sem_destroy(sem_t *s)
{
  (void)s;
  return 0;
}

int
sem_trywait(sem_t *s)
{
  int v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);

  while (v > 0) {
    if (__atomic_compare_exchange_n(&s->value, &v, v - 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return 0;
  }
  errno = EAGAIN;
  return -1;
}

int
sem_wait(sem_t *s)
{
  for (;;) {
    if (sem_trywait(s) == 0)
      return 0;
    __atomic_fetch_add(&s->waiters, 1, __ATOMIC_RELAXED);
    futex((uint32 *)&s->value, FUTEX_WAIT, 0, 0, 0, 0);
    __atomic_fetch_sub(&s->waiters, 1, __ATOMIC_RELAXED);
    if (errno == EINTR)
      return -1;
  }
}

int
sem_timedwait(sem_t *s, const struct timespec *abs)
{
  for (;;) {
    if (sem_trywait(s) == 0)
      return 0;
    struct timespec now, wait;
    clock_gettime(CLOCK_REALTIME, &now);
    wait.tv_sec = abs->tv_sec - now.tv_sec;
    wait.tv_nsec = abs->tv_nsec - now.tv_nsec;
    if (wait.tv_nsec < 0) {
      wait.tv_nsec += 1000000000L;
      wait.tv_sec--;
    }
    if (wait.tv_sec < 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    __atomic_fetch_add(&s->waiters, 1, __ATOMIC_RELAXED);
    int r = futex((uint32 *)&s->value, FUTEX_WAIT, 0, &wait, 0, 0);
    __atomic_fetch_sub(&s->waiters, 1, __ATOMIC_RELAXED);
    if (r < 0 && (errno == ETIMEDOUT || errno == EINTR))
      return -1;
  }
}

int
sem_post(sem_t *s)
{
  __atomic_fetch_add(&s->value, 1, __ATOMIC_RELEASE);
  if (__atomic_load_n(&s->waiters, __ATOMIC_RELAXED))
    futex((uint32 *)&s->value, FUTEX_WAKE, 1, 0, 0, 0);
  return 0;
}

int
sem_getvalue(sem_t *s, int *value)
{
  *value = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
  return 0;
}

sem_t *
sem_open(const char *name, int flags, ...)
{
  (void)name;
  (void)flags;
  errno = ENOSYS; // named semaphores need a file system for them
  return SEM_FAILED;
}

int
sem_close(sem_t *s)
{
  (void)s;
  errno = ENOSYS;
  return -1;
}

int
sem_unlink(const char *name)
{
  (void)name;
  errno = ENOSYS;
  return -1;
}

// ---- scheduling ----

// the raw system calls; sched.h declares only the POSIX shapes
int __sched_setaffinity(pid_t pid, size_t n, const unsigned long *mask);
int __sched_getaffinity(pid_t pid, size_t n, unsigned long *mask);
int __sched_yield(void);
int __getcpu(void);

int
sched_yield(void)
{
  return __sched_yield();
}

int
sched_getcpu(void)
{
  return __getcpu();
}

int
sched_get_priority_min(int policy)
{
  (void)policy;
  return 0;
}

int
sched_get_priority_max(int policy)
{
  (void)policy;
  return 0;
}

int
sched_setscheduler(pid_t pid, int policy, const struct sched_param *p)
{
  (void)pid;
  (void)policy;
  (void)p;
  return 0;
}

int
sched_getscheduler(pid_t pid)
{
  (void)pid;
  return SCHED_OTHER;
}

int
sched_setparam(pid_t pid, const struct sched_param *p)
{
  (void)pid;
  (void)p;
  return 0;
}

int
sched_getparam(pid_t pid, struct sched_param *p)
{
  (void)pid;
  p->sched_priority = 0;
  return 0;
}

int
sched_rr_get_interval(pid_t pid, struct timespec *ts)
{
  (void)pid;
  ts->tv_sec = 0;
  ts->tv_nsec = 10000000L;
  return 0;
}

// the kernel speaks one 64-bit word of harts, which is every hart this
// machine can have; the cpu_set_t a caller passes may be larger.
int
sched_setaffinity(pid_t pid, size_t n, const cpu_set_t *set)
{
  unsigned long mask;

  if (n < sizeof(unsigned long) || set == 0) {
    errno = EINVAL;
    return -1;
  }
  mask = set->bits[0];
  return __sched_setaffinity(pid, sizeof(mask), &mask);
}

int
sched_getaffinity(pid_t pid, size_t n, cpu_set_t *set)
{
  unsigned long mask = 0;

  if (n < sizeof(unsigned long) || set == 0) {
    errno = EINVAL;
    return -1;
  }
  if (__sched_getaffinity(pid, sizeof(mask), &mask) < 0)
    return -1;
  CPU_ZERO(set);
  set->bits[0] = mask;
  return 0;
}

int
__cpu_count(const cpu_set_t *s)
{
  int n = 0;

  for (int i = 0; i < CPU_SETSIZE / 64; i++)
    n += __builtin_popcountl(s->bits[i]);
  return n;
}
