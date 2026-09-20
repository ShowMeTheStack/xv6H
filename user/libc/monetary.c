//
// <monetary.h>: strfmon, formatting an amount of money.
//
// the C locale is the only one, and it has no currency symbol, no
// thousands separator and no grouping, which takes most of the work
// out of this: what is left is the sign, the fill and the two
// precisions. two decisions the locale leaves open are made here and
// said out loud:
//
//   - the C locale sets frac_digits and int_frac_digits to CHAR_MAX,
//     which means "unspecified". two digits after the point is what
//     every caller means by money, so that is the default, and a
//     .right precision overrides it.
//   - the C locale's negative_sign is the empty string. printing a
//     negative amount with no mark at all would be worse than useless,
//     so a minus sign is used unless the ( flag asked for parentheses.
//
// %i and %n therefore produce the same text, since the international
// and local currency symbols are both empty.
//
#include <monetary.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

// the destination, with the overflow remembered rather than checked at
// every step: strfmon has to report E2BIG for the whole conversion, so
// there is nothing to do about it until the end.
struct out {
  char *s;
  size_t n, used;
  int over;
};

static void
emit(struct out *o, char c)
{
  if (o->used < o->n)
    o->s[o->used] = c;
  else
    o->over = 1;
  o->used++;
}

static ssize_t
vstrfmon(char *s, size_t n, const char *fmt, va_list ap)
{
  struct out o = {s, n, 0, 0};

  while (*fmt) {
    if (*fmt != '%') {
      emit(&o, *fmt++);
      continue;
    }
    fmt++;
    if (*fmt == '%') {
      emit(&o, *fmt++);
      continue;
    }

    // the flags. ^ turns off grouping, + asks for the locale's signs
    // and ! drops the currency symbol: in this locale all three are
    // already true, so they are accepted and have nothing to do.
    char fill = ' ';
    int leftjust = 0, parens = 0;
    for (;;) {
      if (*fmt == '=' && fmt[1]) {
        fill = fmt[1];
        fmt += 2;
      } else if (*fmt == '(') {
        parens = 1;
        fmt++;
      } else if (*fmt == '-') {
        leftjust = 1;
        fmt++;
      } else if (*fmt == '^' || *fmt == '+' || *fmt == '!') {
        fmt++;
      } else {
        break;
      }
    }

    size_t width = 0;
    while (isdigit((unsigned char)*fmt))
      width = width * 10 + (size_t)(*fmt++ - '0');
    int left = -1, right = -1;
    if (*fmt == '#') {
      fmt++;
      left = 0;
      while (isdigit((unsigned char)*fmt))
        left = left * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
      fmt++;
      right = 0;
      while (isdigit((unsigned char)*fmt))
        right = right * 10 + (*fmt++ - '0');
    }
    if (*fmt != 'i' && *fmt != 'n') {
      errno = EINVAL;
      return -1;
    }
    fmt++;

    double v = va_arg(ap, double);
    int neg = v < 0;
    char num[512];
    int nd = snprintf(num, sizeof(num), "%.*f", right < 0 ? 2 : right, neg ? -v : v);
    if (nd < 0 || (size_t)nd >= sizeof(num)) {
      errno = E2BIG;
      return -1;
    }
    const char *point = strchr(num, '.');
    int intdigits = point ? (int)(point - num) : nd;

    // the conversion is built whole before anything is written,
    // because the field width has to pad what came out of it.
    char field[640];
    size_t fl = 0;
#define PUT(c)                                                                                     \
  do {                                                                                             \
    if (fl >= sizeof(field)) {                                                                     \
      errno = E2BIG;                                                                               \
      return -1;                                                                                   \
    }                                                                                              \
    field[fl++] = (c);                                                                             \
  } while (0)

    if (neg)
      PUT(parens ? '(' : '-');
    // #left asks for the digits before the point to line up in a
    // column that wide, filled on the left.
    for (int i = intdigits; left > 0 && i < left; i++)
      PUT(fill);
    for (int i = 0; i < nd; i++)
      PUT(num[i]);
    if (parens)
      PUT(neg ? ')' : ' '); // a positive amount keeps the column width

    if (!leftjust)
      for (size_t i = fl; i < width; i++)
        emit(&o, fill);
    for (size_t i = 0; i < fl; i++)
      emit(&o, field[i]);
    if (leftjust)
      for (size_t i = fl; i < width; i++)
        emit(&o, fill);
#undef PUT
  }

  // the terminating null has to fit too, so a result exactly filling
  // the buffer is still too big.
  if (o.over || o.used >= n) {
    errno = E2BIG;
    return -1;
  }
  s[o.used] = 0;
  return (ssize_t)o.used;
}

ssize_t
strfmon(char *s, size_t n, const char *fmt, ...)
{
  va_list ap;
  ssize_t r;

  va_start(ap, fmt);
  r = vstrfmon(s, n, fmt, ap);
  va_end(ap);
  return r;
}

ssize_t
strfmon_l(char *s, size_t n, locale_t loc, const char *fmt, ...)
{
  va_list ap;
  ssize_t r;

  (void)loc; // there is one locale, and this is it
  va_start(ap, fmt);
  r = vstrfmon(s, n, fmt, ap);
  va_end(ap);
  return r;
}
