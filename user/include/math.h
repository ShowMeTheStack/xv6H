#ifndef _MATH_H
#define _MATH_H
#include <sys/cdefs.h>

// the evaluation types of C99 7.12: rv64 with the D extension evaluates
// each operation in its own format, so FLT_EVAL_METHOD is 0 and these are
// just float and double.
typedef float float_t;
typedef double double_t;

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

// what ilogb answers for arguments that have no exponent to report
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN 2147483647

// the classification codes; the values match what __builtin_fpclassify is
// told to hand back below, so nothing but this header depends on them.
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

// the compiler expands all of these inline into bit tests, which is both
// faster than a call and safe for every argument type.
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isfinite(x)   __builtin_isfinite(x)
#define isinf(x)      __builtin_isinf(x)
#define isnan(x)      __builtin_isnan(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)

// the quiet comparison macros: they must not raise invalid on a NaN, which
// is exactly what these builtins promise.
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)

// fma is a single fmadd.d, so C99's promise that it is fast holds here.
#define FP_FAST_FMA  1
#define FP_FAST_FMAF 1
#define FP_FAST_FMAL 0

// we report domain and range errors through errno only; RISC-V raises the
// fcsr flags in hardware but never traps on them, so MATH_ERREXCEPT would
// promise more than the library actually delivers.
#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

// the constants of XSI math.h, in double and in binary128 for the *l calls
#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define M_El        2.718281828459045235360287471352662498L
#define M_LOG2El    1.442695040888963407359924681001892137L
#define M_LOG10El   0.434294481903251827651128918916605082L
#define M_LN2l      0.693147180559945309417232121458176568L
#define M_LN10l     2.302585092994045684017991454684364208L
#define M_PIl       3.141592653589793238462643383279502884L
#define M_PI_2l     1.570796326794896619231321691639751442L
#define M_PI_4l     0.785398163397448309615660845819875721L
#define M_1_PIl     0.318309886183790671537767526745028724L
#define M_2_PIl     0.636619772367581343075535053490057448L
#define M_2_SQRTPIl 1.128379167095512573896158903121545172L
#define M_SQRT2l    1.414213562373095048801688724209698079L
#define M_SQRT1_2l  0.707106781186547524400844362104849039L

double acos(double);
double asin(double);
double atan(double);
double atan2(double, double);
double cos(double);
double sin(double);
double tan(double);
double acosh(double);
double asinh(double);
double atanh(double);
double cosh(double);
double sinh(double);
double tanh(double);
double exp(double);
double exp2(double);
double expm1(double);
double frexp(double, int *);
int ilogb(double);
double ldexp(double, int);
double log(double);
double log10(double);
double log1p(double);
double log2(double);
double logb(double);
double modf(double, double *);
double scalbn(double, int);
double scalbln(double, long);
double cbrt(double);
double fabs(double);
double hypot(double, double);
double pow(double, double);
double sqrt(double);
double erf(double);
double erfc(double);
double lgamma(double);
double tgamma(double);
double ceil(double);
double floor(double);
double nearbyint(double);
double rint(double);
long lrint(double);
long long llrint(double);
double round(double);
long lround(double);
long long llround(double);
double trunc(double);
double fmod(double, double);
double remainder(double, double);
double remquo(double, double, int *);
double copysign(double, double);
double nan(const char *);
double nextafter(double, double);
double nexttoward(double, long double);
double fdim(double, double);
double fmax(double, double);
double fmin(double, double);
double fma(double, double, double);

float acosf(float);
float asinf(float);
float atanf(float);
float atan2f(float, float);
float cosf(float);
float sinf(float);
float tanf(float);
float acoshf(float);
float asinhf(float);
float atanhf(float);
float coshf(float);
float sinhf(float);
float tanhf(float);
float expf(float);
float exp2f(float);
float expm1f(float);
float frexpf(float, int *);
int ilogbf(float);
float ldexpf(float, int);
float logf(float);
float log10f(float);
float log1pf(float);
float log2f(float);
float logbf(float);
float modff(float, float *);
float scalbnf(float, int);
float scalblnf(float, long);
float cbrtf(float);
float fabsf(float);
float hypotf(float, float);
float powf(float, float);
float sqrtf(float);
float erff(float);
float erfcf(float);
float lgammaf(float);
float tgammaf(float);
float ceilf(float);
float floorf(float);
float nearbyintf(float);
float rintf(float);
long lrintf(float);
long long llrintf(float);
float roundf(float);
long lroundf(float);
long long llroundf(float);
float truncf(float);
float fmodf(float, float);
float remainderf(float, float);
float remquof(float, float, int *);
float copysignf(float, float);
float nanf(const char *);
float nextafterf(float, float);
float nexttowardf(float, long double);
float fdimf(float, float);
float fmaxf(float, float);
float fminf(float, float);
float fmaf(float, float, float);

long double acosl(long double);
long double asinl(long double);
long double atanl(long double);
long double atan2l(long double, long double);
long double cosl(long double);
long double sinl(long double);
long double tanl(long double);
long double acoshl(long double);
long double asinhl(long double);
long double atanhl(long double);
long double coshl(long double);
long double sinhl(long double);
long double tanhl(long double);
long double expl(long double);
long double exp2l(long double);
long double expm1l(long double);
long double frexpl(long double, int *);
int ilogbl(long double);
long double ldexpl(long double, int);
long double logl(long double);
long double log10l(long double);
long double log1pl(long double);
long double log2l(long double);
long double logbl(long double);
long double modfl(long double, long double *);
long double scalbnl(long double, int);
long double scalblnl(long double, long);
long double cbrtl(long double);
long double fabsl(long double);
long double hypotl(long double, long double);
long double powl(long double, long double);
long double sqrtl(long double);
long double erfl(long double);
long double erfcl(long double);
long double lgammal(long double);
long double tgammal(long double);
long double ceill(long double);
long double floorl(long double);
long double nearbyintl(long double);
long double rintl(long double);
long lrintl(long double);
long long llrintl(long double);
long double roundl(long double);
long lroundl(long double);
long long llroundl(long double);
long double truncl(long double);
long double fmodl(long double, long double);
long double remainderl(long double, long double);
long double remquol(long double, long double, int *);
long double copysignl(long double, long double);
long double nanl(const char *);
long double nextafterl(long double, long double);
long double nexttowardl(long double, long double);
long double fdiml(long double, long double);
long double fmaxl(long double, long double);
long double fminl(long double, long double);
long double fmal(long double, long double, long double);

// the sign that lgamma computed, as XSI requires
extern int signgam;

// bessel functions of the first and second kind, from XSI
double j0(double);
double j1(double);
double jn(int, double);
double y0(double);
double y1(double);
double yn(int, double);
float j0f(float);
float j1f(float);
float jnf(int, float);
float y0f(float);
float y1f(float);
float ynf(int, float);
long double j0l(long double);
long double j1l(long double);
long double jnl(int, long double);
long double y0l(long double);
long double y1l(long double);
long double ynl(int, long double);

// an extension: one argument reduction serves both, so asking for the pair
// costs barely more than asking for either one
void sincos(double, double *, double *);
void sincosf(float, float *, float *);
void sincosl(long double, long double *, long double *);

// helpers shared between the sources of this library. they are not part of
// any standard; the leading underscores keep them out of the namespace a
// program is allowed to use, and they are declared here because every
// non-static definition needs a prototype in scope.

// e^(hi+lo) * 2^k for |hi+lo| <= 0.35, the tail of every exponential
double __exp_core(double hi, double lo, int k);

// log(x) to about 2^-105, as the unevaluated sum hi+*lo; x must be finite
// and strictly positive
double __log_dd(double x, double *lo);

// x reduced modulo pi/2: stores the remainder as hi+lo in r[0],r[1] and
// returns the quadrant, so that x = q*(pi/2) + r
int __rem_pio2(double x, double *r);

// sin, cos and tan of a reduced argument hi+lo with |hi| <= pi/4. tan takes
// the quadrant parity in odd: 1 asks for -1/tan(x) instead of tan(x).
double __sin_kernel(double hi, double lo);
double __cos_kernel(double hi, double lo);
double __tan_kernel(double hi, double lo, int odd);
#endif
