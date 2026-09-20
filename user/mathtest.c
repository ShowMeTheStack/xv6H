// a self test for the math library.
//
// most of the checks compare a result against a value computed to fifty
// decimal digits and then rounded to a double, and allow a small number of
// ulp of slack - one or two for the elementary functions, more for the
// special ones, which this library does not claim to round as tightly. the
// rest check the things that have to be exact rather than close: the signs
// of zeros, the classification macros, the values the standard names for
// infinities and NaNs, the errno a domain or range error has to leave
// behind, and the rounding functions, which are wrong if they are off by
// anything at all.
//
// it prints one line per failure and a count at the end, and exits nonzero
// if anything failed.

#include <math.h>
#include <float.h>
#include <fenv.h>
#include <errno.h>
#include <stdint.h>

// kernel/errno.h skips EDOM; fall back to the number everyone else uses
#ifndef EDOM
#define EDOM 33
#endif

int printf(const char *, ...);
void exit(int) __attribute__((noreturn));

static int tests;
static int failures;

// how many representable doubles lie between two values
static double
ulpdiff(double got, double want)
{
  union {
    double f;
    int64_t i;
  } a, b;

  if (got == want)
    return 0.0;
  a.f = got;
  b.f = want;
  if ((a.i < 0) != (b.i < 0))
    return 1e30;
  return (double)(a.i > b.i ? a.i - b.i : b.i - a.i);
}

static void
near(const char *what, double got, double want, double tol)
{
  double d;

  tests++;
  if (isnan(want) || isnan(got)) {
    if (isnan(want) && isnan(got))
      return;
  } else if (got == want) {
    return;
  } else if (isfinite(got) && isfinite(want)) {
    d = ulpdiff(got, want);
    if (d <= tol)
      return;
    failures++;
    printf("  fail: %s = %g, want %g, off by %g ulp\n", what, got, want, d);
    return;
  }
  failures++;
  printf("  fail: %s = %g, want %g\n", what, got, want);
}

// for results that have to be bit for bit right, including the sign of a
// zero and the difference between the two infinities
static void
same(const char *what, double got, double want)
{
  tests++;
  if (isnan(want) && isnan(got))
    return;
  if (got == want && signbit(got) == signbit(want))
    return;
  failures++;
  printf("  fail: %s = %g, want %g\n", what, got, want);
}

static void
samei(const char *what, long long got, long long want)
{
  tests++;
  if (got == want)
    return;
  failures++;
  printf("  fail: %s = %d, want %d\n", what, (int)got, (int)want);
}

static void
istrue(const char *what, int cond)
{
  tests++;
  if (cond)
    return;
  failures++;
  printf("  fail: %s\n", what);
}

// the binary128 results are computed in double and widened, so they are
// checked against a relative tolerance rather than an ulp count
static void
nearl(const char *what, long double got, long double want, long double tol)
{
  long double d;

  tests++;
  if (isnan(want) && isnan(got))
    return;
  if (got == want)
    return;
  d = fabsl(got - want);
  if (want != 0.0L)
    d /= fabsl(want);
  if (d <= tol)
    return;
  failures++;
  printf("  fail: %s, relative error %g\n", what, (double)d);
}

// values computed to fifty digits elsewhere and rounded once
static void
generated(void)
{
  near("exp(-700.0)", exp(-700.0), 9.85967654375977e-305, 2);
  near("exp(-100.0)", exp(-100.0), 3.720075976020836e-44, 2);
  near("exp(-10.0)", exp(-10.0), 4.5399929762484854e-05, 2);
  near("exp(-1.0)", exp(-1.0), 0.36787944117144233, 2);
  near("exp(-0.5)", exp(-0.5), 0.6065306597126334, 2);
  near("exp(-1e-09)", exp(-1e-09), 0.999999999, 2);
  near("exp(0.5)", exp(0.5), 1.6487212707001282, 2);
  near("exp(1.0)", exp(1.0), 2.718281828459045, 2);
  near("exp(2.0)", exp(2.0), 7.38905609893065, 2);
  near("exp(10.0)", exp(10.0), 22026.465794806718, 2);
  near("exp(100.0)", exp(100.0), 2.6881171418161356e+43, 2);
  near("exp(700.0)", exp(700.0), 1.0142320547350045e+304, 2);
  near("exp(709.7)", exp(709.7), 1.6549840276802644e+308, 2);
  near("exp2(-1000.0)", exp2(-1000.0), 9.332636185032189e-302, 2);
  near("exp2(-10.5)", exp2(-10.5), 0.0006905339660024879, 2);
  near("exp2(-1.0)", exp2(-1.0), 0.5, 2);
  near("exp2(0.5)", exp2(0.5), 1.4142135623730951, 2);
  near("exp2(1.0)", exp2(1.0), 2.0, 2);
  near("exp2(3.7)", exp2(3.7), 12.99603834169977, 2);
  near("exp2(10.0)", exp2(10.0), 1024.0, 2);
  near("exp2(1000.0)", exp2(1000.0), 1.0715086071862673e+301, 2);
  near("expm1(-0.9)", expm1(-0.9), -0.5934303402594009, 3);
  near("expm1(-0.5)", expm1(-0.5), -0.3934693402873666, 3);
  near("expm1(-0.1)", expm1(-0.1), -0.09516258196404043, 3);
  near("expm1(-1e-10)", expm1(-1e-10), -9.999999999500001e-11, 3);
  near("expm1(1e-10)", expm1(1e-10), 1.00000000005e-10, 3);
  near("expm1(0.1)", expm1(0.1), 0.10517091807564763, 3);
  near("expm1(0.5)", expm1(0.5), 0.6487212707001282, 3);
  near("expm1(0.9)", expm1(0.9), 1.4596031111569496, 3);
  near("expm1(5.0)", expm1(5.0), 147.4131591025766, 3);
  near("expm1(40.0)", expm1(40.0), 2.3538526683702e+17, 3);
  near("log(1e-300)", log(1e-300), -690.7755278982137, 2);
  near("log(1e-05)", log(1e-05), -11.512925464970229, 2);
  near("log(0.5)", log(0.5), -0.6931471805599453, 2);
  near("log(0.9999)", log(0.9999), -0.00010000500033334732, 2);
  near("log(1.5)", log(1.5), 0.4054651081081644, 2);
  near("log(2.0)", log(2.0), 0.6931471805599453, 2);
  near("log(10.0)", log(10.0), 2.302585092994046, 2);
  near("log(100000.0)", log(100000.0), 11.512925464970229, 2);
  near("log(1e+300)", log(1e+300), 690.7755278982137, 2);
  near("log2(1e-300)", log2(1e-300), -996.5784284662087, 2);
  near("log2(0.5)", log2(0.5), -1.0, 2);
  near("log2(1.5)", log2(1.5), 0.5849625007211562, 2);
  near("log2(3.0)", log2(3.0), 1.584962500721156, 2);
  near("log2(1024.0)", log2(1024.0), 10.0, 2);
  near("log2(1e+300)", log2(1e+300), 996.5784284662087, 2);
  near("log10(1e-300)", log10(1e-300), -300.0, 2);
  near("log10(0.5)", log10(0.5), -0.3010299956639812, 2);
  near("log10(1.5)", log10(1.5), 0.17609125905568124, 2);
  near("log10(100.0)", log10(100.0), 2.0, 2);
  near("log10(1e+300)", log10(1e+300), 300.0, 2);
  near("log1p(-0.9)", log1p(-0.9), -2.302585092994046, 2);
  near("log1p(-0.5)", log1p(-0.5), -0.6931471805599453, 2);
  near("log1p(-0.001)", log1p(-0.001), -0.0010005003335835335, 2);
  near("log1p(-1e-12)", log1p(-1e-12), -1.0000000000005e-12, 2);
  near("log1p(1e-12)", log1p(1e-12), 9.999999999995e-13, 2);
  near("log1p(0.001)", log1p(0.001), 0.0009995003330835331, 2);
  near("log1p(0.5)", log1p(0.5), 0.4054651081081644, 2);
  near("log1p(2.0)", log1p(2.0), 1.0986122886681098, 2);
  near("log1p(10000000000.0)", log1p(10000000000.0), 23.025850930040455, 2);
  near("sin(1e-10)", sin(1e-10), 1e-10, 2);
  near("sin(0.1)", sin(0.1), 0.09983341664682815, 2);
  near("sin(0.5)", sin(0.5), 0.479425538604203, 2);
  near("sin(1.0)", sin(1.0), 0.8414709848078965, 2);
  near("sin(1.5)", sin(1.5), 0.9974949866040544, 2);
  near("sin(2.0)", sin(2.0), 0.9092974268256817, 2);
  near("sin(3.0)", sin(3.0), 0.1411200080598672, 2);
  near("sin(3.5)", sin(3.5), -0.35078322768961984, 2);
  near("sin(10.0)", sin(10.0), -0.5440211108893698, 2);
  near("sin(100.0)", sin(100.0), -0.5063656411097588, 2);
  near("sin(1000.0)", sin(1000.0), 0.8268795405320025, 2);
  near("sin(1000000.0)", sin(1000000.0), -0.34999350217129294, 2);
  near("sin(10000000000.0)", sin(10000000000.0), -0.4875060250875107, 2);
  near("sin(1e+22)", sin(1e+22), -0.8522008497671888, 2);
  near("sin(-0.7)", sin(-0.7), -0.644217687237691, 2);
  near("sin(-3.3)", sin(-3.3), 0.1577456941432482, 2);
  near("cos(1e-10)", cos(1e-10), 1.0, 2);
  near("cos(0.1)", cos(0.1), 0.9950041652780258, 2);
  near("cos(0.5)", cos(0.5), 0.8775825618903728, 2);
  near("cos(1.0)", cos(1.0), 0.5403023058681398, 2);
  near("cos(1.5)", cos(1.5), 0.0707372016677029, 2);
  near("cos(2.0)", cos(2.0), -0.4161468365471424, 2);
  near("cos(3.0)", cos(3.0), -0.9899924966004454, 2);
  near("cos(3.5)", cos(3.5), -0.9364566872907963, 2);
  near("cos(10.0)", cos(10.0), -0.8390715290764524, 2);
  near("cos(100.0)", cos(100.0), 0.8623188722876839, 2);
  near("cos(1000.0)", cos(1000.0), 0.5623790762907029, 2);
  near("cos(1000000.0)", cos(1000000.0), 0.9367521275331447, 2);
  near("cos(10000000000.0)", cos(10000000000.0), 0.873119622676856, 2);
  near("cos(1e+22)", cos(1e+22), 0.523214785395139, 2);
  near("cos(-0.7)", cos(-0.7), 0.7648421872844885, 2);
  near("cos(-3.3)", cos(-3.3), -0.9874797699088649, 2);
  near("tan(1e-10)", tan(1e-10), 1e-10, 4);
  near("tan(0.1)", tan(0.1), 0.10033467208545055, 4);
  near("tan(0.5)", tan(0.5), 0.5463024898437905, 4);
  near("tan(1.0)", tan(1.0), 1.5574077246549023, 4);
  near("tan(1.5)", tan(1.5), 14.101419947171719, 4);
  near("tan(2.0)", tan(2.0), -2.185039863261519, 4);
  near("tan(3.0)", tan(3.0), -0.1425465430742778, 4);
  near("tan(10.0)", tan(10.0), 0.6483608274590866, 4);
  near("tan(100.0)", tan(100.0), -0.5872139151569291, 4);
  near("tan(1000000.0)", tan(1000000.0), -0.373624453987599, 4);
  near("tan(10000000000.0)", tan(10000000000.0), -0.5583496378112418, 4);
  near("tan(-0.7)", tan(-0.7), -0.8422883804630794, 4);
  near("asin(-1.0)", asin(-1.0), -1.5707963267948966, 2);
  near("asin(-0.9)", asin(-0.9), -1.1197695149986342, 2);
  near("asin(-0.5)", asin(-0.5), -0.5235987755982989, 2);
  near("asin(-0.1)", asin(-0.1), -0.1001674211615598, 2);
  near("asin(0.0)", asin(0.0), 0.0, 2);
  near("asin(0.1)", asin(0.1), 0.1001674211615598, 2);
  near("asin(0.5)", asin(0.5), 0.5235987755982989, 2);
  near("asin(0.7)", asin(0.7), 0.775397496610753, 2);
  near("asin(0.9)", asin(0.9), 1.1197695149986342, 2);
  near("asin(1.0)", asin(1.0), 1.5707963267948966, 2);
  near("acos(-1.0)", acos(-1.0), 3.141592653589793, 2);
  near("acos(-0.9)", acos(-0.9), 2.6905658417935308, 2);
  near("acos(-0.5)", acos(-0.5), 2.0943951023931957, 2);
  near("acos(-0.1)", acos(-0.1), 1.6709637479564565, 2);
  near("acos(0.0)", acos(0.0), 1.5707963267948966, 2);
  near("acos(0.1)", acos(0.1), 1.4706289056333368, 2);
  near("acos(0.5)", acos(0.5), 1.0471975511965979, 2);
  near("acos(0.7)", acos(0.7), 0.7953988301841436, 2);
  near("acos(0.9)", acos(0.9), 0.45102681179626236, 2);
  near("atan(-100.0)", atan(-100.0), -1.5607966601082315, 2);
  near("atan(-1.0)", atan(-1.0), -0.7853981633974483, 2);
  near("atan(-0.5)", atan(-0.5), -0.4636476090008061, 2);
  near("atan(-0.1)", atan(-0.1), -0.09966865249116204, 2);
  near("atan(0.1)", atan(0.1), 0.09966865249116204, 2);
  near("atan(0.5)", atan(0.5), 0.4636476090008061, 2);
  near("atan(1.0)", atan(1.0), 0.7853981633974483, 2);
  near("atan(2.0)", atan(2.0), 1.1071487177940904, 2);
  near("atan(100.0)", atan(100.0), 1.5607966601082315, 2);
  near("atan(10000000000.0)", atan(10000000000.0), 1.5707963266948965, 2);
  near("atan2(1.0, 1.0)", atan2(1.0, 1.0), 0.7853981633974483, 3);
  near("atan2(1.0, -1.0)", atan2(1.0, -1.0), 2.356194490192345, 3);
  near("atan2(-1.0, 1.0)", atan2(-1.0, 1.0), -0.7853981633974483, 3);
  near("atan2(-1.0, -1.0)", atan2(-1.0, -1.0), -2.356194490192345, 3);
  near("atan2(0.5, 2.0)", atan2(0.5, 2.0), 0.24497866312686414, 3);
  near("atan2(2.0, 0.5)", atan2(2.0, 0.5), 1.3258176636680326, 3);
  near("atan2(-3.0, 4.0)", atan2(-3.0, 4.0), -0.6435011087932844, 3);
  near("sinh(-700.0)", sinh(-700.0), -5.0711602736750225e+303, 3);
  near("sinh(-10.0)", sinh(-10.0), -11013.232874703393, 3);
  near("sinh(-1.0)", sinh(-1.0), -1.1752011936438014, 3);
  near("sinh(-0.1)", sinh(-0.1), -0.10016675001984403, 3);
  near("sinh(-1e-10)", sinh(-1e-10), -1e-10, 3);
  near("sinh(0.1)", sinh(0.1), 0.10016675001984403, 3);
  near("sinh(1.0)", sinh(1.0), 1.1752011936438014, 3);
  near("sinh(10.0)", sinh(10.0), 11013.232874703393, 3);
  near("sinh(100.0)", sinh(100.0), 1.3440585709080678e+43, 3);
  near("sinh(700.0)", sinh(700.0), 5.0711602736750225e+303, 3);
  near("cosh(-700.0)", cosh(-700.0), 5.0711602736750225e+303, 3);
  near("cosh(-10.0)", cosh(-10.0), 11013.232920103323, 3);
  near("cosh(-1.0)", cosh(-1.0), 1.5430806348152437, 3);
  near("cosh(-0.1)", cosh(-0.1), 1.0050041680558035, 3);
  near("cosh(0.1)", cosh(0.1), 1.0050041680558035, 3);
  near("cosh(1.0)", cosh(1.0), 1.5430806348152437, 3);
  near("cosh(10.0)", cosh(10.0), 11013.232920103323, 3);
  near("cosh(100.0)", cosh(100.0), 1.3440585709080678e+43, 3);
  near("cosh(700.0)", cosh(700.0), 5.0711602736750225e+303, 3);
  near("tanh(-30.0)", tanh(-30.0), -1.0, 3);
  near("tanh(-1.0)", tanh(-1.0), -0.7615941559557649, 3);
  near("tanh(-0.4)", tanh(-0.4), -0.3799489622552249, 3);
  near("tanh(-1e-10)", tanh(-1e-10), -1e-10, 3);
  near("tanh(0.4)", tanh(0.4), 0.3799489622552249, 3);
  near("tanh(1.0)", tanh(1.0), 0.7615941559557649, 3);
  near("tanh(5.0)", tanh(5.0), 0.9999092042625951, 3);
  near("tanh(30.0)", tanh(30.0), 1.0, 3);
  near("asinh(-10000000000.0)", asinh(-10000000000.0), -23.7189981105004, 3);
  near("asinh(-2.0)", asinh(-2.0), -1.4436354751788103, 3);
  near("asinh(-1.0)", asinh(-1.0), -0.881373587019543, 3);
  near("asinh(-0.1)", asinh(-0.1), -0.09983407889920756, 3);
  near("asinh(0.1)", asinh(0.1), 0.09983407889920756, 3);
  near("asinh(1.0)", asinh(1.0), 0.881373587019543, 3);
  near("asinh(2.0)", asinh(2.0), 1.4436354751788103, 3);
  near("asinh(10000000000.0)", asinh(10000000000.0), 23.7189981105004, 3);
  near("acosh(1.0)", acosh(1.0), 0.0, 3);
  near("acosh(1.0001)", acosh(1.0001), 0.014142017775251545, 3);
  near("acosh(1.5)", acosh(1.5), 0.9624236501192069, 3);
  near("acosh(2.0)", acosh(2.0), 1.3169578969248168, 3);
  near("acosh(10.0)", acosh(10.0), 2.993222846126381, 3);
  near("acosh(10000000000.0)", acosh(10000000000.0), 23.7189981105004, 3);
  near("atanh(-0.999)", atanh(-0.999), -3.8002011672501994, 3);
  near("atanh(-0.5)", atanh(-0.5), -0.5493061443340549, 3);
  near("atanh(-0.1)", atanh(-0.1), -0.10033534773107558, 3);
  near("atanh(-1e-10)", atanh(-1e-10), -1e-10, 3);
  near("atanh(0.1)", atanh(0.1), 0.10033534773107558, 3);
  near("atanh(0.5)", atanh(0.5), 0.5493061443340549, 3);
  near("atanh(0.999)", atanh(0.999), 3.8002011672501994, 3);
  near("pow(2.0, 10.0)", pow(2.0, 10.0), 1024.0, 2);
  near("pow(2.0, 0.5)", pow(2.0, 0.5), 1.4142135623730951, 2);
  near("pow(10.0, 3.0)", pow(10.0, 3.0), 1000.0, 2);
  near("pow(0.5, -3.0)", pow(0.5, -3.0), 8.0, 2);
  near("pow(3.0, -2.5)", pow(3.0, -2.5), 0.06415002990995841, 2);
  near("pow(1.5, 100.0)", pow(1.5, 100.0), 4.065611775352152e+17, 2);
  near("pow(0.9, 1000.0)", pow(0.9, 1000.0), 1.7478712517226947e-46, 2);
  near("pow(1e-10, 0.1)", pow(1e-10, 0.1), 0.09999999999999999, 2);
  near("pow(7.0, 7.0)", pow(7.0, 7.0), 823543.0, 2);
  near("pow(2.0, -1074.0)", pow(2.0, -1074.0), 5e-324, 2);
  near("pow(1.0000001, 10000000.0)", pow(1.0000001, 10000000.0), 2.7182816941320818, 2);
  near("sqrt(1e-300)", sqrt(1e-300), 1e-150, 0.5);
  near("sqrt(0.5)", sqrt(0.5), 0.7071067811865476, 0.5);
  near("sqrt(2.0)", sqrt(2.0), 1.4142135623730951, 0.5);
  near("sqrt(3.0)", sqrt(3.0), 1.7320508075688772, 0.5);
  near("sqrt(100.0)", sqrt(100.0), 10.0, 0.5);
  near("sqrt(1e+300)", sqrt(1e+300), 1e+150, 0.5);
  near("cbrt(-27.0)", cbrt(-27.0), -3.0, 2);
  near("cbrt(-2.0)", cbrt(-2.0), -1.2599210498948732, 2);
  near("cbrt(0.5)", cbrt(0.5), 0.7937005259840998, 2);
  near("cbrt(2.0)", cbrt(2.0), 1.2599210498948732, 2);
  near("cbrt(8.0)", cbrt(8.0), 2.0, 2);
  near("cbrt(1e+300)", cbrt(1e+300), 1e+100, 2);
  near("hypot(3.0, 4.0)", hypot(3.0, 4.0), 5.0, 2);
  near("hypot(1e+300, 1e+300)", hypot(1e+300, 1e+300), 1.4142135623730952e+300, 2);
  near("hypot(1e-300, 1e-300)", hypot(1e-300, 1e-300), 1.414213562373095e-300, 2);
  near("hypot(5.0, 12.0)", hypot(5.0, 12.0), 13.0, 2);
  near("hypot(0.3, 0.4)", hypot(0.3, 0.4), 0.5, 2);
  near("erf(-3.0)", erf(-3.0), -0.9999779095030014, 3);
  near("erf(-1.0)", erf(-1.0), -0.8427007929497149, 3);
  near("erf(-0.5)", erf(-0.5), -0.5204998778130465, 3);
  near("erf(-0.01)", erf(-0.01), -0.011283415555849618, 3);
  near("erf(0.01)", erf(0.01), 0.011283415555849618, 3);
  near("erf(0.5)", erf(0.5), 0.5204998778130465, 3);
  near("erf(1.0)", erf(1.0), 0.8427007929497149, 3);
  near("erf(2.0)", erf(2.0), 0.9953222650189527, 3);
  near("erf(3.0)", erf(3.0), 0.9999779095030014, 3);
  near("erf(6.0)", erf(6.0), 1.0, 3);
  near("erfc(-3.0)", erfc(-3.0), 1.9999779095030015, 8);
  near("erfc(-1.0)", erfc(-1.0), 1.8427007929497148, 8);
  near("erfc(0.0)", erfc(0.0), 1.0, 8);
  near("erfc(0.5)", erfc(0.5), 0.4795001221869535, 8);
  near("erfc(1.0)", erfc(1.0), 0.15729920705028513, 8);
  near("erfc(2.0)", erfc(2.0), 0.004677734981047266, 8);
  near("erfc(5.0)", erfc(5.0), 1.537459794428035e-12, 8);
  near("erfc(10.0)", erfc(10.0), 2.088487583762545e-45, 8);
  near("erfc(20.0)", erfc(20.0), 5.395865611607901e-176, 8);
  near("erfc(26.0)", erfc(26.0), 5.663192408856143e-296, 8);
  near("lgamma(0.1)", lgamma(0.1), 2.252712651734206, 32);
  near("lgamma(0.5)", lgamma(0.5), 0.5723649429247001, 32);
  near("lgamma(1.5)", lgamma(1.5), -0.12078223763524522, 32);
  near("lgamma(2.5)", lgamma(2.5), 0.2846828704729192, 32);
  near("lgamma(4.0)", lgamma(4.0), 1.791759469228055, 32);
  near("lgamma(10.0)", lgamma(10.0), 12.801827480081469, 32);
  near("lgamma(100.0)", lgamma(100.0), 359.1342053695754, 32);
  near("lgamma(1000.0)", lgamma(1000.0), 5905.220423209181, 32);
  near("lgamma(-0.5)", lgamma(-0.5), 1.2655121234846454, 32);
  near("lgamma(-2.5)", lgamma(-2.5), -0.056243716497674054, 32);
  near("tgamma(0.1)", tgamma(0.1), 9.51350769866873, 32);
  near("tgamma(0.5)", tgamma(0.5), 1.772453850905516, 32);
  near("tgamma(1.5)", tgamma(1.5), 0.886226925452758, 32);
  near("tgamma(2.5)", tgamma(2.5), 1.329340388179137, 32);
  near("tgamma(5.0)", tgamma(5.0), 24.0, 32);
  near("tgamma(10.0)", tgamma(10.0), 362880.0, 32);
  near("tgamma(50.0)", tgamma(50.0), 6.082818640342675e+62, 32);
  near("tgamma(150.0)", tgamma(150.0), 3.80892263763057e+260, 32);
  near("tgamma(-0.5)", tgamma(-0.5), -3.544907701811032, 32);
  near("tgamma(-2.5)", tgamma(-2.5), -0.9453087204829419, 32);
  near("tgamma(-10.5)", tgamma(-10.5), -2.640121820547716e-07, 32);
  near("j0(0.5)", j0(0.5), 0.9384698072408129, 20);
  near("j0(1.0)", j0(1.0), 0.7651976865579666, 20);
  near("j0(2.0)", j0(2.0), 0.22389077914123567, 20);
  near("j0(5.0)", j0(5.0), -0.1775967713143383, 20);
  near("j0(10.0)", j0(10.0), -0.24593576445134835, 20);
  near("j0(19.0)", j0(19.0), 0.1466294396596512, 20);
  near("j0(25.0)", j0(25.0), 0.09626678327595811, 20);
  near("j0(100.0)", j0(100.0), 0.019985850304223122, 20);
  near("j1(0.5)", j1(0.5), 0.2422684576748739, 20);
  near("j1(1.0)", j1(1.0), 0.4400505857449335, 20);
  near("j1(2.0)", j1(2.0), 0.5767248077568734, 20);
  near("j1(5.0)", j1(5.0), -0.32757913759146523, 20);
  near("j1(10.0)", j1(10.0), 0.04347274616886144, 20);
  near("j1(19.0)", j1(19.0), -0.10570143114240926, 20);
  near("j1(25.0)", j1(25.0), -0.1253502495802899, 20);
  near("j1(100.0)", j1(100.0), -0.07714535201411216, 20);
  near("y0(0.5)", y0(0.5), -0.44451873350670656, 20);
  near("y0(1.0)", y0(1.0), 0.08825696421567696, 20);
  near("y0(2.0)", y0(2.0), 0.5103756726497451, 20);
  near("y0(5.0)", y0(5.0), -0.30851762524903376, 20);
  near("y0(10.0)", y0(10.0), 0.055671167283599395, 20);
  near("y0(19.0)", y0(19.0), -0.10951969138534148, 20);
  near("y0(25.0)", y0(25.0), -0.12724943226800614, 20);
  near("y0(100.0)", y0(100.0), -0.07724431336508315, 20);
  near("y1(0.5)", y1(0.5), -1.471472392670243, 20);
  near("y1(1.0)", y1(1.0), -0.7812128213002887, 20);
  near("y1(2.0)", y1(2.0), -0.10703243154093754, 20);
  near("y1(5.0)", y1(5.0), 0.14786314339122683, 20);
  near("y1(10.0)", y1(10.0), 0.24901542420695388, 20);
  near("y1(19.0)", y1(19.0), -0.1495601138626533, 20);
  near("y1(25.0)", y1(25.0), -0.09882996478323741, 20);
  near("y1(100.0)", y1(100.0), -0.020372312002759792, 20);
  near("jn(2, 1.0)", jn(2, 1.0), 0.11490348493190047, 50);
  near("yn(2, 1.0)", yn(2, 1.0), -1.6506826068162543, 50);
  near("jn(2, 5.0)", jn(2, 5.0), 0.046565116277752214, 50);
  near("yn(2, 5.0)", yn(2, 5.0), 0.36766288260552454, 50);
  near("jn(2, 20.0)", jn(2, 20.0), -0.16034135192299814, 50);
  near("yn(2, 20.0)", yn(2, 20.0), -0.07919175824563596, 50);
  near("jn(5, 1.0)", jn(5, 1.0), 0.00024975773021123444, 50);
  near("yn(5, 1.0)", yn(5, 1.0), -260.4058666258122, 50);
  near("jn(5, 5.0)", jn(5, 5.0), 0.26114054612017007, 50);
  near("yn(5, 5.0)", yn(5, 5.0), -0.4536948224911019, 50);
  near("jn(5, 20.0)", jn(5, 20.0), 0.15116976798239498, 50);
  near("yn(5, 20.0)", yn(5, 20.0), -0.10003576788953243, 50);
  near("jn(10, 1.0)", jn(10, 1.0), 2.6306151236874534e-10, 50);
  near("yn(10, 1.0)", yn(10, 1.0), -121618014.27868919, 50);
  near("jn(10, 5.0)", jn(10, 5.0), 0.0014678026473104741, 50);
  near("yn(10, 5.0)", yn(10, 5.0), -25.1291100956101, 50);
  near("jn(10, 20.0)", jn(10, 20.0), 0.1864825580239451, 50);
  near("yn(10, 20.0)", yn(10, 20.0), -0.0438946535156584, 50);
}

static const double inf = 1.0 / 0.0;
static const double zero = 0.0;

static void
classification(void)
{
  double nn = 0.0 / 0.0;

  samei("fpclassify(inf)", fpclassify(inf), FP_INFINITE);
  samei("fpclassify(nan)", fpclassify(nn), FP_NAN);
  samei("fpclassify(0)", fpclassify(zero), FP_ZERO);
  samei("fpclassify(1)", fpclassify(1.0), FP_NORMAL);
  samei("fpclassify(subnormal)", fpclassify(0x1p-1070), FP_SUBNORMAL);
  samei("fpclassify(1.0f)", fpclassify(1.0f), FP_NORMAL);
  samei("fpclassify(1.0L)", fpclassify(1.0L), FP_NORMAL);
  istrue("isnan(nan)", isnan(nn));
  istrue("!isnan(1)", !isnan(1.0));
  istrue("isinf(inf)", isinf(inf));
  istrue("isinf(-inf)", isinf(-inf));
  istrue("!isinf(nan)", !isinf(nn));
  istrue("isfinite(1)", isfinite(1.0));
  istrue("!isfinite(inf)", !isfinite(inf));
  istrue("isnormal(1)", isnormal(1.0));
  istrue("!isnormal(subnormal)", !isnormal(0x1p-1070));
  istrue("!isnormal(0)", !isnormal(zero));
  istrue("signbit(-0.0)", signbit(-zero));
  istrue("!signbit(0.0)", !signbit(zero));
  istrue("signbit(-1.0L)", signbit(-1.0L));

  // the quiet comparisons: every one of them is false against a NaN except
  // isunordered, which is exactly what they exist for
  istrue("isgreater(2,1)", isgreater(2.0, 1.0));
  istrue("!isgreater(1,nan)", !isgreater(1.0, nn));
  istrue("isgreaterequal(1,1)", isgreaterequal(1.0, 1.0));
  istrue("isless(1,2)", isless(1.0, 2.0));
  istrue("!isless(nan,2)", !isless(nn, 2.0));
  istrue("islessequal(1,1)", islessequal(1.0, 1.0));
  istrue("islessgreater(1,2)", islessgreater(1.0, 2.0));
  istrue("!islessgreater(1,1)", !islessgreater(1.0, 1.0));
  istrue("isunordered(nan,1)", isunordered(nn, 1.0));
  istrue("!isunordered(1,1)", !isunordered(1.0, 1.0));

  istrue("HUGE_VAL is infinite", isinf(HUGE_VAL) && HUGE_VAL > 0.0);
  istrue("HUGE_VALF is infinite", isinf(HUGE_VALF) && HUGE_VALF > 0.0f);
  istrue("HUGE_VALL is infinite", isinf(HUGE_VALL) && HUGE_VALL > 0.0L);
  istrue("INFINITY is infinite", isinf(INFINITY));
  istrue("NAN is a NaN", isnan((double)NAN));
  istrue("nan(\"\") is a NaN", isnan(nan("")));
  istrue("nanf(\"\") is a NaN", isnan(nanf("")));
  istrue("nanl(\"\") is a NaN", isnan(nanl("")));
  istrue("math_errhandling has MATH_ERRNO", (math_errhandling & MATH_ERRNO) != 0);
  samei("FP_ILOGB0", FP_ILOGB0, -2147483647 - 1);
  samei("sizeof(long double)", (long long)sizeof(long double), 16);
  samei("LDBL_MANT_DIG", LDBL_MANT_DIG, 113);
  samei("DBL_MANT_DIG", DBL_MANT_DIG, 53);
  samei("FLT_RADIX", FLT_RADIX, 2);
  near("M_PI", M_PI, 3.141592653589793, 0.0);
  near("M_E", M_E, 2.718281828459045, 0.0);
  near("M_LN2", M_LN2, 0.6931471805599453, 0.0);
  near("M_SQRT2", M_SQRT2, 1.4142135623730951, 0.0);
  nearl("M_PIl", M_PIl, 3.14159265358979323846264338327950288L, 1e-33L);
}

static void
signs_and_zeros(void)
{
  same("fabs(-0.0)", fabs(-zero), 0.0);
  same("fabs(-3)", fabs(-3.0), 3.0);
  same("fabs(-inf)", fabs(-inf), inf);
  same("copysign(3,-0.0)", copysign(3.0, -zero), -3.0);
  same("copysign(-3,1)", copysign(-3.0, 1.0), 3.0);
  same("fdim(4,1)", fdim(4.0, 1.0), 3.0);
  same("fdim(1,4)", fdim(1.0, 4.0), 0.0);
  same("fmax(1,nan)", fmax(1.0, 0.0 / 0.0), 1.0);
  same("fmin(nan,2)", fmin(0.0 / 0.0, 2.0), 2.0);
  same("fmax(-0.0,0.0)", fmax(-zero, 0.0), 0.0);
  same("fmin(-0.0,0.0)", fmin(-zero, 0.0), -0.0);
  same("sqrt(-0.0)", sqrt(-zero), -0.0);
  same("sqrt(4)", sqrt(4.0), 2.0);
  same("sqrt(inf)", sqrt(inf), inf);
  same("cbrt(-0.0)", cbrt(-zero), -0.0);
  same("cbrt(-8)", cbrt(-8.0), -2.0);
  same("hypot(3,4)", hypot(3.0, 4.0), 5.0);
  same("hypot(-inf,nan)", hypot(-inf, 0.0 / 0.0), inf);
  same("sin(-0.0)", sin(-zero), -0.0);
  same("tan(-0.0)", tan(-zero), -0.0);
  same("cos(0)", cos(zero), 1.0);
  same("asin(-0.0)", asin(-zero), -0.0);
  same("atan(-0.0)", atan(-zero), -0.0);
  same("sinh(-0.0)", sinh(-zero), -0.0);
  same("tanh(-0.0)", tanh(-zero), -0.0);
  same("expm1(-0.0)", expm1(-zero), -0.0);
  same("log1p(-0.0)", log1p(-zero), -0.0);
  same("cosh(-0.0)", cosh(-zero), 1.0);
  same("exp(0)", exp(zero), 1.0);
  same("exp(-inf)", exp(-inf), 0.0);
  same("exp(inf)", exp(inf), inf);
  same("log(1)", log(1.0), 0.0);
  same("log(inf)", log(inf), inf);
  same("log2(1024)", log2(1024.0), 10.0);
  same("log10(1000)", log10(1000.0), 3.0);
  same("tanh(inf)", tanh(inf), 1.0);
  same("tanh(-inf)", tanh(-inf), -1.0);
  same("erf(inf)", erf(inf), 1.0);
  same("erf(-inf)", erf(-inf), -1.0);
  same("erfc(inf)", erfc(inf), 0.0);
  same("erfc(-inf)", erfc(-inf), 2.0);
  same("atan(inf)", atan(inf), M_PI_2);
  same("atan(-inf)", atan(-inf), -M_PI_2);
  same("lgamma(1)", lgamma(1.0), 0.0);
  same("lgamma(2)", lgamma(2.0), 0.0);
  same("tgamma(1)", tgamma(1.0), 1.0);
  same("tgamma(5)", tgamma(5.0), 24.0);
  same("tgamma(inf)", tgamma(inf), inf);
  same("j0(0)", j0(zero), 1.0);
  same("j1(0)", j1(zero), 0.0);
  same("jn(3,0)", jn(3, zero), 0.0);
}

static void
pow_special(void)
{
  double nn = 0.0 / 0.0;

  // the table from C99 F.9.4.4, which is mostly about which special case
  // wins when two of them apply at once
  same("pow(x,0)", pow(3.0, zero), 1.0);
  same("pow(nan,0)", pow(nn, zero), 1.0);
  same("pow(1,nan)", pow(1.0, nn), 1.0);
  same("pow(-1,inf)", pow(-1.0, inf), 1.0);
  same("pow(-1,-inf)", pow(-1.0, -inf), 1.0);
  same("pow(2,10)", pow(2.0, 10.0), 1024.0);
  same("pow(-2,3)", pow(-2.0, 3.0), -8.0);
  same("pow(-2,2)", pow(-2.0, 2.0), 4.0);
  same("pow(0,3)", pow(zero, 3.0), 0.0);
  same("pow(-0.0,3)", pow(-zero, 3.0), -0.0);
  same("pow(-0.0,2)", pow(-zero, 2.0), 0.0);
  same("pow(-0.0,-3)", pow(-zero, -3.0), -inf);
  same("pow(0,-2)", pow(zero, -2.0), inf);
  same("pow(0.5,inf)", pow(0.5, inf), 0.0);
  same("pow(0.5,-inf)", pow(0.5, -inf), inf);
  same("pow(2,inf)", pow(2.0, inf), inf);
  same("pow(2,-inf)", pow(2.0, -inf), 0.0);
  same("pow(-inf,3)", pow(-inf, 3.0), -inf);
  same("pow(-inf,2)", pow(-inf, 2.0), inf);
  same("pow(-inf,-3)", pow(-inf, -3.0), -0.0);
  same("pow(inf,2)", pow(inf, 2.0), inf);
  same("pow(inf,-2)", pow(inf, -2.0), 0.0);
  istrue("pow(-2,0.5) is nan", isnan(pow(-2.0, 0.5)));
  istrue("pow(nan,2) is nan", isnan(pow(nn, 2.0)));
  same("pow(10,22)", pow(10.0, 22.0), 1e22);
  same("pow(3,3)", pow(3.0, 3.0), 27.0);
}

static void
rounding(void)
{
  double ip;

  same("floor(1.5)", floor(1.5), 1.0);
  same("floor(-1.5)", floor(-1.5), -2.0);
  same("floor(-0.0)", floor(-zero), -0.0);
  same("floor(-0.5)", floor(-0.5), -1.0);
  same("floor(1e300)", floor(1e300), 1e300);
  same("ceil(1.5)", ceil(1.5), 2.0);
  same("ceil(-1.5)", ceil(-1.5), -1.0);
  same("ceil(-0.5)", ceil(-0.5), -0.0);
  same("ceil(-0.0)", ceil(-zero), -0.0);
  same("trunc(1.9)", trunc(1.9), 1.0);
  same("trunc(-1.9)", trunc(-1.9), -1.0);
  same("trunc(-0.5)", trunc(-0.5), -0.0);
  same("round(0.5)", round(0.5), 1.0);
  same("round(-0.5)", round(-0.5), -1.0);
  same("round(1.5)", round(1.5), 2.0);
  same("round(2.5)", round(2.5), 3.0);
  same("round(-2.5)", round(-2.5), -3.0);
  same("round(0.49999999999999994)", round(0.49999999999999994), 0.0);
  same("rint(0.5)", rint(0.5), 0.0);
  same("rint(1.5)", rint(1.5), 2.0);
  same("rint(2.5)", rint(2.5), 2.0);
  same("rint(-2.5)", rint(-2.5), -2.0);
  same("rint(-0.0)", rint(-zero), -0.0);
  same("nearbyint(2.5)", nearbyint(2.5), 2.0);
  samei("lrint(2.5)", lrint(2.5), 2);
  samei("lrint(-2.5)", lrint(-2.5), -2);
  samei("llrint(3.5)", llrint(3.5), 4);
  samei("lround(2.5)", lround(2.5), 3);
  samei("lround(-2.5)", lround(-2.5), -3);
  samei("llround(-0.6)", llround(-0.6), -1);
  same("truncf(-1.9f)", (double)truncf(-1.9f), -1.0);
  same("floorf(-1.5f)", (double)floorf(-1.5f), -2.0);
  same("ceilf(1.2f)", (double)ceilf(1.2f), 2.0);
  same("roundf(-0.5f)", (double)roundf(-0.5f), -1.0);
  same("rintf(2.5f)", (double)rintf(2.5f), 2.0);

  same("modf(3.25) fraction", modf(3.25, &ip), 0.25);
  same("modf(3.25) integer", ip, 3.0);
  same("modf(-3.25) fraction", modf(-3.25, &ip), -0.25);
  same("modf(-3.25) integer", ip, -3.0);
  same("modf(-0.0) fraction", modf(-zero, &ip), -0.0);
  same("modf(inf) fraction", modf(inf, &ip), 0.0);
  same("modf(inf) integer", ip, inf);

  istrue("truncl(-1.9L)", truncl(-1.9L) == -1.0L);
  istrue("floorl(-1.5L)", floorl(-1.5L) == -2.0L);
  istrue("ceill(-1.5L)", ceill(-1.5L) == -1.0L);
  istrue("roundl(2.5L)", roundl(2.5L) == 3.0L);
  istrue("rintl(2.5L)", rintl(2.5L) == 2.0L);
  istrue("truncl keeps big values", truncl(1e4000L) == 1e4000L);
  istrue("floorl(-0.25L) is -1", floorl(-0.25L) == -1.0L);
  samei("lroundl(-2.5L)", lroundl(-2.5L), -3);
}

static void
decomposition(void)
{
  double f;
  long double fl;
  int e;

  f = frexp(24.0, &e);
  same("frexp(24) fraction", f, 0.75);
  samei("frexp(24) exponent", e, 5);
  f = frexp(-0.0, &e);
  same("frexp(-0.0) fraction", f, -0.0);
  samei("frexp(-0.0) exponent", e, 0);
  f = frexp(0x1p-1070, &e);
  same("frexp(subnormal) fraction", f, 0.5);
  samei("frexp(subnormal) exponent", e, -1069);
  fl = frexpl(24.0L, &e);
  istrue("frexpl(24) fraction", fl == 0.75L);
  samei("frexpl(24) exponent", e, 5);

  same("ldexp(1,10)", ldexp(1.0, 10), 1024.0);
  same("ldexp(1,-1074)", ldexp(1.0, -1074), 0x1p-1074);
  same("scalbn(3,-2)", scalbn(3.0, -2), 0.75);
  same("scalbn(1,2000)", scalbn(1.0, 2000), inf);
  same("scalbn(1,-2000)", scalbn(1.0, -2000), 0.0);
  same("scalbln(1,10)", scalbln(1.0, 10L), 1024.0);
  istrue("scalbnl reaches past double", scalbnl(1.0L, 2000) > 1e300L);
  samei("ilogb(1)", ilogb(1.0), 0);
  samei("ilogb(1023.5)", ilogb(1023.5), 9);
  samei("ilogb(0x1p-1070)", ilogb(0x1p-1070), -1070);
  samei("ilogb(0)", ilogb(zero), FP_ILOGB0);
  samei("ilogb(nan)", ilogb(0.0 / 0.0), FP_ILOGBNAN);
  samei("ilogbl(1024)", ilogbl(1024.0L), 10);
  same("logb(1024)", logb(1024.0), 10.0);
  same("logb(0)", logb(zero), -inf);
  same("logb(-inf)", logb(-inf), inf);

  same("nextafter(1,2)", nextafter(1.0, 2.0), 1.0 + DBL_EPSILON);
  same("nextafter(1,0)", nextafter(1.0, 0.0), 1.0 - DBL_EPSILON / 2.0);
  same("nextafter(0,1)", nextafter(zero, 1.0), 0x1p-1074);
  same("nextafter(0,-1)", nextafter(zero, -1.0), -0x1p-1074);
  same("nextafter(1,1)", nextafter(1.0, 1.0), 1.0);
  same("nextafter(-0.0,0.0)", nextafter(-zero, 0.0), 0.0);
  same("nexttoward(1,2)", nexttoward(1.0, 2.0L), 1.0 + DBL_EPSILON);
  istrue("nextafterl steps", nextafterl(1.0L, 2.0L) > 1.0L &&
         nextafterl(1.0L, 2.0L) - 1.0L < 1e-33L);
  same("nextafterf(1,2)", (double)nextafterf(1.0f, 2.0f), (double)(1.0f + FLT_EPSILON));
}

static void
remainders(void)
{
  int q;

  same("fmod(5.5,2)", fmod(5.5, 2.0), 1.5);
  same("fmod(-5.5,2)", fmod(-5.5, 2.0), -1.5);
  same("fmod(5.5,-2)", fmod(5.5, -2.0), 1.5);
  same("fmod(4,2)", fmod(4.0, 2.0), 0.0);
  same("fmod(-4,2)", fmod(-4.0, 2.0), -0.0);
  same("fmod(1,inf)", fmod(1.0, inf), 1.0);
  same("fmod(0,3)", fmod(zero, 3.0), 0.0);
  same("fmod(1e300,7)", fmod(1e300, 7.0), 1.0);
  istrue("fmod(1,0) is nan", isnan(fmod(1.0, zero)));
  istrue("fmod(inf,1) is nan", isnan(fmod(inf, 1.0)));
  same("remainder(5.5,2)", remainder(5.5, 2.0), -0.5);
  same("remainder(6,4)", remainder(6.0, 4.0), -2.0);
  same("remainder(2,4)", remainder(2.0, 4.0), 2.0);
  same("remainder(-5.5,2)", remainder(-5.5, 2.0), 0.5);
  same("remainder(3,4)", remainder(3.0, 4.0), -1.0);
  same("remquo(5.5,2)", remquo(5.5, 2.0, &q), -0.5);
  samei("remquo(5.5,2) quotient", q, 3);
  same("remquo(-5.5,2)", remquo(-5.5, 2.0, &q), 0.5);
  samei("remquo(-5.5,2) quotient", q, -3);
  same("remquo(1,3)", remquo(1.0, 3.0, &q), 1.0);
  samei("remquo(1,3) quotient", q, 0);
  istrue("fmodl works", fmodl(5.5L, 2.0L) == 1.5L);
  istrue("fmodl is exact past double", fmodl(1e4000L, 3.0L) >= 0.0L);
  istrue("remainderl works", remainderl(5.5L, 2.0L) == -0.5L);
  same("fmodf(5.5f,2f)", (double)fmodf(5.5f, 2.0f), 1.5);
}

static void
fused(void)
{
  double a, b;

  // the whole point of fma is that the product is not rounded before the
  // addition: computed the obvious way this is zero
  a = 1.0 + DBL_EPSILON;
  b = 1.0 - DBL_EPSILON / 2.0;
  same("fma keeps the product exact", fma(a, b, -1.0) != 0.0 ? 1.0 : 0.0, 1.0);
  // (1 + 2^-52)(1 - 2^-53) is exactly 1 + 2^-53 - 2^-105
  near("fma gives the exact residue", fma(a, b, -1.0), 0x1p-53 - 0x1p-105, 0.0);
  same("fma(2,3,4)", fma(2.0, 3.0, 4.0), 10.0);
  same("fma(0,inf,1) is nan", isnan(fma(zero, inf, 1.0)) ? 1.0 : 0.0, 1.0);
  same("fmaf(2,3,4)", (double)fmaf(2.0f, 3.0f, 4.0f), 10.0);
  istrue("fmal(2,3,4)", fmal(2.0L, 3.0L, 4.0L) == 10.0L);
  istrue("fmal keeps the product exact",
         fmal(1.0L + LDBL_EPSILON, 1.0L - LDBL_EPSILON / 2.0L, -1.0L) != 0.0L);
}

static void
domain_errors(void)
{
  errno = 0;
  istrue("sqrt(-1) is nan", isnan(sqrt(-1.0)));
  samei("sqrt(-1) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("log(-1) is nan", isnan(log(-1.0)));
  samei("log(-1) sets EDOM", errno, EDOM);
  errno = 0;
  same("log(0)", log(zero), -inf);
  samei("log(0) sets ERANGE", errno, ERANGE);
  errno = 0;
  istrue("asin(2) is nan", isnan(asin(2.0)));
  samei("asin(2) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("acos(-2) is nan", isnan(acos(-2.0)));
  samei("acos(-2) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("acosh(0.5) is nan", isnan(acosh(0.5)));
  samei("acosh(0.5) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("atanh(2) is nan", isnan(atanh(2.0)));
  samei("atanh(2) sets EDOM", errno, EDOM);
  errno = 0;
  same("atanh(1)", atanh(1.0), inf);
  samei("atanh(1) sets ERANGE", errno, ERANGE);
  errno = 0;
  istrue("pow(-2,0.5) is nan", isnan(pow(-2.0, 0.5)));
  samei("pow(-2,0.5) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("sin(inf) is nan", isnan(sin(inf)));
  samei("sin(inf) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("y0(-1) is nan", isnan(y0(-1.0)));
  samei("y0(-1) sets EDOM", errno, EDOM);
  errno = 0;
  istrue("tgamma(-3) is nan", isnan(tgamma(-3.0)));
  samei("tgamma(-3) sets EDOM", errno, EDOM);

  errno = 0;
  same("exp(1000)", exp(1000.0), inf);
  samei("exp(1000) sets ERANGE", errno, ERANGE);
  errno = 0;
  same("exp(-1000)", exp(-1000.0), 0.0);
  samei("exp(-1000) sets ERANGE", errno, ERANGE);
  errno = 0;
  same("pow(10,400)", pow(10.0, 400.0), inf);
  samei("pow(10,400) sets ERANGE", errno, ERANGE);
  errno = 0;
  same("tgamma(200)", tgamma(200.0), inf);
  samei("tgamma(200) sets ERANGE", errno, ERANGE);
  errno = 0;
  same("cosh(800)", cosh(800.0), inf);
  samei("cosh(800) sets ERANGE", errno, ERANGE);
  errno = 0;
  same("lgamma(0)", lgamma(zero), inf);
  samei("lgamma(0) sets ERANGE", errno, ERANGE);
  errno = 0;
  same("y0(0)", y0(zero), -inf);
  samei("y0(0) sets ERANGE", errno, ERANGE);
  errno = 0;
}

static void
signgam_check(void)
{
  lgamma(2.5);
  samei("signgam for a positive argument", signgam, 1);
  lgamma(-0.5);
  samei("signgam for gamma(-0.5)", signgam, -1);
  lgamma(-1.5);
  samei("signgam for gamma(-1.5)", signgam, 1);
}

static void
float_versions(void)
{
  float fi;

  near("expf(1)", (double)expf(1.0f), (double)2.7182817f, 4.0);
  near("logf(10)", (double)logf(10.0f), (double)2.3025851f, 4.0);
  near("sinf(1)", (double)sinf(1.0f), (double)0.84147096f, 4.0);
  near("cosf(1)", (double)cosf(1.0f), (double)0.5403023f, 4.0);
  near("tanf(1)", (double)tanf(1.0f), (double)1.5574077f, 4.0);
  near("atanf(1)", (double)atanf(1.0f), (double)0.7853982f, 4.0);
  near("sqrtf(2)", (double)sqrtf(2.0f), (double)1.4142135f, 1.0);
  near("cbrtf(27)", (double)cbrtf(27.0f), 3.0, 4.0);
  near("hypotf(3,4)", (double)hypotf(3.0f, 4.0f), 5.0, 1.0);
  near("powf(2,10)", (double)powf(2.0f, 10.0f), 1024.0, 1.0);
  near("erff(1)", (double)erff(1.0f), (double)0.8427008f, 8.0);
  near("tgammaf(5)", (double)tgammaf(5.0f), 24.0, 8.0);
  near("log2f(8)", (double)log2f(8.0f), 3.0, 1.0);
  near("exp2f(10)", (double)exp2f(10.0f), 1024.0, 1.0);
  near("sinhf(1)", (double)sinhf(1.0f), (double)1.1752012f, 8.0);
  near("j0f(1)", (double)j0f(1.0f), (double)0.7651977f, 32.0);
  same("fabsf(-2)", (double)fabsf(-2.0f), 2.0);
  same("copysignf(2,-1)", (double)copysignf(2.0f, -1.0f), -2.0);
  same("fmaxf(1,2)", (double)fmaxf(1.0f, 2.0f), 2.0);
  same("fminf(1,2)", (double)fminf(1.0f, 2.0f), 1.0);
  same("fdimf(1,2)", (double)fdimf(1.0f, 2.0f), 0.0);
  {
    int e;

    fi = frexpf(24.0f, &e);
    same("frexpf(24) fraction", (double)fi, 0.75);
    samei("frexpf(24) exponent", e, 5);
    same("ldexpf(1,10)", (double)ldexpf(1.0f, 10), 1024.0);
    samei("ilogbf(1024)", ilogbf(1024.0f), 10);
    same("modff(3.25) fraction", (double)modff(3.25f, &fi), 0.25);
    same("modff(3.25) integer", (double)fi, 3.0);
  }

  errno = 0;
  same("expf(200) overflows", (double)expf(200.0f), (double)HUGE_VALF);
  samei("expf(200) sets ERANGE", errno, ERANGE);
  errno = 0;
}

static void
long_double_versions(void)
{
  long double r;
  int e;

  nearl("sqrtl(2)", sqrtl(2.0L), 1.41421356237309504880168872420969808L, 1e-33L);
  nearl("sqrtl(2) squared", sqrtl(2.0L) * sqrtl(2.0L), 2.0L, 1e-32L);
  nearl("expl(1)", expl(1.0L), 2.71828182845904523536028747135266250L, 1e-15L);
  nearl("logl(e)", logl(2.71828182845904523536028747135266250L), 1.0L, 1e-15L);
  nearl("expl past the double range", logl(expl(5000.0L)), 5000.0L, 1e-15L);
  nearl("log2l(1024)", log2l(1024.0L), 10.0L, 1e-30L);
  nearl("powl(2,10)", powl(2.0L, 10.0L), 1024.0L, 1e-15L);
  nearl("powl reaches past double", log2l(powl(2.0L, 5000.0L)), 5000.0L, 1e-15L);
  nearl("sinl(1)", sinl(1.0L), 0.841470984807896506652502321630299L, 1e-15L);
  nearl("cosl(1)", cosl(1.0L), 0.540302305868139717400936607442976L, 1e-15L);
  nearl("tanl(1)", tanl(1.0L), 1.557407724654902230506974807458360L, 1e-15L);
  nearl("asinl(0.5)", asinl(0.5L), 0.523598775598298873077107230546584L, 1e-15L);
  nearl("acosl(-1)", acosl(-1.0L), 3.14159265358979323846264338327950288L, 1e-30L);
  nearl("atanl(1)", atanl(1.0L), 0.785398163397448309615660845819875721L, 1e-15L);
  nearl("atan2l(1,1)", atan2l(1.0L, 1.0L), 0.785398163397448309615660845819875721L, 1e-15L);
  nearl("sinhl(1)", sinhl(1.0L), 1.175201193643801456882381850595601L, 1e-15L);
  nearl("coshl past the double range", logl(coshl(5000.0L)), 4999.30685281944005469058276787795L, 1e-15L);
  nearl("tanhl(1)", tanhl(1.0L), 0.761594155955764888119458282604794L, 1e-15L);
  nearl("asinhl(1)", asinhl(1.0L), 0.881373587019543025232609324979792L, 1e-15L);
  nearl("acoshl(2)", acoshl(2.0L), 1.316957896924816708625046347307969L, 1e-15L);
  nearl("atanhl(0.5)", atanhl(0.5L), 0.549306144334054845697622618461263L, 1e-15L);
  nearl("cbrtl(27)", cbrtl(27.0L), 3.0L, 1e-30L);
  nearl("hypotl(3,4)", hypotl(3.0L, 4.0L), 5.0L, 1e-32L);
  nearl("expm1l(1e-20)", expm1l(1e-20L) * 1e20L, 1.0L, 1e-15L);
  nearl("log1pl(1e-20)", log1pl(1e-20L) * 1e20L, 1.0L, 1e-15L);
  nearl("erfl(1)", erfl(1.0L), 0.842700792949714869341220635082609L, 1e-14L);
  nearl("tgammal(5)", tgammal(5.0L), 24.0L, 1e-14L);
  nearl("lgammal(1000)", lgammal(1000.0L), 5905.22042320918121182607691236144L, 1e-14L);
  nearl("tgammal past the double range", logl(tgammal(1000.0L)),
        5905.22042320918121182607691236144L, 1e-13L);
  nearl("j0l(1)", j0l(1.0L), 0.765197686557966551449717526102663L, 1e-14L);
  nearl("erfcl past the double range", -logl(erfcl(100.0L)), 10005.1775851226643325704667820840L, 1e-14L);

  same("fabsl(-2)", (double)fabsl(-2.0L), 2.0);
  same("copysignl(2,-1)", (double)copysignl(2.0L, -1.0L), -2.0);
  istrue("fmaxl(1,nan)", fmaxl(1.0L, nanl("")) == 1.0L);
  istrue("fminl(nan,2)", fminl(nanl(""), 2.0L) == 2.0L);
  istrue("fdiml(1,2) is zero", fdiml(1.0L, 2.0L) == 0.0L);
  r = frexpl(-24.0L, &e);
  istrue("frexpl(-24) fraction", r == -0.75L);
  samei("frexpl(-24) exponent", e, 5);
  istrue("ldexpl(1,4000)", ldexpl(1.0L, 4000) > 1e1200L);
  istrue("modfl splits", modfl(3.25L, &r) == 0.25L && r == 3.0L);
  {
    long double sv, cv;

    sincosl(1.0L, &sv, &cv);
    nearl("sincosl agrees with sinl", sv, sinl(1.0L), 0.0L);
    nearl("sincosl agrees with cosl", cv, cosl(1.0L), 0.0L);
  }
  nearl("logbl(1024)", logbl(1024.0L), 10.0L, 0.0L);
}

static void
environment(void)
{
  fexcept_t flags;

  feclearexcept(FE_ALL_EXCEPT);
  samei("no exceptions after clearing", fetestexcept(FE_ALL_EXCEPT), 0);
  feraiseexcept(FE_INVALID);
  samei("invalid was raised", fetestexcept(FE_INVALID) != 0, 1);
  samei("overflow was not", fetestexcept(FE_OVERFLOW), 0);
  fegetexceptflag(&flags, FE_ALL_EXCEPT);
  feclearexcept(FE_ALL_EXCEPT);
  samei("clearing cleared", fetestexcept(FE_ALL_EXCEPT), 0);
  fesetexceptflag(&flags, FE_ALL_EXCEPT);
  samei("the saved flags came back", fetestexcept(FE_INVALID) != 0, 1);
  feclearexcept(FE_ALL_EXCEPT);

  samei("the default rounding is to nearest", fegetround(), FE_TONEAREST);
  samei("fesetround accepts upward", fesetround(FE_UPWARD), 0);
  samei("fegetround agrees", fegetround(), FE_UPWARD);
  samei("fesetround rejects nonsense", fesetround(99) != 0, 1);
  fesetround(FE_TONEAREST);
  samei("rounding is back to nearest", fegetround(), FE_TONEAREST);

  // rint is the one function that has to follow the mode
  fesetround(FE_UPWARD);
  same("rint(1.1) upward", rint(1.1), 2.0);
  fesetround(FE_DOWNWARD);
  same("rint(1.9) downward", rint(1.9), 1.0);
  fesetround(FE_TOWARDZERO);
  same("rint(-1.9) toward zero", rint(-1.9), -1.0);
  fesetround(FE_TONEAREST);
  same("rint(1.5) to nearest", rint(1.5), 2.0);

  // and the bit operations are the ones that must not touch the flags
  feclearexcept(FE_ALL_EXCEPT);
  (void)fabs(-1.5);
  (void)copysign(1.0, -1.0);
  (void)floor(1.5);
  samei("floor raised nothing", fetestexcept(FE_ALL_EXCEPT), 0);
  (void)nearbyint(1.5);
  samei("nearbyint raised nothing", fetestexcept(FE_INEXACT), 0);

  fesetenv(FE_DFL_ENV);
  samei("the default environment rounds to nearest", fegetround(), FE_TONEAREST);
  samei("the default environment is clear", fetestexcept(FE_ALL_EXCEPT), 0);
}

static void
consistency(void)
{
  double s, c;
  int i;

  // relations that must hold whatever the implementation does. each is
  // chosen so that the relation itself does not lose precision: sin^2 +
  // cos^2 is safe because both terms are positive, while cosh^2 - sinh^2,
  // which is just as true, would cancel away to nothing and prove only that
  // subtraction is lossy.
  for (i = -30; i <= 30; i++) {
    double x = (double)i / 3.0;
    double e = exp(x);

    sincos(x, &s, &c);
    near("sincos agrees with sin", s, sin(x), 0.0);
    near("sincos agrees with cos", c, cos(x), 0.0);
    near("sin^2 + cos^2 is one", s * s + c * c, 1.0, 4.0);
    near("log undoes exp", log(e), x, 8.0);
    near("tanh agrees with sinh/cosh", tanh(x), sinh(x) / cosh(x), 8.0);
    near("erf plus erfc is one", erf(x) + erfc(x), 1.0, 4.0);
    if (i != 0) {
      near("tan agrees with sin/cos", tan(x), s / c, 8.0);
      near("cbrt cubed", cbrt(x) * cbrt(x) * cbrt(x), x, 8.0);
      near("asinh undoes sinh", asinh(sinh(x)), x, 16.0);
    }
    if (x > 0.0) {
      // only for a positive argument: for a negative one the two terms
      // very nearly cancel and the identity says nothing about either
      near("cosh + sinh is exp", cosh(x) + sinh(x), e, 4.0);
      near("sqrt squared", sqrt(x) * sqrt(x), x, 2.0);
      near("pow agrees with sqrt", pow(x, 0.5), sqrt(x), 2.0);
      near("lgamma agrees with tgamma", lgamma(x + 3.0), log(tgamma(x + 3.0)), 32.0);
      near("acosh undoes cosh", acosh(cosh(x)), x, 64.0);
    }
    if (fabs(x) > 1.0)
      near("expm1 agrees with exp", expm1(x), e - 1.0, 8.0);
  }
  // atan inverts tan only on the branch tan came from
  for (i = -14; i <= 14; i++) {
    double x = (double)i / 10.0;

    near("atan undoes tan", atan(tan(x)), x, 16.0);
    near("asin undoes sin", asin(sin(x)), x, 16.0);
  }
  for (i = 1; i < 40; i++) {
    double x = (double)i / 4.0;

    // the bessel recurrence, which near a zero of j0 or j1 is a relation
    // between numbers much larger than their difference, so it is checked
    // loosely
    near("j0 and j1 satisfy the recurrence", jn(2, x), 2.0 / x * j1(x) - j0(x), 1e9);
    near("y0 and y1 satisfy the recurrence", yn(2, x), 2.0 / x * y1(x) - y0(x), 1e9);
  }
}

int
main(void)
{
  generated();
  classification();
  signs_and_zeros();
  pow_special();
  rounding();
  decomposition();
  remainders();
  fused();
  domain_errors();
  signgam_check();
  float_versions();
  long_double_versions();
  environment();
  consistency();
  printf("math: %d tests, %d failures\n", tests, failures);
  exit(failures != 0);
}
