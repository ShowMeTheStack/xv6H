// bessel functions of the first and second kind, orders zero and one, and
// the recurrences that carry them to any order.
//
// two regimes again. for x below 20 the ascending series are used:
//
//   J0(x) = sum (-z)^k / (k!)^2,          z = x^2/4
//   J1(x) = (x/2) * sum (-z)^k / (k!(k+1)!)
//   Y0(x) = (2/pi) [ (ln(x/2) + gamma) J0(x) - sum H(k) (-z)^k/(k!)^2 ]
//   Y1(x) = (2/pi) (ln(x/2) + gamma) J1(x) - 2/(pi x)
//           - (x/(2 pi)) sum (H(k) + H(k+1)) (-z)^k / (k!(k+1)!)
//
// with H(k) the harmonic numbers. these converge everywhere, but they
// alternate, and at x = 20 the largest term is 400 million times the
// answer: computed in plain doubles they would deliver nothing at all.
// everything in them is therefore carried as a pair of doubles, about 105
// bits, of which the cancellation eats 27 and the rest is the answer. that
// includes the harmonic numbers, which are not exact either and would
// otherwise put their own rounding error in at full term size.
//
// above 20 the series would need hundreds of terms, and the hankel
// asymptotic expansion takes over:
//
//   J(x) = sqrt(2/(pi x)) (P cos w - Q sin w),  w = x - (2nu+1) pi/4
//   Y(x) = sqrt(2/(pi x)) (P sin w + Q cos w)
//
// P and Q being asymptotic series in 1/x which are summed until their terms
// stop shrinking. at x = 20 the smallest term reached is about 1e-20, which
// is where the crossover was put. the phase is not computed as x - pi/4:
// subtracting an irrational multiple of pi from a large x would lose
// exactly the digits the cosine needs. cos x and sin x come from sincos,
// which reduces the argument properly, and the quarter turn is applied with
// the addition formulas instead.
//
// the higher orders use recurrences in n. Y is computed upwards, which is
// stable because Y grows with the order. J shrinks with the order, so
// upwards is stable only while n stays below x; beyond that the recurrence
// runs downwards from a high order seeded arbitrarily, and the answer is
// normalised at the end by the identity J0 + 2 J2 + 2 J4 + ... = 1.
//
// accuracy is a few ulp, except near the zeros of these functions, where a
// result that passes through zero cannot be held to a relative error by any
// method that does not also know the zero exactly.

#include <math.h>
#include <errno.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

static const double PI = 3.141592653589793;
static const double TWO_OVER_PI = 0.6366197723675814;
static const double SQRT1_2 = 0.7071067811865476;
static const double EULER_HI = 0.5772156649015329;
static const double EULER_LO = -4.942915152430646e-18;

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

static void
dd_add(double ah, double al, double bh, double bl, double *rh, double *rl)
{
  double s, e;

  twosum(ah, bh, &s, &e);
  e += al + bl;
  twosum(s, e, rh, rl);
}

static void
dd_mul(double ah, double al, double bh, double bl, double *rh, double *rl)
{
  double p, e;

  twoprod(ah, bh, &p, &e);
  e += ah * bl + al * bh;
  twosum(p, e, rh, rl);
}

// divide a pair by an ordinary double, recovering the remainder with fma
static void
dd_divd(double ah, double al, double b, double *rh, double *rl)
{
  double q, r;

  q = ah / b;
  r = fma(-q, b, ah) + al;
  twosum(q, r / b, rh, rl);
}

// the ascending series for J and Y of order zero and one. nu picks the
// order; both functions of that order come out of the same pass, because
// they are built from the same terms.
static void
series(double x, int nu, double *jp, double *yp)
{
  double h, zh, zl, th, tl, sh, sl, wh, wl, hh, hl, ph, pl, gh, gl;
  double lh, ll, jh, jl, rh, rl;
  int k;

  // z = (x/2)^2, exactly
  h = 0.5 * x;
  twoprod(h, h, &zh, &zl);

  // t is the running term, s the sum of the terms, w the sum weighted by
  // the harmonic numbers, and hh the harmonic number itself
  th = 1.0;
  tl = 0.0;
  sh = 1.0;
  sl = 0.0;
  // the order one weighted sum starts at k = 0, where the term is one and
  // its weight H(0) + H(1) is also one; the order zero one starts at k = 1
  wh = nu == 0 ? 0.0 : 1.0;
  wl = 0.0;
  hh = 0.0;
  hl = 0.0;
  for (k = 1; k < 200; k++) {
    double kk = (double)k;
    double den = nu == 0 ? kk * kk : kk * (kk + 1.0);
    double ih, il;

    // t *= -z/den
    dd_mul(th, tl, -zh, -zl, &ph, &pl);
    dd_divd(ph, pl, den, &th, &tl);
    dd_add(sh, sl, th, tl, &sh, &sl);

    // H(k) = H(k-1) + 1/k, kept wide because it multiplies a term that may
    // be hundreds of millions of times the final answer
    ih = 1.0 / kk;
    il = fma(-ih, kk, 1.0) / kk;
    dd_add(hh, hl, ih, il, &hh, &hl);

    // the weighted sum: H(k) times the term for order zero, and
    // H(k) + H(k+1) times it for order one
    if (nu == 0) {
      dd_mul(th, tl, hh, hl, &ph, &pl);
      dd_add(wh, wl, ph, pl, &wh, &wl);
    } else {
      double jh, jl2;

      jh = 1.0 / (kk + 1.0);
      jl2 = fma(-jh, kk + 1.0, 1.0) / (kk + 1.0);
      dd_add(hh, hl, hh, hl, &gh, &gl);
      dd_add(gh, gl, jh, jl2, &gh, &gl);
      dd_mul(th, tl, gh, gl, &ph, &pl);
      dd_add(wh, wl, ph, pl, &wh, &wl);
    }
    if (fabs(th) < 1e-32 * fabs(sh) && kk > h)
      break;
  }

  if (nu == 0) {
    jh = sh;
    jl = sl;
  } else {
    dd_mul(sh, sl, h, 0.0, &jh, &jl);
  }
  *jp = jh + jl;

  if (yp == 0)
    return;

  // ln(x/2) + gamma, and the logarithm has to be wide too: it multiplies J
  lh = __log_dd(h, &ll);
  dd_add(lh, ll, EULER_HI, EULER_LO, &gh, &gl);
  dd_mul(gh, gl, jh, jl, &ph, &pl);
  if (nu == 0) {
    // Y0 = (2/pi) * ((ln(x/2)+gamma) J0 - sum H(k) t(k))
    dd_add(ph, pl, -wh, -wl, &rh, &rl);
    *yp = TWO_OVER_PI * (rh + rl);
  } else {
    // Y1 = (2/pi)(ln(x/2)+gamma) J1 - 2/(pi x) - (x/4)(2/pi) sum ...
    double qh, ql;

    dd_mul(wh, wl, h * 0.5, 0.0, &qh, &ql);
    dd_add(ph, pl, -qh, -ql, &rh, &rl);
    dd_divd(1.0, 0.0, x, &qh, &ql);
    dd_add(rh, rl, -qh, -ql, &rh, &rl);
    *yp = TWO_OVER_PI * (rh + rl);
  }
}

// the hankel asymptotic expansion, for x at least 20
static void
hankel(double x, int nu, double *jp, double *yp)
{
  double mu, u, p, q, prev, s, c, amp, cw, sw, d;
  int k;

  mu = nu ? 4.0 : 0.0;
  u = 1.0;
  p = 1.0;
  q = 0.0;
  prev = 1.0;
  for (k = 1; k < 60; k++) {
    d = (double)((2 * k - 1) * (2 * k - 1));
    u *= (mu - d) / (8.0 * (double)k * x);
    // the series is asymptotic: it is worth nothing past the point where
    // its terms start growing again, and nothing more once they are below
    // the last bit of the answer
    if (fabs(u) > prev || fabs(u) < 1e-20)
      break;
    switch (k & 3) {
    case 1:
      q += u;
      break;
    case 2:
      p -= u;
      break;
    case 3:
      q -= u;
      break;
    default:
      p += u;
    }
    prev = fabs(u);
  }

  // cos(x - pi/4) = (cos x + sin x)/sqrt(2) and so on: the quarter turn is
  // applied to an accurately reduced sine and cosine rather than to x
  sincos(x, &s, &c);
  amp = sqrt(2.0 / (PI * x)) * SQRT1_2;
  if (nu == 0) {
    cw = c + s;
    sw = s - c;
  } else {
    cw = s - c;
    sw = -(s + c);
  }
  *jp = amp * (p * cw - q * sw);
  if (yp != 0)
    *yp = amp * (p * sw + q * cw);
}

double
j0(double x)
{
  double j;

  if (isnan(x))
    return x;
  x = fabs(x);  // J0 is even
  if (isinf(x))
    return 0.0;
  if (x < 20.0)
    series(x, 0, &j, 0);
  else
    hankel(x, 0, &j, 0);
  return j;
}

double
j1(double x)
{
  double j, s;

  if (isnan(x))
    return x;
  s = copysign(1.0, x);  // J1 is odd
  x = fabs(x);
  if (isinf(x))
    return 0.0;
  if (x < 20.0)
    series(x, 1, &j, 0);
  else
    hankel(x, 1, &j, 0);
  return s * j;
}

double
y0(double x)
{
  double j, y;

  if (isnan(x))
    return x;
  if (x < 0.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x == 0.0) {
    errno = ERANGE;
    return -1.0 / 0.0;
  }
  if (isinf(x))
    return 0.0;
  if (x < 20.0)
    series(x, 0, &j, &y);
  else
    hankel(x, 0, &j, &y);
  return y;
}

double
y1(double x)
{
  double j, y;

  if (isnan(x))
    return x;
  if (x < 0.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x == 0.0) {
    errno = ERANGE;
    return -1.0 / 0.0;
  }
  if (isinf(x))
    return 0.0;
  if (x < 20.0)
    series(x, 1, &j, &y);
  else
    hankel(x, 1, &j, &y);
  return y;
}

double
jn(int n, double x)
{
  double a, b, t, sum, ans, tox;
  int k, m, sign;

  if (isnan(x))
    return x;
  sign = 1;
  if (n < 0) {  // J of a negative order differs only by a sign
    n = -n;
    if (n & 1)
      sign = -sign;
  }
  if (n == 0)
    return j0(x);
  if (n == 1)
    return sign * j1(x);
  if (x < 0.0) {  // J(n, -x) = (-1)^n J(n, x)
    x = -x;
    if (n & 1)
      sign = -sign;
  }
  if (x == 0.0)
    return 0.0;
  if (isinf(x))
    return 0.0;

  tox = 2.0 / x;
  if ((double)n < x) {
    // upwards is stable while the order stays below the argument
    a = j0(x);
    b = j1(x);
    for (k = 1; k < n; k++) {
      t = (double)k * tox * b - a;
      a = b;
      b = t;
    }
    return sign * b;
  }

  // miller's method: start well above the order wanted with an arbitrary
  // seed, recur downwards, and fix the scale at the end with the identity
  // J0 + 2 J2 + 2 J4 + ... = 1
  m = n + (int)(sqrt(160.0 * (double)n) + 2.0 * x) + 10;
  m += m & 1;  // the normalising sum wants to end on an even order
  a = 0.0;
  b = 0x1p-500;
  sum = 0.0;
  ans = 0.0;
  for (k = m; k > 0; k--) {
    // each pass produces the term one order below the one it was given
    t = (double)k * tox * b - a;
    a = b;
    b = t;
    if (fabs(b) > 0x1p500) {  // keep the run from overflowing
      b *= 0x1p-500;
      a *= 0x1p-500;
      ans *= 0x1p-500;
      sum *= 0x1p-500;
    }
    if (k == n + 1)
      ans = b;
    if (k > 1 && ((k - 1) & 1) == 0)
      sum += b;
  }
  sum = 2.0 * sum + b;  // b now holds order zero, which is counted once
  return sign * (ans / sum);
}

double
yn(int n, double x)
{
  double a, b, t, tox;
  int k, sign;

  if (isnan(x))
    return x;
  sign = 1;
  if (n < 0) {
    n = -n;
    if (n & 1)
      sign = -sign;
  }
  if (x < 0.0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x == 0.0) {
    errno = ERANGE;
    return sign * (-1.0 / 0.0);
  }
  if (isinf(x))
    return 0.0;
  if (n == 0)
    return sign * y0(x);
  if (n == 1)
    return sign * y1(x);

  // Y grows with the order, so the upward recurrence is stable for it
  tox = 2.0 / x;
  a = y0(x);
  b = y1(x);
  for (k = 1; k < n; k++) {
    t = (double)k * tox * b - a;
    a = b;
    b = t;
    if (isinf(b)) {  // the order has outrun the range of a double
      errno = ERANGE;
      return sign * b;
    }
  }
  return sign * b;
}

float
j0f(float x)
{
  return (float)j0((double)x);
}

float
j1f(float x)
{
  return (float)j1((double)x);
}

float
jnf(int n, float x)
{
  return (float)jn(n, (double)x);
}

float
y0f(float x)
{
  return (float)y0((double)x);
}

float
y1f(float x)
{
  return (float)y1((double)x);
}

float
ynf(int n, float x)
{
  return (float)yn(n, (double)x);
}

// the binary128 forms are the double ones widened. the series and the
// asymptotic expansion above are both built out of double arithmetic, so
// these carry about 53 bits, not 113, and an argument too large for a
// double gives up entirely.
long double
j0l(long double x)
{
  return (long double)j0((double)x);
}

long double
j1l(long double x)
{
  return (long double)j1((double)x);
}

long double
jnl(int n, long double x)
{
  return (long double)jn(n, (double)x);
}

long double
y0l(long double x)
{
  return (long double)y0((double)x);
}

long double
y1l(long double x)
{
  return (long double)y1((double)x);
}

long double
ynl(int n, long double x)
{
  return (long double)yn(n, (double)x);
}
