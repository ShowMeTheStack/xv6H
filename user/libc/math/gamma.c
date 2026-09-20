// the gamma function, as a logarithm and directly.
//
// lgamma is built on three pieces. on [1, 2] it is a chebyshev fit of
// lgamma(1+y)/(y*(y-1)), with the two zeros at y = 0 and y = 1 divided out
// first so that the polynomial stays accurate relative to a result that
// passes through zero twice; the fit is degree twenty and measures 1.3 ulp.
// above 12 it is stirling's series, whose terms are the bernoulli numbers
// over powers of x and which is already good to a part in 10^19 by the time
// x reaches 12. between 2 and 12 the recurrence gamma(x+1) = x*gamma(x)
// walks the argument back into [1, 2], and below 1 it walks it forward.
//
// negative arguments go through the reflection formula
//
//   gamma(x) * gamma(1-x) = pi / sin(pi x)
//
// which needs sin(pi x) rather than sin of anything: multiplying x by pi
// first would destroy exactly the information the sine is being asked
// about. the helper below therefore reduces x modulo two in exact steps and
// only then multiplies the small remainder by pi, keeping the product as a
// pair so that the trigonometric kernels get all of it.
//
// tgamma is not exp(lgamma). in the stirling range that would be hopeless:
// lgamma reaches 711 before gamma overflows, so an error of one ulp in the
// logarithm is an error of three hundred ulp in the result. the logarithm
// is therefore computed as a pair of doubles there, wide enough that its
// error is far below one ulp of the answer, and the exponential is applied
// to the pair. everywhere else tgamma uses the same recurrence lgamma does,
// multiplying rather than adding, which keeps the answer to a few ulp.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

int signgam;

static const double
  G0 = 0.5772156649015329,
  G1 = -0.2452513685225759,
  G2 = 0.15543426586329281,
  G3 = -0.11514654252510402,
  G4 = 0.09223900725237584,
  G5 = -0.07731814517650464,
  G6 = 0.06673142618826892,
  G7 = -0.05877516383097031,
  G8 = 0.052537419190277455,
  G9 = -0.04744503662529866,
  G10 = 0.04301415542600463,
  G11 = -0.03867042441832247,
  G12 = 0.03370537137511358,
  G13 = -0.027534187815233195,
  G14 = 0.020215502781759945,
  G15 = -0.012742266956440252,
  G16 = 0.006568778005711162,
  G17 = -0.0026202979485050316,
  G18 = 0.0007510944620906334,
  G19 = -0.00013673749845170442,
  G20 = 1.1820969445467145e-05;

// the bernoulli coefficients of stirling's series, B(2n)/(2n(2n-1))
static const double
  S0 = 0.08333333333333333,
  S1 = -0.002777777777777778,
  S2 = 0.0007936507936507937,
  S3 = -0.0005952380952380953,
  S4 = 0.0008417508417508417,
  S5 = -0.0019175269175269176,
  S6 = 0.00641025641025641;

static const double LOG2PI_HI = 0.9189385332046728;
static const double LOG2PI_LO = -3.8782941580672414e-17;
static const double LOGPI = 1.1447298858494002;
static const double PI_HI = 3.141592653589793;
static const double PI_LO = 1.2246467991473532e-16;

static void
twosum(double a, double b, double *s, double *e)
{
  double t;

  *s = a + b;
  t = *s - a;
  *e = (a - (*s - t)) + (b - t);
}

static void
twoprod(double a, double b, double *p, double *e)
{
  *p = a * b;
  *e = fma(a, b, -*p);
}

static double
gpoly(double y)
{
  return G0 + y * (G1 + y * (G2 + y * (G3 + y * (G4 + y * (G5 + y * (G6 +
         y * (G7 + y * (G8 + y * (G9 + y * (G10 + y * (G11 + y * (G12 +
         y * (G13 + y * (G14 + y * (G15 + y * (G16 + y * (G17 + y * (G18 +
         y * (G19 + y * G20)))))))))))))))))));
}

// sin(pi*x), with every step of the reduction exact. the argument is cut
// down to [0, 1/4] by identities that only ever subtract numbers within a
// factor of two of each other, and pi times what is left is kept as a pair
// so that the kernel sees more than a double's worth of it.
static double
sinpi(double x)
{
  double y, hi, lo;
  int neg = 0, quarter = 0;

  if (x < 0.0) {
    x = -x;
    neg = 1;
  }
  y = fmod(x, 2.0);
  if (y >= 1.0) {
    y -= 1.0;
    neg = !neg;
  }
  if (y > 0.5)
    y = 1.0 - y;
  if (y > 0.25) {
    y = 0.5 - y;  // sin(pi/2 - t) is cos t
    quarter = 1;
  }
  hi = PI_HI * y;
  lo = fma(PI_HI, y, -hi) + PI_LO * y;
  hi = quarter ? __cos_kernel(hi, lo) : __sin_kernel(hi, lo);
  return neg ? -hi : hi;
}

// stirling's series for x >= 12, as a pair whose two halves together are
// good to about a part in 10^19 - far more than the result needs, but
// exactly what tgamma needs before exponentiating it
static double
stirling(double x, double *lo)
{
  double lh, ll, t, ah, al, s, se, te, z, ser;

  lh = __log_dd(x, &ll);
  t = x - 0.5;  // exact
  twoprod(t, lh, &ah, &al);
  al += t * ll;
  twosum(ah, -x, &s, &se);
  se += al;
  twosum(s, LOG2PI_HI, &s, &te);
  se += te + LOG2PI_LO;
  z = 1.0 / (x * x);
  ser = (S0 + z * (S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)))))) / x;
  twosum(s, ser, &s, &te);
  se += te;
  twosum(s, se, &s, &se);
  *lo = se;
  return s;
}

// lgamma for 0 < x < 12, by walking the argument into [1, 2]
static double
lgamma_small(double x)
{
  double r, p, y;
  int n, i;

  if (x < 1.0) {
    // gamma(x) = gamma(1+x)/x, and 1+x is exactly what the polynomial is
    // parameterised by, so it never has to be formed: computing x+1 here
    // would round, and the recurrence would carry that rounding straight
    // into the answer
    return x * (x - 1.0) * gpoly(x) - log(x);
  }
  if (x <= 2.0) {
    y = x - 1.0;
    return y * (y - 1.0) * gpoly(y);
  }
  // gamma(x) = gamma(r) * r * (r+1) * ... with r the fractional part plus one
  n = (int)x - 1;
  r = x - (double)n;
  p = 1.0;
  for (i = 0; i < n; i++)
    p *= r + (double)i;
  y = r - 1.0;
  return y * (y - 1.0) * gpoly(y) + log(p);
}

static double
lgamma_pos(double x)
{
  double lo;

  if (x >= 12.0)
    return stirling(x, &lo) + lo;
  return lgamma_small(x);
}

double
lgamma(double x)
{
  double s;

  signgam = 1;
  if (isnan(x))
    return x;
  if (isinf(x))
    return HUGE_VAL;  // both infinities, since gamma blows up at either end
  if (x == 0.0) {
    errno = ERANGE;
    signgam = signbit(x) ? -1 : 1;
    return HUGE_VAL;
  }
  if (x == 1.0 || x == 2.0)
    return 0.0;  // the polynomial would hand back a negative zero here
  if (x > 0.0)
    return lgamma_pos(x);

  // the reflection formula, with gamma(1-x) written as (-x)*gamma(-x).
  // that matters: -x is exact where 1-x would round, and for an argument in
  // the hundreds the rounding of 1-x alone is worth hundreds of ulp in the
  // answer. at a negative integer the sine is zero and gamma has a pole.
  s = sinpi(x);
  if (s == 0.0) {
    errno = ERANGE;
    return HUGE_VAL;
  }
  signgam = s < 0.0 ? -1 : 1;
  return ((LOGPI - log(fabs(s))) - log(-x)) - lgamma_pos(-x);
}

double
tgamma(double x)
{
  double r, p, y, hi, lo, s, t;
  int n, i;

  if (isnan(x))
    return x;
  if (isinf(x)) {
    if (x > 0.0)
      return x;
    errno = EDOM;  // gamma oscillates without limit towards -infinity
    return (x - x) / (x - x);
  }
  if (x == 0.0) {
    errno = ERANGE;
    return copysign(HUGE_VAL, x);
  }

  if (x < 0.0) {
    s = sinpi(x);
    if (s == 0.0) {  // a negative integer: gamma has a pole with no sign
      errno = EDOM;
      return (x - x) / (x - x);
    }
    // gamma(1-x) as (-x)*gamma(-x), so that the argument of the recursive
    // call is exact rather than rounded
    t = -x;
    if (t > 170.0) {
      // t*gamma(t) would overflow, so take logarithms and let the
      // exponential deliver the very small answer directly
      hi = stirling(t, &lo);
      r = exp((((LOGPI - log(fabs(s))) - log(t)) - hi) - lo);
      if (r == 0.0)
        errno = ERANGE;
      return s < 0.0 ? -r : r;
    }
    return PI_HI / (s * (t * tgamma(t)));
  }

  if (x >= 12.0) {
    if (x > 171.7) {
      errno = ERANGE;
      return 0x1p1023 * 0x1p1023;
    }
    // the logarithm is carried wide here because the exponential magnifies
    // whatever error it has by the size of the logarithm itself
    hi = stirling(x, &lo);
    return exp(hi) * (1.0 + lo);
  }

  if (x < 1.0)
    return exp(x * (x - 1.0) * gpoly(x)) / x;
  if (x <= 2.0) {
    y = x - 1.0;
    return exp(y * (y - 1.0) * gpoly(y));
  }
  n = (int)x - 1;
  r = x - (double)n;
  p = 1.0;
  for (i = 0; i < n; i++)
    p *= r + (double)i;
  y = r - 1.0;
  return exp(y * (y - 1.0) * gpoly(y)) * p;
}

float
lgammaf(float x)
{
  return (float)lgamma((double)x);
}

float
tgammaf(float x)
{
  double r = tgamma((double)x);
  float f = (float)r;

  if (isfinite(r) && r != 0.0 && (!isfinite(f) || f == 0.0f))
    errno = ERANGE;
  return f;
}

// the binary128 forms keep the wider range: stirling's series is evaluated
// in binary128 for large arguments, so lgammal and tgammal work well past
// the point where a double would overflow. the precision is still that of
// the double logarithm underneath, about 53 bits, and tgammal loses more
// than that at the top of its range because the exponential magnifies the
// logarithm's error by the logarithm's own size.
static long double
stirlingl(long double x)
{
  long double z, ser;

  z = 1.0L / (x * x);
  ser = (0.083333333333333333333333333333333333L +
         z * (-0.002777777777777777777777777777777778L +
         z * (0.000793650793650793650793650793650794L +
         z * (-0.000595238095238095238095238095238095L +
         z * (0.000841750841750841750841750841750842L +
         z * (-0.001917526917526917526917526917526918L +
         z * 0.006410256410256410256410256410256410L)))))) / x;
  return (x - 0.5L) * logl(x) - x + 0.918938533204672741780329736405618L + ser;
}

long double
lgammal(long double x)
{
  long double s;

  signgam = 1;
  if (isnan(x))
    return x;
  if (isinf(x))
    return HUGE_VALL;
  if (x == 0.0L) {
    errno = ERANGE;
    signgam = signbit(x) ? -1 : 1;
    return HUGE_VALL;
  }
  if (x > 0.0L) {
    if (x >= 12.0L)
      return stirlingl(x);
    return (long double)lgamma((double)x);
  }
  if (x > -0x1p52L && floorl(x) == x) {
    errno = ERANGE;
    return HUGE_VALL;
  }
  s = (long double)sinpi((double)fmodl(x, 2.0L));
  signgam = s < 0.0L ? -1 : 1;
  if (1.0L - x >= 12.0L)
    return (1.1447298858494001741434273513530587L - logl(fabsl(s))) - stirlingl(1.0L - x);
  return (long double)lgamma((double)x);
}

long double
tgammal(long double x)
{
  long double t, s;

  if (isnan(x))
    return x;
  if (isinf(x)) {
    if (x > 0.0L)
      return x;
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x == 0.0L) {
    errno = ERANGE;
    return copysignl(HUGE_VALL, x);
  }
  if (x > 0.0L && x < 171.0L)
    return (long double)tgamma((double)x);
  if (x < 0.0L) {
    if (x > -0x1p52L && floorl(x) == x) {
      errno = EDOM;
      return (x - x) / (x - x);
    }
    if (x > -171.0L)
      return (long double)tgamma((double)x);
    s = (long double)sinpi((double)fmodl(x, 2.0L));
    t = expl(1.1447298858494001741434273513530587L - logl(fabsl(s)) - stirlingl(1.0L - x));
    if (t == 0.0L)
      errno = ERANGE;
    return s < 0.0L ? -t : t;
  }
  if (x > 1756.0L) {
    errno = ERANGE;
    return 0x1p16383L * 0x1p16383L;
  }
  return expl(stirlingl(x));
}
