//
// <iconv.h>: converting text between the encodings this system knows.
//
// every conversion goes through Unicode code points: the input is
// decoded one character at a time and the code point re-encoded for
// the output. that costs nothing here, since all the encodings are
// Unicode ones or subsets of it, and it means a new encoding is two
// small functions rather than a matrix of pairs.
//
// what a caller can name is UTF-8, UTF-16 and UTF-32 in both byte
// orders, UCS-2, ISO-8859-1 and ASCII, under the usual aliases, with
// case and the punctuation in the names ignored. a "//TRANSLIT" or
// "//IGNORE" suffix is accepted and has no effect, because the
// substitution below is what both of them would do anyway.
//
#include <iconv.h>
#include <wchar.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

enum {
  ENC_UTF8,
  ENC_UTF16LE,
  ENC_UTF16BE,
  ENC_UTF32LE,
  ENC_UTF32BE,
  ENC_UCS2LE,
  ENC_UCS2BE,
  ENC_LATIN1,
  ENC_ASCII,
};

struct iconv_cd {
  unsigned char from, to;
};

// the names, and the encoding each one means. the two arrays are read
// together, so an entry added to one needs an entry in the other.
// plain "UTF-16", "UTF-32" and "UCS-2" are big-endian here: nothing in
// this library writes or reads a byte-order mark.
static const char *const encnames[] = {
  "UTF8",    "UTF16LE", "UTF16BE", "UTF16",  "UTF32LE", "UTF32BE", "UTF32",
  "UCS4",    "UCS4LE",  "UCS4BE",  "UCS2",   "UCS2LE",  "UCS2BE",  "LATIN1",
  "ISO88591", "ISOIR100", "L1",    "CP819",  "ASCII",   "USASCII", "ISO646US",
  "ANSIX3.41968",
};
static const unsigned char encids[] = {
  ENC_UTF8,    ENC_UTF16LE, ENC_UTF16BE, ENC_UTF16BE, ENC_UTF32LE, ENC_UTF32BE, ENC_UTF32BE,
  ENC_UTF32BE, ENC_UTF32LE, ENC_UTF32BE, ENC_UCS2BE,  ENC_UCS2LE,  ENC_UCS2BE,  ENC_LATIN1,
  ENC_LATIN1,  ENC_LATIN1,  ENC_LATIN1,  ENC_LATIN1,  ENC_ASCII,   ENC_ASCII,   ENC_ASCII,
  ENC_ASCII,
};

// the names differ only in case and in where the hyphens fall, so the
// comparison skips both: "utf_8", "UTF-8" and "UTF8" are one name.
static int
nameeq(const char *a, const char *b)
{
  for (;;) {
    while (*a == '-' || *a == '_' || *a == ' ')
      a++;
    while (*b == '-' || *b == '_' || *b == ' ')
      b++;
    if (*a == 0 || *b == 0)
      return *a == *b;
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return 0;
    a++;
    b++;
  }
}

static int
lookup(const char *name)
{
  char buf[32];
  const char *end = strstr(name, "//");
  size_t n = end ? (size_t)(end - name) : strlen(name);

  if (n >= sizeof(buf))
    return -1;
  memcpy(buf, name, n);
  buf[n] = 0;
  for (size_t i = 0; i < sizeof(encids); i++)
    if (nameeq(buf, encnames[i]))
      return encids[i];
  return -1;
}

iconv_t
iconv_open(const char *to, const char *from)
{
  int t = lookup(to), f = lookup(from);

  if (t < 0 || f < 0) {
    errno = EINVAL;
    return (iconv_t)-1;
  }
  struct iconv_cd *cd = malloc(sizeof(*cd));
  if (cd == 0)
    return (iconv_t)-1; // malloc has set ENOMEM
  cd->from = f;
  cd->to = t;
  return cd;
}

int
iconv_close(iconv_t cd)
{
  if (cd == (iconv_t)-1 || cd == 0) {
    errno = EBADF;
    return -1;
  }
  free(cd);
  return 0;
}

static unsigned int
get16(const unsigned char *s, int le)
{
  return le ? s[0] | (s[1] << 8) : (s[0] << 8) | s[1];
}

static void
put16(unsigned char *d, unsigned int v, int le)
{
  d[0] = le ? v & 0xff : v >> 8;
  d[1] = le ? v >> 8 : v & 0xff;
}

// decode one character. the return is the number of bytes it used, or
// 0 when the input stops in the middle of one (EINVAL for the caller)
// and -1 when the bytes are not a character at all (EILSEQ).
static int
decode(int enc, const unsigned char *s, size_t n, unsigned int *cp)
{
  switch (enc) {
  case ENC_UTF8: {
    // the library's own decoder, given a fresh state each time so that
    // nothing of a half-read character is kept behind the caller's
    // back: an unfinished character stays in the caller's buffer.
    mbstate_t st;
    wchar_t wc;
    memset(&st, 0, sizeof(st));
    size_t r = mbrtowc(&wc, (const char *)s, n, &st);
    if (r == (size_t)-2)
      return 0;
    if (r == (size_t)-1)
      return -1;
    *cp = (unsigned int)wc;
    return r == 0 ? 1 : (int)r; // a null byte is a character one byte long
  }
  case ENC_UTF16LE:
  case ENC_UTF16BE: {
    int le = enc == ENC_UTF16LE;
    if (n < 2)
      return 0;
    unsigned int u = get16(s, le);
    if (u >= 0xdc00 && u <= 0xdfff)
      return -1; // a low surrogate with no high one before it
    if (u >= 0xd800 && u <= 0xdbff) {
      if (n < 4)
        return 0;
      unsigned int lo = get16(s + 2, le);
      if (lo < 0xdc00 || lo > 0xdfff)
        return -1;
      *cp = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
      return 4;
    }
    *cp = u;
    return 2;
  }
  case ENC_UCS2LE:
  case ENC_UCS2BE: {
    if (n < 2)
      return 0;
    unsigned int u = get16(s, enc == ENC_UCS2LE);
    if (u >= 0xd800 && u <= 0xdfff)
      return -1; // UCS-2 has no surrogates: the halves are not characters
    *cp = u;
    return 2;
  }
  case ENC_UTF32LE:
  case ENC_UTF32BE: {
    if (n < 4)
      return 0;
    unsigned int v;
    if (enc == ENC_UTF32LE)
      v = s[0] | ((unsigned)s[1] << 8) | ((unsigned)s[2] << 16) | ((unsigned)s[3] << 24);
    else
      v = s[3] | ((unsigned)s[2] << 8) | ((unsigned)s[1] << 16) | ((unsigned)s[0] << 24);
    if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
      return -1;
    *cp = v;
    return 4;
  }
  case ENC_LATIN1:
    if (n < 1)
      return 0;
    *cp = s[0]; // ISO-8859-1 is the first 256 code points, in order
    return 1;
  default: // ENC_ASCII
    if (n < 1)
      return 0;
    if (s[0] > 0x7f)
      return -1;
    *cp = s[0];
    return 1;
  }
}

// encode one code point into at most four bytes. a code point the
// target cannot hold is replaced by a question mark and counted, which
// is what POSIX calls an irreversible conversion.
static int
encode(int enc, unsigned int cp, unsigned char *d, size_t *irrev)
{
  switch (enc) {
  case ENC_UTF8: {
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    return (int)wcrtomb((char *)d, (wchar_t)cp, &st);
  }
  case ENC_UTF16LE:
  case ENC_UTF16BE: {
    int le = enc == ENC_UTF16LE;
    if (cp >= 0x10000) {
      put16(d, 0xd800 + ((cp - 0x10000) >> 10), le);
      put16(d + 2, 0xdc00 + ((cp - 0x10000) & 0x3ff), le);
      return 4;
    }
    put16(d, cp, le);
    return 2;
  }
  case ENC_UCS2LE:
  case ENC_UCS2BE: {
    int le = enc == ENC_UCS2LE;
    if (cp >= 0x10000) {
      (*irrev)++;
      cp = '?';
    }
    put16(d, cp, le);
    return 2;
  }
  case ENC_UTF32LE:
    d[0] = cp;
    d[1] = cp >> 8;
    d[2] = cp >> 16;
    d[3] = cp >> 24;
    return 4;
  case ENC_UTF32BE:
    d[0] = cp >> 24;
    d[1] = cp >> 16;
    d[2] = cp >> 8;
    d[3] = cp;
    return 4;
  case ENC_LATIN1:
    if (cp > 0xff) {
      (*irrev)++;
      cp = '?';
    }
    d[0] = cp;
    return 1;
  default: // ENC_ASCII
    if (cp > 0x7f) {
      (*irrev)++;
      cp = '?';
    }
    d[0] = cp;
    return 1;
  }
}

size_t
iconv(iconv_t cd, char **inbuf, size_t *inleft, char **outbuf, size_t *outleft)
{
  struct iconv_cd *c = cd;
  size_t irrev = 0;

  if (cd == (iconv_t)-1 || cd == 0) {
    errno = EBADF;
    return (size_t)-1;
  }
  // a call with no input, or with nowhere to put the output, is a
  // request to reset the descriptor. no encoding here has a shift
  // state, so there is nothing to do and nothing to write.
  if (inbuf == 0 || *inbuf == 0 || outbuf == 0 || *outbuf == 0)
    return 0;

  while (*inleft > 0) {
    unsigned int cp;
    unsigned char tmp[4];
    int used = decode(c->from, (const unsigned char *)*inbuf, *inleft, &cp);
    if (used == 0) {
      // the input stops part way through a character; the caller must
      // bring the rest, so the pointers stay on its first byte.
      errno = EINVAL;
      return (size_t)-1;
    }
    if (used < 0) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    size_t irr = 0;
    int len = encode(c->to, cp, tmp, &irr);
    if ((size_t)len > *outleft) {
      errno = E2BIG;
      return (size_t)-1;
    }
    memcpy(*outbuf, tmp, len);
    *outbuf += len;
    *outleft -= len;
    *inbuf += used;
    *inleft -= used;
    irrev += irr;
  }
  return irrev;
}
