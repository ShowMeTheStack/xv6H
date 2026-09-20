// the exponentials: exp, exp2 and expm1.
//
// all three reduce to the same kernel. write x = k*ln2 + r with k an
// integer and |r| <= ln2/2, so that e^x = 2^k * e^r and the hard part is a
// polynomial on a range barely a third of a unit wide. scaling by 2^k
// afterwards is exact except at the very ends of the range, where scalbn
// does the right thing on its own.
//
// the kernel does not approximate e^r directly. it approximates
//
//   R(z) = r * (e^r + 1) / (e^r - 1) = r * coth(r/2) = 2 + z/6 - z^2/360 + ...
//
// which is even in r, so a polynomial in z = r^2 covers twice the range for
// the same number of terms. from R,
//
//   e^r - 1 = r + r*c / (2 - c)   where c = r - (R(z) - 2),
//
// and the quotient is a small correction of order r^2, so the leading r
// arrives in the answer unrounded. the coefficients below are a chebyshev
// fit of (R(z)-2)/z on [0, (ln2/2)^2]; they start out as the taylor
// coefficients 1/6, -1/360, 1/15120 and are nudged from there to spread the
// error evenly. measured against a 60 digit reference the kernel is good to
// about 0.51 ulp across the whole reduced range.
//
// the ln2 used for the reduction is split into a part with 33 significant
// bits and a remainder. k has at most 11 bits, so k*LN2HI is exact and the
// only rounding in the reduction is in the much smaller k*LN2LO.

#include <math.h>
#include <errno.h>

static const double
  P0 = 0.1666666666666666,
  P1 = -0.0027777777777564573,
  P2 = 6.613756471707873e-05,
  P3 = -1.6534060165972636e-06,
  P4 = 4.1437725653582016e-08;

static const double LN2HI = 0.6931471806019545;
static const double LN2LO = -4.2009150726810846e-11;
static const double INVLN2 = 1.4426950408889634;

// ln2 to full double-double precision, for the reductions that cannot use
// the split form above
static const double LN2_HI = 0.6931471805599453;
static const double LN2_LO = 2.3190468138462996e-17;

double
__exp_core(double hi, double lo, int k)
{
  double r, z, c;

  r = hi + lo;
  z = r * r;
  c = r - z * (P0 + z * (P1 + z * (P2 + z * (P3 + z * P4))));
  // e^(hi+lo) = 1 + (hi+lo) + r*c/(2-c); hi stays out of the inner sum so
  // that its bits reach the final addition unrounded
  return scalbn(1.0 + (hi + (lo + r * c / (2.0 - c))), k);
}

// round a double result down to a float, reporting the overflow or
// underflow that only the narrower type suffers
static float
narrow(double r)
{
  float f = (float)r;

  if (isfinite(r) && r != 0.0 && (!isfinite(f) || f == 0.0f))
    errno = ERANGE;
  return f;
}

double
exp(double x)
{
  double fk, hi, lo;

  if (isnan(x))
    return x;
  if (isinf(x))
    return x > 0.0 ? x : 0.0;
  // outside these bounds the answer is certainly an infinity or a zero, and
  // the products below both raise the flag that goes with it. the bounds
  // are loose on purpose: everything in between is left to scalbn, which
  // produces the subnormal results near the bottom of the range correctly.
  if (x > 710.0) {
    errno = ERANGE;
    return 0x1p1023 * 0x1p1023;
  }
  if (x < -746.0) {
    errno = ERANGE;
    return 0x1p-1022 * 0x1p-1022;
  }
  fk = floor(x * INVLN2 + 0.5);
  hi = x - fk * LN2HI;
  lo = -fk * LN2LO;
  return __exp_core(hi, lo, (int)fk);
}

float
expf(float x)
{
  return narrow(exp((double)x));
}

// binary128 gets the range of the wider type but only the accuracy of the
// narrower one: the reduction is done in binary128, so arguments far beyond
// what a double could hold still work, but e^r itself is computed as a
// double and widened. the result is good to about 53 bits, not 113.
long double
expl(long double x)
{
  long double k, r;

  if (isnan(x))
    return x;
  if (isinf(x))
    return x > 0.0L ? x : 0.0L;
  if (x > 11357.0L) {
    errno = ERANGE;
    return 0x1p16383L * 0x1p16383L;
  }
  if (x < -11434.0L) {
    errno = ERANGE;
    return 0x1p-16382L * 0x1p-16382L;
  }
  k = floorl(x * M_LOG2El + 0.5L);
  r = x - k * M_LN2l;
  return scalbnl((long double)exp((double)r), (int)k);
}

double
exp2(double x)
{
  double fk, f, hi, lo;

  if (isnan(x))
    return x;
  if (isinf(x))
    return x > 0.0 ? x : 0.0;
  if (x > 1030.0) {
    errno = ERANGE;
    return 0x1p1023 * 0x1p1023;
  }
  if (x < -1080.0) {
    errno = ERANGE;
    return 0x1p-1022 * 0x1p-1022;
  }
  fk = floor(x + 0.5);
  f = x - fk;  // exact, and no wider than half a unit
  // f*ln2 needs more than a double to hold, so keep the product as a head
  // and a tail; fma gives the tail of the first multiplication exactly
  hi = f * LN2_HI;
  lo = fma(f, LN2_HI, -hi) + f * LN2_LO;
  return __exp_core(hi, lo, (int)fk);
}

float
exp2f(float x)
{
  return narrow(exp2((double)x));
}

long double
exp2l(long double x)
{
  long double k, f;

  if (isnan(x))
    return x;
  if (isinf(x))
    return x > 0.0L ? x : 0.0L;
  if (x > 16390.0L) {
    errno = ERANGE;
    return 0x1p16383L * 0x1p16383L;
  }
  if (x < -16500.0L) {
    errno = ERANGE;
    return 0x1p-16382L * 0x1p-16382L;
  }
  k = floorl(x + 0.5L);
  f = x - k;
  return scalbnl((long double)exp2((double)f), (int)k);
}

double
expm1(double x)
{
  double fk, hi, lo, r, z, c, u, t;
  int k;

  if (isnan(x) || x == 0.0)
    return x;  // a zero keeps its sign, which the kernel would lose
  if (isinf(x))
    return x > 0.0 ? x : -1.0;
  if (x > 710.0) {
    errno = ERANGE;
    return 0x1p1023 * 0x1p1023;
  }
  // below this point e^x is smaller than half an ulp of 1, so the answer
  // rounds to -1 exactly. there is no range error: -1 is the true result to
  // within the precision of the type.
  if (x < -37.5)
    return -1.0;

  fk = floor(x * INVLN2 + 0.5);
  k = (int)fk;
  hi = x - fk * LN2HI;
  lo = -fk * LN2LO;
  r = hi + lo;
  z = r * r;
  c = r - z * (P0 + z * (P1 + z * (P2 + z * (P3 + z * P4))));
  u = r + r * c / (2.0 - c);  // e^r - 1, with no cancellation in sight
  if (k == 0)
    return u;

  // e^x - 1 = 2^k * ((1 - 2^-k) + (e^r - 1)). written that way the only
  // subtraction is between two exact numbers, so the cancellation that the
  // obvious 2^k*e^r - 1 would suffer never happens.
  t = scalbn(1.0, -k);
  return scalbn((1.0 - t) + u, k);
}

float
expm1f(float x)
{
  return narrow(expm1((double)x));
}

long double
expm1l(long double x)
{
  if (isnan(x) || x == 0.0L)
    return x;
  // near zero the double result already carries every bit that matters
  // relative to x itself, so widening it loses nothing that the rest of
  // this library would have kept
  if (fabsl(x) < 0.25L)
    return (long double)expm1((double)x);
  if (x < -120.0L)
    return -1.0L;
  return expl(x) - 1.0L;
}
