// wall-clock helpers for user programs: the clock is the RTC, read
// through clock_gettime; calendar conversion is the proleptic Gregorian
// calendar, UTC (xv6 has no time zones).
#include "kernel/types.h"
#include "kernel/time.h"
#include "user/user.h"

long
time(long *tloc)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    return -1;
  if (tloc)
    *tloc = ts.sec;
  return ts.sec;
}

// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm).
static void
civil_from_days(long z, int *y, int *m, int *d)
{
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = yoe + era * 400;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yy + (*m <= 2);
}

static long
days_from_civil(long y, int m, int d)
{
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void
gmtime_r(long secs, struct tm *tm)
{
  long days = secs / 86400, rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    days--;
  }
  tm->hour = rem / 3600;
  tm->min = (rem % 3600) / 60;
  tm->sec = rem % 60;
  civil_from_days(days, &tm->year, &tm->mon, &tm->mday);
  tm->wday = (days + 4) % 7; // 1970-01-01 was a Thursday
  if (tm->wday < 0)
    tm->wday += 7;
  tm->yday = days - days_from_civil(tm->year, 1, 1);
}

long
timegm(struct tm *tm)
{
  return days_from_civil(tm->year, tm->mon, tm->mday) * 86400 + tm->hour * 3600 + tm->min * 60 + tm->sec;
}

static char *wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static char *
two(char *p, int v)
{
  *p++ = '0' + v / 10;
  *p++ = '0' + v % 10;
  return p;
}

char *
ctime_r(long secs, char *buf)
{
  struct tm tm;
  gmtime_r(secs, &tm);
  char *p = buf;
  memmove(p, wdays[tm.wday], 3); p += 3; *p++ = ' ';
  memmove(p, months[tm.mon - 1], 3); p += 3; *p++ = ' ';
  if (tm.mday >= 10) *p++ = '0' + tm.mday / 10; else *p++ = ' ';
  *p++ = '0' + tm.mday % 10; *p++ = ' ';
  p = two(p, tm.hour); *p++ = ':';
  p = two(p, tm.min); *p++ = ':';
  p = two(p, tm.sec);
  memmove(p, " UTC ", 5); p += 5;
  int y = tm.year, n = 0;
  char digits[8];
  do { digits[n++] = '0' + y % 10; y /= 10; } while (y > 0);
  while (n > 0) *p++ = digits[--n];
  *p = 0;
  return buf;
}
