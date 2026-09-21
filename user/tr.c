// tr: replace, squeeze or delete characters.
//
//   tr SET1 SET2    tr -d SET1    tr -s SET1 [SET2]    tr -c ...
//
// a set is characters, A-Z ranges, and the classes [:alpha:],
// [:digit:], [:space:], [:upper:], [:lower:], [:punct:], [:alnum:].
#include "user/user.h"

static unsigned char map[256];
static unsigned char inset1[256], inset2[256];
static char set2chars[256];
static int nset2;

static void
add_class(unsigned char *set, const char *name, int len)
{
  for (int c = 0; c < 256; c++) {
    int in = 0;
    if (strncmp(name, "alpha", len) == 0)
      in = isalpha(c);
    else if (strncmp(name, "digit", len) == 0)
      in = isdigit(c);
    else if (strncmp(name, "alnum", len) == 0)
      in = isalnum(c);
    else if (strncmp(name, "space", len) == 0)
      in = isspace(c);
    else if (strncmp(name, "upper", len) == 0)
      in = isupper(c);
    else if (strncmp(name, "lower", len) == 0)
      in = islower(c);
    else if (strncmp(name, "punct", len) == 0)
      in = ispunct(c);
    else if (strncmp(name, "print", len) == 0)
      in = isprint(c);
    if (in)
      set[c] = 1;
  }
}

// expand a set into `set` (a membership table) and, for the second
// set, into an ordered list so that SET1[i] maps to SET2[i]
static void
expand(const char *spec, unsigned char *set, char *ordered, int *nordered)
{
  const char *p = spec;

  while (*p) {
    if (p[0] == '[' && p[1] == ':') {
      const char *end = strstr(p + 2, ":]");
      if (end) {
        add_class(set, p + 2, (int)(end - (p + 2)));
        p = end + 2;
        continue;
      }
    }
    unsigned char c = (unsigned char)*p;
    if (c == '\\' && p[1]) {
      p++;
      switch (*p) {
      case 'n': c = '\n'; break;
      case 't': c = '\t'; break;
      case 'r': c = '\r'; break;
      case 'a': c = '\a'; break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'v': c = '\v'; break;
      case '\\': c = '\\'; break;
      case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        // an octal byte: \0 is the zero byte that -print0 writes
        int v = 0, n = 0;
        while (n < 3 && *p >= '0' && *p <= '7') {
          v = v * 8 + (*p++ - '0');
          n++;
        }
        p--;
        c = (unsigned char)v;
        break;
      }
      default: c = (unsigned char)*p; break;
      }
    }
    if (p[1] == '-' && p[2] && p[2] != ']') {
      unsigned char to = (unsigned char)p[2];
      for (int k = c; k <= to; k++) {
        set[k] = 1;
        if (ordered && *nordered < 256)
          ordered[(*nordered)++] = (char)k;
      }
      p += 3;
      continue;
    }
    set[c] = 1;
    if (ordered && *nordered < 256)
      ordered[(*nordered)++] = (char)c;
    p++;
  }
}

int
main(int argc, char *argv[])
{
  int del = 0, squeeze = 0, complement = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *p = argv[i] + 1; *p; p++)
      switch (*p) {
      case 'd': del = 1; break;
      case 's': squeeze = 1; break;
      case 'c': case 'C': complement = 1; break;
      default:
        fprintf(stderr, "usage: tr [-dsc] SET1 [SET2]\n");
        exit(1);
      }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: tr [-dsc] SET1 [SET2]\n");
    exit(1);
  }
  char ordered1[256];
  int n1 = 0;
  expand(argv[i], inset1, ordered1, &n1);
  if (complement)
    for (int c = 0; c < 256; c++)
      inset1[c] = !inset1[c];
  if (i + 1 < argc)
    expand(argv[i + 1], inset2, set2chars, &nset2);

  for (int c = 0; c < 256; c++)
    map[c] = (unsigned char)c;
  if (nset2 > 0) {
    // SET1 to SET2, the last character of SET2 repeated if it is shorter
    int k = 0;
    for (int c = 0; c < 256; c++)
      if (inset1[c]) {
        map[c] = (unsigned char)set2chars[k < nset2 ? k : nset2 - 1];
        k++;
      }
  }

  int c, last = -1;
  while ((c = getchar()) != EOF) {
    unsigned char u = (unsigned char)c;
    if (del && inset1[u])
      continue;
    unsigned char out = (nset2 > 0 && inset1[u]) ? map[u] : u;
    if (squeeze && last >= 0 && out == (unsigned char)last &&
        (nset2 > 0 ? inset2[out] : inset1[out]))
      continue;
    putchar(out);
    last = out;
  }
  exit(0);
}
