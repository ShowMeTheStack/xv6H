//
// the scanf family.
//
// the engine reads through a small source, which is either a stream or
// a string, so that sscanf costs no descriptor and no buffer. every
// conversion works the same way: skip what should be skipped, collect
// the longest prefix that could belong to the conversion into a
// bounded buffer, and hand that to the matching strto* function.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include "libc.h"

struct src {
  FILE *f;
  const char *s;   // a string source
  size_t pos;
  int back;        // one character pushed back, or -1
  long read;       // characters consumed, for %n
};

static int
next(struct src *in)
{
  int c;

  if (in->back >= 0) {
    c = in->back;
    in->back = -1;
  } else if (in->f) {
    c = fgetc(in->f);
  } else {
    c = (unsigned char)in->s[in->pos];
    if (c == 0)
      c = EOF;
    else
      in->pos++;
  }
  if (c != EOF)
    in->read++;
  return c;
}

static void
back(struct src *in, int c)
{
  if (c == EOF)
    return;
  in->back = c;
  in->read--;
}

// the set of characters a %[...] conversion accepts
struct set {
  unsigned char bits[32];
  int negate;
};

static const char *
parse_set(const char *p, struct set *set)
{
  memset(set, 0, sizeof(*set));
  if (*p == '^') {
    set->negate = 1;
    p++;
  }
  int first = 1;
  for (; *p && (*p != ']' || first); p++, first = 0) {
    if (p[0] == '-' && p[1] && p[1] != ']' && !first) {
      // a range, taking the previous character as its start
      unsigned char lo = p[-1], hi = p[1];
      for (unsigned c = lo; c <= hi; c++)
        set->bits[c / 8] |= 1 << (c % 8);
      p++;
      continue;
    }
    unsigned char c = *p;
    set->bits[c / 8] |= 1 << (c % 8);
  }
  return *p == ']' ? p + 1 : p;
}

static int
inset(const struct set *set, int c)
{
  if (c == EOF)
    return 0;
  int has = (set->bits[(unsigned char)c / 8] >> ((unsigned char)c % 8)) & 1;
  return set->negate ? !has : has;
}

static int
vsrc(struct src *in, const char *fmt, va_list ap)
{
  int assigned = 0;
  char buf[512];

  for (const char *p = fmt; *p; p++) {
    if (isspace((unsigned char)*p)) {
      int c;
      while ((c = next(in)) != EOF && isspace(c))
        ;
      back(in, c);
      continue;
    }
    if (*p != '%') {
      int c = next(in);
      if (c != (unsigned char)*p) {
        back(in, c);
        return assigned ? assigned : (c == EOF ? EOF : assigned);
      }
      continue;
    }

    p++;
    int suppress = 0;
    if (*p == '*') {
      suppress = 1;
      p++;
    }
    int width = 0;
    while (isdigit((unsigned char)*p))
      width = width * 10 + (*p++ - '0');
    if (width == 0)
      width = INT_MAX;

    enum { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF, L_LDOUBLE } len = L_INT;
    for (;;) {
      if (*p == 'h') {
        len = (len == L_SHORT) ? L_CHAR : L_SHORT;
        p++;
      } else if (*p == 'l') {
        len = (len == L_LONG) ? L_LLONG : L_LONG;
        p++;
      } else if (*p == 'j' || *p == 'q') {
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
    if (conv == 0)
      break;

    // every conversion but these skips leading whitespace first
    if (conv != 'c' && conv != '[' && conv != 'n') {
      int c;
      while ((c = next(in)) != EOF && isspace(c))
        ;
      back(in, c);
    }

    switch (conv) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
      int base = (conv == 'd' || conv == 'u') ? 10 : (conv == 'o' ? 8 : (conv == 'i' ? 0 : 16));
      int n = 0, c;
      if (width > (int)sizeof(buf) - 1)
        width = sizeof(buf) - 1;
      c = next(in);
      if (c == '+' || c == '-') {
        buf[n++] = c;
        c = next(in);
      }
      if ((base == 16 || base == 0) && c == '0') {
        buf[n++] = c;
        c = next(in);
        if (c == 'x' || c == 'X') {
          buf[n++] = c;
          base = 16;
          c = next(in);
        }
      }
      while (c != EOF && n < width) {
        int d;
        if (isdigit(c))
          d = c - '0';
        else if (isalpha(c))
          d = tolower(c) - 'a' + 10;
        else
          break;
        if (base && d >= base)
          break;
        buf[n++] = c;
        c = next(in);
      }
      back(in, c);
      if (n == 0 || (n == 1 && (buf[0] == '+' || buf[0] == '-')))
        return assigned;
      buf[n] = 0;
      if (suppress)
        break;
      if (conv == 'd' || conv == 'i') {
        long v = strtol(buf, 0, base);
        switch (len) {
        case L_CHAR: *va_arg(ap, signed char *) = v; break;
        case L_SHORT: *va_arg(ap, short *) = v; break;
        case L_LONG: case L_SIZE: case L_PTRDIFF: *va_arg(ap, long *) = v; break;
        case L_LLONG: *va_arg(ap, long long *) = v; break;
        default: *va_arg(ap, int *) = v; break;
        }
      } else {
        unsigned long v = strtoul(buf, 0, base);
        if (conv == 'p') {
          *va_arg(ap, void **) = (void *)v;
        } else {
          switch (len) {
          case L_CHAR: *va_arg(ap, unsigned char *) = v; break;
          case L_SHORT: *va_arg(ap, unsigned short *) = v; break;
          case L_LONG: case L_SIZE: case L_PTRDIFF: *va_arg(ap, unsigned long *) = v; break;
          case L_LLONG: *va_arg(ap, unsigned long long *) = v; break;
          default: *va_arg(ap, unsigned *) = v; break;
          }
        }
      }
      assigned++;
      break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
      // collect the longest prefix that looks like a number and let
      // strtod decide where it really ends.
      int n = 0, c;
      if (width > (int)sizeof(buf) - 1)
        width = sizeof(buf) - 1;
      c = next(in);
      while (c != EOF && n < width &&
             (isdigit(c) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E' || c == 'x' ||
              c == 'X' || c == 'p' || c == 'P' || isalpha(c))) {
        buf[n++] = c;
        c = next(in);
      }
      back(in, c);
      buf[n] = 0;
      char *end;
      long double v = strtold(buf, &end);
      if (end == buf)
        return assigned;
      // give back what strtod did not use
      for (const char *q = buf + n; q > end;) {
        q--;
        if (in->f)
          ungetc((unsigned char)*q, in->f);
        else
          in->pos--;
        in->read--;
      }
      if (suppress)
        break;
      switch (len) {
      case L_LDOUBLE: *va_arg(ap, long double *) = v; break;
      case L_LONG: *va_arg(ap, double *) = v; break;
      default: *va_arg(ap, float *) = v; break;
      }
      assigned++;
      break;
    }
    case 'c': {
      int n = width == INT_MAX ? 1 : width;
      char *d = suppress ? 0 : va_arg(ap, char *);
      for (int i = 0; i < n; i++) {
        int c = next(in);
        if (c == EOF)
          return assigned ? assigned : EOF;
        if (d)
          d[i] = c;
      }
      if (d)
        assigned++;
      break;
    }
    case 's': {
      char *d = suppress ? 0 : va_arg(ap, char *);
      int n = 0, c;
      while ((c = next(in)) != EOF && !isspace(c) && n < width) {
        if (d)
          d[n] = c;
        n++;
      }
      back(in, c);
      if (n == 0)
        return assigned ? assigned : EOF;
      if (d) {
        d[n] = 0;
        assigned++;
      }
      break;
    }
    case '[': {
      struct set set;
      p = parse_set(p + 1, &set) - 1;
      char *d = suppress ? 0 : va_arg(ap, char *);
      int n = 0, c;
      while ((c = next(in)) != EOF && inset(&set, c) && n < width) {
        if (d)
          d[n] = c;
        n++;
      }
      back(in, c);
      if (n == 0)
        return assigned;
      if (d) {
        d[n] = 0;
        assigned++;
      }
      break;
    }
    case 'n': {
      if (suppress)
        break;
      switch (len) {
      case L_CHAR: *va_arg(ap, signed char *) = in->read; break;
      case L_SHORT: *va_arg(ap, short *) = in->read; break;
      case L_LONG: case L_SIZE: case L_PTRDIFF: *va_arg(ap, long *) = in->read; break;
      case L_LLONG: *va_arg(ap, long long *) = in->read; break;
      default: *va_arg(ap, int *) = in->read; break;
      }
      break;
    }
    case '%': {
      int c = next(in);
      if (c != '%') {
        back(in, c);
        return assigned;
      }
      break;
    }
    default:
      return assigned;
    }
  }
  return assigned;
}

int
vfscanf(FILE *f, const char *fmt, va_list ap)
{
  struct src in = {.f = f, .back = -1};
  return vsrc(&in, fmt, ap);
}

int
vsscanf(const char *s, const char *fmt, va_list ap)
{
  struct src in = {.s = s, .back = -1};
  return vsrc(&in, fmt, ap);
}

int
vscanf(const char *fmt, va_list ap)
{
  return vfscanf(stdin, fmt, ap);
}

int
fscanf(FILE *f, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vfscanf(f, fmt, ap);
  va_end(ap);
  return n;
}

int
sscanf(const char *s, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsscanf(s, fmt, ap);
  va_end(ap);
  return n;
}

int
scanf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vfscanf(stdin, fmt, ap);
  va_end(ap);
  return n;
}
