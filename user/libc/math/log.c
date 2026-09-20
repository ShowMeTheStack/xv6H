// the logarithms: log, log2, log10 and log1p.
//
// write x = 2^n * m with m in [sqrt(2)/2, sqrt(2)), so that log x = n*ln2 +
// log m. for log m the substitution s = (m-1)/(m+1) turns the logarithm
// into an odd series that converges quickly, because |s| stays below 0.172:
//
//   log m = 2*atanh(s) = 2s * (1 + z/3 + z^2/5 + z^3/7 + ...),  z = s^2.
//
// the coefficients are the exact reciprocals of the odd numbers, not a
// fitted polynomial: with z below 0.03 the series is already short, and
// exact coefficients are easier to trust than remembered ones.
//
// the arithmetic is the interesting part. everything here is carried as an
// unevaluated sum of two doubles, which holds about 105 bits, and the pair
// is what __log_dd hands back. log itself just adds the two halves
// together, so its result is within a hair of correctly rounded; pow needs
// the whole pair, because it multiplies the logarithm by an exponent as
// large as a thousand and any error in the logarithm is magnified by that
// much before the exponential undoes it.
//
// only the leading z/3 term is kept to the full width. the rest of the
// series is under 2e-4 in size, so rounding it as a plain double leaves an
// error near 2e-20, which is small enough to disappear under the 2^-63 that
// pow needs. the tail is carried to z^13, where the terms fall below 1e-21.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

static const double LN2_HI = 0.6931471805599453;
static const double LN2_LO = 2.3190468138462996e-17;
static const double INVLN2_HI = 1.4426950408889634;
static const double INVLN2_LO = 2.0355273740931033e-17;
static const double INVLN10_HI = 0.4342944819032518;
static const double INVLN10_LO = 1.098319650216765e-17;
static const double THIRD_HI = 0.3333333333333333;
static const double THIRD_LO = 1.850371707708594e-17;

// 1/5, 1/7, ... 1/29: the tail of the atanh series
static const double
  A5 = 0.2,
  A7 = 0.14285714285714285,
  A9 = 0.1111111111111111,
  A11 = 0.09090909090909091,
  A13 = 0.07692307692307693,
  A15 = 0.06666666666666667,
  A17 = 0.058823529411764705,
  A19 = 0.05263157894736842,
  A21 = 0.047619047619047616,
  A23 = 0.043478260869565216,
  A25 = 0.04,
  A27 = 0.037037037037037035,
  A29 = 0.034482758620689655;

// the sum of two doubles, and the part of it that the sum could not hold.
// knuth's algorithm: correct for any two arguments, with no assumption
// about which is larger.
static void
twosum(double a, double b, double *s, double *e)
{
  double t;

  *s = a + b;
  t = *s - a;
  *e = (a - (*s - t)) + (b - t);
}

// the product of two doubles and its exact remainder, which the fused
// multiply-add hands over directly
static void
twoprod(double a, double b, double *p, double *e)
{
  *p = a * b;
  *e = fma(a, b, -*p);
}

// log(1+f) as a pair, for f in (-0.2929, 0.4143). f must be exact: it is
// either an argument of log1p or the difference m-1, which is exact for
// every m the reduction produces.
static double
logf_dd(double f, double *lo)
{
  double d, dl, s, sl, z, zl, q, ql, rest, c, cl, u, ul, p, pe, pl;

  if (f == 0.0) {
    *lo = 0.0;
    return f;  // keeps the sign of a zero argument
  }
  twosum(2.0, f, &d, &dl);  // d + dl = 2 + f exactly
  s = f / d;
  // one newton step in the wider arithmetic: the residual of the division
  // is exact, so dividing it again gives the bits s could not hold
  sl = (fma(-s, d, f) - s * dl) / d;

  twoprod(s, s, &z, &zl);
  zl += 2.0 * s * sl;

  twoprod(z, THIRD_HI, &q, &ql);
  ql += z * THIRD_LO + zl * THIRD_HI;
  rest = z * z *
         (A5 + z * (A7 + z * (A9 + z * (A11 + z * (A13 + z * (A15 + z * (A17 +
          z * (A19 + z * (A21 + z * (A23 + z * (A25 + z * (A27 + z * A29))))))))))));
  twosum(q, rest, &c, &cl);
  cl += ql;

  twosum(1.0, c, &u, &ul);
  ul += cl;

  // log(1+f) = 2s * (1 + c), and doubling at the end is exact
  twoprod(s, u, &p, &pe);
  pl = pe + (s * ul + sl * u);
  twosum(p, pl, &p, &pe);
  *lo = 2.0 * pe;
  return 2.0 * p;
}

double
__log_dd(double x, double *lo)
{
  double m, hi, l, p, pe, s, se;
  int n;

  m = frexp(x, &n);
  // pull m up into [sqrt(2)/2, sqrt(2)) so that |s| below stays small; the
  // borrowed factor of two comes off the exponent
  if (m < 0.7071067811865476) {
    m *= 2.0;
    n--;
  }
  hi = logf_dd(m - 1.0, &l);
  if (n == 0) {
    *lo = l;
    return hi;
  }
  twoprod((double)n, LN2_HI, &p, &pe);
  pe += (double)n * LN2_LO;
  twosum(p, hi, &s, &se);
  twosum(s, se + (pe + l), &s, &se);
  *lo = se;
  return s;
}

// multiply a pair by a pair, keeping the result as a pair
static double
ddmul(double ah, double al, double bh, double bl, double *lo)
{
  double p, pe;

  twoprod(ah, bh, &p, &pe);
  pe += ah * bl + al * bh;
  twosum(p, pe, &p, &pe);
  *lo = pe;
  return p;
}

// the domain and pole checks the three logarithms share. returns 1 and
// leaves the answer in *r when x is not an ordinary positive number.
static int
special(double x, double *r)
{
  if (isnan(x) || (isinf(x) && x > 0.0)) {
    *r = x;
    return 1;
  }
  if (x == 0.0) {
    errno = ERANGE;
    *r = -1.0 / 0.0;
    return 1;
  }
  if (x < 0.0) {
    errno = EDOM;
    *r = (x - x) / (x - x);
    return 1;
  }
  return 0;
}

double
log(double x)
{
  double hi, lo, r;

  if (special(x, &r))
    return r;
  hi = __log_dd(x, &lo);
  return hi + lo;
}

double
log2(double x)
{
  double hi, lo, m, r;
  int n;

  if (special(x, &r))
    return r;
  // keep the exponent out of the multiplication so that a power of two
  // comes back as the exact integer it is
  m = frexp(x, &n);
  if (m < 0.7071067811865476) {
    m *= 2.0;
    n--;
  }
  hi = logf_dd(m - 1.0, &lo);
  hi = ddmul(hi, lo, INVLN2_HI, INVLN2_LO, &lo);
  return (double)n + (hi + lo);
}

double
log10(double x)
{
  double hi, lo, r;

  if (special(x, &r))
    return r;
  hi = __log_dd(x, &lo);
  hi = ddmul(hi, lo, INVLN10_HI, INVLN10_LO, &lo);
  return hi + lo;
}

double
log1p(double x)
{
  double hi, lo, u, c;

  if (isnan(x) || x == 0.0 || (isinf(x) && x > 0.0))
    return x;  // a zero keeps its sign, which the addition below would lose
  if (x == -1.0) {
    errno = ERANGE;
    return -1.0 / 0.0;
  }
  if (x < -1.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x > -0.2928932188134525 && x < 0.4142135623730951) {
    // the argument is already inside the kernel's range, so it goes in
    // untouched and small arguments keep all of their bits
    hi = logf_dd(x, &lo);
    return hi + lo;
  }
  u = 1.0 + x;
  // c is what the addition above dropped, so u + c is exactly 1 + x. adding
  // 1 and x is a two-sum with one of the two known to be the larger.
  c = fabs(x) < 1.0 ? (1.0 - u) + x : (x - u) + 1.0;
  hi = __log_dd(u, &lo);
  // log(u + c) = log(u) + log(1 + c/u), and c/u is far too small for the
  // difference between it and its logarithm to matter
  return hi + (lo + c / u);
}

float
logf(float x)
{
  return (float)log((double)x);
}

float
log2f(float x)
{
  return (float)log2((double)x);
}

float
log10f(float x)
{
  return (float)log10((double)x);
}

float
log1pf(float x)
{
  return (float)log1p((double)x);
}

// the binary128 logarithms take the exponent apart in the wider type, which
// gives them the whole binary128 range, but the mantissa's logarithm is
// computed as a double and widened. they are therefore accurate to about 53
// bits rather than 113.
static long double
logl_core(long double x, long double *nout)
{
  long double m;
  int n;

  m = frexpl(x, &n);
  if (m < 0.7071067811865476L) {
    m *= 2.0L;
    n--;
  }
  *nout = (long double)n;
  return (long double)log((double)m);
}

static int
speciall(long double x, long double *r)
{
  if (isnan(x) || (isinf(x) && x > 0.0L)) {
    *r = x;
    return 1;
  }
  if (x == 0.0L) {
    errno = ERANGE;
    *r = -1.0L / 0.0L;
    return 1;
  }
  if (x < 0.0L) {
    errno = EDOM;
    *r = (x - x) / (x - x);
    return 1;
  }
  return 0;
}

long double
logl(long double x)
{
  long double n, lm, r;

  if (speciall(x, &r))
    return r;
  lm = logl_core(x, &n);
  return n * M_LN2l + lm;
}

long double
log2l(long double x)
{
  long double n, lm, r;

  if (speciall(x, &r))
    return r;
  lm = logl_core(x, &n);
  return n + lm * M_LOG2El;
}

long double
log10l(long double x)
{
  long double n, lm, r;

  if (speciall(x, &r))
    return r;
  lm = logl_core(x, &n);
  return n * 0.301029995663981195213738894724493027L + lm * M_LOG10El;
}

long double
log1pl(long double x)
{
  if (isnan(x) || (isinf(x) && x > 0.0L))
    return x;
  if (x == -1.0L) {
    errno = ERANGE;
    return -1.0L / 0.0L;
  }
  if (x < -1.0L) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  // near zero the double result carries every bit that matters relative to
  // x, and further out the ordinary logarithm is just as good
  if (fabsl(x) < 0.25L)
    return (long double)log1p((double)x);
  return logl(1.0L + x);
}
