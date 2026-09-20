#ifndef _SCHED_H
#define _SCHED_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include "kernel/thread.h" // the CLONE_* flags

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5

struct sched_param {
  int sched_priority;
};

#define CPU_SETSIZE 64
typedef struct {
  unsigned long bits[CPU_SETSIZE / 64];
} cpu_set_t;
#define CPU_ZERO(s)      ((s)->bits[0] = 0)
#define CPU_SET(c, s)    ((s)->bits[(c) / 64] |= 1UL << ((c) % 64))
#define CPU_CLR(c, s)    ((s)->bits[(c) / 64] &= ~(1UL << ((c) % 64)))
#define CPU_ISSET(c, s)  (((s)->bits[(c) / 64] >> ((c) % 64)) & 1)
#define CPU_COUNT(s)     __cpu_count(s)
int __cpu_count(const cpu_set_t *s);

int sched_yield(void);
int sched_getcpu(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *p);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param *p);
int sched_getparam(pid_t pid, struct sched_param *p);
int sched_rr_get_interval(pid_t pid, struct timespec *ts);
int sched_setaffinity(pid_t pid, size_t n, const cpu_set_t *set);
int sched_getaffinity(pid_t pid, size_t n, cpu_set_t *set);
int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...);
#endif
