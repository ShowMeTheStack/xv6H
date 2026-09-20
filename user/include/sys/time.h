#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include "kernel/posix.h" // struct timeval
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
struct itimerval {
  struct timeval it_interval, it_value;
};
int gettimeofday(struct timeval *tv, void *tz);
int settimeofday(const struct timeval *tv, const void *tz);
int getitimer(int which, struct itimerval *v);
int setitimer(int which, const struct itimerval *v, struct itimerval *old);
int utimes(const char *path, const struct timeval t[2]);
#define timeradd(a, b, r) do { \
  (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
  (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
  if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } \
} while (0)
#define timersub(a, b, r) do { \
  (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
  (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
  if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } \
} while (0)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timercmp(a, b, op) \
  ((a)->tv_sec == (b)->tv_sec ? (a)->tv_usec op (b)->tv_usec : (a)->tv_sec op (b)->tv_sec)
#endif
