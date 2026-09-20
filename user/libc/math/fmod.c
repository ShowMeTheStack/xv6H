// division with a remainder: fmod, remainder and remquo.
//
// all three come from the same long division. scale |y| up so that it sits
// in the same binade as |x|, then walk back down one binade at a time,
// subtracting whenever the divisor still fits. each subtraction is exact:
// at that point the running remainder r and the shifted divisor b satisfy
// b <= r < 2b, and sterbenz's lemma says the difference of two numbers that
// close is representable, so nothing is ever rounded away. the quotient
// bits fall out of the same loop, one per step, which is what remquo needs
// and what tells remainder whether a result exactly half way between two
// multiples should round up or down.
//
// the loop runs once per binade between the two arguments, so it can take a
// couple of thousand turns for arguments at opposite ends of the range.
// that is the price of an exact answer without a wider arithmetic type.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

// the shared long division. returns |x| mod |y| and leaves the low bits of
// the quotient in *q. both arguments must be finite and non-zero, and |x|
// must be at least half of |y|, which is as small as either caller lets it
// get before answering with x itself.
static double
divide(double x, double y, unsigned *q)
{
  double b, r;
  int n;

  r = fabs(x);
  b = fabs(y);
  n = ilogb(r) - ilogb(b);
  b = scalbn(b, n);
  *q = 0;
  for (; n >= 0; n--) {
    *q <<= 1;
    if (r >= b) {
      r -= b;
      (*q)++;
    }
    b *= 0.5;
  }
  return r;
}

double
fmod(double x, double y)
{
  unsigned q;

  if (isnan(x) || isnan(y))
    return x + y;
  if (isinf(x) || y == 0.0) {
    errno = EDOM;
    return (x - x) / (y - y);
  }
  if (isinf(y) || x == 0.0 || fabs(x) < fabs(y))
    return x;
  return copysign(divide(x, y, &q), x);
}

// the shared tail of remainder and remquo: turn the truncated remainder
// into the one nearest zero, reporting how the quotient changed
static double
nearest(double x, double y, unsigned *q)
{
  double r, ay, half;

  r = divide(x, y, q);
  ay = fabs(y);
  half = 0.5 * ay;
  // a remainder past the half way mark belongs to the next multiple; one
  // exactly at it goes to the multiple with the even quotient
  if (r > half || (r == half && (*q & 1))) {
    r -= ay;
    (*q)++;
  }
  // the remainder nearest zero can end up on either side of it, so it is
  // the sign of x that flips it, not copysign, which would throw away the
  // sign the subtraction just produced
  return signbit(x) ? -r : r;
}

double
remainder(double x, double y)
{
  unsigned q;

  if (isnan(x) || isnan(y))
    return x + y;
  if (isinf(x) || y == 0.0) {
    errno = EDOM;
    return (x - x) / (y - y);
  }
  if (isinf(y))
    return x;
  if (x == 0.0)
    return x;
  if (fabs(x) < 0.5 * fabs(y))
    return x;
  return nearest(x, y, &q);
}

double
remquo(double x, double y, int *quo)
{
  unsigned q;
  double r;

  *quo = 0;
  if (isnan(x) || isnan(y))
    return x + y;
  if (isinf(x) || y == 0.0) {
    errno = EDOM;
    return (x - x) / (y - y);
  }
  if (isinf(y) || x == 0.0)
    return x;
  if (fabs(x) < 0.5 * fabs(y)) {
    // the quotient rounds to zero, so there is nothing to report
    return x;
  }
  r = nearest(x, y, &q);
  // C99 asks only for the low bits of the magnitude, given the sign the
  // true quotient would have had
  *quo = (int)(q & 0x7fffffffU);
  if (signbit(x) != signbit(y))
    *quo = -*quo;
  return r;
}

float
fmodf(float x, float y)
{
  return (float)fmod((double)x, (double)y);
}

float
remainderf(float x, float y)
{
  return (float)remainder((double)x, (double)y);
}

float
remquof(float x, float y, int *quo)
{
  return (float)remquo((double)x, (double)y, quo);
}

// the same long division in binary128. widening to double would throw away
// exactly the bits the answer is made of, so this one is done for real.
static long double
dividel(long double x, long double y, unsigned *q)
{
  long double b, r;
  int n;

  r = fabsl(x);
  b = fabsl(y);
  n = ilogbl(r) - ilogbl(b);
  b = scalbnl(b, n);
  *q = 0;
  for (; n >= 0; n--) {
    *q <<= 1;
    if (r >= b) {
      r -= b;
      (*q)++;
    }
    b *= 0.5L;
  }
  return r;
}

long double
fmodl(long double x, long double y)
{
  unsigned q;

  if (isnan(x) || isnan(y))
    return x + y;
  if (isinf(x) || y == 0.0L) {
    errno = EDOM;
    return (x - x) / (y - y);
  }
  if (isinf(y) || x == 0.0L || fabsl(x) < fabsl(y))
    return x;
  return copysignl(dividel(x, y, &q), x);
}

static long double
nearestl(long double x, long double y, unsigned *q)
{
  long double r, ay, half;

  r = dividel(x, y, q);
  ay = fabsl(y);
  half = 0.5L * ay;
  if (r > half || (r == half && (*q & 1))) {
    r -= ay;
    (*q)++;
  }
  return signbit(x) ? -r : r;
}

long double
remainderl(long double x, long double y)
{
  unsigned q;

  if (isnan(x) || isnan(y))
    return x + y;
  if (isinf(x) || y == 0.0L) {
    errno = EDOM;
    return (x - x) / (y - y);
  }
  if (isinf(y) || x == 0.0L)
    return x;
  if (fabsl(x) < 0.5L * fabsl(y))
    return x;
  return nearestl(x, y, &q);
}

long double
remquol(long double x, long double y, int *quo)
{
  unsigned q;
  long double r;

  *quo = 0;
  if (isnan(x) || isnan(y))
    return x + y;
  if (isinf(x) || y == 0.0L) {
    errno = EDOM;
    return (x - x) / (y - y);
  }
  if (isinf(y) || x == 0.0L)
    return x;
  if (fabsl(x) < 0.5L * fabsl(y))
    return x;
  r = nearestl(x, y, &q);
  *quo = (int)(q & 0x7fffffffU);
  if (signbit(x) != signbit(y))
    *quo = -*quo;
  return r;
}
