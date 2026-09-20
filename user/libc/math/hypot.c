// hypot: the length of a right triangle's hypotenuse.
//
// the obvious sqrt(x*x + y*y) is wrong twice over. it overflows when the
// answer would not, because squaring doubles the exponent, and it rounds
// twice, once in the sum and once in the root, which costs about a bit and
// a half of the result.
//
// scaling the larger argument into [1, 2) fixes the range: the scale is a
// power of two, so it is exact going in and coming out, and only the last
// step can overflow, which is when the answer really does.
//
// the precision is recovered with fma. the exact remainder of each square
// is available for free, the two squares are added with their remainders
// kept, and the square root of that pair is corrected by one newton step
// whose residual is again exact. what is left is a single rounding.

#include <math.h>
#include <errno.h>

double
hypot(double x, double y)
{
  double a, b, t, w, we, v, ve, s, se, h;
  int k;

  // an infinite side gives an infinite hypotenuse even if the other side is
  // a NaN, which is the one place C99 asks for a NaN to be ignored
  if (isinf(x) || isinf(y))
    return HUGE_VAL;
  if (isnan(x) || isnan(y))
    return x + y;

  a = fabs(x);
  b = fabs(y);
  if (b > a) {
    t = a;
    a = b;
    b = t;
  }
  if (b == 0.0)
    return a;

  k = ilogb(a);
  a = scalbn(a, -k);
  b = scalbn(b, -k);

  // a*a + b*b as a head and an exact tail
  w = a * a;
  we = fma(a, a, -w);
  v = b * b;
  ve = fma(b, b, -v);
  s = w + v;
  se = ((w - s) + v) + (we + ve);

  h = sqrt(s);
  // sqrt(s + se) = h + (s + se - h*h)/(2h), and fma gives the residual of
  // the root exactly
  h += (fma(-h, h, s) + se) / (2.0 * h);

  h = scalbn(h, k);
  if (isinf(h))
    errno = ERANGE;
  return h;
}

// a float squared is exact as a double, so their sum is one rounding away
// from exact and the root is the only other; no scaling is needed because
// no float squared can leave the double range
float
hypotf(float x, float y)
{
  double a = (double)x, b = (double)y;

  if (isinf(x) || isinf(y))
    return HUGE_VALF;
  if (isnan(x) || isnan(y))
    return x + y;
  return (float)sqrt(a * a + b * b);
}

// binary128 has no hardware fma, so the tails are left out here: the sum
// and the root each round once, which costs about a bit at the bottom of a
// 113 bit answer
long double
hypotl(long double x, long double y)
{
  long double a, b, t, h;
  int k;

  if (isinf(x) || isinf(y))
    return HUGE_VALL;
  if (isnan(x) || isnan(y))
    return x + y;

  a = fabsl(x);
  b = fabsl(y);
  if (b > a) {
    t = a;
    a = b;
    b = t;
  }
  if (b == 0.0L)
    return a;

  k = ilogbl(a);
  a = scalbnl(a, -k);
  b = scalbnl(b, -k);
  h = scalbnl(sqrtl(a * a + b * b), k);
  if (isinf(h))
    errno = ERANGE;
  return h;
}
