//
// <glob.h>: expanding a shell filename pattern against the file system.
//
// the pattern is expanded one path component at a time. a component
// with no metacharacter is simply appended to the paths built so far;
// a component with one is matched by fnmatch() against everything
// readdir() hands back for each of those paths. so "a/*/c*" opens a,
// keeps the entries that match "*", and opens each of them in turn --
// the file system is only ever read where the pattern really asks a
// question, and a path that cannot be a directory drops out on its own
// when opendir() fails.
//
// the results are collected into a growable vector and sorted at the
// end with strcmp, which is what the C locale's collating order is.
// with GLOB_APPEND only the names this call found are sorted, since
// the ones already there were sorted by the call that found them.
//
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define GLOB_PATHBUF (PATH_MAX + NAME_MAX + 2)

struct gvec {
  char **v;
  size_t n, cap;
};

struct gctx {
  struct gvec out;
  int flags;
  int (*errfunc)(const char *, int);
  int err;            // GLOB_NOSPACE or GLOB_ABORTED, once something has gone wrong
};

static int
gvec_add(struct gvec *g, const char *s, size_t len, int mark)
{
  if (g->n + 1 >= g->cap) {
    size_t nc = g->cap ? g->cap * 2 : 16;
    char **nv = realloc(g->v, nc * sizeof(*nv));
    if (nv == 0)
      return -1;
    g->v = nv;
    g->cap = nc;
  }
  if (mark && len && s[len - 1] == '/')
    mark = 0;   // the root, or a path the pattern already ended with a slash
  char *copy = malloc(len + mark + 1);
  if (copy == 0)
    return -1;
  memcpy(copy, s, len);
  if (mark)
    copy[len] = '/';
  copy[len + mark] = 0;
  g->v[g->n++] = copy;
  return 0;
}

// a metacharacter the pattern really means, rather than one a backslash
// has turned back into a character
static int
has_magic(const char *p, size_t len, int flags)
{
  for (size_t i = 0; i < len; i++) {
    if (p[i] == '\\' && !(flags & GLOB_NOESCAPE)) {
      i++;
      continue;
    }
    if (p[i] == '*' || p[i] == '?' || p[i] == '[')
      return 1;
  }
  return 0;
}

// the component with its backslashes taken off, which is what the name
// on disk has to be
static size_t
unescape(char *dst, const char *p, size_t len, int flags)
{
  size_t n = 0;

  for (size_t i = 0; i < len; i++) {
    if (p[i] == '\\' && !(flags & GLOB_NOESCAPE) && i + 1 < len)
      i++;
    dst[n++] = p[i];
  }
  dst[n] = 0;
  return n;
}

// does this path exist, and may it be kept?
static int
keep(struct gctx *c, const char *path, int *isdir)
{
  struct stat sb;

  if (stat(path, &sb) < 0)
    return 0;
  *isdir = S_ISDIR(ST_MODE(sb));
  if ((c->flags & GLOB_ONLYDIR) && !*isdir)
    return 0;
  return 1;
}

static int
report(struct gctx *c, const char *path, int err)
{
  // a path that is simply not there, or not a directory, is the normal
  // way a branch of the pattern dies: it is not an error to report.
  if (err == ENOENT || err == ENOTDIR)
    return 0;
  if (c->errfunc && c->errfunc(path, err))
    return -1;
  if (c->flags & GLOB_ERR)
    return -1;
  return 0;
}

// expand `pat` under the directory already built into path[0..plen).
static void
expand(struct gctx *c, char *path, size_t plen, const char *pat)
{
  if (c->err)
    return;

  // leading slashes belong to the prefix, not to a component
  while (*pat == '/') {
    if (plen + 1 >= GLOB_PATHBUF)
      return;
    path[plen++] = '/';
    pat++;
  }
  path[plen] = 0;
  if (*pat == 0) {
    int isdir;
    if (plen && keep(c, path, &isdir))
      if (gvec_add(&c->out, path, plen, (c->flags & GLOB_MARK) && isdir) < 0)
        c->err = GLOB_NOSPACE;
    return;
  }

  const char *slash = strchr(pat, '/');
  size_t clen = slash ? (size_t)(slash - pat) : strlen(pat);
  const char *rest = slash ? slash : pat + clen;   // "" or "/..."

  if (!has_magic(pat, clen, c->flags)) {
    // nothing to ask the file system yet: just extend the path
    char name[GLOB_PATHBUF];
    if (clen >= sizeof(name) || plen + clen + 1 >= GLOB_PATHBUF)
      return;
    size_t n = unescape(name, pat, clen, c->flags);
    memcpy(path + plen, name, n);
    path[plen + n] = 0;
    if (*rest == 0) {
      int isdir;
      if (keep(c, path, &isdir))
        if (gvec_add(&c->out, path, plen + n, (c->flags & GLOB_MARK) && isdir) < 0)
          c->err = GLOB_NOSPACE;
    } else {
      expand(c, path, plen + n, rest);
    }
    return;
  }

  // a real question for the file system
  char comp[GLOB_PATHBUF];
  if (clen >= sizeof(comp))
    return;
  memcpy(comp, pat, clen);
  comp[clen] = 0;

  DIR *d = opendir(plen ? path : ".");
  if (d == 0) {
    if (report(c, plen ? path : ".", errno) < 0)
      c->err = GLOB_ABORTED;
    return;
  }
  int fnflags = FNM_PERIOD | ((c->flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0);
  struct dirent *e;
  while ((e = readdir(d)) != 0 && !c->err) {
    size_t nlen = strlen(e->d_name);
    if (fnmatch(comp, e->d_name, fnflags) != 0)
      continue;
    if (plen + nlen + 1 >= GLOB_PATHBUF)
      continue;
    memcpy(path + plen, e->d_name, nlen);
    path[plen + nlen] = 0;
    if (*rest == 0) {
      int isdir;
      if (keep(c, path, &isdir))
        if (gvec_add(&c->out, path, plen + nlen, (c->flags & GLOB_MARK) && isdir) < 0)
          c->err = GLOB_NOSPACE;
    } else {
      expand(c, path, plen + nlen, rest);
    }
  }
  closedir(d);
}

static int
cmpstr(const void *a, const void *b)
{
  return strcmp(*(char *const *)a, *(char *const *)b);
}

int
glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g)
{
  struct gctx c;
  char path[GLOB_PATHBUF];

  if (pattern == 0 || g == 0)
    return GLOB_NOSPACE;
  memset(&c, 0, sizeof(c));
  c.flags = flags;
  c.errfunc = errfunc;

  size_t offs = (flags & GLOB_DOOFFS) ? g->gl_offs : 0;
  size_t oldc = (flags & GLOB_APPEND) ? g->gl_pathc : 0;
  if (!(flags & GLOB_APPEND)) {
    g->gl_pathc = 0;
    g->gl_pathv = 0;
  }
  g->gl_flags = flags;
  if (has_magic(pattern, strlen(pattern), flags))
    g->gl_flags |= GLOB_MAGCHAR;

  path[0] = 0;
  expand(&c, path, 0, pattern);

  int rc = 0;
  if (c.err) {
    rc = c.err;
    // GLOB_ERR and GLOB_NOSPACE both keep what was found so far, which
    // POSIX allows and which globfree() will still release.
  } else if (c.out.n == 0) {
    if (flags & GLOB_NOCHECK) {
      if (gvec_add(&c.out, pattern, strlen(pattern), 0) < 0)
        rc = GLOB_NOSPACE;
    } else {
      rc = GLOB_NOMATCH;
    }
  }
  if (rc == GLOB_NOMATCH || (rc && c.out.n == 0)) {
    for (size_t i = 0; i < c.out.n; i++)
      free(c.out.v[i]);
    free(c.out.v);
    return rc;
  }
  if (!(flags & GLOB_NOSORT) && c.out.n > 1)
    qsort(c.out.v, c.out.n, sizeof(*c.out.v), cmpstr);

  char **nv = realloc((flags & GLOB_APPEND) ? g->gl_pathv : 0,
                      (offs + oldc + c.out.n + 1) * sizeof(char *));
  if (nv == 0) {
    for (size_t i = 0; i < c.out.n; i++)
      free(c.out.v[i]);
    free(c.out.v);
    return GLOB_NOSPACE;
  }
  if (!(flags & GLOB_APPEND))
    for (size_t i = 0; i < offs; i++)
      nv[i] = 0;
  for (size_t i = 0; i < c.out.n; i++)
    nv[offs + oldc + i] = c.out.v[i];
  nv[offs + oldc + c.out.n] = 0;
  free(c.out.v);
  g->gl_pathv = nv;
  g->gl_pathc = oldc + c.out.n;
  return rc;
}

void
globfree(glob_t *g)
{
  if (g == 0 || g->gl_pathv == 0)
    return;
  size_t offs = (g->gl_flags & GLOB_DOOFFS) ? g->gl_offs : 0;
  for (size_t i = 0; i < g->gl_pathc; i++)
    free(g->gl_pathv[offs + i]);
  free(g->gl_pathv);
  g->gl_pathv = 0;
  g->gl_pathc = 0;
}
