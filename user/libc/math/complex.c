// the complex functions of C99, built out of the real library.
//
// every one of these is a two line formula in a textbook and a trap in
// practice, because the textbook form throws away the bits that matter.
// the arrangements chosen here are the standard cures:
//
//   - cabs is hypot and carg is atan2, so the scaling and the quadrant are
//     already someone else's solved problem, infinities included.
//   - cexp splits e^x into two factors of e^(x/2) once |x| is large, so an
//     answer the format can hold is not lost to an intermediate it cannot.
//     csin, ccos, csinh and ccosh borrow the same split, because past that
//     point sinh and cosh are the same e^|x|/2 to every bit.
//   - clog switches to log1p(|z|^2 - 1) in the annulus around the unit
//     circle. log(|z|) there is the logarithm of a number next to one,
//     which keeps only the bits by which |z| differs from one, while
//     |z|^2 - 1 is that difference itself.
//   - csqrt uses t = sqrt(2*(|z| + |x|)), a sum that cannot cancel, and
//     puts t/2 into whichever part the sign of x makes the larger; the
//     other part becomes a quotient, which is where the cancellation of
//     the naive form would have gone.
//   - ctan and ctanh divide by cos^2 + sinh^2 instead of by 1 + tan*tanh.
//     the first is a sum of squares and cannot cancel; the second can, and
//     does, wherever one of the tangents is large. past |x| = 22 the
//     hyperbolic tangent is one to every bit a double keeps, and the other
//     part is written out as its 4*e^(-2|x|) tail.
//   - the inverses go through the logarithm, arranged to keep whichever
//     part is small: casinh reflects into the first quadrant, where
//     z + sqrt(z^2+1) never cancels, and leaves the small real part to
//     clog's own log1p branch; cacos uses kahan's atan2 and asinh pair,
//     which stays accurate at both branch points where pi/2 - casin(z)
//     would lose the answer in the subtraction; catanh and catan take
//     their small part from log1p(4x/((1-x)^2 + y^2)) rather than from the
//     logarithm of a ratio that is near one.
//
// the parts are taken apart and put back together with __real__, __imag__
// and CMPLX, never as x + I*y, which would turn a negative zero into a
// positive one and put csqrt and clog on the wrong side of their branch
// cuts. products of two complex numbers are written out in real arithmetic
// because the compiler would otherwise call into libgcc, which this
// library does not have.
//
// the special cases cover infinities, NaNs and the signs of zeros where
// they are cheap, which is most places; this is not the whole of annex G.
//
// the float entry points compute in double and round once, which is well
// inside a few ulp of float. the long double ones are written out against
// the *l functions so that they keep the binary128 range; their accuracy
// is the accuracy of those, which is about 53 bits.

#include <complex.h>
#include <math.h>

// e^x * (c + i*s), with the exponential split into two equal factors so
// that a product the format can still hold is not lost to an intermediate
// that overflows on its own. the same split rescues the other end, where
// e^(x/2) is still normal long after e^x has flushed to zero.
static double complex
cis_scaled(double x, double c, double s)
{
  double e = exp(0.5 * x);

  return CMPLX((e * c) * e, (e * s) * e);
}

double
creal(double complex z)
{
  return __real__ z;
}

double
cimag(double complex z)
{
  return __imag__ z;
}

double complex
conj(double complex z)
{
  return CMPLX(__real__ z, -__imag__ z);
}

double complex
cproj(double complex z)
{
  // every infinity is the same point of the riemann sphere; the sign of
  // the imaginary zero is all that survives of the direction it came from
  if (isinf(__real__ z) || isinf(__imag__ z))
    return CMPLX(HUGE_VAL, copysign(0.0, __imag__ z));
  return z;
}

double
cabs(double complex z)
{
  return hypot(__real__ z, __imag__ z);
}

double
carg(double complex z)
{
  return atan2(__imag__ z, __real__ z);
}

double complex
cexp(double complex z)
{
  double x = __real__ z, y = __imag__ z, e, c, s;

  // a real argument has a real answer, and cos(+-0) is one while sin(+-0)
  // is that zero, so the sign of the zero passes straight through
  if (y == 0.0)
    return CMPLX(exp(x), y);
  if (isnan(x))
    return CMPLX(x, x);
  if (!isfinite(y)) {
    // there is no angle to turn through; only the zero that e^-inf leaves
    // is an answer whatever the angle was
    if (x == -HUGE_VAL)
      return CMPLX(0.0, 0.0);
    return CMPLX(y - y, y - y);
  }
  sincos(y, &s, &c);
  if (isinf(x)) {
    if (x < 0.0)
      return CMPLX(copysign(0.0, c), copysign(0.0, s));
    return CMPLX(copysign(HUGE_VAL, c), copysign(HUGE_VAL, s));
  }
  if (fabs(x) > 709.0)
    return cis_scaled(x, c, s);
  e = exp(x);
  return CMPLX(e * c, e * s);
}

double complex
clog(double complex z)
{
  double x = __real__ z, y = __imag__ z, ax, ay, t, h, r;

  ax = fabs(x);
  ay = fabs(y);
  if (ay > ax) {
    t = ax;
    ax = ay;
    ay = t;
  }
  h = hypot(ax, ay);
  if (h > 0.70710678118654752 && h < 1.41421356237309505) {
    // the answer is smaller than the bits of h, so log(h) would report
    // only the few that distinguish h from one. the larger part is at
    // least 1/sqrt(2) here, so ax*ax - 1 is where the cancellation is, and
    // the two fma's round the whole of |z|^2 - 1 just twice.
    r = 0.5 * log1p(fma(ay, ay, fma(ax, ax, -1.0)));
  } else {
    r = log(h);
  }
  // atan2 already puts a negative real part on the side of the cut that
  // the sign of the imaginary zero asks for
  return CMPLX(r, atan2(y, x));
}

double complex
csqrt(double complex z)
{
  double x = __real__ z, y = __imag__ z, t;
  int k = 0;

  // both roots of a zero are that zero, and the imaginary part keeps its
  // sign so that the two sides of the cut stay apart
  if (x == 0.0 && y == 0.0)
    return CMPLX(0.0, y);
  // an infinite imaginary part gives an infinite root in both parts even
  // when the real part is a NaN
  if (isinf(y))
    return CMPLX(HUGE_VAL, y);
  if (isinf(x)) {
    // the root of -inf runs up the imaginary axis and the root of +inf
    // along the real one; a NaN in the other part still has a sign
    if (signbit(x))
      return CMPLX(fabs(y - y), copysign(x, y));
    return CMPLX(x, copysign(y - y, y));
  }

  // a power of four comes back out of the root exactly, as a power of two,
  // so the whole range can be brought into the middle for free: without
  // this the sum below would overflow near the top and the root would be
  // computed from a denormal near the bottom
  if (fabs(x) >= 0x1p500 || fabs(y) >= 0x1p500) {
    x = scalbn(x, -4);
    y = scalbn(y, -4);
    k = 2;
  } else if (fabs(x) < 0x1p-500 && fabs(y) < 0x1p-500) {
    x = scalbn(x, 600);
    y = scalbn(y, 600);
    k = -300;
  }

  // |z| + |x| is a sum of two non-negative numbers and cannot cancel. the
  // part that gets t/2 is the larger one; the smaller is left to the
  // quotient, which divides the cancellation away instead of subtracting.
  t = sqrt(2.0 * (hypot(x, y) + fabs(x)));
  if (x >= 0.0)
    return CMPLX(scalbn(0.5 * t, k), scalbn(y / t, k));
  return CMPLX(scalbn(fabs(y) / t, k), scalbn(copysign(0.5 * t, y), k));
}

double complex
cpow(double complex z, double complex w)
{
  double a = __real__ w, b = __imag__ w, u, v;
  double complex l;

  // z^0 is one for every z, including the zeros and infinities that the
  // logarithm below could not have survived
  if (a == 0.0 && b == 0.0)
    return CMPLX(1.0, 0.0);
  if (__real__ z == 0.0 && __imag__ z == 0.0) {
    // log(0) is an infinity that the multiplication would turn into a NaN
    // in the part that should have stayed zero, so the exponents that do
    // have an answer are taken here
    if (a > 0.0)
      return CMPLX(0.0, 0.0);  // whatever direction w turns, |z^w| is zero
    if (b == 0.0 && a < 0.0)
      return CMPLX(HUGE_VAL, 0.0);
    return CMPLX(NAN, NAN);
  }
  l = clog(z);
  u = __real__ l;
  v = __imag__ l;
  return cexp(CMPLX(a * u - b * v, a * v + b * u));
}

double complex
csin(double complex z)
{
  double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(x)) {
    // the sine of an infinity or a NaN has no value; only the zero it is
    // multiplied by in the other part survives
    if (y == 0.0)
      return CMPLX(x - x, y);
    return CMPLX(x - x, x - x);
  }
  if (!isfinite(y)) {
    // cosh has run away and sin x says which way it went, unless there is
    // no real part to say it with
    if (x == 0.0)
      return CMPLX(x, y);
    return CMPLX(y - y, y - y);
  }
  sincos(x, &s, &c);
  if (fabs(y) > 709.0)
    // cosh y and |sinh y| are the same e^|y|/2 by now, and only the sign
    // of the odd one still depends on y
    return cis_scaled(fabs(y), 0.5 * s, copysign(0.5, y) * c);
  return CMPLX(s * cosh(y), c * sinh(y));
}

double complex
ccos(double complex z)
{
  double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(x)) {
    if (y == 0.0)
      return CMPLX(x - x, -y);
    return CMPLX(x - x, x - x);
  }
  if (!isfinite(y)) {
    // cos is even, so the answer at x == 0 is the run-away itself; the
    // imaginary zero takes the sign that -sin(x)*sinh(y) would have given
    if (x == 0.0)
      return CMPLX(y * y, copysign(0.0, -y));
    return CMPLX(y - y, y - y);
  }
  sincos(x, &s, &c);
  if (fabs(y) > 709.0)
    return cis_scaled(fabs(y), 0.5 * c, -copysign(0.5, y) * s);
  return CMPLX(c * cosh(y), -s * sinh(y));
}

double complex
ctan(double complex z)
{
  double x = __real__ z, y = __imag__ z, c, s, sh, d, t;

  if (!isfinite(y)) {
    // tan tends to +-i as the imaginary part runs away, and it gets there
    // from whichever side the sign of y says
    if (isnan(y))
      return CMPLX(y, y);
    return CMPLX(copysign(0.0, x) * 0.0, copysign(1.0, y));
  }
  if (!isfinite(x))
    return CMPLX(x - x, x - x);
  sincos(x, &s, &c);
  if (fabs(y) >= 22.0) {
    // the imaginary part is one to every bit a double keeps; what is left
    // of the real part is sin x cos x / sinh^2 y, which is 4*e^(-2|y|)
    // times as small and would be lost entirely to the formula below
    t = exp(-fabs(y));
    return CMPLX(4.0 * s * c * t * t, copysign(1.0, y));
  }
  sh = sinh(y);
  // cos^2 x + sinh^2 y, which is a sum of squares and so cannot cancel;
  // the usual 1 + tan x * tanh y denominator can, and does, near the poles
  d = c * c + sh * sh;
  return CMPLX(s * c / d, sh * cosh(y) / d);
}

double complex
csinh(double complex z)
{
  double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(y)) {
    // there is no angle left; sinh of a real zero is the only part that
    // still has an answer without one
    if (x == 0.0)
      return CMPLX(x, y - y);
    return CMPLX(y - y, y - y);
  }
  if (!isfinite(x)) {
    if (isnan(x))
      return CMPLX(x, y == 0.0 ? y : x);
    if (y == 0.0)
      return CMPLX(x, y);
    // sinh and cosh are both infinite, so what is left is an infinity
    // pointing along the angle y
    sincos(y, &s, &c);
    return CMPLX(copysign(HUGE_VAL, c * x), copysign(HUGE_VAL, s));
  }
  sincos(y, &s, &c);
  if (fabs(x) > 709.0)
    // sinh x and cosh x are the same e^|x|/2 here; only the sign of the
    // odd one still remembers x
    return cis_scaled(fabs(x), copysign(0.5, x) * c, 0.5 * s);
  return CMPLX(sinh(x) * c, cosh(x) * s);
}

double complex
ccosh(double complex z)
{
  double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(y)) {
    // cosh is even, so an x of either zero leaves the same undefined
    // product, but the imaginary part is still a zero
    if (x == 0.0)
      return CMPLX(y - y, copysign(0.0, x));
    return CMPLX(y - y, y - y);
  }
  if (!isfinite(x)) {
    if (isnan(x))
      return CMPLX(x * x, y == 0.0 ? y : x);
    if (y == 0.0)
      return CMPLX(fabs(x), copysign(y, x));
    sincos(y, &s, &c);
    return CMPLX(copysign(HUGE_VAL, c), copysign(HUGE_VAL, s * x));
  }
  sincos(y, &s, &c);
  if (fabs(x) > 709.0)
    return cis_scaled(fabs(x), 0.5 * c, copysign(0.5, x) * s);
  return CMPLX(cosh(x) * c, sinh(x) * s);
}

double complex
ctanh(double complex z)
{
  double x = __real__ z, y = __imag__ z, c, s, sh, d, t;

  if (!isfinite(x)) {
    if (isnan(x))
      return CMPLX(x, y == 0.0 ? y : x);
    // tanh has already reached +-1, and the imaginary part is the zero
    // that sin y cos y / sinh^2 x has shrunk to
    if (!isfinite(y))
      return CMPLX(copysign(1.0, x), copysign(0.0, y));
    sincos(y, &s, &c);
    return CMPLX(copysign(1.0, x), copysign(0.0, s * c));
  }
  if (!isfinite(y)) {
    if (x == 0.0)
      return CMPLX(x, y - y);
    return CMPLX(y - y, y - y);
  }
  sincos(y, &s, &c);
  if (fabs(x) >= 22.0) {
    // the real part is one to every bit a double keeps; the imaginary part
    // is sin y cos y / sinh^2 x, written out as the 4*e^(-2|x|) tail it
    // has become rather than computed from a squared sinh
    t = exp(-fabs(x));
    return CMPLX(copysign(1.0, x), 4.0 * s * c * t * t);
  }
  sh = sinh(x);
  // sinh^2 x + cos^2 y for the same reason ctan divides by cos^2 + sinh^2
  d = sh * sh + c * c;
  return CMPLX(sh * cosh(x) / d, s * c / d);
}

double complex
casinh(double complex z)
{
  double x = fabs(__real__ z), y = fabs(__imag__ z), u, v;
  double complex w;

  if (isnan(x) || isnan(y)) {
    // asinh(+-i*inf) is an infinity whatever the other part was
    if (isinf(x) || isinf(y))
      return CMPLX(copysign(HUGE_VAL, __real__ z), __imag__ z + __imag__ z);
    return CMPLX(x + y, x + y);
  }
  if (x >= 0x1p250 || y >= 0x1p250) {
    // z^2 would leave the range, but the root has become z itself long
    // before this, so the answer is log(2z)
    u = log(hypot(x, y)) + M_LN2;
    v = atan2(y, x);
  } else {
    // in the first quadrant z and sqrt(z^2+1) both point the same way, so
    // their sum keeps every bit of both. the real part is small when z is,
    // and clog's log1p branch is what keeps it: the sum is then a number
    // next to one.
    w = csqrt(CMPLX((x - y) * (x + y) + 1.0, 2.0 * x * y));
    w = clog(CMPLX(x + __real__ w, y + __imag__ w));
    u = __real__ w;
    v = __imag__ w;
  }
  // asinh is odd and takes conjugates to conjugates, so the quadrant the
  // argument came from is put back one sign at a time
  return CMPLX(copysign(u, __real__ z), copysign(v, __imag__ z));
}

double complex
casin(double complex z)
{
  // asin(z) = -i*asinh(i*z). both rotations are exact, so this borrows the
  // other function's care with the cancellation for nothing.
  double complex w = casinh(CMPLX(-__imag__ z, __real__ z));

  return CMPLX(__imag__ w, -__real__ w);
}

double complex
cacos(double complex z)
{
  double complex p = csqrt(CMPLX(1.0 + __real__ z, __imag__ z));
  double complex q = csqrt(CMPLX(1.0 - __real__ z, -__imag__ z));

  // kahan's form. the real part is an angle, and atan2 is exact about
  // small angles, so it survives z near +1 where the answer goes to zero;
  // the imaginary part is an asinh, which survives z near the real axis
  // for the same reason. taking pi/2 - casin(z) instead would lose either
  // of those in the subtraction.
  return CMPLX(2.0 * atan2(__real__ q, __real__ p),
               asinh(__real__ p * __imag__ q - __imag__ p * __real__ q));
}

double complex
cacosh(double complex z)
{
  double complex w = cacos(z);
  double u = __real__ w, v = __imag__ w;

  // acosh and acos differ by the quarter turn that puts the real part in
  // the right half plane, which is the half the principal value lives in
  if (signbit(v))
    return CMPLX(-v, u);
  return CMPLX(v, -u);
}

double complex
catanh(double complex z)
{
  double x = __real__ z, y = __imag__ z, d;

  if (isinf(x) || isinf(y))
    return CMPLX(copysign(0.0, x), copysign(M_PI_2, y));
  if (isnan(x) || isnan(y))
    return CMPLX(x + y, x + y);
  if (fabs(x) >= 0x1p250 || fabs(y) >= 0x1p250) {
    // (1-x)^2 + y^2 would overflow; at this size atanh(z) is 1/z, with the
    // half turn of the logarithm left on the imaginary part
    d = hypot(x, y);
    return CMPLX((x / d) / d, copysign(M_PI_2, y));
  }
  // the real part is 0.25*log(((1+x)^2 + y^2)/((1-x)^2 + y^2)), a ratio
  // that is near one exactly when the answer is near zero. subtracting the
  // denominator from the numerator first leaves 4x, and log1p then reads
  // the small answer off the small quotient instead of off the ratio.
  d = (1.0 - x) * (1.0 - x) + y * y;
  return CMPLX(0.25 * log1p(4.0 * x / d),
               0.5 * atan2(2.0 * y, (1.0 - x) * (1.0 + x) - y * y));
}

double complex
catan(double complex z)
{
  double x = __real__ z, y = __imag__ z, d;

  if (isinf(x) || isinf(y))
    return CMPLX(copysign(M_PI_2, x), copysign(0.0, y));
  if (isnan(x) || isnan(y))
    return CMPLX(x + y, x + y);
  if (fabs(x) >= 0x1p250 || fabs(y) >= 0x1p250) {
    d = hypot(x, y);
    return CMPLX(copysign(M_PI_2, x), (y / d) / d);
  }
  // atan(z) = -i*atanh(i*z) written out, so that the imaginary part is the
  // one that log1p keeps when it is small
  d = (1.0 - y) * (1.0 - y) + x * x;
  return CMPLX(0.5 * atan2(2.0 * x, (1.0 - x) * (1.0 + x) - y * y),
               0.25 * log1p(4.0 * y / d));
}

// the float entry points. a float argument is exact as a double and every
// one of these answers is far from the double range, so computing there
// and rounding once is well inside a few ulp of float.

float
crealf(float complex z)
{
  return __real__ z;
}

float
cimagf(float complex z)
{
  return __imag__ z;
}

float complex
conjf(float complex z)
{
  return CMPLXF(__real__ z, -__imag__ z);
}

float complex
cprojf(float complex z)
{
  if (isinf(__real__ z) || isinf(__imag__ z))
    return CMPLXF(HUGE_VALF, copysignf(0.0f, __imag__ z));
  return z;
}

float
cabsf(float complex z)
{
  return hypotf(__real__ z, __imag__ z);
}

float
cargf(float complex z)
{
  return atan2f(__imag__ z, __real__ z);
}

float complex
cexpf(float complex z)
{
  double complex r = cexp(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
clogf(float complex z)
{
  double complex r = clog(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
csqrtf(float complex z)
{
  double complex r = csqrt(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
cpowf(float complex z, float complex w)
{
  double complex r = cpow(CMPLX(__real__ z, __imag__ z), CMPLX(__real__ w, __imag__ w));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
csinf(float complex z)
{
  double complex r = csin(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
ccosf(float complex z)
{
  double complex r = ccos(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
ctanf(float complex z)
{
  double complex r = ctan(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
csinhf(float complex z)
{
  double complex r = csinh(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
ccoshf(float complex z)
{
  double complex r = ccosh(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
ctanhf(float complex z)
{
  double complex r = ctanh(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
casinf(float complex z)
{
  double complex r = casin(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
cacosf(float complex z)
{
  double complex r = cacos(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
catanf(float complex z)
{
  double complex r = catan(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
casinhf(float complex z)
{
  double complex r = casinh(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
cacoshf(float complex z)
{
  double complex r = cacosh(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

float complex
catanhf(float complex z)
{
  double complex r = catanh(CMPLX(__real__ z, __imag__ z));

  return CMPLXF(__real__ r, __imag__ r);
}

// the binary128 entry points. they are the same formulas against the *l
// real functions, which is what keeps the binary128 range: a double would
// overflow at 1e308 where these still have four thousand decades to go.
// their accuracy is the accuracy of those functions, which is about 53
// bits, not the 113 the format could hold.

// the thresholds move with the format: expl overflows at 11356 rather than
// 709, and the hyperbolic tangent needs |x| past 40 rather than 22 before
// e^(-2|x|) is below an ulp of one.
static long double complex
cis_scaledl(long double x, long double c, long double s)
{
  long double e = expl(0.5L * x);

  return CMPLXL((e * c) * e, (e * s) * e);
}

long double
creall(long double complex z)
{
  return __real__ z;
}

long double
cimagl(long double complex z)
{
  return __imag__ z;
}

long double complex
conjl(long double complex z)
{
  return CMPLXL(__real__ z, -__imag__ z);
}

long double complex
cprojl(long double complex z)
{
  if (isinf(__real__ z) || isinf(__imag__ z))
    return CMPLXL(HUGE_VALL, copysignl(0.0L, __imag__ z));
  return z;
}

long double
cabsl(long double complex z)
{
  return hypotl(__real__ z, __imag__ z);
}

long double
cargl(long double complex z)
{
  return atan2l(__imag__ z, __real__ z);
}

long double complex
cexpl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, e, c, s;

  if (y == 0.0L)
    return CMPLXL(expl(x), y);
  if (isnan(x))
    return CMPLXL(x, x);
  if (!isfinite(y)) {
    if (x == -HUGE_VALL)
      return CMPLXL(0.0L, 0.0L);
    return CMPLXL(y - y, y - y);
  }
  sincosl(y, &s, &c);
  if (isinf(x)) {
    if (x < 0.0L)
      return CMPLXL(copysignl(0.0L, c), copysignl(0.0L, s));
    return CMPLXL(copysignl(HUGE_VALL, c), copysignl(HUGE_VALL, s));
  }
  if (fabsl(x) > 11356.0L)
    return cis_scaledl(x, c, s);
  e = expl(x);
  return CMPLXL(e * c, e * s);
}

long double complex
clogl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, ax, ay, t, h, r;

  ax = fabsl(x);
  ay = fabsl(y);
  if (ay > ax) {
    t = ax;
    ax = ay;
    ay = t;
  }
  h = hypotl(ax, ay);
  if (h > 0.70710678118654752440L && h < 1.41421356237309504880L) {
    // ax is at least 1/sqrt(2) and at most sqrt(2) here, so ax - 1 is
    // exact and the difference |z|^2 - 1 loses nothing but the roundings
    // of the two products. binary128 has no hardware fma, so the pairs are
    // not carried along as they are in the double form.
    r = 0.5L * log1pl((ax - 1.0L) * (ax + 1.0L) + ay * ay);
  } else {
    r = logl(h);
  }
  return CMPLXL(r, atan2l(y, x));
}

long double complex
csqrtl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, t;
  int k = 0;

  if (x == 0.0L && y == 0.0L)
    return CMPLXL(0.0L, y);
  if (isinf(y))
    return CMPLXL(HUGE_VALL, y);
  if (isinf(x)) {
    if (signbit(x))
      return CMPLXL(fabsl(y - y), copysignl(x, y));
    return CMPLXL(x, copysignl(y - y, y));
  }

  if (fabsl(x) >= 0x1p8000L || fabsl(y) >= 0x1p8000L) {
    x = scalbnl(x, -4);
    y = scalbnl(y, -4);
    k = 2;
  } else if (fabsl(x) < 0x1p-8000L && fabsl(y) < 0x1p-8000L) {
    x = scalbnl(x, 9000);
    y = scalbnl(y, 9000);
    k = -4500;
  }

  t = sqrtl(2.0L * (hypotl(x, y) + fabsl(x)));
  if (x >= 0.0L)
    return CMPLXL(scalbnl(0.5L * t, k), scalbnl(y / t, k));
  return CMPLXL(scalbnl(fabsl(y) / t, k), scalbnl(copysignl(0.5L * t, y), k));
}

long double complex
cpowl(long double complex z, long double complex w)
{
  long double a = __real__ w, b = __imag__ w, u, v;
  long double complex l;

  if (a == 0.0L && b == 0.0L)
    return CMPLXL(1.0L, 0.0L);
  if (__real__ z == 0.0L && __imag__ z == 0.0L) {
    if (a > 0.0L)
      return CMPLXL(0.0L, 0.0L);
    if (b == 0.0L && a < 0.0L)
      return CMPLXL(HUGE_VALL, 0.0L);
    return CMPLXL(NAN, NAN);
  }
  l = clogl(z);
  u = __real__ l;
  v = __imag__ l;
  return cexpl(CMPLXL(a * u - b * v, a * v + b * u));
}

long double complex
csinl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(x)) {
    if (y == 0.0L)
      return CMPLXL(x - x, y);
    return CMPLXL(x - x, x - x);
  }
  if (!isfinite(y)) {
    if (x == 0.0L)
      return CMPLXL(x, y);
    return CMPLXL(y - y, y - y);
  }
  sincosl(x, &s, &c);
  if (fabsl(y) > 11356.0L)
    return cis_scaledl(fabsl(y), 0.5L * s, copysignl(0.5L, y) * c);
  return CMPLXL(s * coshl(y), c * sinhl(y));
}

long double complex
ccosl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(x)) {
    if (y == 0.0L)
      return CMPLXL(x - x, -y);
    return CMPLXL(x - x, x - x);
  }
  if (!isfinite(y)) {
    if (x == 0.0L)
      return CMPLXL(y * y, copysignl(0.0L, -y));
    return CMPLXL(y - y, y - y);
  }
  sincosl(x, &s, &c);
  if (fabsl(y) > 11356.0L)
    return cis_scaledl(fabsl(y), 0.5L * c, -copysignl(0.5L, y) * s);
  return CMPLXL(c * coshl(y), -s * sinhl(y));
}

long double complex
ctanl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, c, s, sh, d, t;

  if (!isfinite(y)) {
    if (isnan(y))
      return CMPLXL(y, y);
    return CMPLXL(copysignl(0.0L, x) * 0.0L, copysignl(1.0L, y));
  }
  if (!isfinite(x))
    return CMPLXL(x - x, x - x);
  sincosl(x, &s, &c);
  if (fabsl(y) >= 40.0L) {
    t = expl(-fabsl(y));
    return CMPLXL(4.0L * s * c * t * t, copysignl(1.0L, y));
  }
  sh = sinhl(y);
  d = c * c + sh * sh;
  return CMPLXL(s * c / d, sh * coshl(y) / d);
}

long double complex
csinhl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(y)) {
    if (x == 0.0L)
      return CMPLXL(x, y - y);
    return CMPLXL(y - y, y - y);
  }
  if (!isfinite(x)) {
    if (isnan(x))
      return CMPLXL(x, y == 0.0L ? y : x);
    if (y == 0.0L)
      return CMPLXL(x, y);
    sincosl(y, &s, &c);
    return CMPLXL(copysignl(HUGE_VALL, c * x), copysignl(HUGE_VALL, s));
  }
  sincosl(y, &s, &c);
  if (fabsl(x) > 11356.0L)
    return cis_scaledl(fabsl(x), copysignl(0.5L, x) * c, 0.5L * s);
  return CMPLXL(sinhl(x) * c, coshl(x) * s);
}

long double complex
ccoshl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, c, s;

  if (!isfinite(y)) {
    if (x == 0.0L)
      return CMPLXL(y - y, copysignl(0.0L, x));
    return CMPLXL(y - y, y - y);
  }
  if (!isfinite(x)) {
    if (isnan(x))
      return CMPLXL(x * x, y == 0.0L ? y : x);
    if (y == 0.0L)
      return CMPLXL(fabsl(x), copysignl(y, x));
    sincosl(y, &s, &c);
    return CMPLXL(copysignl(HUGE_VALL, c), copysignl(HUGE_VALL, s * x));
  }
  sincosl(y, &s, &c);
  if (fabsl(x) > 11356.0L)
    return cis_scaledl(fabsl(x), 0.5L * c, copysignl(0.5L, x) * s);
  return CMPLXL(coshl(x) * c, sinhl(x) * s);
}

long double complex
ctanhl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, c, s, sh, d, t;

  if (!isfinite(x)) {
    if (isnan(x))
      return CMPLXL(x, y == 0.0L ? y : x);
    if (!isfinite(y))
      return CMPLXL(copysignl(1.0L, x), copysignl(0.0L, y));
    sincosl(y, &s, &c);
    return CMPLXL(copysignl(1.0L, x), copysignl(0.0L, s * c));
  }
  if (!isfinite(y)) {
    if (x == 0.0L)
      return CMPLXL(x, y - y);
    return CMPLXL(y - y, y - y);
  }
  sincosl(y, &s, &c);
  if (fabsl(x) >= 40.0L) {
    t = expl(-fabsl(x));
    return CMPLXL(copysignl(1.0L, x), 4.0L * s * c * t * t);
  }
  sh = sinhl(x);
  d = sh * sh + c * c;
  return CMPLXL(sh * coshl(x) / d, s * c / d);
}

long double complex
casinhl(long double complex z)
{
  long double x = fabsl(__real__ z), y = fabsl(__imag__ z), u, v;
  long double complex w;

  if (isnan(x) || isnan(y)) {
    if (isinf(x) || isinf(y))
      return CMPLXL(copysignl(HUGE_VALL, __real__ z), __imag__ z + __imag__ z);
    return CMPLXL(x + y, x + y);
  }
  if (x >= 0x1p4000L || y >= 0x1p4000L) {
    u = logl(hypotl(x, y)) + M_LN2l;
    v = atan2l(y, x);
  } else {
    w = csqrtl(CMPLXL((x - y) * (x + y) + 1.0L, 2.0L * x * y));
    w = clogl(CMPLXL(x + __real__ w, y + __imag__ w));
    u = __real__ w;
    v = __imag__ w;
  }
  return CMPLXL(copysignl(u, __real__ z), copysignl(v, __imag__ z));
}

long double complex
casinl(long double complex z)
{
  long double complex w = casinhl(CMPLXL(-__imag__ z, __real__ z));

  return CMPLXL(__imag__ w, -__real__ w);
}

long double complex
cacosl(long double complex z)
{
  long double complex p = csqrtl(CMPLXL(1.0L + __real__ z, __imag__ z));
  long double complex q = csqrtl(CMPLXL(1.0L - __real__ z, -__imag__ z));

  return CMPLXL(2.0L * atan2l(__real__ q, __real__ p),
                asinhl(__real__ p * __imag__ q - __imag__ p * __real__ q));
}

long double complex
cacoshl(long double complex z)
{
  long double complex w = cacosl(z);
  long double u = __real__ w, v = __imag__ w;

  if (signbit(v))
    return CMPLXL(-v, u);
  return CMPLXL(v, -u);
}

long double complex
catanhl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, d;

  if (isinf(x) || isinf(y))
    return CMPLXL(copysignl(0.0L, x), copysignl(M_PI_2l, y));
  if (isnan(x) || isnan(y))
    return CMPLXL(x + y, x + y);
  if (fabsl(x) >= 0x1p8000L || fabsl(y) >= 0x1p8000L) {
    d = hypotl(x, y);
    return CMPLXL((x / d) / d, copysignl(M_PI_2l, y));
  }
  d = (1.0L - x) * (1.0L - x) + y * y;
  return CMPLXL(0.25L * log1pl(4.0L * x / d),
                0.5L * atan2l(2.0L * y, (1.0L - x) * (1.0L + x) - y * y));
}

long double complex
catanl(long double complex z)
{
  long double x = __real__ z, y = __imag__ z, d;

  if (isinf(x) || isinf(y))
    return CMPLXL(copysignl(M_PI_2l, x), copysignl(0.0L, y));
  if (isnan(x) || isnan(y))
    return CMPLXL(x + y, x + y);
  if (fabsl(x) >= 0x1p8000L || fabsl(y) >= 0x1p8000L) {
    d = hypotl(x, y);
    return CMPLXL(copysignl(M_PI_2l, x), (y / d) / d);
  }
  d = (1.0L - y) * (1.0L - y) + x * x;
  return CMPLXL(0.5L * atan2l(2.0L * x, (1.0L - x) * (1.0L + x) - y * y),
                0.25L * log1pl(4.0L * y / d));
}
