// rounding to an integral value: floor, ceil, trunc, round, rint,
// nearbyint, the lrint and lround families, and modf.
//
// these are done by masking, not by arithmetic. once the exponent says
// which mantissa bits sit below the binary point, truncation is an and,
// rounding away from zero is an add of that mask followed by the same and,
// and rounding to nearest is an add of just the top fraction bit. working
// in the bits this way is exact for every argument, never overflows, and
// never raises a flag that the caller did not ask for.
//
// rint is the exception: it has to follow whatever rounding mode fcsr
// currently selects, so it is the one that goes through the arithmetic unit
// and lets the hardware decide. nearbyint is rint with the inexact flag put
// back the way it was found.
//
// the float entry points hand the work to the double ones. that is exact
// rather than approximate: every float widens to a double without loss, the
// integral result of a float always fits back into a float, and so no
// second rounding can happen.

#include <math.h>
#include <stdint.h>
#include <fenv.h>

union dbits {
  double f;
  uint64_t i;
};

union ldbits {
  long double f;
  struct {
    uint64_t lo;
    uint64_t hi;
  } w;
};

double
trunc(double x)
{
  union dbits u;
  int e;

  u.f = x;
  e = (int)(u.i >> 52 & 0x7ff) - 0x3ff;
  if (e >= 52)  // no fraction bits left, or an infinity or a NaN
    return x;
  if (e < 0)  // |x| < 1, so only the sign survives
    return copysign(0.0, x);
  u.i &= ~(0x000fffffffffffffUL >> e);
  return u.f;
}

double
floor(double x)
{
  union dbits u;
  uint64_t m;
  int e;

  u.f = x;
  e = (int)(u.i >> 52 & 0x7ff) - 0x3ff;
  if (e >= 52)
    return x;
  if (e < 0)
    return x < 0.0 ? -1.0 : copysign(0.0, x);
  m = 0x000fffffffffffffUL >> e;
  if ((u.i & m) == 0)
    return x;
  if (x < 0.0)  // step away from zero, which for a negative is downwards
    u.i += m;
  u.i &= ~m;
  return u.f;
}

double
ceil(double x)
{
  union dbits u;
  uint64_t m;
  int e;

  u.f = x;
  e = (int)(u.i >> 52 & 0x7ff) - 0x3ff;
  if (e >= 52)
    return x;
  if (e < 0)
    return x > 0.0 ? 1.0 : copysign(0.0, x);
  m = 0x000fffffffffffffUL >> e;
  if ((u.i & m) == 0)
    return x;
  if (x > 0.0)
    u.i += m;
  u.i &= ~m;
  return u.f;
}

double
round(double x)
{
  union dbits u;
  uint64_t m;
  int e;

  u.f = x;
  e = (int)(u.i >> 52 & 0x7ff) - 0x3ff;
  if (e >= 52)
    return x;
  if (e < 0)
    return fabs(x) >= 0.5 ? copysign(1.0, x) : copysign(0.0, x);
  m = 0x000fffffffffffffUL >> e;
  if ((u.i & m) == 0)
    return x;
  // adding the top fraction bit carries into the integer part exactly when
  // the fraction is one half or more, which is the tie-away-from-zero rule
  u.i += (m + 1) >> 1;
  u.i &= ~m;
  return u.f;
}

double
rint(double x)
{
  double t;

  t = fabs(x);
  if (t >= 0x1p52 || t == 0.0)
    return x;
  // 2^52 is the first double with no fraction bits, so adding it forces the
  // hardware to round x at the binary point under the current mode, and
  // taking it away again leaves that rounded value behind
  t = (t + 0x1p52) - 0x1p52;
  return copysign(t, x);
}

double
nearbyint(double x)
{
  fexcept_t saved;
  double r;

  fegetexceptflag(&saved, FE_INEXACT);
  r = rint(x);
  fesetexceptflag(&saved, FE_INEXACT);
  return r;
}

long
lrint(double x)
{
  return (long)rint(x);
}

long long
llrint(double x)
{
  return (long long)rint(x);
}

long
lround(double x)
{
  return (long)round(x);
}

long long
llround(double x)
{
  return (long long)round(x);
}

double
modf(double x, double *iptr)
{
  if (isinf(x)) {  // the whole of an infinity is its integral part
    *iptr = x;
    return copysign(0.0, x);
  }
  *iptr = trunc(x);
  if (isnan(x))
    return x;
  return copysign(x - *iptr, x);
}

float
truncf(float x)
{
  return (float)trunc((double)x);
}

float
floorf(float x)
{
  return (float)floor((double)x);
}

float
ceilf(float x)
{
  return (float)ceil((double)x);
}

float
roundf(float x)
{
  return (float)round((double)x);
}

float
rintf(float x)
{
  float t;

  t = fabsf(x);
  if (t >= 0x1p23f || t == 0.0f)
    return x;
  t = (t + 0x1p23f) - 0x1p23f;
  return copysignf(t, x);
}

float
nearbyintf(float x)
{
  fexcept_t saved;
  float r;

  fegetexceptflag(&saved, FE_INEXACT);
  r = rintf(x);
  fesetexceptflag(&saved, FE_INEXACT);
  return r;
}

long
lrintf(float x)
{
  return (long)rintf(x);
}

long long
llrintf(float x)
{
  return (long long)rintf(x);
}

long
lroundf(float x)
{
  return (long)roundf(x);
}

long long
llroundf(float x)
{
  return (long long)roundf(x);
}

float
modff(float x, float *iptr)
{
  double ip, fr;

  fr = modf((double)x, &ip);
  *iptr = (float)ip;
  return (float)fr;
}

// binary128 keeps its mantissa in 112 bits spread over both halves, so the
// mask of the fraction bits comes back as a pair. bit 47 of the high half
// is the first mantissa bit, and e is the unbiased exponent.
static void
ldmask(int e, uint64_t *mh, uint64_t *ml)
{
  if (e < 48) {
    *mh = 0x0000ffffffffffffUL >> e;
    *ml = ~0UL;
  } else {
    *mh = 0;
    *ml = ~0UL >> (e - 48);
  }
}

// the fraction bit worth one half, again as a pair
static void
ldhalf(int e, uint64_t *hh, uint64_t *hl)
{
  if (e < 48) {
    *hh = 1UL << (47 - e);
    *hl = 0;
  } else {
    *hh = 0;
    *hl = 1UL << (111 - e);
  }
}

// add a 128-bit value to the number in u, carrying between the halves
static void
ldadd(union ldbits *u, uint64_t ah, uint64_t al)
{
  uint64_t lo;

  lo = u->w.lo + al;
  if (lo < u->w.lo)
    ah++;
  u->w.lo = lo;
  u->w.hi += ah;
}

long double
truncl(long double x)
{
  union ldbits u;
  uint64_t mh, ml;
  int e;

  u.f = x;
  e = (int)(u.w.hi >> 48 & 0x7fff) - 0x3fff;
  if (e >= 112)
    return x;
  if (e < 0)
    return copysignl(0.0L, x);
  ldmask(e, &mh, &ml);
  u.w.hi &= ~mh;
  u.w.lo &= ~ml;
  return u.f;
}

long double
floorl(long double x)
{
  union ldbits u;
  uint64_t mh, ml;
  int e;

  u.f = x;
  e = (int)(u.w.hi >> 48 & 0x7fff) - 0x3fff;
  if (e >= 112)
    return x;
  if (e < 0)
    return x < 0.0L ? -1.0L : copysignl(0.0L, x);
  ldmask(e, &mh, &ml);
  if ((u.w.hi & mh) == 0 && (u.w.lo & ml) == 0)
    return x;
  if (x < 0.0L)
    ldadd(&u, mh, ml);
  u.w.hi &= ~mh;
  u.w.lo &= ~ml;
  return u.f;
}

long double
ceill(long double x)
{
  union ldbits u;
  uint64_t mh, ml;
  int e;

  u.f = x;
  e = (int)(u.w.hi >> 48 & 0x7fff) - 0x3fff;
  if (e >= 112)
    return x;
  if (e < 0)
    return x > 0.0L ? 1.0L : copysignl(0.0L, x);
  ldmask(e, &mh, &ml);
  if ((u.w.hi & mh) == 0 && (u.w.lo & ml) == 0)
    return x;
  if (x > 0.0L)
    ldadd(&u, mh, ml);
  u.w.hi &= ~mh;
  u.w.lo &= ~ml;
  return u.f;
}

long double
roundl(long double x)
{
  union ldbits u;
  uint64_t mh, ml, hh, hl;
  int e;

  u.f = x;
  e = (int)(u.w.hi >> 48 & 0x7fff) - 0x3fff;
  if (e >= 112)
    return x;
  if (e < 0)
    return fabsl(x) >= 0.5L ? copysignl(1.0L, x) : copysignl(0.0L, x);
  ldmask(e, &mh, &ml);
  if ((u.w.hi & mh) == 0 && (u.w.lo & ml) == 0)
    return x;
  ldhalf(e, &hh, &hl);
  ldadd(&u, hh, hl);
  u.w.hi &= ~mh;
  u.w.lo &= ~ml;
  return u.f;
}

long double
rintl(long double x)
{
  long double t;

  t = fabsl(x);
  if (t >= 0x1p112L || t == 0.0L)
    return x;
  t = (t + 0x1p112L) - 0x1p112L;
  return copysignl(t, x);
}

long double
nearbyintl(long double x)
{
  fexcept_t saved;
  long double r;

  fegetexceptflag(&saved, FE_INEXACT);
  r = rintl(x);
  fesetexceptflag(&saved, FE_INEXACT);
  return r;
}

long
lrintl(long double x)
{
  return (long)rintl(x);
}

long long
llrintl(long double x)
{
  return (long long)rintl(x);
}

long
lroundl(long double x)
{
  return (long)roundl(x);
}

long long
llroundl(long double x)
{
  return (long long)roundl(x);
}

long double
modfl(long double x, long double *iptr)
{
  if (isinf(x)) {
    *iptr = x;
    return copysignl(0.0L, x);
  }
  *iptr = truncl(x);
  if (isnan(x))
    return x;
  return copysignl(x - *iptr, x);
}
