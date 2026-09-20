// the error function and its complement.
//
// two methods meet at x = 1. below it erf is a chebyshev fit of erf(x)/x
// on [0, 1], a polynomial in x^2 of degree twelve, measured at 0.91 ulp;
// erf is odd, so that covers the negative side too, and erfc there is one
// minus it.
//
// above it erfc is worth computing directly, because it is falling like
// e^(-x^2) and one minus erf would have nothing left. the continued
// fraction
//
//   erfc(x) = e^(-x^2) / (sqrt(pi) * (x + (1/2)/(x + 1/(x + (3/2)/(x + ...)))))
//
// is evaluated from the tail inwards. starting at the far end and working
// back is both simpler than the forward recurrences and markedly more
// accurate - a tenth of an ulp where building it from the front leaves ten
// - because every step is a division by a number near x and no error has
// anywhere to accumulate. how far out to start depends on x, since the
// fraction converges faster the larger x is, and the rule below is a fit to
// where the terms stop mattering. erf up there is one minus erfc, which
// loses nothing because erfc is the small one.
//
// the exponential needs care. x^2 for x near 26 is around 700, and an error
// of an ulp in that exponent is an error of a part in 10^14 in the result,
// so x^2 is kept as a head and an exact tail and the tail is applied as the
// first order correction e^-l = 1 - l.
//
// accuracy is around one ulp almost everywhere. the exception is erfc just
// below x = 1, where it is computed as one minus erf and so inherits erf's
// error magnified by six. that is the price of not carrying a second fitted
// polynomial across that stretch.

#include <math.h>
#include <errno.h>

static const double
  E0 = 1.1283791670955126,
  E1 = -0.3761263890318375,
  E2 = 0.11283791670954879,
  E3 = -0.026866170645076792,
  E4 = 0.0052239776248180145,
  E5 = -0.000854832698083379,
  E6 = 0.0001205533111164271,
  E7 = -1.4925595266831182e-05,
  E8 = 1.6461000484121368e-06,
  E9 = -1.6350312701054695e-07,
  E10 = 1.4659775274047436e-08,
  E11 = -1.1372848856791674e-09,
  E12 = 5.957176147748911e-11;

static const double INVSQRTPI = 0.5641895835477563;

// erf(x) for |x| <= 1
static double
erf_small(double x)
{
  double z = x * x;

  return x * (E0 + z * (E1 + z * (E2 + z * (E3 + z * (E4 + z * (E5 + z * (E6 +
             z * (E7 + z * (E8 + z * (E9 + z * (E10 + z * (E11 + z * E12))))))))))));
}

// the continued fraction, for x >= 1. what it returns is the denominator,
// so that erfc(x) is e^(-x^2)/(sqrt(pi) * this).
static double
erfc_cf(double x)
{
  double f;
  int k, n;

  n = (int)(140.0 / x) + 60;
  f = 0.0;
  for (k = n; k > 0; k--)
    f = (0.5 * (double)k) / (x + f);
  return x + f;
}

// e^(-x*x), with x*x carried as a head and an exact tail so that the
// exponent is right to the last bit
static double
exp_negsq(double x)
{
  double h, l;

  h = x * x;
  l = fma(x, x, -h);
  return exp(-h) * (1.0 - l);
}

// erfc(x) for x >= 1
static double
erfc_big(double x)
{
  double r;

  // no cutoff is needed at the bottom of the range: e^(-x*x) underflows on
  // its own, through subnormals and then to zero, and reports the range
  // error itself when it gets there
  r = exp_negsq(x) * INVSQRTPI / erfc_cf(x);
  if (r == 0.0)
    errno = ERANGE;
  return r;
}

double
erf(double x)
{
  double a;

  if (isnan(x))
    return x;
  if (isinf(x))
    return copysign(1.0, x);
  a = fabs(x);
  if (a <= 1.0)
    return erf_small(x);
  return copysign(1.0 - erfc_big(a), x);
}

double
erfc(double x)
{
  if (isnan(x))
    return x;
  if (isinf(x))
    return x > 0.0 ? 0.0 : 2.0;
  if (x >= 1.0)
    return erfc_big(x);
  if (x <= -1.0)
    return 2.0 - erfc_big(-x);
  return 1.0 - erf_small(x);
}

float
erff(float x)
{
  return (float)erf((double)x);
}

float
erfcf(float x)
{
  double r = erfc((double)x);
  float f = (float)r;

  if (r != 0.0 && f == 0.0f)
    errno = ERANGE;
  return f;
}

// the same continued fraction in binary128. it is only used where x is
// large enough for it to converge in a handful of steps, which is also
// where a double erfc would have underflowed to zero although binary128
// still has thousands of orders of magnitude in hand.
static long double
erfc_bigl(long double x)
{
  long double f;
  int k, n;

  n = (int)(140.0L / x) + 60;
  f = 0.0L;
  for (k = n; k > 0; k--)
    f = (0.5L * (long double)k) / (x + f);
  return expl(-x * x) * 0.564189583547756286948079451560772586L / (x + f);
}

// the binary128 forms are the double ones inside the double range, so they
// carry about 53 bits, and switch to the wider continued fraction where a
// double would have lost the answer entirely
long double
erfl(long double x)
{
  if (isnan(x))
    return x;
  if (isinf(x))
    return copysignl(1.0L, x);
  if (fabsl(x) > 6.0L)  // erf is one to every bit binary128 keeps
    return copysignl(1.0L, x) - copysignl(erfc_bigl(fabsl(x)), x);
  return (long double)erf((double)x);
}

long double
erfcl(long double x)
{
  if (isnan(x))
    return x;
  if (isinf(x))
    return x > 0.0L ? 0.0L : 2.0L;
  if (x > 20.0L)
    return erfc_bigl(x);
  if (x < -20.0L)
    return 2.0L - erfc_bigl(-x);
  return (long double)erfc((double)x);
}
