// binary floating point to decimal digits, exactly.
//
// printf cannot round a decimal digit string that it computed with
// floating point arithmetic, because the error of that arithmetic is
// exactly the quantity the rounding decision depends on. so this file
// does the whole job in integers, as Steele and White's dragon4 does,
// in the form Burger and Dybvig gave it: the value is written as the
// exact fraction R/S, a digit is the integer quotient of 10*R by S, and
// the new R is the exact remainder. no digit is ever an approximation
// of anything, which is what makes mode 2 and mode 3 correctly rounded
// to any number of places, however many are asked for.
//
// the shortest mode carries two more integers, M+ and M-, the distances
// to the neighbouring binary values, halved. digits stop as soon as the
// remainder proves that everything still unwritten lies closer to our
// value than to either neighbour, which is precisely the condition for
// strtod to read the digits back as the value we started from. M- is
// half of M+ at a power of two, where the next value down is nearer,
// and the comparisons are inclusive when the significand is even, since
// a decimal sitting exactly on a boundary is read back by ties-to-even
// as the even significand, which is then ours.
//
// sizing: the exact expansion of a binary128 runs to about 11500
// significant digits, and R, S, M+ and M- have to hold the scaled
// numerator and denominator of a value whose exponent reaches 2^-16494.
// the largest of them is R = f * 4 * 10^4966 at 16609 bits and
// S = 2^16496 at 16496 bits, to which the loop can add another four
// bits by multiplying by ten; 16620 bits is 260 limbs, and DLIMBS is
// 272 for margin. that is about 8.7 kB of stack for the four of them,
// which is why nothing here is recursive and nothing is copied.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "fltfmt.h"

typedef unsigned __int128 u128;

// the riscv64 abi makes long double an ieee binary128, which the
// decomposition below takes apart by hand.
typedef char __ld_is_binary128[sizeof(long double) == 16 ? 1 : -1];

#define LDBL_MANT 113
#define LDBL_EMIN (-16382)
#define LDBL_BIAS 16383

#define DLIMBS 272

typedef struct {
  int n;
  uint64_t v[DLIMBS];
} bigint;

static void
bi_set(bigint *a, uint64_t x)
{
  a->v[0] = x;
  a->n = x ? 1 : 0;
}

static uint64_t
bi_limb(const bigint *a, int i)
{
  return i < a->n ? a->v[i] : 0;
}

// a = a * m, with m small enough to fit in a limb; every multiplication
// in this file is of that shape, so all of them run in place.
static void
bi_mul(bigint *a, uint64_t m)
{
  uint64_t c = 0;
  int i;

  for (i = 0; i < a->n; i++) {
    u128 t = (u128)a->v[i] * m + c;
    a->v[i] = (uint64_t)t;
    c = (uint64_t)(t >> 64);
  }
  if (c && a->n < DLIMBS)
    a->v[a->n++] = c;
}

// a *= 10^k, in chunks of the largest power of ten that fits in a limb.
static void
bi_mul10(bigint *a, int k)
{
  uint64_t m = 1;
  int i;

  while (k >= 19) {
    bi_mul(a, 10000000000000000000ULL);
    k -= 19;
  }
  for (i = 0; i < k; i++)
    m *= 10;
  if (m != 1)
    bi_mul(a, m);
}

// a = 2^k.
static void
bi_pow2(bigint *a, int k)
{
  int i, n = k / 64 + 1;

  for (i = 0; i < n; i++)
    a->v[i] = 0;
  a->v[n - 1] = (uint64_t)1 << (k % 64);
  a->n = n;
}

static void
bi_shl(bigint *a, int k)
{
  int w = k / 64, b = k % 64, n, i;

  if (a->n == 0 || k == 0)
    return;
  n = a->n + w + (b ? 1 : 0);
  if (n > DLIMBS)
    n = DLIMBS;
  for (i = n - 1; i >= 0; i--) {
    int j = i - w;
    uint64_t hi = (j >= 0 && j < a->n) ? a->v[j] : 0;
    uint64_t lo = (j >= 1 && j - 1 < a->n) ? a->v[j - 1] : 0;
    a->v[i] = b ? (hi << b) | (lo >> (64 - b)) : hi;
  }
  while (n > 0 && a->v[n - 1] == 0)
    n--;
  a->n = n;
}

static int
bi_cmp(const bigint *a, const bigint *b)
{
  int i;

  if (a->n != b->n)
    return a->n > b->n ? 1 : -1;
  for (i = a->n - 1; i >= 0; i--)
    if (a->v[i] != b->v[i])
      return a->v[i] > b->v[i] ? 1 : -1;
  return 0;
}

static void
bi_sub(bigint *a, const bigint *b)
{
  uint64_t borrow = 0;
  int n = a->n, i;

  for (i = 0; i < n; i++) {
    u128 t = (u128)a->v[i] - bi_limb(b, i) - borrow;
    a->v[i] = (uint64_t)t;
    borrow = (uint64_t)(t >> 64) & 1;
  }
  while (n > 0 && a->v[n - 1] == 0)
    n--;
  a->n = n;
}

// the sign of (a + b) * mul - s, where b may be absent. the sum is
// never built: the limbs are combined as they are read and the carry
// out of the top one gives the sign, which keeps the comparison free of
// a fifth big integer.
static int
bi_cmp_sum(const bigint *a, const bigint *b, const bigint *s, uint64_t mul)
{
  __int128 c = 0;
  int n, i, nz = 0;

  n = a->n;
  if (b && b->n > n)
    n = b->n;
  if (s->n > n)
    n = s->n;
  n += 2;
  if (n > DLIMBS)
    n = DLIMBS;
  for (i = 0; i < n; i++) {
    __int128 t = (__int128)bi_limb(a, i) * mul - bi_limb(s, i) + c;
    if (b)
      t += (__int128)bi_limb(b, i) * mul;
    if ((uint64_t)t)
      nz = 1;
    c = t >> 64;
  }
  if (c != 0)
    return c < 0 ? -1 : 1;
  return nz;
}

// one decimal digit: the quotient of r by s, which is at most nine
// because the caller has just multiplied a remainder by ten.
static int
bi_divstep(bigint *r, const bigint *s)
{
  int d = 0;

  while (bi_cmp(r, s) >= 0) {
    bi_sub(r, s);
    d++;
  }
  return d;
}

static void
bi_setu128(bigint *a, u128 x)
{
  a->v[0] = (uint64_t)x;
  a->v[1] = (uint64_t)(x >> 64);
  a->n = a->v[1] ? 2 : (a->v[0] ? 1 : 0);
}

static int
u128_bitlen(u128 x)
{
  uint64_t hi = (uint64_t)(x >> 64);

  if (hi)
    return 128 - __builtin_clzll(hi);
  if ((uint64_t)x)
    return 64 - __builtin_clzll((uint64_t)x);
  return 0;
}

// the three special values, written the way printf wants to see them.
static int
special(u128 frac, char *buf, int bufsize)
{
  if (bufsize >= 4)
    memcpy(buf, frac ? "nan" : "inf", 4);
  return frac ? -2 : -1;
}

int
__dtoa(long double v, int mode, int ndigits, int *decpt, int *sign, char *buf, int bufsize)
{
  bigint R, S, Mp, Mm;
  union { long double l; u128 u; } x;
  u128 f;
  int be, e, even, ispow2, notmin, shortest, p, emin, i, k, n, d, c;

  x.l = v;
  *sign = (int)(x.u >> 127);
  be = (int)((x.u >> 112) & 0x7fff);
  f = x.u & (((u128)1 << 112) - 1);
  *decpt = 0;
  if (bufsize < 8)
    return 0;
  if (be == 0x7fff)
    return special(f, buf, bufsize);
  if (be == 0 && f == 0) {
    buf[0] = '0';
    buf[1] = 0;
    *decpt = 1;
    return 1;
  }
  if (be == 0) {
    e = LDBL_EMIN - (LDBL_MANT - 1);
  } else {
    f |= (u128)1 << (LDBL_MANT - 1);
    e = be - LDBL_BIAS - (LDBL_MANT - 1);
  }
  shortest = (mode == 0);
  notmin = be > 1;
  p = LDBL_MANT;
  emin = LDBL_EMIN;
  if (shortest) {
    // the shortest string is only shortest with respect to a precision,
    // and callers widen everything to long double before getting here,
    // so a double printed as a binary128 would come out at 34 digits
    // where 17 read it back. a value that is exactly a double is
    // therefore treated as one, which is what makes the result round
    // trip through strtod. going narrower still, to float, is not safe
    // without being told: a double that happens to fit in a float would
    // lose digits it needs, so only an explicit ndigits of 24 asks for
    // that, and 53 or 113 likewise override the guess.
    int hi = u128_bitlen(f) + e - 1, lo = e, ne;
    u128 t = f;
    while (!(t & 1)) {
      t >>= 1;
      lo++;
    }
    if (ndigits == 24 || ndigits == 53 || ndigits == 113) {
      p = ndigits;
      emin = p == 24 ? -126 : p == 53 ? -1022 : LDBL_EMIN;
    } else if (hi <= 1023 && lo >= (hi - 52 > -1074 ? hi - 52 : -1074)) {
      p = 53;
      emin = -1022;
    }
    // restate the value at that format's spacing, which is exact
    // because the value is one of its numbers.
    ne = hi - p + 1;
    if (ne < emin - p + 1)
      ne = emin - p + 1;
    f >>= ne - e;
    e = ne;
    notmin = hi > emin;
  }
  even = !((int)f & 1);
  // at a power of two the value below is only half an ulp away, so the
  // two neighbour distances differ; at the smallest normal it is not,
  // because the values below it are subnormal and evenly spaced.
  ispow2 = (f == (u128)1 << (p - 1)) && notmin;

  // v = R/S exactly, with M+ and M- half the distance to the next value
  // up and down, scaled the same way.
  bi_setu128(&R, f);
  if (e >= 0) {
    bi_shl(&R, ispow2 ? e + 2 : e + 1);
    bi_set(&S, ispow2 ? 4 : 2);
    bi_pow2(&Mm, e);
    bi_pow2(&Mp, ispow2 ? e + 1 : e);
  } else {
    bi_shl(&R, ispow2 ? 2 : 1);
    bi_pow2(&S, ispow2 ? 2 - e : 1 - e);
    bi_set(&Mm, 1);
    bi_set(&Mp, ispow2 ? 2 : 1);
  }

  // a first guess at the decimal exponent from the binary one, using
  // 30103/100000 for log10(2), rounded down; the loop below then makes
  // it exact, and it is never more than a step or two out.
  {
    long t = (long)(u128_bitlen(f) + e) * 30103;
    k = (int)(t / 100000);
    if (t < 0 && t % 100000)
      k--;
  }
  if (k >= 0) {
    bi_mul10(&S, k);
  } else {
    bi_mul10(&R, -k);
    if (shortest) {
      bi_mul10(&Mp, -k);
      bi_mul10(&Mm, -k);
    }
  }
  // scale until 1/10 <= R/S < 1, so that the first digit generated is
  // both nonzero and the first digit of the value. in the shortest mode
  // the upper test is against R + M+ instead, because a value that
  // rounds up into the next power of ten must be printed there.
  for (;;) {
    c = bi_cmp_sum(&R, shortest ? &Mp : 0, &S, 1);
    if (shortest ? (even ? c >= 0 : c > 0) : c >= 0) {
      bi_mul(&S, 10);
      k++;
      continue;
    }
    c = bi_cmp_sum(&R, shortest ? &Mp : 0, &S, 10);
    if (shortest ? (even ? c < 0 : c <= 0) : c < 0) {
      bi_mul(&R, 10);
      if (shortest) {
        bi_mul(&Mp, 10);
        bi_mul(&Mm, 10);
      }
      k--;
      continue;
    }
    break;
  }

  if (shortest) {
    // stop as soon as what is left is nearer to this value than to
    // either neighbour, since strtod will then land back here. the
    // digit can be incremented without a carry: the scaling invariant
    // R + M+ <= S makes the ninth digit's remainder too small for the
    // high test to fire.
    for (n = 0;;) {
      bi_mul(&R, 10);
      bi_mul(&Mp, 10);
      bi_mul(&Mm, 10);
      d = bi_divstep(&R, &S);
      c = bi_cmp(&R, &Mm);
      i = even ? c <= 0 : c < 0;                       // low: ours is nearer
      c = bi_cmp_sum(&R, &Mp, &S, 1);
      c = even ? c >= 0 : c > 0;                       // high: the next one up
      if (!i && !c) {
        if (n >= bufsize - 1)
          break;
        buf[n++] = (char)('0' + d);
        continue;
      }
      if (n < bufsize - 1) {
        if (i && !c)
          buf[n++] = (char)('0' + d);
        else if (c && !i)
          buf[n++] = (char)('0' + d + 1);
        else
          buf[n++] = (char)('0' + d + (bi_cmp_sum(&R, 0, &S, 2) < 0 ? 0 : 1));
      }
      break;
    }
  } else {
    n = (mode == 3) ? k + ndigits : (ndigits < 1 ? 1 : ndigits);
    if (n > bufsize - 1)
      n = bufsize - 1;
    if (n <= 0) {
      // every digit asked for lies above the value, so the only way to
      // get one at all is for the value to carry into the last place.
      if (n == 0 && bi_cmp_sum(&R, 0, &S, 2) > 0) {
        buf[0] = '1';
        buf[1] = 0;
        *decpt = k + 1;
        return 1;
      }
      buf[0] = 0;
      *decpt = k;
      return 0;
    }
    for (i = 0; i < n; i++) {
      bi_mul(&R, 10);
      buf[i] = (char)('0' + bi_divstep(&R, &S));
      if (R.n == 0) {
        n = i + 1;
        break;
      }
    }
    // the remainder decides the last digit exactly: more than half a
    // place rounds up, exactly half goes to the even digit.
    c = bi_cmp_sum(&R, 0, &S, 2);
    if (c > 0 || (c == 0 && ((buf[n - 1] - '0') & 1))) {
      for (i = n - 1; i >= 0 && buf[i] == '9'; i--)
        buf[i] = '0';
      if (i < 0) {
        buf[0] = '1';
        k++;
      } else {
        buf[i]++;
      }
    }
    while (n > 1 && buf[n - 1] == '0')
      n--;
  }
  buf[n] = 0;
  *decpt = k;
  return n;
}

// the same value in hexadecimal, where nothing has to be computed: the
// significand already is the digit string, so the only work is rounding
// it to the requested width and deciding what the leading digit is.
// a subnormal keeps a leading zero and the smallest normal exponent,
// which is how %a is expected to show it.
int
__hextoa(long double v, int ndigits, int upper, int *exp2, int *sign, char *buf, int bufsize)
{
  static const char lc[] = "0123456789abcdef";
  static const char uc[] = "0123456789ABCDEF";
  const char *hex = upper ? uc : lc;
  union { long double l; u128 u; } x;
  u128 f, rest;
  int be, n, i, lead, drop, nreal, sh;

  x.l = v;
  *sign = (int)(x.u >> 127);
  be = (int)((x.u >> 112) & 0x7fff);
  f = x.u & (((u128)1 << 112) - 1);
  *exp2 = 0;
  if (bufsize < 4)
    return 0;
  if (be == 0x7fff)
    return special(f, buf, bufsize);
  lead = be ? 1 : 0;
  if (be || f)
    *exp2 = be ? be - LDBL_BIAS : LDBL_EMIN;

  // the fraction is 112 bits, which is exactly 28 hexadecimal digits.
  n = ndigits < 0 ? 28 : ndigits;
  if (n > bufsize - 2)
    n = bufsize - 2;
  if (n < 28) {
    drop = (28 - n) * 4;
    rest = f & (((u128)1 << (drop - 1)) - 1);
    i = (int)((f >> (drop - 1)) & 1);
    f >>= drop;
    // a carry out of the last fraction digit runs all the way into the
    // leading one, which then reads 2 rather than 1. that is what %a is
    // conventionally shown as, so the exponent is left alone.
    if (i && (rest != 0 || (f & 1))) {
      f++;
      if ((f >> (n * 4)) != 0) {
        f = 0;
        lead++;
      }
    }
  }
  buf[0] = hex[lead];
  nreal = n < 28 ? n : 28;
  sh = n < 28 ? n * 4 : 112;
  for (i = 0; i < nreal; i++) {
    sh -= 4;
    buf[1 + i] = hex[(int)((f >> sh) & 15)];
  }
  for (; i < n; i++)
    buf[1 + i] = '0';
  if (ndigits < 0)
    while (n > 0 && buf[n] == '0')
      n--;
  buf[n + 1] = 0;
  return n + 1;
}
