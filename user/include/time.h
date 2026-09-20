#ifndef _TIME_H
#define _TIME_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <sys/types.h>
#include "kernel/time.h" // CLOCK_*, struct timespec

#define CLOCKS_PER_SEC ((clock_t)100)
#define TIMER_ABSTIME 1

struct tm {
  int tm_sec;   // 0-60 (60 for a leap second)
  int tm_min;   // 0-59
  int tm_hour;  // 0-23
  int tm_mday;  // 1-31
  int tm_mon;   // 0-11
  int tm_year;  // years since 1900
  int tm_wday;  // 0-6, Sunday first
  int tm_yday;  // 0-365
  int tm_isdst; // > 0 summer time, 0 not, < 0 unknown
  long tm_gmtoff;
  const char *tm_zone;
};

struct itimerspec {
  struct timespec it_interval, it_value;
};

time_t time(time_t *tloc);
double difftime(time_t a, time_t b);
clock_t clock(void);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *tm);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *tm);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);
size_t strftime(char *s, size_t n, const char *fmt, const struct tm *tm);
char *strptime(const char *s, const char *fmt, struct tm *tm);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;

int clock_gettime(clockid_t clk, struct timespec *ts);
int clock_settime(clockid_t clk, const struct timespec *ts);
int clock_getres(clockid_t clk, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
#endif
