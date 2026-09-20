// the handful of functions that the F and D extensions answer directly.
//
// square root and fused multiply-add are single instructions for float and
// for double, so these are wrappers around fsqrt and fmadd rather than
// algorithms. the instructions already produce the special cases the
// standard asks for (a negative operand gives a NaN and raises invalid, a
// zero keeps its sign), so all that is left for us is errno.
//
// binary128 has no hardware, so sqrtl starts from the double result and
// polishes it with newton's iteration, and fmal builds the exact product
// with dekker's splitting.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

double
sqrt(double x)
{
  double r;

  asm("fsqrt.d %0, %1" : "=f"(r) : "f"(x));
  if (x < 0.0)
    errno = EDOM;
  return r;
}

float
sqrtf(float x)
{
  float r;

  asm("fsqrt.s %0, %1" : "=f"(r) : "f"(x));
  if (x < 0.0f)
    errno = EDOM;
  return r;
}

double
fma(double x, double y, double z)
{
  double r;

  asm("fmadd.d %0, %1, %2, %3" : "=f"(r) : "f"(x), "f"(y), "f"(z));
  return r;
}

float
fmaf(float x, float y, float z)
{
  float r;

  asm("fmadd.s %0, %1, %2, %3" : "=f"(r) : "f"(x), "f"(y), "f"(z));
  return r;
}

// dekker's splitting: multiplying by 2^57+1 and subtracting rounds a
// binary128 number to its top 56 bits, and the remainder keeps the rest.
// every partial product of two such halves is then exact.
static void
split(long double a, long double *hi, long double *lo)
{
  long double t;

  t = (0x1p57L + 1.0L) * a;
  *hi = t - (t - a);
  *lo = a - *hi;
}

// the error of a rounded binary128 product, so that a*b == p + mulerr(a,b,p)
// exactly. both factors must be scaled into [0.5, 1) by the caller, which
// keeps the splitting from overflowing.
static long double
mulerr(long double a, long double b, long double p)
{
  long double ah, al, bh, bl;

  split(a, &ah, &al);
  split(b, &bh, &bl);
  return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

long double
sqrtl(long double x)
{
  long double y, m;
  int e;

  if (isnan(x) || x == 0.0L || (isinf(x) && x > 0.0L))
    return x + x;
  if (x < 0.0L) {
    errno = EDOM;
    return (x - x) / (x - x);
  }

  // split off an even power of two so the mantissa stays in [0.5, 2), where
  // the double result below is a good starting point
  m = frexpl(x, &e);
  if (e & 1) {
    m *= 2.0L;
    e--;
  }

  // newton's iteration for sqrt doubles the number of correct bits each
  // time, so two passes carry the 53 bits of the double seed past the 113
  // that binary128 holds. the result is within an ulp or so of correct but
  // is not correctly rounded.
  y = (long double)sqrt((double)m);
  y = 0.5L * (y + m / y);
  y = 0.5L * (y + m / y);
  return scalbnl(y, e / 2);
}

long double
fmal(long double x, long double y, long double z)
{
  long double mx, my, mz, p, pe, s, se;
  int ex, ey, ez;

  // with a zero, an infinity or a NaN in hand there is only one rounding to
  // begin with, so the plain expression is already the right answer
  if (!isfinite(x) || !isfinite(y) || !isfinite(z) || x == 0.0L || y == 0.0L)
    return x * y + z;

  mx = frexpl(x, &ex);
  my = frexpl(y, &ey);
  p = mx * my;
  pe = mulerr(mx, my, p);
  ex += ey;
  if (z == 0.0L)
    return scalbnl(p + pe, ex);

  // line the addend up with the product. when it is far enough away that no
  // bit of it can reach the product's mantissa, the ordinary expression
  // rounds the same way and we hand the work back to the compiler.
  frexpl(z, &ez);
  if (ez > ex + 200 || ez < ex - 250)
    return x * y + z;
  mz = scalbnl(z, -ex);

  // one two-sum to collect the exact head and tail, then a single rounding.
  // the tail itself is rounded once more, so a result that lands exactly
  // between two binary128 numbers may go the wrong way.
  s = p + mz;
  se = (p - s) + mz;
  return scalbnl(s + (se + pe), ex);
}
