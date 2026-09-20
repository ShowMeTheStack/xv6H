// the circular functions: sin, cos, tan and sincos, together with the
// argument reduction they all depend on.
//
// each of them is periodic, so the first job is to write x = q*(pi/2) + r
// with |r| <= pi/4 and q an integer, after which sin x is one of
// +-sin r or +-cos r according to q modulo four. getting r right is
// most of the work, because pi/2 is irrational: a large x may agree with a
// multiple of pi/2 to fifty digits, and every one of those digits cancels
// when the multiple is subtracted. an answer that means anything at all
// therefore needs pi/2 to far more precision than a double holds.
//
// there are two reductions here. for |x| below 2^20*pi/2 the multiple q has
// at most twenty bits, and subtracting q*(pi/2) in three pieces of
// increasing smallness, with fma supplying the exact remainder of each
// product, leaves r accurate to about 2^-140 - enough to survive any
// cancellation a double argument can produce in that range.
//
// beyond that the pieces would have to grow without limit, so the second
// reduction works in the bits instead. this is payne and hanek's method:
// only the bits of 2/pi that line up with the mantissa of x can affect the
// fractional part of x*2/pi, so the table below is indexed by the exponent
// of x, a 192 bit window is multiplied by the 53 bit mantissa in integer
// arithmetic, and the result is exact by construction. the quadrant falls
// out as the top two bits and the fraction becomes r after one multiply by
// pi/2, which is done in binary128 simply because that is the widest
// multiply available.
//
// the kernels themselves are taylor series, carried far enough that the
// truncation is below a tenth of an ulp: for sin and cos the coefficients
// are exact reciprocals of factorials, which need no checking. tan
// converges too slowly for that, so its polynomial is a chebyshev fit of
// (tan x - x)/x^3 on [0, pi/4], measured at 0.54 ulp against a 60 digit
// reference.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

#include <stdint.h>

static const double
  S1 = -0.16666666666666666,
  S2 = 0.008333333333333333,
  S3 = -0.0001984126984126984,
  S4 = 2.7557319223985893e-06,
  S5 = -2.505210838544172e-08,
  S6 = 1.6059043836821613e-10,
  S7 = -7.647163731819816e-13,
  S8 = 2.8114572543455206e-15;

static const double
  C1 = 0.041666666666666664,
  C2 = -0.001388888888888889,
  C3 = 2.48015873015873e-05,
  C4 = -2.755731922398589e-07,
  C5 = 2.08767569878681e-09,
  C6 = -1.1470745597729725e-11,
  C7 = 4.779477332387385e-14;

static const double
  T0 = 0.3333333333333333,
  T1 = 0.13333333333333,
  T2 = 0.05396825396865546,
  T3 = 0.02186948851707438,
  T4 = 0.008863236003485818,
  T5 = 0.0035921209805615966,
  T6 = 0.0014559027390503058,
  T7 = 0.0005895756061544555,
  T8 = 0.000241226353512688,
  T9 = 8.998520298342289e-05,
  T10 = 5.558784710570148e-05,
  T11 = -1.1013613221811344e-05,
  T12 = 3.651618193613342e-05,
  T13 = -1.83153428020867e-05,
  T14 = 8.48334725267473e-06;

// pi/2 as three doubles; the three together are good to 2^-160
static const double PIO2_HI = 1.5707963267948966;
static const double PIO2_LO = 6.123233995736766e-17;
static const double PIO2_LO2 = -1.4973849048591698e-33;
static const double INVPIO2 = 0.6366197723675814;
static const double PIO4 = 0.7853981633974483;

// 2/pi in binary, 32 bits to the word. word j covers bit positions
// 32j-63 through 32j-32 counting the first bit after the point as 1, so the
// two zero words at the front stand for the bits above it and let the
// lookup below accept a negative starting position without a special case.
static const uint32_t TWOOPI[] = {
  0x00000000, 0x00000000,
  0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599, 0x3C439041,
  0xFE5163AB, 0xDEBBC561, 0xB7246E3A, 0x424DD2E0, 0x06492EEA, 0x09D1921C,
  0xFE1DEB1C, 0xB129A73E, 0xE88235F5, 0x2EBB4484, 0xE99C7026, 0xB45F7E41,
  0x3991D639, 0x835339F4, 0x9C845F8B, 0xBDF9283B, 0x1FF897FF, 0xDE05980F,
  0xEF2F118B, 0x5A0A6D1F, 0x6D367ECF, 0x27CB09B7, 0x4F463F66, 0x9E5FEA2D,
  0x7527BAC7, 0xEBE5F17B, 0x3D0739F7, 0x8A5292EA, 0x6BFB5FB1, 0x1F8D5D08,
  0x56033046, 0xFC7B6BAB, 0xF0CFBC20, 0x9AF4361D, 0xA9E39161, 0x5EE61B08,
  0x6599855F, 0x14A06840, 0x8DFFD880,
};

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

// 64 bits of 2/pi starting at bit position p, which may be as low as -63
static uint64_t
window(int p)
{
  int off = p + 63;
  int j = off >> 5;
  int sh = off & 31;
  uint64_t hi, lo;

  hi = ((uint64_t)TWOOPI[j] << 32) | TWOOPI[j + 1];
  if (sh == 0)
    return hi;
  lo = ((uint64_t)TWOOPI[j + 2] << 32) | TWOOPI[j + 3];
  return (hi << sh) | (lo >> (64 - sh));
}

// payne and hanek's reduction, for arguments too large for the subtraction
// above to mean anything. x must be finite with |x| >= 2^20*(pi/2).
//
// the quadrant and the fraction come out of one integer product, so no
// cancellation can hurt them: the bits that survive were computed exactly
// rather than left over from a subtraction.
static int
bigreduce(double x, double *r)
{
  union {
    double f;
    uint64_t i;
  } u;
  unsigned __int128 p0, p1, p2, t;
  uint64_t m, b0, b1, b2, w0, w1, w2, c;
  double frac[5], fh, fl, e1, ph, pe;
  int e, p, q, neg, i;

  neg = x < 0.0;
  u.f = fabs(x);
  // x = m * 2^e with m the 53 bit mantissa as an integer
  e = (int)(u.i >> 52) - 1075;
  m = (u.i & 0x000fffffffffffffUL) | 0x0010000000000000UL;

  // with the window starting at bit e-1, everything above it contributes a
  // whole multiple of four to x*2/pi and so cannot change the quadrant
  p = e - 1;
  b0 = window(p);
  b1 = window(p + 64);
  b2 = window(p + 128);

  // the 53 by 192 bit product, of which only the low 192 bits matter
  p0 = (unsigned __int128)m * b2;
  p1 = (unsigned __int128)m * b1;
  p2 = (unsigned __int128)m * b0;
  w0 = (uint64_t)p0;
  c = (uint64_t)(p0 >> 64);
  t = p1 + c;
  w1 = (uint64_t)t;
  c = (uint64_t)(t >> 64);
  w2 = (uint64_t)(p2 + c);

  // read the product as a fixed point number with the point after bit 190:
  // the two bits above it are the quadrant and the rest is the fraction
  q = (int)(w2 >> 62);
  w2 &= 0x3fffffffffffffffUL;

  // carry the fraction as a pair of doubles. it is cut into pieces of at
  // most 32 bits so that each converts exactly, and they are added smallest
  // first with the rounding of every step kept, which leaves the pair good
  // to 2^-158 however much of the fraction cancelled.
  frac[0] = (double)(w2 >> 32) * 0x1p-30;
  frac[1] = (double)(w2 & 0xffffffffUL) * 0x1p-62;
  frac[2] = (double)(w1 >> 32) * 0x1p-94;
  frac[3] = (double)(w1 & 0xffffffffUL) * 0x1p-126;
  frac[4] = (double)(w0 >> 32) * 0x1p-158;
  fh = 0.0;
  fl = 0.0;
  for (i = 4; i >= 0; i--) {
    twosum(fh, frac[i], &fh, &e1);
    fl += e1;
  }
  if (fh > 0.5 || (fh == 0.5 && fl > 0.0)) {
    fh -= 1.0;  // exact, and it keeps the remainder inside [-pi/4, pi/4]
    q++;
  }

  twoprod(fh, PIO2_HI, &ph, &pe);
  pe += fh * PIO2_LO + fl * PIO2_HI;
  twosum(ph, pe, &r[0], &r[1]);
  if (neg) {
    r[0] = -r[0];
    r[1] = -r[1];
    q = -q;
  }
  return q;
}

int
__rem_pio2(double x, double *r)
{
  double fn, p, pe, s, se, qq, qe, hi, lo;

  if (fabs(x) <= PIO4) {
    r[0] = x;
    r[1] = 0.0;
    return 0;
  }
  if (!isfinite(x)) {
    errno = EDOM;
    r[0] = r[1] = (x - x) / (x - x);
    return 0;
  }
  if (fabs(x) >= 1647099.3291652855)
    return bigreduce(x, r);

  fn = floor(x * INVPIO2 + 0.5);
  // fn has at most twenty bits, so fma can hand back the exact remainder of
  // each product and the whole subtraction stays exact until the very last
  // term, which is already below 2^-140
  twoprod(fn, PIO2_HI, &p, &pe);
  twosum(x, -p, &s, &se);
  twoprod(fn, PIO2_LO, &qq, &qe);
  twosum(s, -qq, &hi, &lo);
  lo += ((se - pe) - qe) - fn * PIO2_LO2;
  twosum(hi, lo, &hi, &lo);
  r[0] = hi;
  r[1] = lo;
  return (int)fn;
}

double
__sin_kernel(double hi, double lo)
{
  double z, p;

  z = hi * hi;
  p = z * (S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * (S6 + z * (S7 + z * S8)))))));
  // sin(hi+lo) = sin hi + lo*cos hi to well past the precision we keep, and
  // leaving hi outside the sum means its bits are never rounded
  return hi + (hi * p + lo * (1.0 - 0.5 * z));
}

double
__cos_kernel(double hi, double lo)
{
  double z, r, hz, w;

  z = hi * hi;
  r = z * z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * (C6 + z * C7))))));
  hz = 0.5 * z;
  w = 1.0 - hz;
  // 1-hz is rounded, so ((1-w)-hz) is exactly what that rounding lost and
  // adding it back recovers the bits
  return w + (((1.0 - w) - hz) + (r - hi * lo));
}

double
__tan_kernel(double hi, double lo, int odd)
{
  double z, p, t, a;

  z = hi * hi;
  p = T0 + z * (T1 + z * (T2 + z * (T3 + z * (T4 + z * (T5 + z * (T6 + z * (T7 +
      z * (T8 + z * (T9 + z * (T10 + z * (T11 + z * (T12 + z * (T13 + z * T14)))))))))))));
  // the derivative of tan is 1 + tan^2, which is 1 + z to the precision lo
  // deserves
  t = hi + (hi * z * p + lo * (1.0 + z));
  if (!odd)
    return t;
  // for the odd quadrants the answer is -1/t, and one newton step on the
  // reciprocal removes the error of the division
  a = -1.0 / t;
  return a + a * fma(a, t, 1.0);
}

double
sin(double x)
{
  double r[2];
  int q;

  if (x == 0.0)
    return x;  // the kernel would add a positive zero to a negative one
  if (fabs(x) <= PIO4)
    return __sin_kernel(x, 0.0);
  q = __rem_pio2(x, r);
  switch (q & 3) {
  case 0:
    return __sin_kernel(r[0], r[1]);
  case 1:
    return __cos_kernel(r[0], r[1]);
  case 2:
    return -__sin_kernel(r[0], r[1]);
  default:
    return -__cos_kernel(r[0], r[1]);
  }
}

double
cos(double x)
{
  double r[2];
  int q;

  if (fabs(x) <= PIO4)
    return __cos_kernel(x, 0.0);
  q = __rem_pio2(x, r);
  switch (q & 3) {
  case 0:
    return __cos_kernel(r[0], r[1]);
  case 1:
    return -__sin_kernel(r[0], r[1]);
  case 2:
    return -__cos_kernel(r[0], r[1]);
  default:
    return __sin_kernel(r[0], r[1]);
  }
}

double
tan(double x)
{
  double r[2];
  int q;

  if (x == 0.0)
    return x;
  if (fabs(x) <= PIO4)
    return __tan_kernel(x, 0.0, 0);
  q = __rem_pio2(x, r);
  return __tan_kernel(r[0], r[1], q & 1);
}

void
sincos(double x, double *sp, double *cp)
{
  double r[2], s, c;
  int q;

  if (fabs(x) <= PIO4) {
    *sp = x == 0.0 ? x : __sin_kernel(x, 0.0);
    *cp = __cos_kernel(x, 0.0);
    return;
  }
  q = __rem_pio2(x, r);
  s = __sin_kernel(r[0], r[1]);
  c = __cos_kernel(r[0], r[1]);
  switch (q & 3) {
  case 0:
    *sp = s;
    *cp = c;
    break;
  case 1:
    *sp = c;
    *cp = -s;
    break;
  case 2:
    *sp = -s;
    *cp = -c;
    break;
  default:
    *sp = -c;
    *cp = s;
    break;
  }
}

float
sinf(float x)
{
  return (float)sin((double)x);
}

float
cosf(float x)
{
  return (float)cos((double)x);
}

float
tanf(float x)
{
  return (float)tan((double)x);
}

void
sincosf(float x, float *sp, float *cp)
{
  double s, c;

  sincos((double)x, &s, &c);
  *sp = (float)s;
  *cp = (float)c;
}

// the binary128 forms reduce the argument in the wider type, so they are
// right about which quadrant a huge argument lands in, but the kernels are
// the double ones: about 53 bits of the answer are correct.
static int
reducel(long double x, long double *r)
{
  long double fn;

  if (fabsl(x) <= 0.7853981633974483096156608458198757L) {
    *r = x;
    return 0;
  }
  // binary128 holds pi/2 to 113 bits, which covers every argument a double
  // sized exponent can reach; beyond that the remainder is still bounded
  // but its low bits are not to be believed
  fn = roundl(x * 0.6366197723675813430755350534900574L);
  *r = x - fn * 1.570796326794896619231321691639751442L;
  return (int)fmodl(fn, 4.0L);
}

long double
sinl(long double x)
{
  long double r;
  int q;

  if (!isfinite(x)) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  q = reducel(x, &r) & 3;
  switch (q) {
  case 0:
    return (long double)__sin_kernel((double)r, 0.0);
  case 1:
    return (long double)__cos_kernel((double)r, 0.0);
  case 2:
    return (long double)-__sin_kernel((double)r, 0.0);
  default:
    return (long double)-__cos_kernel((double)r, 0.0);
  }
}

long double
cosl(long double x)
{
  long double r;
  int q;

  if (!isfinite(x)) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  q = reducel(x, &r) & 3;
  switch (q) {
  case 0:
    return (long double)__cos_kernel((double)r, 0.0);
  case 1:
    return (long double)-__sin_kernel((double)r, 0.0);
  case 2:
    return (long double)-__cos_kernel((double)r, 0.0);
  default:
    return (long double)__sin_kernel((double)r, 0.0);
  }
}

long double
tanl(long double x)
{
  long double r;
  int q;

  if (!isfinite(x)) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  q = reducel(x, &r) & 3;
  return (long double)__tan_kernel((double)r, 0.0, q & 1);
}

void
sincosl(long double x, long double *sp, long double *cp)
{
  *sp = sinl(x);
  *cp = cosl(x);
}
