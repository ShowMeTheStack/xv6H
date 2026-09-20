//
// <wchar.h>: the UTF-8 conversions and the wide string functions.
//
// the only locale is C.UTF-8, so "multibyte" means UTF-8 here and the
// conversions have no table to consult. an mbstate_t exists for one
// reason: a program reading a file in blocks will hand us a character
// split across two buffers, and the bytes seen so far have to wait
// somewhere until the rest of them arrive.
//
// the wide string functions are the byte ones with a wider element and
// no word-at-a-time tricks: a wchar_t is already the machine's natural
// size, so there is nothing to gain by reading several at once.
//
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h> // EOF, which btowc and wctob are defined in terms of
#include <errno.h>

// ---- UTF-8 ----

// the smallest code point each sequence length may encode. a value
// below its own minimum came from an overlong form, which has to be
// refused: accepting it would let "\xc0\x80" smuggle a null byte past
// a check that looked at the bytes.
static const unsigned int mincode[5] = {0, 0, 0x80, 0x800, 0x10000};

// a code point is a scalar value: the surrogate halves exist only
// inside UTF-16 and never appear as characters.
static int
badcode(unsigned int v)
{
  return v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff);
}

// the encoder, shared by wcrtomb and wcsrtombs; returns the length or
// -1 without touching errno, which the callers set.
static int
utf8enc(char *d, wchar_t wc)
{
  unsigned int v = (unsigned int)wc;

  if (wc < 0 || badcode(v))
    return -1;
  if (v < 0x80) {
    d[0] = v;
    return 1;
  }
  if (v < 0x800) {
    d[0] = 0xc0 | (v >> 6);
    d[1] = 0x80 | (v & 0x3f);
    return 2;
  }
  if (v < 0x10000) {
    d[0] = 0xe0 | (v >> 12);
    d[1] = 0x80 | ((v >> 6) & 0x3f);
    d[2] = 0x80 | (v & 0x3f);
    return 3;
  }
  d[0] = 0xf0 | (v >> 18);
  d[1] = 0x80 | ((v >> 12) & 0x3f);
  d[2] = 0x80 | ((v >> 6) & 0x3f);
  d[3] = 0x80 | (v & 0x3f);
  return 4;
}

size_t
mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *st)
{
  static mbstate_t internal;
  size_t used = 0;

  if (st == 0)
    st = &internal;
  if (s == 0) {
    // POSIX makes a null pointer a question about the state, answered
    // by converting a single null byte.
    s = "";
    n = 1;
    wc = 0;
  }

  if (st->want == 0) {
    if (n == 0)
      return (size_t)-2;
    unsigned char c = s[0];
    if (c < 0x80) {
      if (wc)
        *wc = c;
      return c ? 1 : 0;
    }
    // 0xc0 and 0xc1 could only begin an overlong two-byte form, and no
    // byte above 0xf4 can begin a code point below 0x110000.
    if (c < 0xc2 || c > 0xf4) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    st->buf[0] = c;
    st->have = 1;
    st->want = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    used = 1;
  }

  while (st->have < st->want) {
    if (used == n)
      return (size_t)-2; // the character is split: keep the state and wait
    unsigned char c = s[used];
    if ((c & 0xc0) != 0x80) {
      st->want = st->have = 0;
      errno = EILSEQ;
      return (size_t)-1;
    }
    st->buf[st->have++] = c;
    used++;
  }

  int len = st->want;
  unsigned int v = st->buf[0] & (0xff >> (len + 1));
  for (int i = 1; i < len; i++)
    v = (v << 6) | (st->buf[i] & 0x3f);
  st->want = st->have = 0;
  // the overlong and surrogate checks wait until the value is whole,
  // which costs one or two extra bytes of input on a bad sequence and
  // saves the decoder a second set of special cases.
  if (v < mincode[len] || badcode(v)) {
    errno = EILSEQ;
    return (size_t)-1;
  }
  if (wc)
    *wc = v;
  return used;
}

size_t
mbrlen(const char *s, size_t n, mbstate_t *st)
{
  static mbstate_t internal;
  return mbrtowc(0, s, n, st ? st : &internal);
}

int
mbsinit(const mbstate_t *st)
{
  return st == 0 || st->want == 0;
}

size_t
wcrtomb(char *s, wchar_t wc, mbstate_t *st)
{
  char tmp[4];
  int len;

  (void)st; // UTF-8 has no shift state to return from
  if (s == 0) {
    // "return to the initial state", which for UTF-8 is a null byte
    s = tmp;
    wc = 0;
  }
  if ((len = utf8enc(s, wc)) < 0) {
    errno = EILSEQ;
    return (size_t)-1;
  }
  return len;
}

// how many bytes of a null-terminated string the decoder may look at:
// never past the null byte, which it must see rather than read around.
static size_t
avail(const char *s)
{
  size_t k = 0;

  while (k < 4 && s[k])
    k++;
  return k < 4 ? k + 1 : 4;
}

size_t
mbsrtowcs(wchar_t *d, const char **src, size_t n, mbstate_t *st)
{
  static mbstate_t internal;
  const char *s = *src;
  size_t i = 0;

  if (st == 0)
    st = &internal;
  while (d == 0 || i < n) {
    wchar_t wc;
    size_t r = mbrtowc(&wc, s, avail(s), st);
    if (r == (size_t)-1 || r == (size_t)-2) {
      // a sequence cut short by the end of the string is as bad as an
      // illegal one, because nothing more is coming.
      errno = EILSEQ;
      if (d)
        *src = s;
      return (size_t)-1;
    }
    if (d)
      d[i] = wc;
    if (wc == 0) {
      if (d)
        *src = 0;
      return i; // the terminator is not counted
    }
    s += r;
    i++;
  }
  if (d)
    *src = s;
  return i;
}

size_t
wcsrtombs(char *d, const wchar_t **src, size_t n, mbstate_t *st)
{
  const wchar_t *s = *src;
  size_t used = 0;
  char tmp[4];

  (void)st;
  for (;;) {
    int len = utf8enc(tmp, *s);
    if (len < 0) {
      errno = EILSEQ;
      if (d)
        *src = s;
      return (size_t)-1;
    }
    // a character that does not fit whole is not written at all
    if (d && used + (size_t)len > n)
      break;
    if (d)
      memcpy(d + used, tmp, len);
    if (*s == 0) {
      if (d)
        *src = 0;
      return used;
    }
    used += len;
    s++;
  }
  *src = s;
  return used;
}

wint_t
btowc(int c)
{
  // above ASCII a single byte is never a whole UTF-8 character.
  if (c == EOF || (unsigned)c > 0x7f)
    return WEOF;
  return c;
}

int
wctob(wint_t c)
{
  return c < 0x80 ? (int)c : EOF;
}

// ---- wide strings ----

size_t
wcslen(const wchar_t *s)
{
  const wchar_t *p = s;

  while (*p)
    p++;
  return p - s;
}

size_t
wcsnlen(const wchar_t *s, size_t n)
{
  size_t i = 0;

  while (i < n && s[i])
    i++;
  return i;
}

wchar_t *
wcscpy(wchar_t *d, const wchar_t *s)
{
  wchar_t *r = d;

  while ((*d++ = *s++) != 0)
    ;
  return r;
}

wchar_t *
wcpcpy(wchar_t *d, const wchar_t *s)
{
  while ((*d = *s++) != 0)
    d++;
  return d;
}

wchar_t *
wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
  wchar_t *r = d;

  while (n && *s) {
    *d++ = *s++;
    n--;
  }
  while (n--) // the standard pads the rest with nulls
    *d++ = 0;
  return r;
}

wchar_t *
wcscat(wchar_t *d, const wchar_t *s)
{
  wcscpy(d + wcslen(d), s);
  return d;
}

wchar_t *
wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
  wchar_t *p = d + wcslen(d);

  while (n-- && *s)
    *p++ = *s++;
  *p = 0;
  return d;
}

int
wcscmp(const wchar_t *a, const wchar_t *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  // the difference of two code points can overflow an int, so the
  // comparison yields the sign directly.
  return (*a > *b) - (*a < *b);
}

int
wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
  if (n == 0)
    return 0;
  while (--n && *a && *a == *b) {
    a++;
    b++;
  }
  return (*a > *b) - (*a < *b);
}

// without other locales, collation is comparison.
int
wcscoll(const wchar_t *a, const wchar_t *b)
{
  return wcscmp(a, b);
}

int
wcscasecmp(const wchar_t *a, const wchar_t *b)
{
  wchar_t x, y;

  for (;;) {
    x = towlower(*a);
    y = towlower(*b);
    if (x == 0 || x != y)
      break;
    a++;
    b++;
  }
  return (x > y) - (x < y);
}

int
wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
  wchar_t x = 0, y = 0;

  while (n--) {
    x = towlower(*a);
    y = towlower(*b);
    if (x == 0 || x != y)
      break;
    a++;
    b++;
  }
  return (x > y) - (x < y);
}

wchar_t *
wcschr(const wchar_t *s, wchar_t c)
{
  for (;; s++) {
    if (*s == c)
      return (wchar_t *)s;
    if (*s == 0)
      return 0;
  }
}

wchar_t *
wcsrchr(const wchar_t *s, wchar_t c)
{
  const wchar_t *last = 0;

  for (;; s++) {
    if (*s == c)
      last = s;
    if (*s == 0)
      return (wchar_t *)last;
  }
}

wchar_t *
wcsstr(const wchar_t *h, const wchar_t *n)
{
  size_t nl = wcslen(n);

  if (nl == 0)
    return (wchar_t *)h;
  for (; *h; h++)
    if (*h == *n && wcsncmp(h, n, nl) == 0)
      return (wchar_t *)h;
  return 0;
}

// the wide sets are small and unsorted, so the span functions do the
// obvious linear scan rather than the bitmap the byte versions use: a
// 0x110000-bit set is not worth building for a handful of characters.
static int
inset(const wchar_t *set, wchar_t c)
{
  for (; *set; set++)
    if (*set == c)
      return 1;
  return 0;
}

size_t
wcsspn(const wchar_t *s, const wchar_t *set)
{
  const wchar_t *p = s;

  while (*p && inset(set, *p))
    p++;
  return p - s;
}

size_t
wcscspn(const wchar_t *s, const wchar_t *set)
{
  const wchar_t *p = s;

  while (*p && !inset(set, *p))
    p++;
  return p - s;
}

wchar_t *
wcspbrk(const wchar_t *s, const wchar_t *set)
{
  s += wcscspn(s, set);
  return *s ? (wchar_t *)s : 0;
}

wchar_t *
wcstok(wchar_t *s, const wchar_t *sep, wchar_t **save)
{
  if (s == 0)
    s = *save;
  if (s == 0)
    return 0;
  s += wcsspn(s, sep);
  if (*s == 0) {
    *save = s;
    return 0;
  }
  wchar_t *end = s + wcscspn(s, sep);
  if (*end) {
    *end = 0;
    *save = end + 1;
  } else {
    *save = end;
  }
  return s;
}

wchar_t *
wcsdup(const wchar_t *s)
{
  size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
  wchar_t *p = malloc(n);

  if (p)
    memcpy(p, s, n);
  return p;
}

wchar_t *
wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{
  return memcpy(d, s, n * sizeof(wchar_t));
}

wchar_t *
wmemmove(wchar_t *d, const wchar_t *s, size_t n)
{
  return memmove(d, s, n * sizeof(wchar_t));
}

wchar_t *
wmemset(wchar_t *d, wchar_t c, size_t n)
{
  for (size_t i = 0; i < n; i++)
    d[i] = c;
  return d;
}

int
wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return (a[i] > b[i]) - (a[i] < b[i]);
  return 0;
}

wchar_t *
wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (s[i] == c)
      return (wchar_t *)(s + i);
  return 0;
}

// ---- how much room a character takes on a terminal ----

struct range {
  unsigned int lo, hi;
};

static int
inranges(const struct range *t, size_t n, unsigned int c)
{
  size_t lo = 0, hi = n;

  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c < t[mid].lo)
      hi = mid;
    else if (c > t[mid].hi)
      lo = mid + 1;
    else
      return 1;
  }
  return 0;
}

// the marks that attach to the previous character and the formatting
// characters that are never drawn at all. this is the common part of
// the Unicode Mf/Mn and Cf categories, not all of it: a full table is
// thousands of ranges and would dwarf the rest of this file.
static const struct range zerowidth[] = {
  {0x0300, 0x036f}, {0x0483, 0x0489}, {0x0591, 0x05bd}, {0x05bf, 0x05bf},
  {0x05c1, 0x05c2}, {0x05c4, 0x05c5}, {0x05c7, 0x05c7}, {0x0610, 0x061a},
  {0x064b, 0x065f}, {0x0670, 0x0670}, {0x06d6, 0x06dc}, {0x06df, 0x06e4},
  {0x06e7, 0x06e8}, {0x06ea, 0x06ed}, {0x0711, 0x0711}, {0x0730, 0x074a},
  {0x07a6, 0x07b0}, {0x07eb, 0x07f3}, {0x0816, 0x0819}, {0x081b, 0x0823},
  {0x0825, 0x0827}, {0x0829, 0x082d}, {0x0900, 0x0902}, {0x093c, 0x093c},
  {0x0941, 0x0948}, {0x094d, 0x094d}, {0x0951, 0x0957}, {0x0962, 0x0963},
  {0x0e31, 0x0e31}, {0x0e34, 0x0e3a}, {0x0e47, 0x0e4e}, {0x1160, 0x11ff},
  {0x135d, 0x135f}, {0x1ab0, 0x1aff}, {0x1dc0, 0x1dff}, {0x200b, 0x200f},
  {0x2028, 0x202e}, {0x2060, 0x2064}, {0x20d0, 0x20f0}, {0x2cef, 0x2cf1},
  {0xa66f, 0xa672}, {0xa674, 0xa67d}, {0xfe00, 0xfe0f}, {0xfe20, 0xfe2f},
  {0xfeff, 0xfeff}, {0xfff9, 0xfffb}, {0x101fd, 0x101fd}, {0x10376, 0x1037a},
  {0x1d167, 0x1d169}, {0x1d17b, 0x1d182}, {0x1d185, 0x1d18b},
  {0x1d1aa, 0x1d1ad}, {0x1d242, 0x1d244}, {0xe0100, 0xe01ef},
};

// the East Asian Wide and Fullwidth ranges, which a terminal draws in
// two cells. the emoji blocks are here too, for the same reason.
static const struct range widechar[] = {
  {0x1100, 0x115f}, {0x2329, 0x232a}, {0x2e80, 0x303e}, {0x3041, 0x33ff},
  {0x3400, 0x4dbf}, {0x4e00, 0x9fff}, {0xa000, 0xa4cf}, {0xa960, 0xa97f},
  {0xac00, 0xd7a3}, {0xf900, 0xfaff}, {0xfe10, 0xfe19}, {0xfe30, 0xfe6f},
  {0xff00, 0xff60}, {0xffe0, 0xffe6}, {0x16fe0, 0x16fe4}, {0x17000, 0x18d08},
  {0x1b000, 0x1b16f}, {0x1f300, 0x1f64f}, {0x1f900, 0x1f9ff},
  {0x20000, 0x2fffd}, {0x30000, 0x3fffd},
};

int
wcwidth(wchar_t c)
{
  unsigned int v = (unsigned int)c;

  if (c == 0)
    return 0; // the null wide character takes no room, but is printable
  if (c < 0 || badcode(v))
    return -1;
  // the control characters have no printed width at all, which is a
  // different answer from "takes zero columns".
  if (v < 0x20 || (v >= 0x7f && v < 0xa0))
    return -1;
  if (inranges(zerowidth, sizeof(zerowidth) / sizeof(zerowidth[0]), v))
    return 0;
  if (inranges(widechar, sizeof(widechar) / sizeof(widechar[0]), v))
    return 2;
  return 1;
}

int
wcswidth(const wchar_t *s, size_t n)
{
  int total = 0;

  for (; n && *s; n--, s++) {
    int w = wcwidth(*s);
    if (w < 0)
      return -1;
    total += w;
  }
  return total;
}
