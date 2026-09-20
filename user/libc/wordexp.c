//
// <wordexp.h>: the expansions a shell performs on a word list, minus
// the one that would need a shell.
//
// the string is read once, left to right, building a word as it goes:
// whitespace ends a word, quotes and backslashes turn the next
// characters into ordinary ones, and $NAME or ${NAME} is replaced by
// the environment's value for it. a leading ~ becomes $HOME. what
// comes out is then handed to glob(), so that a word holding a
// metacharacter turns into the names it matches and a word holding
// none stays as it is.
//
// two buffers are built for every word, not one: the word itself, and
// the same word with every character that came from a quote, a
// backslash or a variable put behind a backslash. the second is what
// glob() sees, which is how "*" expands and "'*'" does not.
//
// command substitution is not implemented at all: `...` and $(...)
// both fail with WRDE_CMDSUB, whether or not WRDE_NOCMD was given.
// running a command is the one expansion that cannot be undone if the
// caller did not mean it, so this library would rather refuse.
//
#include <wordexp.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct wbuf {
  char *s;
  size_t n, cap;
};

struct wctx {
  struct wbuf lit;   // the word
  struct wbuf pat;   // the word as a glob pattern
  int have;          // something has begun this word, even if it is empty
  int magic;         // an unquoted metacharacter is in it
  char **v;
  size_t n, cap;
  int flags;
};

static int
wb_put(struct wbuf *b, int c)
{
  if (b->n + 1 >= b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 32;
    char *ns = realloc(b->s, nc);
    if (ns == 0)
      return -1;
    b->s = ns;
    b->cap = nc;
  }
  b->s[b->n++] = c;
  b->s[b->n] = 0;
  return 0;
}

// `quoted` means the character is to be taken literally, so glob must
// not see it as a metacharacter.
static int
put(struct wctx *c, int ch, int quoted)
{
  if (wb_put(&c->lit, ch) < 0)
    return -1;
  if (quoted) {
    if (ch == '*' || ch == '?' || ch == '[' || ch == '\\')
      if (wb_put(&c->pat, '\\') < 0)
        return -1;
  } else if (ch == '*' || ch == '?' || ch == '[') {
    c->magic = 1;
  }
  return wb_put(&c->pat, ch);
}

static int
add_word(struct wctx *c, const char *s)
{
  if (c->n + 1 >= c->cap) {
    size_t nc = c->cap ? c->cap * 2 : 16;
    char **nv = realloc(c->v, nc * sizeof(*nv));
    if (nv == 0)
      return -1;
    c->v = nv;
    c->cap = nc;
  }
  char *copy = strdup(s);
  if (copy == 0)
    return -1;
  c->v[c->n++] = copy;
  return 0;
}

// the word is complete: glob it if it can be globbed, and start the
// next one.
static int
finish(struct wctx *c)
{
  int rc = 0;

  if (!c->have)
    return 0;
  if (c->magic) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    int r = glob(c->pat.s ? c->pat.s : "", 0, 0, &g);
    if (r == 0) {
      for (size_t i = 0; i < g.gl_pathc && rc == 0; i++)
        if (add_word(c, g.gl_pathv[i]) < 0)
          rc = WRDE_NOSPACE;
      globfree(&g);
    } else if (r == GLOB_NOMATCH) {
      // nothing on disk: the word stands for itself, as a shell leaves it
      if (add_word(c, c->lit.s ? c->lit.s : "") < 0)
        rc = WRDE_NOSPACE;
    } else {
      rc = WRDE_NOSPACE;
    }
  } else if (add_word(c, c->lit.s ? c->lit.s : "") < 0) {
    rc = WRDE_NOSPACE;
  }
  c->lit.n = 0;
  c->pat.n = 0;
  if (c->lit.s)
    c->lit.s[0] = 0;
  if (c->pat.s)
    c->pat.s[0] = 0;
  c->have = 0;
  c->magic = 0;
  return rc;
}

static int
is_name(int c)
{
  return isalnum(c) || c == '_';
}

// $NAME or ${NAME} at *pp, which is left after it
static int
expand_var(struct wctx *c, const char **pp, int quoted)
{
  const char *q = *pp + 1;
  const char *name;
  size_t len;
  char buf[64];

  if (*q == '(')
    return WRDE_CMDSUB;
  if (*q == '{') {
    q++;
    name = q;
    while (is_name((unsigned char)*q))
      q++;
    len = q - name;
    // ${#name}, ${name:-word} and the rest of the shell's forms are
    // not here, and saying so is better than expanding them wrongly.
    if (len == 0 || *q != '}')
      return WRDE_SYNTAX;
    q++;
  } else if (isalpha((unsigned char)*q) || *q == '_') {
    name = q;
    while (is_name((unsigned char)*q))
      q++;
    len = q - name;
  } else {
    // a dollar that names nothing is an ordinary character
    c->have = 1;
    if (put(c, '$', quoted) < 0)
      return WRDE_NOSPACE;
    *pp += 1;
    return 0;
  }
  if (len >= sizeof(buf))
    return WRDE_BADVAL;
  memcpy(buf, name, len);
  buf[len] = 0;

  const char *val = getenv(buf);
  if (val == 0) {
    if (c->flags & WRDE_UNDEF)
      return WRDE_BADVAL;
    val = "";
  }
  for (; *val; val++) {
    // an unquoted value splits into words on whitespace, the way a
    // shell would split it; its characters are otherwise literal.
    if (!quoted && (*val == ' ' || *val == '\t' || *val == '\n')) {
      int r = finish(c);
      if (r)
        return r;
      continue;
    }
    c->have = 1;
    if (put(c, (unsigned char)*val, 1) < 0)
      return WRDE_NOSPACE;
  }
  *pp = q;
  return 0;
}

static void
free_ctx(struct wctx *c)
{
  for (size_t i = 0; i < c->n; i++)
    free(c->v[i]);
  free(c->v);
  free(c->lit.s);
  free(c->pat.s);
}

int
wordexp(const char *words, wordexp_t *we, int flags)
{
  struct wctx c;
  int rc = 0;

  if (words == 0 || we == 0)
    return WRDE_SYNTAX;
  if (flags & WRDE_REUSE)
    wordfree(we);
  memset(&c, 0, sizeof(c));
  c.flags = flags;

  for (const char *p = words; *p && rc == 0;) {
    unsigned char ch = *p;

    if (ch == ' ' || ch == '\t' || ch == '\n') {
      rc = finish(&c);
      p++;
      continue;
    }
    switch (ch) {
    case '\'':
      p++;
      c.have = 1;
      while (*p && *p != '\'' && rc == 0)
        if (put(&c, (unsigned char)*p++, 1) < 0)
          rc = WRDE_NOSPACE;
      if (rc == 0 && *p != '\'')
        rc = WRDE_SYNTAX;
      else
        p++;
      break;
    case '"':
      p++;
      c.have = 1;
      while (*p && *p != '"' && rc == 0) {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) {
          if (put(&c, (unsigned char)p[1], 1) < 0)
            rc = WRDE_NOSPACE;
          p += 2;
        } else if (*p == '$') {
          rc = expand_var(&c, &p, 1);
        } else if (*p == '`') {
          rc = WRDE_CMDSUB;
        } else if (put(&c, (unsigned char)*p++, 1) < 0) {
          rc = WRDE_NOSPACE;
        }
      }
      if (rc == 0 && *p != '"')
        rc = WRDE_SYNTAX;
      else if (rc == 0)
        p++;
      break;
    case '\\':
      if (p[1] == 0) {
        rc = WRDE_SYNTAX;
        break;
      }
      c.have = 1;
      if (put(&c, (unsigned char)p[1], 1) < 0)
        rc = WRDE_NOSPACE;
      p += 2;
      break;
    case '$':
      rc = expand_var(&c, &p, 0);
      break;
    case '`':
      rc = WRDE_CMDSUB;
      break;
    case '|': case '&': case ';': case '<': case '>':
    case '(': case ')': case '{': case '}':
      // the characters a shell keeps for itself, and this is not one
      rc = WRDE_BADCHAR;
      break;
    case '~':
      if (!c.have && (p[1] == 0 || p[1] == '/' || p[1] == ' ' || p[1] == '\t')) {
        const char *home = getenv("HOME");
        if (home) {
          c.have = 1;
          for (; *home && rc == 0; home++)
            if (put(&c, (unsigned char)*home, 1) < 0)
              rc = WRDE_NOSPACE;
          p++;
          break;
        }
      }
      c.have = 1;
      if (put(&c, '~', 1) < 0)
        rc = WRDE_NOSPACE;
      p++;
      break;
    default:
      c.have = 1;
      if (put(&c, ch, 0) < 0)
        rc = WRDE_NOSPACE;
      p++;
      break;
    }
  }
  if (rc == 0)
    rc = finish(&c);
  if (rc) {
    free_ctx(&c);
    return rc;
  }

  size_t offs = (flags & WRDE_DOOFFS) ? we->we_offs : 0;
  size_t oldc = (flags & WRDE_APPEND) ? we->we_wordc : 0;
  char **nv = realloc((flags & WRDE_APPEND) ? we->we_wordv : 0,
                      (offs + oldc + c.n + 1) * sizeof(char *));
  if (nv == 0) {
    free_ctx(&c);
    return WRDE_NOSPACE;
  }
  if (!(flags & WRDE_APPEND))
    for (size_t i = 0; i < offs; i++)
      nv[i] = 0;
  for (size_t i = 0; i < c.n; i++)
    nv[offs + oldc + i] = c.v[i];
  nv[offs + oldc + c.n] = 0;
  free(c.v);
  free(c.lit.s);
  free(c.pat.s);
  we->we_wordv = nv;
  we->we_wordc = oldc + c.n;
  if (!(flags & WRDE_DOOFFS))
    we->we_offs = 0;
  return 0;
}

void
wordfree(wordexp_t *we)
{
  if (we == 0 || we->we_wordv == 0)
    return;
  for (size_t i = 0; i < we->we_wordc; i++)
    free(we->we_wordv[we->we_offs + i]);
  free(we->we_wordv);
  we->we_wordv = 0;
  we->we_wordc = 0;
}
