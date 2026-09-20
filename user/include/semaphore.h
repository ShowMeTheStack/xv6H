#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
typedef struct {
  int value;        // waited on with a futex when it would block
  unsigned int waiters;
} sem_t;
#define SEM_FAILED ((sem_t *)0)
int sem_init(sem_t *s, int pshared, unsigned int value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abs);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);
sem_t *sem_open(const char *name, int flags, ...);
int sem_close(sem_t *s);
int sem_unlink(const char *name);
#endif
