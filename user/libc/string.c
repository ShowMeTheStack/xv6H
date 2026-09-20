//
// <string.h> and <strings.h>.
//
// the byte-at-a-time loops are what a freestanding target can always
// rely on; where it pays, the word-at-a-time versions below move eight
// bytes at once and find a zero byte with the usual bit trick, which
// matters because every program here copies and compares strings far
// more than it does anything else.
//
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>

#define WORD unsigned long
#define WSIZE ((size_t)sizeof(WORD))
#define ONES  ((WORD)-1 / 255)          // 0x0101...01
#define HIGHS (ONES * 128)              // 0x8080...80
#define HASZERO(w) (((w) - ONES) & ~(w) & HIGHS)

void *
memcpy(void *dst, const void *src, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  // the aligned middle moves a word at a time; the ends byte by byte.
  if (n >= WSIZE && ((uintptr_t)d % WSIZE) == ((uintptr_t)s % WSIZE)) {
    while ((uintptr_t)d % WSIZE) {
      *d++ = *s++;
      n--;
    }
    while (n >= WSIZE) {
      *(WORD *)d = *(const WORD *)s;
      d += WSIZE;
      s += WSIZE;
      n -= WSIZE;
    }
  }
  while (n--)
    *d++ = *s++;
  return dst;
}

void *
mempcpy(void *dst, const void *src, size_t n)
{
  return (char *)memcpy(dst, src, n) + n;
}

void *
memmove(void *dst, const void *src, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  if (d == s || n == 0)
    return dst;
  if (d < s)
    return memcpy(dst, src, n);

  // overlapping and moving up: copy backwards.
  d += n;
  s += n;
  if (n >= WSIZE && ((uintptr_t)d % WSIZE) == ((uintptr_t)s % WSIZE)) {
    while ((uintptr_t)d % WSIZE) {
      *--d = *--s;
      n--;
    }
    while (n >= WSIZE) {
      d -= WSIZE;
      s -= WSIZE;
      n -= WSIZE;
      *(WORD *)d = *(const WORD *)s;
    }
  }
  while (n--)
    *--d = *--s;
  return dst;
}

void *
memset(void *dst, int c, size_t n)
{
  unsigned char *d = dst;
  WORD w = (unsigned char)c * ONES;

  if (n >= WSIZE) {
    while ((uintptr_t)d % WSIZE) {
      *d++ = c;
      n--;
    }
    while (n >= WSIZE) {
      *(WORD *)d = w;
      d += WSIZE;
      n -= WSIZE;
    }
  }
  while (n--)
    *d++ = c;
  return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
  const unsigned char *x = a, *y = b;

  for (; n && *x == *y; n--, x++, y++)
    ;
  return n ? *x - *y : 0;
}

void *
memchr(const void *s, int c, size_t n)
{
  const unsigned char *p = s;
  unsigned char ch = c;

  for (; n; n--, p++)
    if (*p == ch)
      return (void *)p;
  return 0;
}

void *
memrchr(const void *s, int c, size_t n)
{
  const unsigned char *p = (const unsigned char *)s + n;
  unsigned char ch = c;

  while (n--)
    if (*--p == ch)
      return (void *)p;
  return 0;
}

void *
memmem(const void *h, size_t hn, const void *n, size_t nn)
{
  const unsigned char *hp = h;

  if (nn == 0)
    return (void *)h;
  if (nn > hn)
    return 0;
  for (size_t i = 0; i + nn <= hn; i++)
    if (hp[i] == *(const unsigned char *)n && memcmp(hp + i, n, nn) == 0)
      return (void *)(hp + i);
  return 0;
}

void *
memccpy(void *dst, const void *src, int c, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  while (n--) {
    if ((*d++ = *s++) == (unsigned char)c)
      return d;
  }
  return 0;
}

size_t
strlen(const char *s)
{
  const char *p = s;
  const WORD *w;

  for (; (uintptr_t)p % WSIZE; p++)
    if (!*p)
      return p - s;
  for (w = (const void *)p; !HASZERO(*w); w++)
    ;
  for (p = (const void *)w; *p; p++)
    ;
  return p - s;
}

size_t
strnlen(const char *s, size_t n)
{
  const char *p = memchr(s, 0, n);
  return p ? (size_t)(p - s) : n;
}

char *
strcpy(char *d, const char *s)
{
  char *r = d;
  while ((*d++ = *s++) != 0)
    ;
  return r;
}

char *
strncpy(char *d, const char *s, size_t n)
{
  char *r = d;
  while (n && *s) {
    *d++ = *s++;
    n--;
  }
  while (n--) // the standard pads the rest with zeroes
    *d++ = 0;
  return r;
}

// the OpenBSD form: always terminates, returns the length it wanted.
size_t
strlcpy(char *d, const char *s, size_t n)
{
  size_t len = strlen(s);

  if (n) {
    size_t copy = len < n - 1 ? len : n - 1;
    memcpy(d, s, copy);
    d[copy] = 0;
  }
  return len;
}

char *
strcat(char *d, const char *s)
{
  strcpy(d + strlen(d), s);
  return d;
}

char *
strncat(char *d, const char *s, size_t n)
{
  char *p = d + strlen(d);

  while (n-- && *s)
    *p++ = *s++;
  *p = 0;
  return d;
}

size_t
strlcat(char *d, const char *s, size_t n)
{
  size_t dl = strnlen(d, n);

  if (dl == n)
    return dl + strlen(s);
  return dl + strlcpy(d + dl, s, n - dl);
}

int
strcmp(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
  if (n == 0)
    return 0;
  while (--n && *a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

// without other locales, collation is comparison.
int
strcoll(const char *a, const char *b)
{
  return strcmp(a, b);
}

size_t
strxfrm(char *d, const char *s, size_t n)
{
  size_t len = strlen(s);

  if (len < n)
    strcpy(d, s);
  return len;
}

int
strcasecmp(const char *a, const char *b)
{
  while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
    a++;
    b++;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int
strncasecmp(const char *a, const char *b, size_t n)
{
  if (n == 0)
    return 0;
  while (--n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
    a++;
    b++;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *
strchr(const char *s, int c)
{
  char ch = c;

  for (;; s++) {
    if (*s == ch)
      return (char *)s;
    if (!*s)
      return 0;
  }
}

char *
strchrnul(const char *s, int c)
{
  char *p = strchr(s, c);
  return p ? p : (char *)s + strlen(s);
}

char *
strrchr(const char *s, int c)
{
  return memrchr(s, c, strlen(s) + 1);
}

char *
index(const char *s, int c)
{
  return strchr(s, c);
}

char *
rindex(const char *s, int c)
{
  return strrchr(s, c);
}

// a 256-bit set of the characters in `set`, so the span functions do
// one pass over it and then one pass over the string.
static void
charset(const char *set, WORD *bits)
{
  bits[0] = bits[1] = bits[2] = bits[3] = 0;
  for (; *set; set++) {
    unsigned char c = *set;
    bits[c / 64] |= 1UL << (c % 64);
  }
}

#define INSET(bits, c) ((bits)[(unsigned char)(c) / 64] >> ((unsigned char)(c) % 64) & 1)

size_t
strspn(const char *s, const char *set)
{
  WORD bits[4];
  const char *p = s;

  charset(set, bits);
  while (*p && INSET(bits, *p))
    p++;
  return p - s;
}

size_t
strcspn(const char *s, const char *set)
{
  WORD bits[4];
  const char *p = s;

  charset(set, bits);
  while (*p && !INSET(bits, *p))
    p++;
  return p - s;
}

char *
strpbrk(const char *s, const char *set)
{
  s += strcspn(s, set);
  return *s ? (char *)s : 0;
}

char *
strstr(const char *h, const char *n)
{
  size_t nl = strlen(n);

  if (nl == 0)
    return (char *)h;
  for (; *h; h++)
    if (*h == *n && strncmp(h, n, nl) == 0)
      return (char *)h;
  return 0;
}

char *
strcasestr(const char *h, const char *n)
{
  size_t nl = strlen(n);

  if (nl == 0)
    return (char *)h;
  for (; *h; h++)
    if (strncasecmp(h, n, nl) == 0)
      return (char *)h;
  return 0;
}

char *
strtok_r(char *s, const char *sep, char **save)
{
  if (s == 0)
    s = *save;
  s += strspn(s, sep);
  if (*s == 0) {
    *save = s;
    return 0;
  }
  char *end = s + strcspn(s, sep);
  if (*end) {
    *end = 0;
    *save = end + 1;
  } else {
    *save = end;
  }
  return s;
}

char *
strtok(char *s, const char *sep)
{
  static char *save;
  return strtok_r(s, sep, &save);
}

// unlike strtok, this one reports empty fields.
char *
strsep(char **sp, const char *sep)
{
  char *s = *sp, *end;

  if (s == 0)
    return 0;
  end = s + strcspn(s, sep);
  if (*end)
    *end++ = 0;
  else
    end = 0;
  *sp = end;
  return s;
}

char *
strdup(const char *s)
{
  size_t n = strlen(s) + 1;
  char *p = malloc(n);

  if (p)
    memcpy(p, s, n);
  return p;
}

char *
strndup(const char *s, size_t n)
{
  size_t len = strnlen(s, n);
  char *p = malloc(len + 1);

  if (p) {
    memcpy(p, s, len);
    p[len] = 0;
  }
  return p;
}

void
bzero(void *d, size_t n)
{
  memset(d, 0, n);
}

void
bcopy(const void *s, void *d, size_t n)
{
  memmove(d, s, n);
}

int
bcmp(const void *a, const void *b, size_t n)
{
  return memcmp(a, b, n);
}

int
ffs(int v)
{
  return v ? __builtin_ctz(v) + 1 : 0;
}

int
ffsl(long v)
{
  return v ? __builtin_ctzl(v) + 1 : 0;
}

int
ffsll(long long v)
{
  return v ? __builtin_ctzll(v) + 1 : 0;
}
