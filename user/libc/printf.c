//
// the printf family.
//
// one engine writes through a small sink, which is either a stream or
// a caller's buffer, so snprintf can count the characters a longer
// buffer would have taken while writing only what fits. the floating
// point conversions ask fltfmt.c for exact decimal or hexadecimal
// digits and then lay them out here.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include "libc.h"

// flags
#define F_LEFT  0x01 // -
#define F_PLUS  0x02 // +
#define F_SPACE 0x04 // ' '
#define F_ALT   0x08 // #
#define F_ZERO  0x10 // 0
#define F_UPPER 0x20 // the conversion was upper case

struct sink {
  FILE *f;      // where the bytes go, or
  char *buf;    // a buffer with room for `size` of them
  size_t size;
  size_t len;   // what the whole output would be
  int err;
};

static void
emit(struct sink *o, const char *s, size_t n)
{
  if (o->err)
    return;
  if (o->f) {
    if (fwrite(s, 1, n, o->f) != n)
      o->err = 1;
  } else if (o->buf) {
    size_t room = o->len < o->size ? o->size - o->len : 0;
    if (room)
      memcpy(o->buf + o->len, s, n < room ? n : room);
  }
  o->len += n;
}

static void
pad(struct sink *o, char c, long n)
{
  char block[32];

  if (n <= 0)
    return;
  memset(block, c, sizeof(block));
  while (n > 0) {
    long take = n < (long)sizeof(block) ? n : (long)sizeof(block);
    emit(o, block, take);
    n -= take;
  }
}

// an integer, already turned into digits, with its sign and prefix,
// padded to the requested width.
static void
put_number(struct sink *o, const char *digits, int ndigits, const char *prefix, int flags, int width,
           int prec)
{
  int plen = prefix ? (int)strlen(prefix) : 0;
  int zeros = prec > ndigits ? prec - ndigits : 0;

  // "0" with an explicit precision of zero prints nothing at all
  int body = plen + zeros + ndigits;
  int space = width > body ? width - body : 0;

  if (!(flags & F_LEFT) && !(flags & F_ZERO))
    pad(o, ' ', space);
  if (prefix)
    emit(o, prefix, plen);
  if (!(flags & F_LEFT) && (flags & F_ZERO) && prec < 0) {
    pad(o, '0', space);
    space = 0;
  }
  pad(o, '0', zeros);
  emit(o, digits, ndigits);
  if (flags & F_LEFT)
    pad(o, ' ', space);
}

static int
to_digits(uintmax_t v, int base, int upper, char *buf)
{
  static const char lower[] = "0123456789abcdef";
  static const char upperd[] = "0123456789ABCDEF";
  const char *set = upper ? upperd : lower;
  char tmp[130];
  int n = 0;

  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = set[v % (unsigned)base];
    v /= (unsigned)base;
  }
  for (int i = 0; i < n; i++)
    buf[i] = tmp[n - 1 - i];
  return n;
}

// %e, %f and %g share this: digits from __dtoa, then placement.
static void
put_float(struct sink *o, long double v, char conv, int flags, int width, int prec)
{
  // %f of a large value asks for every digit before the point, and
  // binary128 reaches 10^4932, so the buffers are sized for the widest
  // conversion rather than for the common one. they live on the stack
  // of a thread that has eight megabytes of it.
  char digits[1200];
  char out[1400];
  int decpt = 0, sign = 0, n;
  int upper = (flags & F_UPPER) != 0;

  if (prec < 0)
    prec = 6;

  // infinities and NaNs never take a precision or a zero pad
  if (conv == 'a' || conv == 'A') {
    int e2 = 0;
    n = __hextoa(v, prec >= 0 && (flags & 0x100) ? prec : -1, upper, &e2, &sign, digits, sizeof(digits));
    if (n < 0) {
      const char *s = (n == -1) ? (upper ? "INF" : "inf") : (upper ? "NAN" : "nan");
      const char *pre = sign ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : 0;
      put_number(o, s, 3, pre, flags & ~F_ZERO, width, -1);
      return;
    }
    char *p = out;
    *p++ = '0';
    *p++ = upper ? 'X' : 'x';
    memcpy(p, digits, n);
    p += n;
    *p++ = upper ? 'P' : 'p';
    *p++ = e2 < 0 ? '-' : '+';
    int e = e2 < 0 ? -e2 : e2;
    char ebuf[8];
    int en = to_digits(e, 10, 0, ebuf);
    memcpy(p, ebuf, en);
    p += en;
    const char *pre = sign ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : 0;
    put_number(o, out, p - out, pre, flags, width, -1);
    return;
  }

  int mode = (conv == 'f' || conv == 'F') ? 3 : 2;
  int want = (conv == 'f' || conv == 'F') ? prec : prec + 1;
  if (conv == 'g' || conv == 'G') {
    if (prec == 0)
      prec = 1;
    mode = 2;
    want = prec;
  }
  n = __dtoa(v, mode, want, &decpt, &sign, digits, sizeof(digits));

  const char *pre = sign ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : 0;
  if (n < 0) {
    const char *s = (n == -1) ? (upper ? "INF" : "inf") : (upper ? "NAN" : "nan");
    put_number(o, s, 3, pre, flags & ~F_ZERO, width, -1);
    return;
  }

  // %g picks %e or %f by the exponent, and drops trailing zeroes
  if (conv == 'g' || conv == 'G') {
    int e = decpt - 1;
    if (e < -4 || e >= prec) {
      conv = upper ? 'E' : 'e';
      prec = prec - 1;
    } else {
      conv = upper ? 'F' : 'f';
      prec = prec - decpt;
      if (prec < 0)
        prec = 0;
    }
    if (!(flags & F_ALT)) {
      while (n > 1 && digits[n - 1] == '0')
        n--;
      if (conv == 'e' || conv == 'E')
        prec = n - 1;
      else
        prec = n - decpt > 0 ? n - decpt : 0;
    }
  }

  char *p = out;
  if (conv == 'e' || conv == 'E') {
    *p++ = n > 0 ? digits[0] : '0';
    if (prec > 0 || (flags & F_ALT))
      *p++ = '.';
    for (int i = 0; i < prec; i++)
      *p++ = (i + 1 < n) ? digits[i + 1] : '0';
    *p++ = (conv == 'E') ? 'E' : 'e';
    int e = (n > 0 && !(n == 1 && digits[0] == '0')) ? decpt - 1 : 0;
    *p++ = e < 0 ? '-' : '+';
    int ea = e < 0 ? -e : e;
    if (ea < 10)
      *p++ = '0';
    char ebuf[8];
    int en = to_digits(ea, 10, 0, ebuf);
    memcpy(p, ebuf, en);
    p += en;
  } else {
    // %f: the digits before the point, then the ones after
    if (decpt <= 0) {
      *p++ = '0';
    } else {
      for (int i = 0; i < decpt; i++)
        *p++ = i < n ? digits[i] : '0';
    }
    if (prec > 0 || (flags & F_ALT))
      *p++ = '.';
    for (int i = 0; i < prec; i++) {
      int idx = decpt + i;
      *p++ = (idx >= 0 && idx < n) ? digits[idx] : '0';
    }
  }
  put_number(o, out, p - out, pre, flags, width, -1);
}

// the engine. everything else in this file is a way of pointing it at
// a different destination.
static int
vsink(struct sink *o, const char *fmt, va_list ap)
{
  char digits[132];

  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      const char *start = p;
      while (*p && *p != '%')
        p++;
      emit(o, start, p - start);
      p--;
      continue;
    }
    p++;
    if (*p == '%') {
      emit(o, "%", 1);
      continue;
    }

    int flags = 0;
    for (;; p++) {
      if (*p == '-')
        flags |= F_LEFT;
      else if (*p == '+')
        flags |= F_PLUS;
      else if (*p == ' ')
        flags |= F_SPACE;
      else if (*p == '#')
        flags |= F_ALT;
      else if (*p == '0')
        flags |= F_ZERO;
      else
        break;
    }

    int width = 0;
    if (*p == '*') {
      width = va_arg(ap, int);
      if (width < 0) {
        flags |= F_LEFT;
        width = -width;
      }
      p++;
    } else {
      while (isdigit((unsigned char)*p))
        width = width * 10 + (*p++ - '0');
    }

    int prec = -1;
    if (*p == '.') {
      p++;
      prec = 0;
      if (*p == '*') {
        prec = va_arg(ap, int);
        p++;
      } else {
        while (isdigit((unsigned char)*p))
          prec = prec * 10 + (*p++ - '0');
      }
      if (prec < 0)
        prec = -1;
      else
        flags |= 0x100; // an explicit precision, which %a needs to know
    }

    // length modifiers
    enum { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF, L_LDOUBLE } len = L_INT;
    for (;;) {
      if (*p == 'h') {
        len = (len == L_SHORT) ? L_CHAR : L_SHORT;
        p++;
      } else if (*p == 'l') {
        len = (len == L_LONG) ? L_LLONG : L_LONG;
        p++;
      } else if (*p == 'q' || *p == 'j') {
        len = L_LLONG;
        p++;
      } else if (*p == 'z') {
        len = L_SIZE;
        p++;
      } else if (*p == 't') {
        len = L_PTRDIFF;
        p++;
      } else if (*p == 'L') {
        len = L_LDOUBLE;
        p++;
      } else {
        break;
      }
    }

    char conv = *p;
    switch (conv) {
    case 'd':
    case 'i': {
      long long v;
      switch (len) {
      case L_CHAR: v = (signed char)va_arg(ap, int); break;
      case L_SHORT: v = (short)va_arg(ap, int); break;
      case L_LONG: v = va_arg(ap, long); break;
      case L_LLONG: v = va_arg(ap, long long); break;
      case L_SIZE: v = va_arg(ap, long); break;
      case L_PTRDIFF: v = va_arg(ap, long); break;
      default: v = va_arg(ap, int); break;
      }
      uintmax_t u = v < 0 ? -(uintmax_t)v : (uintmax_t)v;
      int n = to_digits(u, 10, 0, digits);
      if (prec == 0 && v == 0)
        n = 0;
      const char *pre = v < 0 ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : 0;
      put_number(o, digits, n, pre, flags, width, prec);
      break;
    }
    case 'u':
    case 'o':
    case 'x':
    case 'X': {
      unsigned long long v;
      switch (len) {
      case L_CHAR: v = (unsigned char)va_arg(ap, unsigned); break;
      case L_SHORT: v = (unsigned short)va_arg(ap, unsigned); break;
      case L_LONG: v = va_arg(ap, unsigned long); break;
      case L_LLONG: v = va_arg(ap, unsigned long long); break;
      case L_SIZE: v = va_arg(ap, unsigned long); break;
      case L_PTRDIFF: v = va_arg(ap, unsigned long); break;
      default: v = va_arg(ap, unsigned); break;
      }
      int base = conv == 'o' ? 8 : (conv == 'u' ? 10 : 16);
      int n = to_digits(v, base, conv == 'X', digits);
      if (prec == 0 && v == 0)
        n = 0;
      const char *pre = 0;
      if ((flags & F_ALT) && v) {
        if (conv == 'x')
          pre = "0x";
        else if (conv == 'X')
          pre = "0X";
        else if (conv == 'o' && prec <= n)
          prec = n + 1;
      }
      put_number(o, digits, n, pre, flags, width, prec);
      break;
    }
    case 'c': {
      char c = va_arg(ap, int);
      if (!(flags & F_LEFT))
        pad(o, ' ', width - 1);
      emit(o, &c, 1);
      if (flags & F_LEFT)
        pad(o, ' ', width - 1);
      break;
    }
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (s == 0)
        s = "(null)";
      size_t n = prec >= 0 ? strnlen(s, prec) : strlen(s);
      if (!(flags & F_LEFT))
        pad(o, ' ', width - (long)n);
      emit(o, s, n);
      if (flags & F_LEFT)
        pad(o, ' ', width - (long)n);
      break;
    }
    case 'p': {
      void *v = va_arg(ap, void *);
      if (v == 0) {
        const char *s = "(nil)";
        if (!(flags & F_LEFT))
          pad(o, ' ', width - 5);
        emit(o, s, 5);
        if (flags & F_LEFT)
          pad(o, ' ', width - 5);
      } else {
        int n = to_digits((uintmax_t)(uintptr_t)v, 16, 0, digits);
        put_number(o, digits, n, "0x", flags, width, prec);
      }
      break;
    }
    case 'n': {
      void *v = va_arg(ap, void *);
      switch (len) {
      case L_CHAR: *(signed char *)v = o->len; break;
      case L_SHORT: *(short *)v = o->len; break;
      case L_LONG: case L_SIZE: case L_PTRDIFF: *(long *)v = o->len; break;
      case L_LLONG: *(long long *)v = o->len; break;
      default: *(int *)v = o->len; break;
      }
      break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
      long double v = (len == L_LDOUBLE) ? va_arg(ap, long double) : (long double)va_arg(ap, double);
      if (conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A')
        flags |= F_UPPER;
      put_float(o, v, conv, flags, width, prec);
      break;
    }
    case 'm': { // the GNU extension: the current errno as text
      const char *s = strerror(errno);
      emit(o, s, strlen(s));
      break;
    }
    case 0:
      p--;
      break;
    default:
      emit(o, "%", 1);
      emit(o, &conv, 1);
      break;
    }
  }
  if (o->err)
    return -1;
  return (int)o->len;
}

int
vfprintf(FILE *f, const char *fmt, va_list ap)
{
  struct sink o = {.f = f};
  int n = vsink(&o, fmt, ap);

  return n;
}

int
fprintf(FILE *f, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

int
printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}

int
vprintf(const char *fmt, va_list ap)
{
  return vfprintf(stdout, fmt, ap);
}

int
vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
  struct sink o = {.buf = s, .size = n ? n - 1 : 0};

  int len = vsink(&o, fmt, ap);
  if (s && n)
    s[o.len < o.size ? o.len : o.size] = 0;
  return len;
}

int
snprintf(char *s, size_t n, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(s, n, fmt, ap);
  va_end(ap);
  return r;
}

int
vsprintf(char *s, const char *fmt, va_list ap)
{
  return vsnprintf(s, (size_t)-1 / 2, fmt, ap);
}

int
sprintf(char *s, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int r = vsprintf(s, fmt, ap);
  va_end(ap);
  return r;
}

int
vasprintf(char **out, const char *fmt, va_list ap)
{
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(0, 0, fmt, copy);
  va_end(copy);
  if (n < 0)
    return -1;
  char *p = malloc(n + 1);
  if (p == 0)
    return -1;
  vsnprintf(p, n + 1, fmt, ap);
  *out = p;
  return n;
}

int
asprintf(char **out, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vasprintf(out, fmt, ap);
  va_end(ap);
  return n;
}

int
vdprintf(int fd, const char *fmt, va_list ap)
{
  FILE f;
  char buf[BUFSIZ];

  memset(&f, 0, sizeof(f));
  f.fd = fd;
  f.flags = _F_WRITE | _F_NOCLOSE | _F_EXTBUF;
  f.buftype = _IOFBF;
  f.buf = buf;
  f.bufsize = sizeof(buf);
  int n = vfprintf(&f, fmt, ap);
  __stdio_flush_locked(&f);
  return n;
}

int
dprintf(int fd, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vdprintf(fd, fmt, ap);
  va_end(ap);
  return n;
}
