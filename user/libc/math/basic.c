// sign and selection primitives: fabs, copysign, fmax, fmin, fdim and nan.
//
// none of these needs arithmetic. the sign of an IEEE number is one bit at
// the top of its representation, so fabs and copysign are bit operations;
// gcc knows this and turns the __builtin forms below into fsgnjx
// instructions for float and double, and into plain integer masking for
// binary128, which has no hardware of its own.

#include <math.h>
#include <stdint.h>

// binary128 seen as its two halves. rv64 is little-endian, so the half that
// holds the sign and the exponent comes second.
union ldbits {
  long double f;
  struct {
    uint64_t lo;
    uint64_t hi;
  } w;
};

double
fabs(double x)
{
  return __builtin_fabs(x);
}

float
fabsf(float x)
{
  return __builtin_fabsf(x);
}

long double
fabsl(long double x)
{
  union ldbits u;

  u.f = x;
  u.w.hi &= 0x7fffffffffffffffUL;
  return u.f;
}

double
copysign(double x, double y)
{
  return __builtin_copysign(x, y);
}

float
copysignf(float x, float y)
{
  return __builtin_copysignf(x, y);
}

long double
copysignl(long double x, long double y)
{
  union ldbits ux, uy;

  ux.f = x;
  uy.f = y;
  ux.w.hi = (ux.w.hi & 0x7fffffffffffffffUL) | (uy.w.hi & 0x8000000000000000UL);
  return ux.f;
}

// C99 wants fmax and fmin to ignore a NaN argument and answer with the
// other one, which is not what a plain comparison does.
double
fmax(double x, double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)  // both zeros: the positive one is the larger
    return signbit(x) ? y : x;
  return x > y ? x : y;
}

float
fmaxf(float x, float y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? y : x;
  return x > y ? x : y;
}

long double
fmaxl(long double x, long double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? y : x;
  return x > y ? x : y;
}

double
fmin(double x, double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? x : y;
  return x < y ? x : y;
}

float
fminf(float x, float y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? x : y;
  return x < y ? x : y;
}

long double
fminl(long double x, long double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? x : y;
  return x < y ? x : y;
}

// the positive difference. the subtraction happens only when it cannot go
// negative, so an inexact zero can never appear.
double
fdim(double x, double y)
{
  if (isnan(x) || isnan(y))
    return x + y;
  return x > y ? x - y : 0.0;
}

float
fdimf(float x, float y)
{
  if (isnan(x) || isnan(y))
    return x + y;
  return x > y ? x - y : 0.0f;
}

long double
fdiml(long double x, long double y)
{
  if (isnan(x) || isnan(y))
    return x + y;
  return x > y ? x - y : 0.0L;
}

// the tag of nan("...") is an n-char sequence naming a particular quiet
// NaN. we read it as an integer payload, decimal or 0x-prefixed, and drop
// it into the low mantissa bits; anything we cannot read gives the default
// quiet NaN.
static uint64_t
tagvalue(const char *s)
{
  uint64_t v = 0;
  int base = 10;

  if (s == 0 || *s == 0)
    return 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
  }
  for (; *s; s++) {
    int d;

    if (*s >= '0' && *s <= '9')
      d = *s - '0';
    else if (base == 16 && *s >= 'a' && *s <= 'f')
      d = *s - 'a' + 10;
    else if (base == 16 && *s >= 'A' && *s <= 'F')
      d = *s - 'A' + 10;
    else
      return 0;
    v = v * base + d;
  }
  return v;
}

double
nan(const char *tagp)
{
  union {
    double f;
    uint64_t i;
  } u;

  u.i = 0x7ff8000000000000UL | (tagvalue(tagp) & 0x7ffffffffffffUL);
  return u.f;
}

float
nanf(const char *tagp)
{
  union {
    float f;
    uint32_t i;
  } u;

  u.i = 0x7fc00000U | ((uint32_t)tagvalue(tagp) & 0x3fffffU);
  return u.f;
}

long double
nanl(const char *tagp)
{
  union ldbits u;

  u.w.hi = 0x7fff800000000000UL;
  u.w.lo = tagvalue(tagp);
  return u.f;
}
