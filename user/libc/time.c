//
// <time.h>.
//
// the calendar is the proleptic Gregorian one, and the conversion both
// ways is Howard Hinnant's days-from-civil algorithm, which is exact
// for every year a 64-bit time_t can hold and needs no tables.
//
// local time comes from the TZ variable, in the POSIX form
//   std offset [dst [offset] [,start[/time],end[/time]]]
// for instance IST-5:30 or EST5EDT,M3.2.0,M11.1.0/2. without TZ, or
// with an unparsable one, local time is UTC. reading the zone out of
// /etc/localtime in TZif form comes with the file system work.
//
#include <time.h>
#include <sys/times.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include "libc.h"

char *tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;

static const char *const wday_short[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const wday_long[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
static const char *const mon_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const mon_long[] = {"January", "February", "March", "April", "May", "June",
                                       "July", "August", "September", "October", "November", "December"};

// ---- the calendar ----

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

static int
is_leap(long y)
{
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

// ---- the time zone ----

struct tzrule {
  int month, week, day;  // Mm.w.d
  long jday;             // Jn or n, -1 when unused
  long time;             // seconds after midnight local
};

static struct {
  int valid, parsed;
  char std[16], dst[16];
  long stdoff, dstoff;   // seconds east of UTC
  int hasdst;
  struct tzrule start, end;
} zone;

static const char *
parse_name(const char *p, char *out, size_t n)
{
  size_t i = 0;

  if (*p == '<') {
    p++;
    while (*p && *p != '>') {
      if (i + 1 < n)
        out[i++] = *p;
      p++;
    }
    if (*p == '>')
      p++;
  } else {
    while (isalpha((unsigned char)*p)) {
      if (i + 1 < n)
        out[i++] = *p;
      p++;
    }
  }
  out[i] = 0;
  return i >= 3 ? p : 0;
}

// hh[:mm[:ss]], with the POSIX sign convention: positive is west
static const char *
parse_offset(const char *p, long *out)
{
  int neg = 0;
  long v = 0;

  if (*p == '+')
    p++;
  else if (*p == '-') {
    neg = 1;
    p++;
  }
  if (!isdigit((unsigned char)*p))
    return 0;
  while (isdigit((unsigned char)*p))
    v = v * 10 + (*p++ - '0');
  v *= 3600;
  if (*p == ':') {
    p++;
    long m = 0;
    while (isdigit((unsigned char)*p))
      m = m * 10 + (*p++ - '0');
    v += m * 60;
    if (*p == ':') {
      p++;
      long s = 0;
      while (isdigit((unsigned char)*p))
        s = s * 10 + (*p++ - '0');
      v += s;
    }
  }
  *out = neg ? v : -v; // seconds east of UTC
  return p;
}

static const char *
parse_rule(const char *p, struct tzrule *r)
{
  r->jday = -1;
  r->time = 2 * 3600;
  if (*p == 'M') {
    p++;
    r->month = strtol(p, (char **)&p, 10);
    if (*p != '.')
      return 0;
    p++;
    r->week = strtol(p, (char **)&p, 10);
    if (*p != '.')
      return 0;
    p++;
    r->day = strtol(p, (char **)&p, 10);
  } else if (*p == 'J') {
    p++;
    r->jday = strtol(p, (char **)&p, 10);
    r->month = 0;
  } else {
    r->jday = strtol(p, (char **)&p, 10);
    r->month = -1; // a day of the year counting the leap day
  }
  if (*p == '/') {
    p++;
    long t;
    const char *q = parse_offset(p, &t);
    if (q == 0)
      return 0;
    r->time = -t; // parse_offset flips the sign, and here it should not
    p = q;
  }
  return p;
}

void
tzset(void)
{
  const char *tz = getenv("TZ");

  memset(&zone, 0, sizeof(zone));
  zone.parsed = 1;
  tzname[0] = tzname[1] = "UTC";
  timezone = 0;
  daylight = 0;
  if (tz == 0 || *tz == 0 || strcmp(tz, "UTC") == 0 || strcmp(tz, "UTC0") == 0)
    return;
  if (*tz == ':')
    tz++; // ":Region/City" needs the zone files, which are not here yet

  const char *p = parse_name(tz, zone.std, sizeof(zone.std));
  if (p == 0)
    return;
  p = parse_offset(p, &zone.stdoff);
  if (p == 0)
    return;
  zone.valid = 1;
  zone.dstoff = zone.stdoff + 3600;
  tzname[0] = zone.std;
  timezone = -zone.stdoff;

  if (*p && *p != ',') {
    const char *q = parse_name(p, zone.dst, sizeof(zone.dst));
    if (q) {
      zone.hasdst = 1;
      daylight = 1;
      tzname[1] = zone.dst;
      p = q;
      if (*p && *p != ',') {
        const char *r = parse_offset(p, &zone.dstoff);
        if (r)
          p = r;
      }
    }
  }
  if (zone.hasdst) {
    // the default for the United States since 2007, used when the
    // string names summer time but gives no rules.
    zone.start.month = 3; zone.start.week = 2; zone.start.day = 0;
    zone.start.jday = -1; zone.start.time = 2 * 3600;
    zone.end.month = 11; zone.end.week = 1; zone.end.day = 0;
    zone.end.jday = -1; zone.end.time = 2 * 3600;
    if (*p == ',') {
      const char *q = parse_rule(p + 1, &zone.start);
      if (q && *q == ',') {
        const char *r = parse_rule(q + 1, &zone.end);
        if (r == 0)
          zone.hasdst = 0;
      }
    }
  }
}

// the day of the year a rule names, in the given year
static long
rule_day(const struct tzrule *r, long year)
{
  if (r->jday >= 0) {
    if (r->month == 0) // Jn: 1..365, never counting February 29
      return r->jday - 1 + (r->jday >= 60 && is_leap(year) ? 1 : 0);
    return r->jday;    // n: 0..365, counting it
  }
  // Mm.w.d: the w'th d of month m, with w == 5 meaning the last one
  long first = days_from_civil(year, r->month, 1);
  int wday = (int)((first + 4) % 7);
  if (wday < 0)
    wday += 7;
  int delta = (r->day - wday + 7) % 7;
  long day = 1 + delta + (r->week - 1) * 7;
  static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int last = mdays[r->month - 1] + (r->month == 2 && is_leap(year) ? 1 : 0);
  while (day > last)
    day -= 7;
  return days_from_civil(year, r->month, day) - days_from_civil(year, 1, 1);
}

// is this UTC time inside summer time?
static int
in_dst(time_t t)
{
  if (!zone.valid || !zone.hasdst)
    return 0;

  long local = t + zone.stdoff;
  long days = local / 86400;
  long rem = local % 86400;
  if (rem < 0) {
    rem += 86400;
    days--;
  }
  int y, m, d;
  civil_from_days(days, &y, &m, &d);
  long yday = days - days_from_civil(y, 1, 1);
  long now = yday * 86400 + rem;
  long start = rule_day(&zone.start, y) * 86400 + zone.start.time;
  long end = rule_day(&zone.end, y) * 86400 + zone.end.time;

  if (start < end)
    return now >= start && now < end;
  return now >= start || now < end; // southern hemisphere
}

static long
local_offset(time_t t)
{
  if (!zone.parsed)
    tzset();
  if (!zone.valid)
    return 0;
  return in_dst(t) ? zone.dstoff : zone.stdoff;
}

// ---- conversions ----

static void
fill_tm(time_t t, long off, struct tm *tm)
{
  long secs = t + off;
  long days = secs / 86400, rem = secs % 86400;

  if (rem < 0) {
    rem += 86400;
    days--;
  }
  tm->tm_hour = rem / 3600;
  tm->tm_min = (rem % 3600) / 60;
  tm->tm_sec = rem % 60;
  int y, m, d;
  civil_from_days(days, &y, &m, &d);
  tm->tm_year = y - 1900;
  tm->tm_mon = m - 1;
  tm->tm_mday = d;
  tm->tm_wday = (int)((days + 4) % 7);
  if (tm->tm_wday < 0)
    tm->tm_wday += 7;
  tm->tm_yday = days - days_from_civil(y, 1, 1);
  tm->tm_gmtoff = off;
}

struct tm *
gmtime_r(const time_t *t, struct tm *tm)
{
  fill_tm(*t, 0, tm);
  tm->tm_isdst = 0;
  tm->tm_zone = "UTC";
  return tm;
}

struct tm *
gmtime(const time_t *t)
{
  static struct tm tm;
  return gmtime_r(t, &tm);
}

struct tm *
localtime_r(const time_t *t, struct tm *tm)
{
  long off = local_offset(*t);

  fill_tm(*t, off, tm);
  tm->tm_isdst = (!zone.valid || !zone.hasdst) ? 0 : in_dst(*t);
  tm->tm_zone = tm->tm_isdst ? tzname[1] : tzname[0];
  return tm;
}

struct tm *
localtime(const time_t *t)
{
  static struct tm tm;
  return localtime_r(t, &tm);
}

// the inverse, with the fields put back in range first, as the
// standard requires of mktime.
time_t
timegm(struct tm *tm)
{
  long y = tm->tm_year + 1900L;
  long mon = tm->tm_mon;

  y += mon / 12;
  mon %= 12;
  if (mon < 0) {
    mon += 12;
    y--;
  }
  long days = days_from_civil(y, mon + 1, 1) + (tm->tm_mday - 1);
  long secs = days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;

  // hand back the normalized fields
  fill_tm(secs, 0, tm);
  tm->tm_isdst = 0;
  return secs;
}

time_t
mktime(struct tm *tm)
{
  struct tm copy = *tm;
  time_t utc = timegm(&copy);

  if (!zone.parsed)
    tzset();
  if (!zone.valid) {
    *tm = copy;
    tm->tm_zone = "UTC";
    return utc;
  }

  // the offset depends on the answer, so guess with standard time and
  // correct once, which is right except inside the hour a transition
  // skips or repeats, where the standard leaves the result unspecified.
  long off = zone.stdoff;
  time_t t = utc - off;
  if (tm->tm_isdst > 0)
    off = zone.dstoff;
  else if (tm->tm_isdst < 0)
    off = local_offset(t);
  t = utc - off;
  if (tm->tm_isdst < 0) {
    long off2 = local_offset(t);
    if (off2 != off)
      t = utc - off2;
  }
  localtime_r(&t, tm);
  return t;
}

double
difftime(time_t a, time_t b)
{
  return (double)(a - b);
}

time_t
time(time_t *tloc)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    return (time_t)-1;
  if (tloc)
    *tloc = ts.tv_sec;
  return ts.tv_sec;
}

clock_t
clock(void)
{
  struct tms t;

  if (times(&t) == (clock_t)-1)
    return (clock_t)-1;
  return t.tms_utime + t.tms_stime;
}

// ---- text ----

char *
asctime_r(const struct tm *tm, char *buf)
{
  // the standard's own format, which is 26 bytes for any sane year
  snprintf(buf, 26, "%s %s %2d %02d:%02d:%02d %ld\n", wday_short[tm->tm_wday % 7],
           mon_short[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
           tm->tm_year + 1900L);
  return buf;
}

char *
asctime(const struct tm *tm)
{
  static char buf[64];
  return asctime_r(tm, buf);
}

char *
ctime_r(const time_t *t, char *buf)
{
  struct tm tm;
  return asctime_r(localtime_r(t, &tm), buf);
}

char *
ctime(const time_t *t)
{
  static char buf[64];
  return ctime_r(t, buf);
}

static char *
put(char *p, char *end, const char *s)
{
  while (*s && p < end)
    *p++ = *s++;
  return p;
}

static char *
put2(char *p, char *end, long v, int width, char fill)
{
  char tmp[24];
  int n = 0;
  int neg = v < 0;

  if (neg)
    v = -v;
  do {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  } while (v);
  if (neg)
    tmp[n++] = '-';
  for (int pad = width - n; pad > 0 && p < end; pad--)
    *p++ = fill;
  while (n > 0 && p < end)
    *p++ = tmp[--n];
  return p;
}

size_t
strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
  char *p = s, *end = s + (max ? max - 1 : 0);

  if (max == 0)
    return 0;
  for (; *fmt && p < end; fmt++) {
    if (*fmt != '%') {
      *p++ = *fmt;
      continue;
    }
    fmt++;
    if (*fmt == 'E' || *fmt == 'O') // the alternatives are the same here
      fmt++;
    switch (*fmt) {
    case 'a': p = put(p, end, wday_short[tm->tm_wday % 7]); break;
    case 'A': p = put(p, end, wday_long[tm->tm_wday % 7]); break;
    case 'b': case 'h': p = put(p, end, mon_short[tm->tm_mon % 12]); break;
    case 'B': p = put(p, end, mon_long[tm->tm_mon % 12]); break;
    case 'c': {
      char buf[64];
      strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y", tm);
      p = put(p, end, buf);
      break;
    }
    case 'C': p = put2(p, end, (tm->tm_year + 1900) / 100, 2, '0'); break;
    case 'd': p = put2(p, end, tm->tm_mday, 2, '0'); break;
    case 'D': {
      char buf[16];
      strftime(buf, sizeof(buf), "%m/%d/%y", tm);
      p = put(p, end, buf);
      break;
    }
    case 'e': p = put2(p, end, tm->tm_mday, 2, ' '); break;
    case 'F': {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
      p = put(p, end, buf);
      break;
    }
    case 'g': p = put2(p, end, (tm->tm_year + 1900) % 100, 2, '0'); break;
    case 'G': p = put2(p, end, tm->tm_year + 1900, 4, '0'); break;
    case 'H': p = put2(p, end, tm->tm_hour, 2, '0'); break;
    case 'I': p = put2(p, end, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, 2, '0'); break;
    case 'j': p = put2(p, end, tm->tm_yday + 1, 3, '0'); break;
    case 'k': p = put2(p, end, tm->tm_hour, 2, ' '); break;
    case 'l': p = put2(p, end, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, 2, ' '); break;
    case 'm': p = put2(p, end, tm->tm_mon + 1, 2, '0'); break;
    case 'M': p = put2(p, end, tm->tm_min, 2, '0'); break;
    case 'n': p = put(p, end, "\n"); break;
    case 'p': p = put(p, end, tm->tm_hour < 12 ? "AM" : "PM"); break;
    case 'P': p = put(p, end, tm->tm_hour < 12 ? "am" : "pm"); break;
    case 'r': {
      char buf[32];
      strftime(buf, sizeof(buf), "%I:%M:%S %p", tm);
      p = put(p, end, buf);
      break;
    }
    case 'R': {
      char buf[16];
      strftime(buf, sizeof(buf), "%H:%M", tm);
      p = put(p, end, buf);
      break;
    }
    case 's': {
      struct tm copy = *tm;
      p = put2(p, end, mktime(&copy), 1, '0');
      break;
    }
    case 'S': p = put2(p, end, tm->tm_sec, 2, '0'); break;
    case 't': p = put(p, end, "\t"); break;
    case 'T': {
      char buf[16];
      strftime(buf, sizeof(buf), "%H:%M:%S", tm);
      p = put(p, end, buf);
      break;
    }
    case 'u': p = put2(p, end, tm->tm_wday ? tm->tm_wday : 7, 1, '0'); break;
    case 'U': p = put2(p, end, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0'); break;
    case 'V': {
      // the ISO week number: the week holding the year's first Thursday
      int wday = (tm->tm_wday + 6) % 7; // Monday first
      int week = (tm->tm_yday - wday + 10) / 7;
      if (week < 1)
        week = 52 + ((tm->tm_yday - wday + 10) > -2);
      p = put2(p, end, week, 2, '0');
      break;
    }
    case 'w': p = put2(p, end, tm->tm_wday, 1, '0'); break;
    case 'W': p = put2(p, end, (tm->tm_yday + 7 - (tm->tm_wday ? tm->tm_wday - 1 : 6)) / 7, 2, '0'); break;
    case 'x': {
      char buf[16];
      strftime(buf, sizeof(buf), "%m/%d/%y", tm);
      p = put(p, end, buf);
      break;
    }
    case 'X': {
      char buf[16];
      strftime(buf, sizeof(buf), "%H:%M:%S", tm);
      p = put(p, end, buf);
      break;
    }
    case 'y': p = put2(p, end, (tm->tm_year + 1900) % 100, 2, '0'); break;
    case 'Y': p = put2(p, end, tm->tm_year + 1900L, 1, '0'); break;
    case 'z': {
      long off = tm->tm_gmtoff;
      if (p < end)
        *p++ = off < 0 ? '-' : '+';
      if (off < 0)
        off = -off;
      p = put2(p, end, off / 3600, 2, '0');
      p = put2(p, end, (off % 3600) / 60, 2, '0');
      break;
    }
    case 'Z': p = put(p, end, tm->tm_zone ? tm->tm_zone : "UTC"); break;
    case '%': if (p < end) *p++ = '%'; break;
    case 0: fmt--; break;
    default:
      if (p < end)
        *p++ = '%';
      if (p < end)
        *p++ = *fmt;
      break;
    }
  }
  *p = 0;
  return p - s;
}

static const char *
match_name(const char *s, const char *const *names, int n, int *out)
{
  for (int i = 0; i < n; i++) {
    size_t len = strlen(names[i]);
    if (strncasecmp(s, names[i], len) == 0) {
      *out = i;
      return s + len;
    }
  }
  return 0;
}

static const char *
get_num(const char *s, int width, int *out)
{
  int v = 0, n = 0;

  while (isspace((unsigned char)*s))
    s++;
  int neg = 0;
  if (*s == '-' || *s == '+') {
    neg = (*s == '-');
    s++;
  }
  while (isdigit((unsigned char)*s) && n < width) {
    v = v * 10 + (*s++ - '0');
    n++;
  }
  if (n == 0)
    return 0;
  *out = neg ? -v : v;
  return s;
}

char *
strptime(const char *s, const char *fmt, struct tm *tm)
{
  int v;

  for (; *fmt; fmt++) {
    if (isspace((unsigned char)*fmt)) {
      while (isspace((unsigned char)*s))
        s++;
      continue;
    }
    if (*fmt != '%') {
      if (*s != *fmt)
        return 0;
      s++;
      continue;
    }
    fmt++;
    const char *q;
    switch (*fmt) {
    case 'a': case 'A':
      q = match_name(s, wday_long, 7, &v);
      if (q == 0)
        q = match_name(s, wday_short, 7, &v);
      if (q == 0)
        return 0;
      tm->tm_wday = v;
      s = q;
      break;
    case 'b': case 'B': case 'h':
      q = match_name(s, mon_long, 12, &v);
      if (q == 0)
        q = match_name(s, mon_short, 12, &v);
      if (q == 0)
        return 0;
      tm->tm_mon = v;
      s = q;
      break;
    case 'd': case 'e':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_mday = v;
      s = q;
      break;
    case 'H':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_hour = v;
      s = q;
      break;
    case 'I':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_hour = v % 12;
      s = q;
      break;
    case 'j':
      if ((q = get_num(s, 3, &v)) == 0)
        return 0;
      tm->tm_yday = v - 1;
      s = q;
      break;
    case 'm':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_mon = v - 1;
      s = q;
      break;
    case 'M':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_min = v;
      s = q;
      break;
    case 'p': case 'P':
      if (strncasecmp(s, "PM", 2) == 0) {
        if (tm->tm_hour < 12)
          tm->tm_hour += 12;
        s += 2;
      } else if (strncasecmp(s, "AM", 2) == 0) {
        if (tm->tm_hour == 12)
          tm->tm_hour = 0;
        s += 2;
      } else {
        return 0;
      }
      break;
    case 'S':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_sec = v;
      s = q;
      break;
    case 'y':
      if ((q = get_num(s, 2, &v)) == 0)
        return 0;
      tm->tm_year = v < 69 ? v + 100 : v;
      s = q;
      break;
    case 'Y':
      if ((q = get_num(s, 4, &v)) == 0)
        return 0;
      tm->tm_year = v - 1900;
      s = q;
      break;
    case 'n': case 't':
      while (isspace((unsigned char)*s))
        s++;
      break;
    case 'D':
      if ((s = strptime(s, "%m/%d/%y", tm)) == 0)
        return 0;
      break;
    case 'F':
      if ((s = strptime(s, "%Y-%m-%d", tm)) == 0)
        return 0;
      break;
    case 'T':
      if ((s = strptime(s, "%H:%M:%S", tm)) == 0)
        return 0;
      break;
    case 'R':
      if ((s = strptime(s, "%H:%M", tm)) == 0)
        return 0;
      break;
    case 'c':
      if ((s = strptime(s, "%a %b %e %H:%M:%S %Y", tm)) == 0)
        return 0;
      break;
    case 's': {
      char *e;
      long t = strtol(s, &e, 10);
      if (e == s)
        return 0;
      time_t tt = t;
      localtime_r(&tt, tm);
      s = e;
      break;
    }
    case '%':
      if (*s != '%')
        return 0;
      s++;
      break;
    default:
      return 0;
    }
  }
  return (char *)s;
}
