// powers and roots: pow and cbrt.
//
// pow is x^y = e^(y * ln x), and the whole difficulty is that the
// exponential magnifies any error in the logarithm by as much as the
// exponent itself. with |y * ln x| reaching 745 before the result
// overflows, a logarithm good to only 53 bits would leave the answer good
// to 43, so the logarithm has to be carried wider than the result. that is
// what __log_dd in log.c is for: it returns ln x as an unevaluated sum of
// two doubles, about 105 bits in all, and the multiplication by y is done
// the same way, with fma supplying the part of each product that does not
// fit. only when the exponent has been reduced modulo ln2 does the work
// drop back to a single double, by which point the remaining error is
// thousands of times smaller than the final rounding.
//
// cbrt goes the other way: a cheap polynomial gets four correct digits and
// newton's iteration for t^3 = m triples them at every step. the last step
// is written as a relative correction, t*(1 + (m/t^3 - 1)/3), so that the
// cancellation lands in a term that is already small.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

static const double LN2HI = 0.6931471806019545;
static const double LN2LO = -4.2009150726810846e-11;
static const double INVLN2 = 1.4426950408889634;

// how y sits with respect to the integers, which decides the sign of a
// negative base and whether the answer exists at all: 0 not an integer,
// 1 an odd integer, 2 an even integer
static int
inttype(double y)
{
  double t;

  if (!isfinite(y))
    return 0;
  t = fabs(y);
  if (t >= 0x1p53)  // every double this large is an even integer
    return 2;
  if (floor(t) != t)
    return 0;
  return ((long long)t & 1) ? 1 : 2;
}

static int
inttypel(long double y)
{
  long double t;

  if (!isfinite(y))
    return 0;
  t = fabsl(y);
  if (t >= 0x1p113L)
    return 2;
  if (floorl(t) != t)
    return 0;
  return fmodl(t, 2.0L) != 0.0L ? 1 : 2;
}

double
pow(double x, double y)
{
  double ax, lh, ll, ph, pl, hi, lo, fk, sign;
  int yi;

  // C99 gives pow a long list of special cases, and the first two win even
  // over a NaN argument
  if (y == 0.0 || x == 1.0)
    return 1.0;
  if (isnan(x) || isnan(y))
    return x + y;

  // an infinite exponent settles the answer whatever the base is, and in
  // particular pow(-1, +-inf) is 1 rather than a domain error
  if (isinf(y)) {
    if (fabs(x) == 1.0)
      return 1.0;
    return (fabs(x) > 1.0) == (y > 0.0) ? HUGE_VAL : 0.0;
  }

  yi = inttype(y);
  sign = 1.0;
  if (x < 0.0) {
    if (yi == 0) {  // a negative base has no real power unless y is whole
      errno = EDOM;
      return (x - x) / (x - x);
    }
    if (yi == 1)
      sign = -1.0;
  } else if (signbit(x) && yi == 1) {
    // a negative zero is not less than zero, but it still hands its sign to
    // an odd power
    sign = -1.0;
  }
  ax = fabs(x);

  if (isinf(x)) {
    if (y < 0.0)
      return sign > 0.0 ? 0.0 : -0.0;
    return sign * HUGE_VAL;
  }
  if (x == 0.0) {
    if (y < 0.0) {
      errno = ERANGE;
      return sign * (1.0 / 0.0);
    }
    return sign > 0.0 ? 0.0 : -0.0;
  }

  // ln|x| to about 105 bits, then y times it to the same width
  lh = __log_dd(ax, &ll);
  ph = y * lh;
  pl = fma(y, lh, -ph) + y * ll;

  // the bounds are loose; everything between them is handed to scalbn
  // inside the kernel, which builds the subnormal results correctly
  if (ph > 710.0) {
    errno = ERANGE;
    return sign * 0x1p1023 * 0x1p1023;
  }
  if (ph < -746.0) {
    errno = ERANGE;
    return sign * 0x1p-1022 * 0x1p-1022;
  }

  // reduce modulo ln2. k has at most 11 bits and LN2HI has 33, so k*LN2HI
  // is exact and so is the subtraction: nothing of ph is lost here.
  fk = floor(ph * INVLN2 + 0.5);
  hi = ph - fk * LN2HI;
  lo = pl - fk * LN2LO;
  return sign * __exp_core(hi, lo, (int)fk);
}

float
powf(float x, float y)
{
  double r = pow((double)x, (double)y);
  float f = (float)r;

  if (isfinite(r) && r != 0.0 && (!isfinite(f) || f == 0.0f))
    errno = ERANGE;
  return f;
}

// binary128 in name and range only: the logarithm and the exponential are
// both done in double, so about 53 bits of the answer are right. the
// exponent is taken apart in the wider type, which keeps pow(x, y) working
// for results far outside the double range.
long double
powl(long double x, long double y)
{
  long double ax, l, p, k, sign;
  int yi;

  if (y == 0.0L || x == 1.0L)
    return 1.0L;
  if (isnan(x) || isnan(y))
    return x + y;

  if (isinf(y)) {
    if (fabsl(x) == 1.0L)
      return 1.0L;
    return (fabsl(x) > 1.0L) == (y > 0.0L) ? HUGE_VALL : 0.0L;
  }

  yi = inttypel(y);
  sign = 1.0L;
  if (x < 0.0L) {
    if (yi == 0) {
      errno = EDOM;
      return (x - x) / (x - x);
    }
    if (yi == 1)
      sign = -1.0L;
  } else if (signbit(x) && yi == 1) {
    sign = -1.0L;
  }
  ax = fabsl(x);

  if (isinf(x)) {
    if (y < 0.0L)
      return sign > 0.0L ? 0.0L : -0.0L;
    return sign * HUGE_VALL;
  }
  if (x == 0.0L) {
    if (y < 0.0L) {
      errno = ERANGE;
      return sign * (1.0L / 0.0L);
    }
    return sign > 0.0L ? 0.0L : -0.0L;
  }

  // log2 rather than log, so that the integer part of the exponent stays
  // exact and only the fraction goes through the double exponential
  l = log2l(ax);
  p = y * l;
  if (p > 16384.0L) {
    errno = ERANGE;
    return sign * 0x1p16383L * 0x1p16383L;
  }
  if (p < -16500.0L) {
    errno = ERANGE;
    return sign * 0x1p-16382L * 0x1p-16382L;
  }
  k = floorl(p);
  return sign * scalbnl((long double)exp2((double)(p - k)), (int)k);
}

static const double
  C0 = 0.5688726622356585,
  C1 = 0.5219953337402932,
  C2 = -0.10802598672589515,
  C3 = 0.01033395124666829;

double
cbrt(double x)
{
  double m, t, w, e1, r1;
  int e, r;

  if (!isfinite(x) || x == 0.0)
    return x + x;

  m = frexp(fabs(x), &e);
  // split the exponent into a multiple of three and a remainder, and give
  // the remainder to the mantissa: m ends up in [0.5, 4)
  r = e % 3;
  if (r < 0)
    r += 3;
  e = (e - r) / 3;
  m = scalbn(m, r);

  t = C0 + m * (C1 + m * (C2 + m * C3));  // four correct digits
  t = (2.0 * t + m / (t * t)) / 3.0;
  t = (2.0 * t + m / (t * t)) / 3.0;
  t = (2.0 * t + m / (t * t)) / 3.0;
  // the last pass works on the residual m - t^3 instead of on t. writing it
  // that way keeps the cancellation out of the answer: fma gives m - w*t
  // rounded only once, however much of it cancels, so the correction below
  // is accurate even though it is a billionth the size of t.
  w = t * t;
  e1 = fma(t, t, -w);           // t*t = w + e1 exactly
  r1 = fma(-w, t, m) - e1 * t;  // m - t^3
  t += r1 / (3.0 * w);
  return copysign(scalbn(t, e), x);
}

float
cbrtf(float x)
{
  return (float)cbrt((double)x);
}

long double
cbrtl(long double x)
{
  long double m, t;
  int e, r;

  if (!isfinite(x) || x == 0.0L)
    return x + x;

  m = frexpl(fabsl(x), &e);
  r = e % 3;
  if (r < 0)
    r += 3;
  e = (e - r) / 3;
  m = scalbnl(m, r);

  // the double cube root is already good to 53 bits, and two newton passes
  // in binary128 carry it past the 113 that the wider type holds
  t = (long double)cbrt((double)m);
  t = (2.0L * t + m / (t * t)) / 3.0L;
  t = t * (1.0L + (m / (t * t * t) - 1.0L) / 3.0L);
  return copysignl(scalbnl(t, e), x);
}
