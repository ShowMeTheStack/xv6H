// taking a number apart along the exponent: frexp, ldexp, scalbn, scalbln,
// ilogb and logb.
//
// scaling by a power of two is exact whenever the result stays normal, so
// the work is all in the ends of the range. scalbn builds the factor 2^n
// directly in the exponent field and applies it in at most three steps:
// splitting it that way keeps every intermediate value representable, so a
// scale that overflows or underflows does so once, in the last
// multiplication, with the rounding and the sticky flags the hardware would
// have produced anyway.

#include <math.h>
#include <stdint.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

union dbits {
  double f;
  uint64_t i;
};

union ldbits {
  long double f;
  struct {
    uint64_t lo;
    uint64_t hi;
  } w;
};

// note a scale that lost everything, which C99 reports as a range error
static double
scaled(double x, double r)
{
  if (x != 0.0 && isfinite(x) && (r == 0.0 || !isfinite(r)))
    errno = ERANGE;
  return r;
}

double
scalbn(double x, int n)
{
  union dbits u;
  double y = x;

  if (n > 1023) {
    y *= 0x1p1023;
    n -= 1023;
    if (n > 1023) {
      y *= 0x1p1023;
      n -= 1023;
      if (n > 1023)
        n = 1023;
    }
  } else if (n < -1022) {
    // the step down is 2^-969 rather than 2^-1022 so that an argument at
    // the bottom of the normal range does not turn subnormal too early and
    // lose bits that the next step would have kept
    y *= 0x1p-969;
    n += 969;
    if (n < -1022) {
      y *= 0x1p-969;
      n += 969;
      if (n < -1022)
        n = -1022;
    }
  }
  u.i = (uint64_t)(0x3ff + n) << 52;
  return scaled(x, y * u.f);
}

float
scalbnf(float x, int n)
{
  union {
    float f;
    uint32_t i;
  } u;
  float y = x;

  if (n > 127) {
    y *= 0x1p127f;
    n -= 127;
    if (n > 127) {
      y *= 0x1p127f;
      n -= 127;
      if (n > 127)
        n = 127;
    }
  } else if (n < -126) {
    y *= 0x1p-102f;
    n += 102;
    if (n < -126) {
      y *= 0x1p-102f;
      n += 102;
      if (n < -126)
        n = -126;
    }
  }
  u.i = (uint32_t)(0x7f + n) << 23;
  y *= u.f;
  if (x != 0.0f && isfinite(x) && (y == 0.0f || !isfinite(y)))
    errno = ERANGE;
  return y;
}

long double
scalbnl(long double x, int n)
{
  union ldbits u;
  long double y = x;

  if (n > 16383) {
    y *= 0x1p16383L;
    n -= 16383;
    if (n > 16383) {
      y *= 0x1p16383L;
      n -= 16383;
      if (n > 16383)
        n = 16383;
    }
  } else if (n < -16382) {
    y *= 0x1p-16269L;
    n += 16269;
    if (n < -16382) {
      y *= 0x1p-16269L;
      n += 16269;
      if (n < -16382)
        n = -16382;
    }
  }
  u.w.lo = 0;
  u.w.hi = (uint64_t)(0x3fff + n) << 48;
  y *= u.f;
  if (x != 0.0L && isfinite(x) && (y == 0.0L || !isfinite(y)))
    errno = ERANGE;
  return y;
}

// ldexp is scalbn under another name, because FLT_RADIX is 2 here
double
ldexp(double x, int n)
{
  return scalbn(x, n);
}

float
ldexpf(float x, int n)
{
  return scalbnf(x, n);
}

long double
ldexpl(long double x, int n)
{
  return scalbnl(x, n);
}

// scalbln only differs in the width of the count, and a count that does not
// fit in an int is already far past the end of the range
static int
clampn(long n)
{
  if (n > 100000L)
    return 100000;
  if (n < -100000L)
    return -100000;
  return (int)n;
}

double
scalbln(double x, long n)
{
  return scalbn(x, clampn(n));
}

float
scalblnf(float x, long n)
{
  return scalbnf(x, clampn(n));
}

long double
scalblnl(long double x, long n)
{
  return scalbnl(x, clampn(n));
}

double
frexp(double x, int *e)
{
  union dbits u;
  int ee;

  u.f = x;
  ee = (int)(u.i >> 52) & 0x7ff;
  if (ee == 0) {
    *e = 0;
    if (x == 0.0)
      return x;
    // a subnormal has no exponent of its own: normalise it first and pay
    // the borrowed scale back afterwards
    x = frexp(x * 0x1p64, e);
    *e -= 64;
    return x;
  }
  if (ee == 0x7ff) {
    *e = 0;
    return x;
  }
  *e = ee - 0x3fe;
  u.i = (u.i & 0x800fffffffffffffUL) | 0x3fe0000000000000UL;
  return u.f;
}

float
frexpf(float x, int *e)
{
  union {
    float f;
    uint32_t i;
  } u;
  int ee;

  u.f = x;
  ee = (int)(u.i >> 23) & 0xff;
  if (ee == 0) {
    *e = 0;
    if (x == 0.0f)
      return x;
    x = frexpf(x * 0x1p32f, e);
    *e -= 32;
    return x;
  }
  if (ee == 0xff) {
    *e = 0;
    return x;
  }
  *e = ee - 0x7e;
  u.i = (u.i & 0x807fffffU) | 0x3f000000U;
  return u.f;
}

long double
frexpl(long double x, int *e)
{
  union ldbits u;
  int ee;

  u.f = x;
  ee = (int)(u.w.hi >> 48) & 0x7fff;
  if (ee == 0) {
    *e = 0;
    if (x == 0.0L)
      return x;
    x = frexpl(x * 0x1p120L, e);
    *e -= 120;
    return x;
  }
  if (ee == 0x7fff) {
    *e = 0;
    return x;
  }
  *e = ee - 0x3ffe;
  u.w.hi = (u.w.hi & 0x8000ffffffffffffUL) | 0x3ffe000000000000UL;
  return u.f;
}

int
ilogb(double x)
{
  int e;

  if (isnan(x)) {
    errno = EDOM;
    return FP_ILOGBNAN;
  }
  if (x == 0.0) {
    errno = EDOM;
    return FP_ILOGB0;
  }
  if (!isfinite(x)) {
    errno = EDOM;
    return INT32_MAX;
  }
  frexp(x, &e);
  return e - 1;
}

int
ilogbf(float x)
{
  return ilogb((double)x);
}

int
ilogbl(long double x)
{
  int e;

  if (isnan(x)) {
    errno = EDOM;
    return FP_ILOGBNAN;
  }
  if (x == 0.0L) {
    errno = EDOM;
    return FP_ILOGB0;
  }
  if (!isfinite(x)) {
    errno = EDOM;
    return INT32_MAX;
  }
  frexpl(x, &e);
  return e - 1;
}

double
logb(double x)
{
  if (isnan(x))
    return x;
  if (x == 0.0) {
    errno = ERANGE;
    return -1.0 / 0.0;
  }
  if (!isfinite(x))
    return fabs(x);
  return (double)ilogb(x);
}

float
logbf(float x)
{
  return (float)logb((double)x);
}

long double
logbl(long double x)
{
  if (isnan(x))
    return x;
  if (x == 0.0L) {
    errno = ERANGE;
    return -1.0L / 0.0L;
  }
  if (!isfinite(x))
    return fabsl(x);
  return (long double)ilogbl(x);
}
