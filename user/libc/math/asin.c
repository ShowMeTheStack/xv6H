// the inverse circular functions: asin, acos, atan and atan2.
//
// asin and acos share one polynomial. on [-1/2, 1/2] the series
//
//   asin x = x + x^3 * R(x^2)
//
// is used directly, with R a chebyshev fit measured at 0.52 ulp. outside
// that range the identity asin x = pi/2 - 2*asin(sqrt((1-x)/2)) folds the
// argument back into it, and since the new argument is a square root its
// own rounding has to be undone: the exact residual of the root comes from
// fma, and the correction is carried alongside. acos uses the mirror
// identities, choosing the one that keeps the subtraction away from the
// cancelling end.
//
// atan works from a table. the addition rule
//
//   atan x = atan c + atan((x - c)/(1 + c*x))
//
// with c the nearest sixteenth to x leaves a remainder below 1/32, where
// six terms of the alternating series are already past double precision.
// the divisor is formed exactly with fma, so the reduction contributes
// nothing of its own, and atan c is tabulated as a pair of doubles because
// a single one would lose the low bits of a constant that is added to a
// much smaller number.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

static const double
  R0 = 0.16666666666666669,
  R1 = 0.07499999999998433,
  R2 = 0.04464285714635543,
  R3 = 0.030381944138531247,
  R4 = 0.02237217294214989,
  R5 = 0.017352392720869973,
  R6 = 0.013971212973552933,
  R7 = 0.011479177415184906,
  R8 = 0.01032281435018578,
  R9 = 0.005457506718640358,
  R10 = 0.01740087944269402,
  R11 = -0.014851887071247204,
  R12 = 0.028757851367421566;

static const double PIO2_HI = 1.5707963267948966;
static const double PIO2_LO = 6.123233995736766e-17;
static const double PI_HI = 3.141592653589793;
static const double PI_LO = 1.2246467991473532e-16;

// atan(i/16), each as a head and the tail it could not hold
static const double ATANHI[17] = {
  0.0, 0.06241880999595735, 0.12435499454676144,
  0.18534794999569476, 0.24497866312686414, 0.3028848683749714,
  0.35877067027057225, 0.4124104415973873, 0.4636476090008061,
  0.5123894603107377, 0.5585993153435624, 0.6022873461349642,
  0.6435011087932844, 0.6823165548747481, 0.7188299996216245,
  0.7531512809621944, 0.7853981633974483,
};
static const double ATANLO[17] = {
  0.0, -1.5490756308295046e-18, -3.1253241424539383e-18,
  4.180692268843079e-18, 1.0698755618734451e-17, -1.1010827903001369e-17,
  -2.4623815582638635e-17, -1.587652227770689e-17, 2.2698777452961687e-17,
  -2.5462781472855804e-17, -5.4556305485916264e-18, 2.950430737228402e-17,
  1.5834785051444286e-17, 6.943223671560008e-18, -2.1478388444456983e-17,
  -2.4256934659182068e-17, 3.061616997868383e-17,
};

// (asin(x) - x) / x^3 for z = x*x in [0, 1/4]
static double
asinr(double z)
{
  return R0 + z * (R1 + z * (R2 + z * (R3 + z * (R4 + z * (R5 + z * (R6 +
         z * (R7 + z * (R8 + z * (R9 + z * (R10 + z * (R11 + z * R12)))))))))));
}

double
asin(double x)
{
  double a, z, s, c, p;

  if (isnan(x))
    return x;
  a = fabs(x);
  if (a > 1.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (a < 0.5) {
    z = x * x;
    return x + x * z * asinr(z);
  }
  if (a == 1.0)
    return copysign(PIO2_HI, x) + copysign(PIO2_LO, x);

  // asin(x) = pi/2 - 2*asin(s), s = sqrt((1-|x|)/2)
  z = 0.5 * (1.0 - a);
  s = sqrt(z);
  // s is the rounded root; c is what it is short of, so s + c is sqrt(z) to
  // twice the precision and the doubling below does not magnify the error
  c = fma(-s, s, z) / (2.0 * s);
  p = s * z * asinr(z);  // the cubic term of asin(s)
  p = 2.0 * (p + c);
  // pi/2 - 2s - p, arranged so that the two small pieces meet first
  return copysign(PIO2_HI - (2.0 * s - (PIO2_LO - p)), x);
}

double
acos(double x)
{
  double a, z, s, c, p;

  if (isnan(x))
    return x;
  a = fabs(x);
  if (a > 1.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (a < 0.5) {
    // acos(x) = pi/2 - asin(x), and asin is small here so nothing cancels
    z = x * x;
    return PIO2_HI - (x - (PIO2_LO - x * z * asinr(z)));
  }
  if (x == 1.0)
    return 0.0;
  if (x == -1.0)
    return PI_HI + PI_LO;

  z = 0.5 * (1.0 - a);
  s = sqrt(z);
  c = fma(-s, s, z) / (2.0 * s);
  p = s * z * asinr(z);
  p = 2.0 * (s + (p + c));  // 2*asin(sqrt((1-|x|)/2))
  if (x > 0.0)
    return p;  // acos(x) = 2*asin(sqrt((1-x)/2))
  return PI_HI - (p - PI_LO);
}

// atan of a value whose magnitude is at most one
static double
atan_small(double x)
{
  double a, c, d, de, p, pe, u, ue, z, s;
  int i;

  // atan is odd, so the whole reduction runs on the magnitude and the sign
  // goes back on at the end. that also keeps the tabulated tails, which are
  // of both signs, out of reach of copysign.
  a = fabs(x);
  i = (int)(a * 16.0 + 0.5);
  c = (double)i * 0.0625;
  // the divisor 1 + c*a, with the part of the product that did not fit
  p = c * a;
  pe = fma(c, a, -p);
  d = 1.0 + p;
  de = ((1.0 - d) + p) + pe;  // d + de = 1 + c*a exactly
  u = (a - c) / d;            // a - c is exact: c is within 1/32 of a
  ue = (fma(-u, d, a - c) - u * de) / d;

  z = u * u;
  // atan u = u - u^3/3 + u^5/5 - ..., stopped where the next term is below
  // 2^-75 of the answer
  s = z * (-0.3333333333333333 + z * (0.2 + z * (-0.14285714285714285 +
      z * (0.1111111111111111 + z * (-0.09090909090909091 +
      z * (0.07692307692307693 + z * -0.06666666666666667))))));
  // the derivative of atan at u is 1/(1+u^2), which is how the tail of the
  // reduction reaches the answer
  s = u * s + ue / (1.0 + z);
  return copysign(ATANHI[i] + ((u + s) + ATANLO[i]), x);
}

double
atan(double x)
{
  double t, e, r;

  if (isnan(x))
    return x;
  if (isinf(x))
    return copysign(PIO2_HI, x) + copysign(PIO2_LO, x);
  if (fabs(x) <= 1.0)
    return atan_small(x);

  // atan x = pi/2 - atan(1/x). the reciprocal is rounded, so its residual
  // is fed back through the derivative of atan, which is 1/(1+t^2) and is
  // very nearly one for the small t this branch produces
  t = 1.0 / x;
  e = fma(-t, x, 1.0) * t;
  r = atan_small(t) + e / (1.0 + t * t);
  return copysign(PIO2_HI, x) - (r - copysign(PIO2_LO, x));
}

double
atan2(double y, double x)
{
  double r;

  if (isnan(x) || isnan(y))
    return x + y;
  if (y == 0.0) {
    // the sign of a zero numerator decides which side of the cut we are on
    if (signbit(x))
      return copysign(PI_HI + PI_LO, y);
    return copysign(0.0, y);
  }
  if (x == 0.0)
    return copysign(PIO2_HI + PIO2_LO, y);
  if (isinf(x)) {
    if (isinf(y))  // both infinite: the ratio is one of the four diagonals
      return copysign(x > 0.0 ? 0.7853981633974483 : 2.356194490192345, y);
    if (x > 0.0)
      return copysign(0.0, y);
    return copysign(PI_HI + PI_LO, y);
  }
  if (isinf(y))
    return copysign(PIO2_HI + PIO2_LO, y);

  r = atan(fabs(y / x));
  if (x < 0.0)
    r = (PI_HI - r) + PI_LO;
  return copysign(r, y);
}

float
asinf(float x)
{
  return (float)asin((double)x);
}

float
acosf(float x)
{
  return (float)acos((double)x);
}

float
atanf(float x)
{
  return (float)atan((double)x);
}

float
atan2f(float y, float x)
{
  return (float)atan2((double)y, (double)x);
}

// the binary128 forms compute in double and widen: they are accurate to
// about 53 bits, and the constants below supply the rest of pi/2 so that
// the results at least land on the right side of the range.
long double
asinl(long double x)
{
  if (isnan(x))
    return x;
  if (fabsl(x) > 1.0L) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (fabsl(x) == 1.0L)
    return copysignl(M_PI_2l, x);
  return (long double)asin((double)x);
}

long double
acosl(long double x)
{
  if (isnan(x))
    return x;
  if (fabsl(x) > 1.0L) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x == 1.0L)
    return 0.0L;
  if (x == -1.0L)
    return M_PIl;
  return (long double)acos((double)x);
}

long double
atanl(long double x)
{
  if (isnan(x))
    return x;
  if (isinf(x))
    return copysignl(M_PI_2l, x);
  // beyond this the double reciprocal is already zero to 53 bits, so the
  // answer is pi/2 to every bit a double kernel could supply
  if (fabsl(x) > 0x1p60L)
    return copysignl(M_PI_2l, x);
  return (long double)atan((double)x);
}

long double
atan2l(long double y, long double x)
{
  if (isnan(x) || isnan(y))
    return x + y;
  if (y == 0.0L) {
    if (signbit(x))
      return copysignl(M_PIl, y);
    return copysignl(0.0L, y);
  }
  if (x == 0.0L)
    return copysignl(M_PI_2l, y);
  if (isinf(x)) {
    if (isinf(y))
      return copysignl(x > 0.0L ? M_PI_4l : 3.0L * M_PI_4l, y);
    if (x > 0.0L)
      return copysignl(0.0L, y);
    return copysignl(M_PIl, y);
  }
  if (isinf(y))
    return copysignl(M_PI_2l, y);
  return (long double)atan2((double)y, (double)x);
}
