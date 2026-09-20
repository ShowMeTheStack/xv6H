// stepping from one representable number to the next: nextafter and
// nexttoward.
//
// IEEE formats are laid out so that the numbers of one sign are in the same
// order as the integers made of their bits. adding one to the bit pattern
// of a positive number therefore lands exactly on its successor, and it
// carries correctly from the top of one binade into the bottom of the next
// and from the largest subnormal into the smallest normal, with no cases to
// separate out. only the crossing at zero needs care, because the two zeros
// sit at opposite ends of the integer order.

#include <math.h>
#include <stdint.h>
#include <errno.h>

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
nextafter(double x, double y)
{
  union dbits u;
  int up, neg, e;

  if (isnan(x) || isnan(y))
    return x + y;
  if (x == y)  // including the two zeros, where y's sign is the answer
    return y;
  u.f = x;
  if (x == 0.0) {
    u.i = 1;
    return copysign(u.f, y);
  }
  up = x < y;
  neg = (int)(u.i >> 63);
  // a step towards y raises the bit pattern for a positive number and
  // lowers it for a negative one
  if (up != neg)
    u.i++;
  else
    u.i--;
  e = (int)(u.i >> 52) & 0x7ff;
  if (e == 0x7ff || e == 0)  // stepped out of the normal range, either end
    errno = ERANGE;
  return u.f;
}

float
nextafterf(float x, float y)
{
  union {
    float f;
    uint32_t i;
  } u;
  int up, neg, e;

  if (isnan(x) || isnan(y))
    return x + y;
  if (x == y)
    return y;
  u.f = x;
  if (x == 0.0f) {
    u.i = 1;
    return copysignf(u.f, y);
  }
  up = x < y;
  neg = (int)(u.i >> 31);
  if (up != neg)
    u.i++;
  else
    u.i--;
  e = (int)(u.i >> 23) & 0xff;
  if (e == 0xff || e == 0)
    errno = ERANGE;
  return u.f;
}

long double
nextafterl(long double x, long double y)
{
  union ldbits u;
  int up, neg, e;

  if (isnan(x) || isnan(y))
    return x + y;
  if (x == y)
    return y;
  u.f = x;
  if (x == 0.0L) {
    u.w.hi = 0;
    u.w.lo = 1;
    return copysignl(u.f, y);
  }
  up = x < y;
  neg = (int)(u.w.hi >> 63);
  if (up != neg) {
    if (++u.w.lo == 0)
      u.w.hi++;
  } else {
    if (u.w.lo-- == 0)
      u.w.hi--;
  }
  e = (int)(u.w.hi >> 48) & 0x7fff;
  if (e == 0x7fff || e == 0)
    errno = ERANGE;
  return u.f;
}

// nexttoward only differs in taking its direction in the wider type, and a
// double or a float widens to binary128 exactly, so the comparison inside
// nextafter is the only thing that has to be redone here
double
nexttoward(double x, long double y)
{
  union dbits u;
  int up, neg, e;

  if (isnan(x) || isnan(y))
    return x + (double)y;
  if ((long double)x == y)
    return (double)y;
  u.f = x;
  if (x == 0.0) {
    u.i = 1;
    return y < 0.0L ? -u.f : u.f;
  }
  up = (long double)x < y;
  neg = (int)(u.i >> 63);
  if (up != neg)
    u.i++;
  else
    u.i--;
  e = (int)(u.i >> 52) & 0x7ff;
  if (e == 0x7ff || e == 0)
    errno = ERANGE;
  return u.f;
}

float
nexttowardf(float x, long double y)
{
  union {
    float f;
    uint32_t i;
  } u;
  int up, neg, e;

  if (isnan(x) || isnan(y))
    return x + (float)y;
  if ((long double)x == y)
    return (float)y;
  u.f = x;
  if (x == 0.0f) {
    u.i = 1;
    return y < 0.0L ? -u.f : u.f;
  }
  up = (long double)x < y;
  neg = (int)(u.i >> 31);
  if (up != neg)
    u.i++;
  else
    u.i--;
  e = (int)(u.i >> 23) & 0xff;
  if (e == 0xff || e == 0)
    errno = ERANGE;
  return u.f;
}

long double
nexttowardl(long double x, long double y)
{
  return nextafterl(x, y);
}
