#ifndef USER_THREAD_H
#define USER_THREAD_H
//
// threads over clone() and futex(): a thread is a process that shares
// its address space, descriptors, working directory and signal
// handlers with the ones it was cloned from, and the locks below
// spin briefly in userspace before asking the kernel to sleep.
//
#include "kernel/thread.h"

typedef struct thread {
  void *stack;       // the mapping the thread runs on
  uint64 stacksize;
  int tid;           // the kernel zeroes it and wakes waiters at exit
} thread_t;

int thread_create(thread_t *t, void (*fn)(void *), void *arg, uint64 stacksize);
int thread_join(thread_t *t);
void thread_exit(int code) __attribute__((noreturn));

// a mutex is 0 free, 1 held, 2 held with someone waiting
typedef struct mutex {
  uint32 state;
} mutex_t;
#define MUTEX_INIT {0}
void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int mutex_trylock(mutex_t *m); // 0 if it took the lock
void mutex_unlock(mutex_t *m);

typedef struct cond {
  uint32 seq;
} cond_t;
#define COND_INIT {0}
void cond_init(cond_t *c);
void cond_wait(cond_t *c, mutex_t *m);
void cond_signal(cond_t *c);
void cond_broadcast(cond_t *c);

typedef struct barrier {
  mutex_t m;
  cond_t c;
  int count, total, round;
} barrier_t;
void barrier_init(barrier_t *b, int n);
void barrier_wait(barrier_t *b);

// the raw calls these are built on, for programs that want them
int __clone(uint64 flags, void *stack, int *ptid, void *tls, int *ctid, void (*fn)(void *), void *arg);
int futex(unsigned int *uaddr, int op, unsigned int val, const void *timeout, unsigned int *uaddr2,
          int val3);
int gettid(void);
int tkill(int tid, int sig);
int tgkill(int tgid, int tid, int sig);
int exit_group(int status) __attribute__((noreturn));
int set_tid_address(int *tidptr);

#endif
