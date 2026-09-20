// the hyperbolic functions and their inverses.
//
// all six are built from exp, expm1, log and log1p, and the only skill in
// them is choosing which of several algebraically equal formulas to use on
// each part of the range. sinh x = (e^x - e^-x)/2 is worthless near zero,
// where the two terms agree to every bit; written in terms of E = e^x - 1
// as E*(E+2)/(2*(E+1)) it is exact there instead, because expm1 computes E
// without the cancellation. the same trick, mirrored, keeps tanh and the
// inverses accurate near their own awkward points: atanh near zero, acosh
// near one, asinh near zero.
//
// at the other end the formulas are truncated instead. once x passes 22,
// e^-x has fallen below an ulp of e^x and sinh and cosh are both e^x/2 to
// every bit a double keeps, and tanh is 1. beyond the point where e^x
// itself overflows, e^(x-ln2) still fits and covers the last ln2 worth of
// arguments that have a finite answer.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

static const double LN2 = 0.6931471805599453;

// round to float, reporting an overflow that only the narrower type suffers
static float
narrow(double r)
{
  float f = (float)r;

  if (isfinite(r) && r != 0.0 && (!isfinite(f) || f == 0.0f))
    errno = ERANGE;
  return f;
}

double
sinh(double x)
{
  double a, t;

  if (!isfinite(x))
    return x + x;
  a = fabs(x);
  if (a < 1.0) {
    t = expm1(a);
    // E*(E+2)/(2*(E+1)), written so that the small argument case keeps
    // every bit of E rather than of e^x
    return copysign(0.5 * (t + t / (t + 1.0)), x);
  }
  if (a < 22.0) {
    t = exp(a);
    return copysign(0.5 * (t - 1.0 / t), x);
  }
  if (a < 709.7822265625)
    return copysign(0.5 * exp(a), x);
  // e^a has overflowed but half of it has not. halving the argument first
  // is exact, where subtracting ln2 from a number this size would lose
  // fourteen digits of the exponent.
  t = exp(0.5 * a);
  t = (0.5 * t) * t;
  if (isinf(t))
    errno = ERANGE;
  return copysign(t, x);
}

double
cosh(double x)
{
  double a, t;

  if (!isfinite(x))
    return x * x;  // cosh of either infinity is +infinity
  a = fabs(x);
  if (a < 0.3465735902799727) {
    // 1 + E^2/(2*(E+1)), which stays exactly one near zero
    t = expm1(a);
    return 1.0 + t * t / (2.0 * (t + 1.0));
  }
  if (a < 22.0) {
    t = exp(a);
    return 0.5 * (t + 1.0 / t);
  }
  if (a < 709.7822265625)
    return 0.5 * exp(a);
  t = exp(0.5 * a);
  t = (0.5 * t) * t;
  if (isinf(t))
    errno = ERANGE;
  return t;
}

double
tanh(double x)
{
  double a, t;

  if (isnan(x))
    return x;
  if (isinf(x))
    return copysign(1.0, x);
  a = fabs(x);
  if (a < 0.5) {
    // -E/(E+2) with E = e^(-2a) - 1, which is small when a is
    t = expm1(-2.0 * a);
    return copysign(-t / (t + 2.0), x);
  }
  if (a < 22.0) {
    t = expm1(2.0 * a);
    return copysign(1.0 - 2.0 / (t + 2.0), x);
  }
  // e^-2a is below half an ulp of one, so the answer rounds to one exactly
  return copysign(1.0, x);
}

double
asinh(double x)
{
  double a, r;

  if (!isfinite(x) || x == 0.0)
    return x + x;
  a = fabs(x);
  if (a < 2.0) {
    // log1p of the part that is not one, so that a small argument keeps its
    // own precision instead of being lost next to the one
    r = log1p(a + a * a / (1.0 + sqrt(1.0 + a * a)));
  } else if (a < 0x1p28) {
    // 2a + 1/(a + sqrt(a^2+1)) is a + sqrt(a^2+1) rearranged to avoid
    // squaring a number near the top of the range
    r = log(2.0 * a + 1.0 / (a + sqrt(a * a + 1.0)));
  } else {
    r = log(a) + LN2;  // the square root has become a itself
  }
  return copysign(r, x);
}

double
acosh(double x)
{
  double t;

  if (isnan(x))
    return x;
  if (x < 1.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (isinf(x))
    return x;
  if (x < 2.0) {
    t = x - 1.0;  // exact, and it is t rather than x that the answer needs
    return log1p(t + sqrt(2.0 * t + t * t));
  }
  if (x < 0x1p28)
    return log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
  return log(x) + LN2;
}

double
atanh(double x)
{
  double a, r;

  if (isnan(x))
    return x;
  a = fabs(x);
  if (a > 1.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (a == 1.0) {
    errno = ERANGE;
    return copysign(1.0, x) / 0.0;
  }
  if (a < 0.5)
    // 2t + 2t^2/(1-t) is 2t/(1-t) without the cancellation in the numerator
    r = 0.5 * log1p(2.0 * a + 2.0 * a * a / (1.0 - a));
  else
    r = 0.5 * log1p(2.0 * a / (1.0 - a));
  return copysign(r, x);
}

float
sinhf(float x)
{
  return narrow(sinh((double)x));
}

float
coshf(float x)
{
  return narrow(cosh((double)x));
}

float
tanhf(float x)
{
  return (float)tanh((double)x);
}

float
asinhf(float x)
{
  return (float)asinh((double)x);
}

float
acoshf(float x)
{
  return (float)acosh((double)x);
}

float
atanhf(float x)
{
  return (float)atanh((double)x);
}

// the binary128 forms are the double ones wherever a double can hold the
// argument and the answer, which costs them the last sixty bits of
// precision; outside that they fall back on the wider logarithm and
// exponential, so the range is the full binary128 range.
long double
sinhl(long double x)
{
  long double a;

  if (!isfinite(x))
    return x + x;
  a = fabsl(x);
  if (a < 22.0L)
    return (long double)sinh((double)x);
  return copysignl(0.5L * expl(a), x);
}

long double
coshl(long double x)
{
  long double a;

  if (!isfinite(x))
    return x * x;
  a = fabsl(x);
  if (a < 22.0L)
    return (long double)cosh((double)x);
  return 0.5L * expl(a);
}

long double
tanhl(long double x)
{
  if (isnan(x))
    return x;
  if (fabsl(x) >= 40.0L)  // e^-2x is below an ulp of one in binary128
    return copysignl(1.0L, x);
  return (long double)tanh((double)x);
}

long double
asinhl(long double x)
{
  long double a, r;

  if (!isfinite(x) || x == 0.0L)
    return x + x;
  a = fabsl(x);
  if (a < 0x1p60L)
    return (long double)asinh((double)x);
  r = logl(a) + M_LN2l;
  return copysignl(r, x);
}

long double
acoshl(long double x)
{
  if (isnan(x))
    return x;
  if (x < 1.0L) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (isinf(x))
    return x;
  if (x < 0x1p60L)
    return (long double)acosh((double)x);
  return logl(x) + M_LN2l;
}

long double
atanhl(long double x)
{
  long double a, r;

  if (isnan(x))
    return x;
  a = fabsl(x);
  if (a > 1.0L) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (a == 1.0L) {
    errno = ERANGE;
    return copysignl(1.0L, x) / 0.0L;
  }
  // 1-a is exact for every a above a half, so this stays right even where
  // the argument is too close to one for a double to tell it apart
  r = 0.5L * log1pl(2.0L * a / (1.0L - a));
  return copysignl(r, x);
}
