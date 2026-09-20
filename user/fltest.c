// a self test for the two directions of floating point conversion.
//
// the property worth testing is the one printf and scanf depend on:
// whatever __dtoa writes, strtod has to read back as the very same
// bits, and no shorter digit string may do so. that is checked here
// rather than any particular digit string, because a test that hard
// codes digits only proves that two people made the same mistake. the
// values are the known awkward ones -- the ones that need all
// seventeen digits, the extremes of the range, the subnormals -- and
// several hundred generated ones on top of them.

int printf(const char *, ...);
void exit(int) __attribute__((noreturn));

#include <stdlib.h>
#include <string.h>
#include "user/libc/fltfmt.h"

static int tests, failures;

static void
fail(const char *what, const char *s)
{
  failures++;
  if (failures <= 20)
    printf("  fail: %s: %s\n", what, s);
}

static char *
putint(char *p, int v)
{
  char tmp[12];
  int i = 0;

  if (v < 0) {
    *p++ = '-';
    v = -v;
  }
  do {
    tmp[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (i > 0)
    *p++ = tmp[--i];
  return p;
}

// the digits as 0.dddde<exp>, which is exactly what __dtoa's decpt
// means and so needs no adjusting.
static void
tostr(char *out, const char *dig, int n, int decpt, int sign)
{
  char *p = out;
  int i;

  if (sign)
    *p++ = '-';
  *p++ = '0';
  *p++ = '.';
  if (n == 0)
    *p++ = '0';
  for (i = 0; i < n; i++)
    *p++ = dig[i];
  *p++ = 'e';
  p = putint(p, decpt);
  *p = 0;
}

static int
same(double a, double b)
{
  return memcmp(&a, &b, sizeof a) == 0;
}

static void
check(double v)
{
  char dig[64], s[96];
  int decpt, sign, n;

  // the shortest form must read back exactly
  n = __dtoa(v, 0, 0, &decpt, &sign, dig, sizeof dig);
  if (n < 0)
    return;
  tostr(s, dig, n, decpt, sign);
  tests++;
  if (!same(strtod(s, 0), v))
    fail("shortest does not read back", s);
  if (n > 17)
    fail("more than seventeen digits", s);

  // and nothing shorter may, or it was not the shortest
  if (n > 1) {
    char d2[64], s2[96];
    int dp2, sg2, n2 = __dtoa(v, 2, n - 1, &dp2, &sg2, d2, sizeof d2);
    tostr(s2, d2, n2, dp2, sg2);
    tests++;
    if (same(strtod(s2, 0), v))
      fail("a shorter string also reads back", s);
  }

  // seventeen significant digits always identify a double
  n = __dtoa(v, 2, 17, &decpt, &sign, dig, sizeof dig);
  tostr(s, dig, n, decpt, sign);
  tests++;
  if (!same(strtod(s, 0), v))
    fail("mode 2 does not read back", s);

  // so does the exact expansion to as many places as the value has
  // digits, which for these magnitudes fits in the buffer
  if (decpt > -280 && decpt < 280) {
    n = __dtoa(v, 3, 17 - decpt, &decpt, &sign, dig, sizeof dig);
    tostr(s, dig, n, decpt, sign);
    tests++;
    if (!same(strtod(s, 0), v))
      fail("mode 3 does not read back", s);
  }

  // and the hexadecimal digits are exact by construction, so they had
  // better prove it
  {
    char hex[40], *p = s;
    int e2;
    n = __hextoa(v, -1, 0, &e2, &sign, hex, sizeof hex);
    if (sign)
      *p++ = '-';
    *p++ = '0';
    *p++ = 'x';
    *p++ = hex[0];
    *p++ = '.';
    strcpy(p, hex + 1);
    p += n - 1;
    if (n == 1)
      *p++ = '0';
    *p++ = 'p';
    p = putint(p, e2);
    *p = 0;
    tests++;
    if (!same(strtod(s, 0), v))
      fail("hex does not read back", s);
  }
}

// the hard cases: one third of these need all seventeen digits, the
// rest sit on the edges of the format.
static const char *const hard[] = {
  "0.1", "0.2", "0.3", "1e23", "9.999999999999999e22", "5e-324", "2.5e-324", "7.4e-324",
  "1.7976931348623157e308", "2.2250738585072014e-308", "2.2250738585072011e-308",
  "0.30000000000000004", "1.0000000000000002", "9007199254740993", "9007199254740992",
  "1.2345678901234567e-300", "8.98846567431158e307", "4.450147717014403e-308",
  "0.000244140625", "1e-310", "3.1415926535897932", "2.718281828459045",
  "1e22", "1e-22", "1.7e-308", "123456789012345678", "0.5", "1.5", "2.5", "1024", "0",
  "0x1.fffffffffffffp1023", "0x1p-1074", "0x1.8p3", "-0.1", "-5e-324", "1e16", "1e17",
};

int
main(void)
{
  unsigned long long seed = 88172645463325252ULL;
  double v;
  int i, decpt, sign, n;
  char dig[64];

  for (i = 0; (unsigned)i < sizeof hard / sizeof hard[0]; i++)
    check(strtod(hard[i], 0));

  // a few hundred bit patterns, which is the only way to reach the
  // values no one would think to write down
  for (i = 0; i < 400; i++) {
    union { unsigned long long u; double d; } x;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    x.u = seed;
    if (((x.u >> 52) & 0x7ff) == 0x7ff)
      continue;
    check(x.d);
  }

  // and some ordinary arithmetic, where the interesting part is that
  // the results are not decimal at all
  for (i = 1; i <= 100; i++) {
    v = (double)i / 7.0;
    check(v);
    check(v * 1e-30);
    check(v * 1e30);
  }

  // zero, and the two values that have no digits
  n = __dtoa(0.0, 0, 0, &decpt, &sign, dig, sizeof dig);
  tests++;
  if (n != 1 || dig[0] != '0' || decpt != 1 || sign != 0)
    fail("zero", "0.0");
  n = __dtoa(strtod("-0", 0), 0, 0, &decpt, &sign, dig, sizeof dig);
  tests++;
  if (n != 1 || sign != 1)
    fail("negative zero", "-0");
  n = __dtoa(strtod("inf", 0), 0, 0, &decpt, &sign, dig, sizeof dig);
  tests++;
  if (n != -1 || strcmp(dig, "inf") != 0)
    fail("infinity", "inf");
  n = __dtoa(strtod("-nan", 0), 0, 0, &decpt, &sign, dig, sizeof dig);
  tests++;
  if (n != -2 || strcmp(dig, "nan") != 0 || sign != 1)
    fail("nan", "-nan");

  printf("float: %d tests, %d failures\n", tests, failures);
  exit(failures != 0);
}
