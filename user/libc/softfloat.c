//
// binary128 division.
//
// long double on this target is IEEE binary128, which the processor
// has no instructions for, so the compiler calls libgcc. libgcc's
// bare-metal build is not position independent: its divtf3 indexes a
// jump table with absolute addresses, which cannot go into a shared
// library. the operation is written here instead, and because this
// object is linked before libgcc is consulted, it is the one that is
// used everywhere.
//
// the algorithm is restoring division, one quotient bit at a time over
// 128-bit integers. it is not fast, but it is exact: the loop produces
// the significand plus a guard bit, and what is left over decides the
// rounding, which is to nearest with ties to even.
//
#include <stdint.h>

typedef unsigned __int128 u128;

#define BIAS    16383
#define MANT    112                 // stored fraction bits
#define EXPMASK 0x7fffU

union tf {
  long double f;
  u128 u;
};

static long double
pack(int sign, int exp, u128 frac)
{
  union tf v;

  v.u = ((u128)(unsigned)sign << 127) | ((u128)(unsigned)exp << MANT) | (frac & (((u128)1 << MANT) - 1));
  return v.f;
}

long double
__divtf3(long double a, long double b)
{
  union tf ua, ub;

  ua.f = a;
  ub.f = b;
  int sa = (int)(ua.u >> 127), sb = (int)(ub.u >> 127);
  int ea = (int)((ua.u >> MANT) & EXPMASK), eb = (int)((ub.u >> MANT) & EXPMASK);
  u128 ma = ua.u & (((u128)1 << MANT) - 1), mb = ub.u & (((u128)1 << MANT) - 1);
  int sign = sa ^ sb;

  // the special values first, in the order IEEE 754 requires
  if (ea == EXPMASK) {
    if (ma)
      return a;                          // nan
    if (eb == EXPMASK)
      return pack(0, EXPMASK, (u128)1 << (MANT - 1)); // inf / inf is nan
    return pack(sign, EXPMASK, 0);       // inf / finite
  }
  if (eb == EXPMASK) {
    if (mb)
      return b;                          // nan
    return pack(sign, 0, 0);             // finite / inf
  }
  if (eb == 0 && mb == 0) {
    if (ea == 0 && ma == 0)
      return pack(0, EXPMASK, (u128)1 << (MANT - 1)); // 0 / 0 is nan
    return pack(sign, EXPMASK, 0);       // x / 0 is infinity
  }
  if (ea == 0 && ma == 0)
    return pack(sign, 0, 0);             // 0 / x

  // normalize, counting the shift a subnormal operand needs
  int expa = ea, expb = eb;
  // a subnormal is shifted up until its leading one reaches the place
  // the implicit one would occupy, the exponent following it down.
  if (ea == 0) {
    expa = 1;
    while (!((ma >> MANT) & 1)) {
      ma <<= 1;
      expa--;
    }
    ma &= ((u128)1 << MANT) - 1;
  }
  if (eb == 0) {
    expb = 1;
    while (!((mb >> MANT) & 1)) {
      mb <<= 1;
      expb--;
    }
    mb &= ((u128)1 << MANT) - 1;
  }
  u128 na = ma | ((u128)1 << MANT);      // the implicit leading one
  u128 nb = mb | ((u128)1 << MANT);
  int exp = expa - expb + BIAS;

  // long division, one quotient bit per step. the invariant is that
  // the remainder is below twice the divisor when each comparison is
  // made, which holds because the significands differ by less than a
  // factor of two. after MANT+3 steps q holds the quotient scaled by
  // 2^(MANT+2) and rem is the exact remainder, so what follows is a
  // decision on facts rather than an estimate.
  u128 q = 0, rem = na;
  for (int i = 0; i < MANT + 3; i++) {
    q <<= 1;
    if (rem >= nb) {
      rem -= nb;
      q |= 1;
    }
    rem <<= 1;
  }

  int guard, sticky = (rem != 0);
  if (q >> (MANT + 2)) {
    // the dividend was the larger, so the quotient is in [1, 2) and
    // carries two bits below the significand.
    guard = (int)((q >> 1) & 1);
    sticky |= (int)(q & 1);
    q >>= 2;
  } else {
    // in [0.5, 1): one bit below, and the exponent comes down.
    guard = (int)(q & 1);
    q >>= 1;
    exp--;
  }

  if (exp <= 0) {
    // subnormal: everything that falls off the bottom feeds the
    // rounding, the last bit lost becoming the new guard.
    int shift = 1 - exp;
    if (shift > MANT + 2)
      return pack(sign, 0, 0);
    sticky |= guard;
    sticky |= (q & (((u128)1 << (shift - 1)) - 1)) != 0;
    guard = (int)((q >> (shift - 1)) & 1);
    q >>= shift;
    exp = 0;
  }

  // round to nearest, ties to even
  if (guard && (sticky || (q & 1)))
    q++;
  if (q >> (MANT + 1)) { // the rounding carried into the exponent
    q >>= 1;
    exp++;
  }
  if (exp == 0 && (q >> MANT))
    exp = 1; // rounding lifted a subnormal to the smallest normal
  if (exp >= EXPMASK)
    return pack(sign, EXPMASK, 0); // overflow to infinity
  return pack(sign, exp, q);
}
