#ifndef _PTHREAD_H
#define _PTHREAD_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <sched.h>
#include "kernel/signal.h"

// a thread is named by its kernel id; the library keeps what it needs
// in a control block the id maps to.
typedef unsigned long pthread_t;

typedef struct {
  unsigned int state;   // 0 free, 1 held, 2 held with waiters
  int type;             // PTHREAD_MUTEX_NORMAL, _RECURSIVE, _ERRORCHECK
  int count;            // recursion depth
  pthread_t owner;
} pthread_mutex_t;

typedef struct {
  int type;
  int pshared;
  int protocol;
  int robust;
} pthread_mutexattr_t;

typedef struct {
  unsigned int seq;     // bumped on every signal, waited on with a futex
  unsigned int waiters;
  pthread_mutex_t *m;
  clockid_t clock;
} pthread_cond_t;

typedef struct {
  int pshared;
  clockid_t clock;
} pthread_condattr_t;

typedef struct {
  unsigned int state;   // writer bit and reader count
  unsigned int waiters;
} pthread_rwlock_t;

typedef struct {
  int pshared;
} pthread_rwlockattr_t;

typedef struct {
  unsigned int locked;
} pthread_spinlock_t;

typedef struct {
  pthread_mutex_t m;
  pthread_cond_t c;
  unsigned int count, total, round;
} pthread_barrier_t;

typedef struct {
  int pshared;
} pthread_barrierattr_t;

typedef struct {
  unsigned int done;
} pthread_once_t;

typedef unsigned int pthread_key_t;

typedef struct {
  void *stackaddr;
  size_t stacksize;
  size_t guardsize;
  int detachstate;
  int inheritsched;
  int schedpolicy;
  struct sched_param schedparam;
  int scope;
} pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, 1, 0, 0}
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {0, 2, 0, 0}
#define PTHREAD_COND_INITIALIZER {0, 0, 0, 0}
#define PTHREAD_RWLOCK_INITIALIZER {0, 0}
#define PTHREAD_ONCE_INIT {0}

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL
#define PTHREAD_PROCESS_PRIVATE  0
#define PTHREAD_PROCESS_SHARED   1
#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1
#define PTHREAD_INHERIT_SCHED    0
#define PTHREAD_EXPLICIT_SCHED   1
#define PTHREAD_SCOPE_SYSTEM     0
#define PTHREAD_SCOPE_PROCESS    1
#define PTHREAD_CANCEL_ENABLE    0
#define PTHREAD_CANCEL_DISABLE   1
#define PTHREAD_CANCEL_DEFERRED  0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void *)-1)
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_KEYS_MAX 64
#define PTHREAD_STACK_MIN 16384

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_tryjoin_np(pthread_t t, void **ret);
int pthread_detach(pthread_t t);
void pthread_exit(void *ret) __noreturn;
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_kill(pthread_t t, int sig);
int pthread_getattr_np(pthread_t t, pthread_attr_t *attr);
int pthread_setname_np(pthread_t t, const char *name);
int pthread_getname_np(pthread_t t, char *name, size_t n);

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n);
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n);
int pthread_attr_setguardsize(pthread_attr_t *a, size_t n);
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int pthread_attr_setschedpolicy(pthread_attr_t *a, int policy);
int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *policy);
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p);
int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p);
int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit);
int pthread_attr_getinheritsched(const pthread_attr_t *a, int *inherit);
int pthread_attr_setscope(pthread_attr_t *a, int scope);
int pthread_attr_getscope(const pthread_attr_t *a, int *scope);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *shared);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int robust);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *a, int *robust);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *p);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clk);
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clk);
int pthread_condattr_setpshared(pthread_condattr_t *a, int shared);
int pthread_condattr_getpshared(const pthread_condattr_t *a, int *shared);

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *abs);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *abs);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);

int pthread_spin_init(pthread_spinlock_t *l, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *l);
int pthread_spin_lock(pthread_spinlock_t *l);
int pthread_spin_trylock(pthread_spinlock_t *l);
int pthread_spin_unlock(pthread_spinlock_t *l);

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned int n);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *a);
int pthread_barrierattr_destroy(pthread_barrierattr_t *a);

int pthread_once(pthread_once_t *once, void (*fn)(void));
int pthread_key_create(pthread_key_t *key, void (*dtor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_setcancelstate(int state, int *old);
int pthread_setcanceltype(int type, int *old);
int pthread_cancel(pthread_t t);
void pthread_testcancel(void);
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p);
int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p);
int pthread_setschedprio(pthread_t t, int prio);
int pthread_getconcurrency(void);
int pthread_setconcurrency(int n);

// cleanup handlers: a linked list on the thread's own stack
struct __ptcleanup {
  void (*fn)(void *);
  void *arg;
  struct __ptcleanup *next;
};
void __pthread_cleanup_push(struct __ptcleanup *c, void (*fn)(void *), void *arg);
void __pthread_cleanup_pop(struct __ptcleanup *c, int run);
#define pthread_cleanup_push(f, a) do { struct __ptcleanup __c; __pthread_cleanup_push(&__c, (f), (a));
#define pthread_cleanup_pop(run)   __pthread_cleanup_pop(&__c, (run)); } while (0)
#endif
