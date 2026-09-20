// strtod, strtof and strtold: decimal or hexadecimal text to binary
// floating point, correctly rounded to nearest with ties to even.
//
// a decimal fraction is almost never a binary one, so choosing between
// the two binary values that bracket the input needs the exact value of
// a quotient of two integers, not an approximation of it. we therefore
// parse the significant digits into a big integer D, so that the input
// is exactly D * 10^e10, split 10^e10 into 5^e10 * 2^e10, and keep the
// power of five on whichever side of a fraction leaves it an integer:
//
//     value = (num / den) * 2^bexp
//
// with num = D * 5^e10 and den = 1 when e10 >= 0, and num = D and
// den = 5^-e10 otherwise. one left shift then lines the quotient up so
// that it holds exactly as many bits as the destination format, and one
// schoolbook division yields the truncated significand together with a
// remainder. twice that remainder against the divisor is exactly the
// comparison against the halfway point, so a tie is recognised as a tie
// instead of being guessed at, and ties to even is then just a test of
// the low bit.
//
// the shift is capped so that the quotient never reaches below the
// smallest subnormal's last bit, which is how subnormal results come
// out correctly rounded rather than doubly rounded.
//
// the one place digits are dropped is after fmt->maxdig of them, and
// there we or a 1 into the low bit of D. that is exact rather than a
// heuristic. a halfway point is (2m+1) * 2^(emin-p), whose decimal
// expansion is finite and takes at most 0.70 * (p - 1 - emin) + 0.31 * p
// digits: 111 for float, 767 for double, 11565 for binary128. retaining
// more digits than that means a retained prefix equal to a halfway
// point must be all zeros below the halfway point's own last digit,
// hence even, so the sticky bit moves it strictly above, which is where
// the true value lies. a prefix that differs from the halfway point
// differs by at least one unit in the last retained place, and the
// dropped tail is worth strictly less than that, so it cannot pull the
// value back across.

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>

// the riscv64 abi makes long double an ieee binary128; packing a result
// into 128 bits below depends on it, so fail the build if it ever
// changes rather than return nonsense at run time.
typedef char __ld_is_binary128[sizeof(long double) == 16 ? 1 : -1];

typedef unsigned __int128 u128;

// a destination format, described by the handful of numbers the
// conversion actually needs. maxdp and mindp are the decimal point
// positions outside which every input overflows or vanishes, and let us
// refuse absurd exponents before building any big integer at all.
// maxdig is the digit retention bound argued for at the top of the file.
typedef struct {
  int p;      // significand bits, the implicit one included
  int emin;   // exponent of the smallest normal: value = 1.f * 2^emin
  int emax;
  int w;      // width of the encoding in bits
  int maxdp;
  int mindp;
  int maxdig;
} fpfmt;

static const fpfmt fmt_flt = {  24,   -126,   127,  32,   40,   -46,   130 };
static const fpfmt fmt_dbl = {  53,  -1022,  1023,  64,  310,  -325,   800 };
static const fpfmt fmt_ldb = { 113, -16382, 16383, 128, 4934, -4967, 11700 };

// big integers, little endian, n limbs in use and no leading zero limb.
//
// the bound: the divisor is 5^-e10 with -e10 at most maxdig - mindp =
// 16667, which is 38700 bits, and the shifted dividend is at most p
// bits wider than the divisor because the shift is chosen to make the
// quotient p bits long. 38813 bits is 607 limbs; 620 leaves room for
// the carry limb an intermediate multiply can add.
#define BILIMBS 620

typedef struct {
  int n;
  uint64_t v[BILIMBS];
} bigint;

static void
bi_set(bigint *a, uint64_t x)
{
  a->v[0] = x;
  a->n = x ? 1 : 0;
}

static int
bi_bitlen(const bigint *a)
{
  if (a->n == 0)
    return 0;
  return a->n * 64 - __builtin_clzll(a->v[a->n - 1]);
}

// a = a * m + c. every multiplication in this file is by a value that
// fits in a limb, which is what lets all of them run in place: no
// scratch big integer is ever needed, and so none is ever on the stack.
static void
bi_muladd(bigint *a, uint64_t m, uint64_t c)
{
  int i;

  for (i = 0; i < a->n; i++) {
    u128 t = (u128)a->v[i] * m + c;
    a->v[i] = (uint64_t)t;
    c = (uint64_t)(t >> 64);
  }
  if (c && a->n < BILIMBS)
    a->v[a->n++] = c;
}

// a *= 5^k, by repeated multiplication by the largest power of five
// that still fits in a limb.
static void
bi_mul5(bigint *a, int k)
{
  uint64_t big = 1, m = 1;
  int i;

  for (i = 0; i < 27; i++)
    big *= 5;
  while (k >= 27) {
    bi_muladd(a, big, 0);
    k -= 27;
  }
  for (i = 0; i < k; i++)
    m *= 5;
  if (m != 1)
    bi_muladd(a, m, 0);
}

static void
bi_shl(bigint *a, int k)
{
  int w = k / 64, b = k % 64, n, i;

  if (a->n == 0 || k == 0)
    return;
  n = a->n + w + (b ? 1 : 0);
  if (n > BILIMBS)
    n = BILIMBS;
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

// the comparison and the subtraction both take the divisor shifted left
// by s, and synthesise its limbs on the fly rather than building the
// shifted copy, which keeps the division loop free of temporaries.
static uint64_t
bi_limb_shl(const bigint *b, int i, int w, int r)
{
  uint64_t hi = 0, lo = 0;
  int j = i - w;

  if (j >= 0 && j < b->n)
    hi = b->v[j];
  if (r == 0)
    return hi;
  if (j >= 1 && j - 1 < b->n)
    lo = b->v[j - 1];
  return (hi << r) | (lo >> (64 - r));
}

static int
bi_nlimb_shl(const bigint *b, int s)
{
  if (b->n == 0)
    return 0;
  return (bi_bitlen(b) + s + 63) / 64;
}

static int
bi_cmp_shl(const bigint *a, const bigint *b, int s)
{
  int w = s / 64, r = s % 64, nb = bi_nlimb_shl(b, s), i;

  if (a->n != nb)
    return a->n > nb ? 1 : -1;
  for (i = a->n - 1; i >= 0; i--) {
    uint64_t x = a->v[i], y = bi_limb_shl(b, i, w, r);
    if (x != y)
      return x > y ? 1 : -1;
  }
  return 0;
}

// a -= b << s, which the caller has just checked is not negative.
static void
bi_sub_shl(bigint *a, const bigint *b, int s)
{
  int w = s / 64, r = s % 64, n = a->n, i;
  uint64_t borrow = 0;

  for (i = 0; i < n; i++) {
    u128 t = (u128)a->v[i] - bi_limb_shl(b, i, w, r) - borrow;
    a->v[i] = (uint64_t)t;
    borrow = (uint64_t)(t >> 64) & 1;
  }
  while (n > 0 && a->v[n - 1] == 0)
    n--;
  a->n = n;
}

// pack the rounded result. q is the truncated significand whose last
// bit is worth 2^e2, and cmp is the sign of the part that was dropped
// measured against half of that last bit: negative below the halfway
// point, zero exactly on it, positive above. every caller reduces its
// own problem to these three numbers, so this is the only place that
// knows how a float is laid out.
static u128
fp_round(const fpfmt *f, int sign, u128 q, int e2, int cmp)
{
  int ew = f->w - f->p, biased;
  u128 bits;

  if (cmp > 0 || (cmp == 0 && (q & 1)))
    q++;
  // a carry out of the significand is exact: the value has doubled and
  // its new low bit is known to be zero, so shifting it back is free.
  if ((q >> f->p) != 0) {
    q >>= 1;
    e2++;
  }
  if (q == 0) {
    errno = ERANGE;
    bits = 0;
  } else if ((q >> (f->p - 1)) != 0) {
    biased = e2 + f->p - 1 - f->emin + 1;
    if (biased >= (1 << ew) - 1) {
      errno = ERANGE;
      bits = (u128)((1 << ew) - 1) << (f->p - 1);
    } else {
      bits = ((u128)biased << (f->p - 1)) | (q & (((u128)1 << (f->p - 1)) - 1));
    }
  } else {
    // the exponent field stays zero, which is what makes this subnormal,
    // and reaching it at all is an underflow whether or not any bits
    // were lost on the way.
    errno = ERANGE;
    bits = q;
  }
  if (sign)
    bits |= (u128)1 << (f->w - 1);
  return bits;
}

static u128
fp_zero(const fpfmt *f, int sign)
{
  return sign ? (u128)1 << (f->w - 1) : 0;
}

static u128
fp_inf(const fpfmt *f, int sign)
{
  u128 bits = (u128)((1 << (f->w - f->p)) - 1) << (f->p - 1);

  return sign ? bits | ((u128)1 << (f->w - 1)) : bits;
}

static u128
fp_nan(const fpfmt *f, int sign)
{
  return fp_inf(f, sign) | ((u128)1 << (f->p - 2));
}

// the exact powers of ten, for the short-input path below.
static const double pow10tab[23] = {
  1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static u128
dbl_bits(double d)
{
  union { uint64_t u; double d; } x;

  x.d = d;
  return x.u;
}

// the decimal path. s points at the first digit or at the point; on a
// string with no digits at all this sets *endp to null and the caller
// reports that no conversion happened.
//
// the two big integers below are about 10 kB of stack between them, so
// this is deliberately the only frame in the file that holds them.
static u128
scan_dec(const fpfmt *f, const char *s, const char **endp, int sign)
{
  bigint num, den;
  const char *p = s, *lastdig = 0;
  uint64_t chunk = 0, m;
  int nd = 0, dp = 0, seendot = 0, lead = 1, sticky = 0, inchunk = 0;
  int e10, bexp, bn, bd, snorm, ssub, sh, u, i, cmp;
  u128 q;

  bi_set(&num, 0);
  for (;; p++) {
    int c = *p;
    if (c == '.') {
      if (seendot)
        break;
      seendot = 1;
      continue;
    }
    if (c < '0' || c > '9')
      break;
    lastdig = p;
    // leading zeros carry no information except, after the point, the
    // position of the first significant digit.
    if (lead && c == '0') {
      if (seendot)
        dp--;
      continue;
    }
    lead = 0;
    if (!seendot)
      dp++;
    if (nd < f->maxdig) {
      chunk = chunk * 10 + (uint64_t)(c - '0');
      if (++inchunk == 19) {
        bi_muladd(&num, 10000000000000000000ULL, chunk);
        chunk = 0;
        inchunk = 0;
      }
      nd++;
    } else if (c != '0') {
      sticky = 1;
    }
  }
  if (!lastdig) {
    *endp = 0;
    return 0;
  }
  for (m = 1, i = 0; i < inchunk; i++)
    m *= 10;
  if (inchunk)
    bi_muladd(&num, m, chunk);

  // the exponent is only part of the number if it has at least one
  // digit of its own; "1e+" converts as "1" and stops at the 'e'.
  if (*p == 'e' || *p == 'E') {
    const char *q2 = p + 1;
    int esign = 0;
    if (*q2 == '+' || *q2 == '-')
      esign = (*q2++ == '-');
    if (*q2 >= '0' && *q2 <= '9') {
      long ev = 0;
      while (*q2 >= '0' && *q2 <= '9') {
        if (ev < 100000000L)
          ev = ev * 10 + (*q2 - '0');
        q2++;
      }
      dp += esign ? (int)-ev : (int)ev;
      p = q2;
    }
  }
  *endp = p;

  // value = num * 10^(dp - nd), with the digits all significant.
  if (nd == 0)
    return fp_zero(f, sign);
  if (dp > f->maxdp) {
    errno = ERANGE;
    return fp_inf(f, sign);
  }
  if (dp < f->mindp) {
    errno = ERANGE;
    return fp_zero(f, sign);
  }
  if (sticky)
    num.v[0] |= 1;
  e10 = dp - nd;

  // a significand of no more than 53 bits and a power of ten that is
  // itself exact leave a single rounding in a single hardware
  // operation, which is already the correctly rounded answer.
  if (f->p == 53 && !sticky && num.n <= 1 && num.v[0] < (1ULL << 53) &&
      e10 >= -22 && e10 <= 22) {
    double d = (double)num.v[0];
    if (e10 > 0)
      d *= pow10tab[e10];
    else if (e10 < 0)
      d /= pow10tab[-e10];
    return dbl_bits(sign ? -d : d);
  }

  if (e10 >= 0) {
    bi_mul5(&num, e10);
    bi_set(&den, 1);
  } else {
    bi_set(&den, 1);
    bi_mul5(&den, -e10);
  }
  bexp = e10;

  // shift the dividend so that the quotient comes out exactly p bits
  // long, but never past the point where the last quotient bit would be
  // worth less than the smallest subnormal's last bit: rounding has to
  // happen at the destination's own precision, not below it.
  bn = bi_bitlen(&num);
  bd = bi_bitlen(&den);
  snorm = f->p - (bn - bd);
  ssub = bexp - f->emin + f->p - 1;
  sh = snorm < ssub ? snorm : ssub;
  if (sh > 0)
    bi_shl(&num, sh);
  else if (sh < 0)
    bi_shl(&den, -sh);
  u = bexp - sh;
  // the estimate can be one bit generous, in which case doubling the
  // divisor halves the quotient and costs nothing.
  if (bi_cmp_shl(&num, &den, f->p) >= 0) {
    bi_shl(&den, 1);
    u++;
  }

  q = 0;
  for (i = f->p - 1; i >= 0; i--) {
    if (bi_cmp_shl(&num, &den, i) >= 0) {
      bi_sub_shl(&num, &den, i);
      q |= (u128)1 << i;
    }
  }
  // num is the remainder now, and twice it against the divisor is
  // exactly where the value sits relative to the halfway point.
  cmp = -bi_cmp_shl(&den, &num, 1);
  return fp_round(f, sign, q, u, cmp);
}

static int
hexval(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c |= 32;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
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

// a hexadecimal float needs no big integers: the digits are already
// binary, so the significand goes straight into 128 bits and only the
// bits below the destination's last one have to be summarised, as a
// round bit and a sticky bit. s points just past the "0x".
static u128
scan_hex(const fpfmt *f, const char *s, const char **endp, int sign)
{
  const char *p = s;
  u128 m = 0, q, rest;
  int k = 0, seendot = 0, sticky = 0, any = 0, bl, drop, dsub, u, cmp, rb;

  for (;; p++) {
    int d;
    if (*p == '.') {
      if (seendot)
        break;
      seendot = 1;
      continue;
    }
    if ((d = hexval((unsigned char)*p)) < 0)
      break;
    any = 1;
    // once the significand is full the remaining digits can only ever
    // matter as "something nonzero was down there".
    if (m < ((u128)1 << 124)) {
      m = (m << 4) | (unsigned)d;
      if (seendot)
        k -= 4;
    } else {
      if (!seendot)
        k += 4;
      if (d)
        sticky = 1;
    }
  }
  if (!any) {
    *endp = 0;
    return 0;
  }
  if (*p == 'p' || *p == 'P') {
    const char *q2 = p + 1;
    int esign = 0;
    if (*q2 == '+' || *q2 == '-')
      esign = (*q2++ == '-');
    if (*q2 >= '0' && *q2 <= '9') {
      long ev = 0;
      while (*q2 >= '0' && *q2 <= '9') {
        if (ev < 100000000L)
          ev = ev * 10 + (*q2 - '0');
        q2++;
      }
      k += esign ? (int)-ev : (int)ev;
      p = q2;
    }
  }
  *endp = p;
  if (m == 0)
    return fp_zero(f, sign);

  // drop as many low bits as the destination cannot hold, and no more
  // than that, except that a subnormal result must be rounded at the
  // smallest subnormal's last bit instead.
  bl = u128_bitlen(m);
  drop = bl - f->p;
  dsub = f->emin - f->p + 1 - k;
  if (dsub > drop)
    drop = dsub;
  if (drop <= 0) {
    q = m << -drop;
    cmp = -1;
  } else if (drop > 128) {
    q = 0;
    cmp = -1;
  } else {
    q = drop == 128 ? 0 : m >> drop;
    rb = (int)((m >> (drop - 1)) & 1);
    rest = m & (((u128)1 << (drop - 1)) - 1);
    cmp = rb ? ((rest != 0 || sticky) ? 1 : 0) : -1;
  }
  u = k + drop;
  return fp_round(f, sign, q, u, cmp);
}

// how many characters of p match the lower-case word w, ignoring case,
// or zero if the word is not there in full.
static int
matchci(const char *p, const char *w)
{
  int i;

  for (i = 0; w[i]; i++)
    if ((p[i] | 32) != w[i])
      return 0;
  return i;
}

static u128
scan(const fpfmt *f, const char *s, char **end)
{
  const char *p = s, *e = 0;
  int sign = 0, n;
  u128 bits;

  while (isspace((unsigned char)*p))
    p++;
  if (*p == '+' || *p == '-')
    sign = (*p++ == '-');

  // "0x" only starts a hexadecimal float if a hexadecimal digit really
  // follows; otherwise the subject sequence is just the leading zero
  // and the 'x' is left unconsumed, which the decimal path does for us.
  if (p[0] == '0' && (p[1] | 32) == 'x' &&
      (hexval((unsigned char)p[2]) >= 0 ||
       (p[2] == '.' && hexval((unsigned char)p[3]) >= 0))) {
    bits = scan_hex(f, p + 2, &e, sign);
  } else if ((n = matchci(p, "inf")) != 0) {
    int more = matchci(p + n, "inity");
    e = p + n + more;
    bits = fp_inf(f, sign);
  } else if ((n = matchci(p, "nan")) != 0) {
    e = p + n;
    // an n-char sequence in parentheses is part of the nan but says
    // nothing we can represent, so it is matched and discarded.
    if (*e == '(') {
      const char *q2 = e + 1;
      while (*q2 == '_' || isalnum((unsigned char)*q2))
        q2++;
      if (*q2 == ')')
        e = q2 + 1;
    }
    bits = fp_nan(f, sign);
  } else {
    bits = scan_dec(f, p, &e, sign);
  }

  if (e == 0) {
    if (end)
      *end = (char *)s;
    return 0;
  }
  if (end)
    *end = (char *)e;
  return bits;
}

double
strtod(const char *s, char **end)
{
  union { uint64_t u; double d; } x;

  x.u = (uint64_t)scan(&fmt_dbl, s, end);
  return x.d;
}

float
strtof(const char *s, char **end)
{
  union { uint32_t u; float f; } x;

  x.u = (uint32_t)scan(&fmt_flt, s, end);
  return x.f;
}

long double
strtold(const char *s, char **end)
{
  union { u128 u; long double l; } x;

  x.u = scan(&fmt_ldb, s, end);
  return x.l;
}

double
atof(const char *s)
{
  return strtod(s, 0);
}
