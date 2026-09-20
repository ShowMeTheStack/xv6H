//
// <fnmatch.h>: shell filename patterns.
//
// the obvious way to write this is one recursive call per '*', which
// costs exponential time on a pattern like "a*a*a*a*b" because every
// star re-tries every split of the text. the loop below matches
// left to right instead and remembers only the most recent star: when
// the rest of the pattern fails, it goes back to that star, lets it
// swallow one more character and tries again. that is enough, because
// a star that has already been passed can only ever need to grow, so
// the work is bounded by (pattern length) * (string length).
//
// everything else here is the flags. FNM_PATHNAME makes a slash a
// character only a slash matches, so no wildcard may cross one;
// FNM_PERIOD makes a period that begins the string -- or a component,
// under FNM_PATHNAME -- something a wildcard may not match either.
//
#include <fnmatch.h>
#include <string.h>
#include <ctype.h>

// is this position one where FNM_PERIOD protects a period?
static int
leading(const char *string, const char *s, int flags)
{
  if (s == string)
    return 1;
  return (flags & FNM_PATHNAME) && s[-1] == '/';
}

static int
chreq(int a, int b, int flags)
{
  if (flags & FNM_CASEFOLD)
    return tolower((unsigned char)a) == tolower((unsigned char)b);
  return (unsigned char)a == (unsigned char)b;
}

// the twelve [:name:] classes, tested the same way regex.c tests them
static int
named_class(const char *name, size_t len, unsigned char c)
{
  static const char *const names[] = {
    "alpha", "upper", "lower", "digit", "xdigit", "alnum",
    "punct", "space", "print", "graph", "cntrl", "blank",
  };
  int i;

  for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    if (strlen(names[i]) == len && memcmp(name, names[i], len) == 0)
      break;
  switch (i) {
  case 0: return isalpha(c);
  case 1: return isupper(c);
  case 2: return islower(c);
  case 3: return isdigit(c);
  case 4: return isxdigit(c);
  case 5: return isalnum(c);
  case 6: return ispunct(c);
  case 7: return isspace(c);
  case 8: return isprint(c);
  case 9: return isgraph(c);
  case 10: return iscntrl(c);
  case 11: return isblank(c);
  }
  return 0;
}

// a bracket expression starting at p. returns where it ends, with *ok
// saying whether c is in it, or 0 when the '[' closes nothing and is
// therefore an ordinary character.
static const char *
bracket(const char *p, unsigned char c, int flags, int *ok)
{
  const char *q = p + 1;
  int neg = 0, found = 0, first = 1;

  if (*q == '!' || *q == '^') {
    neg = 1;
    q++;
  }
  for (;;) {
    if (*q == 0)
      return 0;
    if (*q == ']' && !first)
      break;
    first = 0;

    if (q[0] == '[' && q[1] == ':') {
      const char *e = strstr(q + 2, ":]");
      if (e == 0)
        return 0;
      if (named_class(q + 2, e - (q + 2), c))
        found = 1;
      q = e + 2;
      continue;
    }
    if (*q == '\\' && !(flags & FNM_NOESCAPE) && q[1])
      q++;
    unsigned char lo = *q++;
    if (*q == '-' && q[1] != ']' && q[1] != 0) {
      q++;
      if (*q == '\\' && !(flags & FNM_NOESCAPE) && q[1])
        q++;
      unsigned char hi = *q++;
      if (flags & FNM_CASEFOLD) {
        unsigned char l = tolower(c), u = toupper(c);
        if ((lo <= l && l <= hi) || (lo <= u && u <= hi))
          found = 1;
      } else if (lo <= c && c <= hi) {
        found = 1;
      }
    } else if (chreq(lo, c, flags)) {
      found = 1;
    }
  }
  *ok = neg ? !found : found;
  return q + 1;
}

// one pattern item against one character: 1 matched, 0 did not, -1 the
// pattern may not look at this character at all because FNM_PERIOD
// protects it. *pp is left after the item on a match.
static int
single(const char **pp, const char *s, const char *string, int flags)
{
  const char *p = *pp;
  unsigned char c = (unsigned char)*s;

  switch (*p) {
  case '?':
    if ((flags & FNM_PATHNAME) && c == '/')
      return 0;
    if ((flags & FNM_PERIOD) && c == '.' && leading(string, s, flags))
      return -1;
    *pp = p + 1;
    return 1;
  case '[': {
    int ok;
    const char *q;
    if ((flags & FNM_PATHNAME) && c == '/')
      return 0;
    if ((flags & FNM_PERIOD) && c == '.' && leading(string, s, flags))
      return -1;
    q = bracket(p, c, flags, &ok);
    if (q == 0)
      break;   // nothing closed it, so it is a plain '['
    *pp = q;
    return ok;
  }
  case '\\':
    if (!(flags & FNM_NOESCAPE) && p[1]) {
      *pp = p + 2;
      return chreq(p[1], c, flags);
    }
    break;
  }
  *pp = p + 1;
  return chreq(*p, c, flags);
}

int
fnmatch(const char *pattern, const char *string, int flags)
{
  const char *p = pattern, *s = string;
  const char *star_p = 0, *star_s = 0;

  for (;;) {
    if (*p == 0) {
      if (*s == 0)
        return 0;
      // FNM_LEADING_DIR: the pattern matched a whole directory prefix
      if ((flags & FNM_LEADING_DIR) && *s == '/')
        return 0;
      goto backtrack;
    }
    if (*p == '*') {
      // a star may never swallow a protected period
      if ((flags & FNM_PERIOD) && *s == '.' && leading(string, s, flags))
        return FNM_NOMATCH;
      while (*p == '*')
        p++;
      star_p = p;
      star_s = s;
      continue;
    }
    if (*s) {
      int r = single(&p, s, string, flags);
      if (r < 0)
        return FNM_NOMATCH;
      if (r) {
        s++;
        continue;
      }
    }
  backtrack:
    // the rest of the pattern did not fit: hand one more character to
    // the last star, if there was one and it is allowed to have it.
    if (star_p == 0 || *star_s == 0)
      return FNM_NOMATCH;
    if ((flags & FNM_PATHNAME) && *star_s == '/')
      return FNM_NOMATCH;
    star_s++;
    s = star_s;
    p = star_p;
  }
}
