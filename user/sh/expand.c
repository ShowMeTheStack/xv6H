//
// expansion.
//
// a word goes through the stages POSIX lays down, in order: tilde
// expansion, then parameter, command and arithmetic substitution, then
// field splitting on IFS, then pathname expansion, and finally quote
// removal. the lexer has already marked which characters were quoted,
// and that mark travels with the text through every stage, because it
// is what decides whether a field is split and whether a star is a
// pattern or a character.
//
#include "sh.h"
#include <glob.h>
#include <fnmatch.h>

// a string being built, with the quoting mark beside it
struct buf {
  char *text;
  char *quoted;
  int len, cap;
};

static void
buf_init(struct buf *b)
{
  b->cap = 64;
  b->text = xmalloc(b->cap);
  b->quoted = xmalloc(b->cap);
  b->len = 0;
}

static void
buf_add(struct buf *b, int c, int q)
{
  if (b->len + 1 >= b->cap) {
    b->cap *= 2;
    char *t = xmalloc(b->cap), *u = xmalloc(b->cap);
    memmove(t, b->text, b->len);
    memmove(u, b->quoted, b->len);
    free(b->text);
    free(b->quoted);
    b->text = t;
    b->quoted = u;
  }
  b->text[b->len] = c;
  b->quoted[b->len] = q;
  b->len++;
}

static void
buf_addstr(struct buf *b, const char *s, int q)
{
  for (; *s; s++)
    buf_add(b, *s, q);
}

static void
buf_free(struct buf *b)
{
  free(b->text);
  free(b->quoted);
}

// ---- the word list an expansion produces ----

void
wl_init(struct wordlist *wl)
{
  wl->cap = 8;
  wl->n = 0;
  wl->words = xmalloc(wl->cap * sizeof(char *));
}

void
wl_add(struct wordlist *wl, char *s)
{
  if (wl->n + 1 >= wl->cap) {
    wl->cap *= 2;
    char **w = xmalloc(wl->cap * sizeof(char *));
    memmove(w, wl->words, wl->n * sizeof(char *));
    free(wl->words);
    wl->words = w;
  }
  wl->words[wl->n++] = s;
  wl->words[wl->n] = 0;
}

void
wl_free(struct wordlist *wl)
{
  for (int i = 0; i < wl->n; i++)
    free(wl->words[i]);
  free(wl->words);
  wl->words = 0;
  wl->n = wl->cap = 0;
}

// ---- the special parameters ----

static char *
positional(int n)
{
  if (n == 0)
    return xstrdup(script_name ? script_name : "sh");
  if (n <= nscript_args)
    return xstrdup(script_args[n - 1]);
  return xstrdup("");
}

static char *
special_param(int c, int *is_at)
{
  char buf[32];

  *is_at = 0;
  switch (c) {
  case '?':
    snprintf(buf, sizeof(buf), "%d", last_status);
    return xstrdup(buf);
  case '$':
    snprintf(buf, sizeof(buf), "%d", shell_pid);
    return xstrdup(buf);
  case '!':
    snprintf(buf, sizeof(buf), "%d", last_bg_pid);
    return xstrdup(buf);
  case '#':
    snprintf(buf, sizeof(buf), "%d", nscript_args);
    return xstrdup(buf);
  case '-': {
    int n = 0;
    if (opt_errexit)
      buf[n++] = 'e';
    if (opt_xtrace)
      buf[n++] = 'x';
    if (opt_noglob)
      buf[n++] = 'f';
    if (opt_nounset)
      buf[n++] = 'u';
    if (opt_verbose)
      buf[n++] = 'v';
    if (interactive)
      buf[n++] = 'i';
    buf[n] = 0;
    return xstrdup(buf);
  }
  case '@':
  case '*':
    *is_at = (c == '@');
    return 0; // the caller joins the arguments itself
  default:
    return 0;
  }
}

// ---- arithmetic ----
//
// the POSIX arithmetic language: signed long integers, C's operators
// and precedence, with variables looked up in the shell's own.

struct arith {
  const char *s;
  int err;
};

static long arith_expr(struct arith *a);

static void
arith_skip(struct arith *a)
{
  while (*a->s == ' ' || *a->s == '\t' || *a->s == '\n')
    a->s++;
}

static long
arith_primary(struct arith *a)
{
  arith_skip(a);
  if (*a->s == '(') {
    a->s++;
    long v = arith_expr(a);
    arith_skip(a);
    if (*a->s == ')')
      a->s++;
    else
      a->err = 1;
    return v;
  }
  if (*a->s == '-') {
    a->s++;
    return -arith_primary(a);
  }
  if (*a->s == '+') {
    a->s++;
    return arith_primary(a);
  }
  if (*a->s == '!') {
    a->s++;
    return !arith_primary(a);
  }
  if (*a->s == '~') {
    a->s++;
    return ~arith_primary(a);
  }
  if (isdigit((unsigned char)*a->s)) {
    char *end;
    long v = strtol(a->s, &end, 0); // 0x and 0 prefixes, as the shell says
    a->s = end;
    return v;
  }
  if (isalpha((unsigned char)*a->s) || *a->s == '_') {
    char name[128];
    int n = 0;
    while ((isalnum((unsigned char)*a->s) || *a->s == '_') && n < (int)sizeof(name) - 1)
      name[n++] = *a->s++;
    name[n] = 0;
    arith_skip(a);
    const char *v = var_get(name);
    long value = v ? strtol(v, 0, 0) : 0;
    if (*a->s == '=' && a->s[1] != '=') {
      a->s++;
      long rhs = arith_expr(a);
      var_set_int(name, rhs);
      return rhs;
    }
    if ((*a->s == '+' && a->s[1] == '+') || (*a->s == '-' && a->s[1] == '-')) {
      long nv = (*a->s == '+') ? value + 1 : value - 1;
      a->s += 2;
      var_set_int(name, nv);
      return value; // the value before, as C says of a postfix operator
    }
    return value;
  }
  a->err = 1;
  return 0;
}

static long
arith_mul(struct arith *a)
{
  long v = arith_primary(a);

  for (;;) {
    arith_skip(a);
    if (*a->s == '*') {
      a->s++;
      v *= arith_primary(a);
    } else if (*a->s == '/') {
      a->s++;
      long d = arith_primary(a);
      if (d == 0) {
        sh_error("division by zero");
        a->err = 1;
        return 0;
      }
      v /= d;
    } else if (*a->s == '%') {
      a->s++;
      long d = arith_primary(a);
      if (d == 0) {
        sh_error("division by zero");
        a->err = 1;
        return 0;
      }
      v %= d;
    } else {
      return v;
    }
  }
}

static long
arith_add(struct arith *a)
{
  long v = arith_mul(a);

  for (;;) {
    arith_skip(a);
    if (*a->s == '+' && a->s[1] != '+') {
      a->s++;
      v += arith_mul(a);
    } else if (*a->s == '-' && a->s[1] != '-') {
      a->s++;
      v -= arith_mul(a);
    } else {
      return v;
    }
  }
}

static long
arith_shift(struct arith *a)
{
  long v = arith_add(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '<' && a->s[1] == '<') {
      a->s += 2;
      v <<= arith_add(a);
    } else if (a->s[0] == '>' && a->s[1] == '>') {
      a->s += 2;
      v >>= arith_add(a);
    } else {
      return v;
    }
  }
}

static long
arith_rel(struct arith *a)
{
  long v = arith_shift(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '<' && a->s[1] == '=') {
      a->s += 2;
      v = v <= arith_shift(a);
    } else if (a->s[0] == '>' && a->s[1] == '=') {
      a->s += 2;
      v = v >= arith_shift(a);
    } else if (a->s[0] == '<') {
      a->s++;
      v = v < arith_shift(a);
    } else if (a->s[0] == '>') {
      a->s++;
      v = v > arith_shift(a);
    } else {
      return v;
    }
  }
}

static long
arith_eq(struct arith *a)
{
  long v = arith_rel(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '=' && a->s[1] == '=') {
      a->s += 2;
      v = v == arith_rel(a);
    } else if (a->s[0] == '!' && a->s[1] == '=') {
      a->s += 2;
      v = v != arith_rel(a);
    } else {
      return v;
    }
  }
}

static long
arith_band(struct arith *a)
{
  long v = arith_eq(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '&' && a->s[1] != '&') {
      a->s++;
      v &= arith_eq(a);
    } else {
      return v;
    }
  }
}

static long
arith_bxor(struct arith *a)
{
  long v = arith_band(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '^') {
      a->s++;
      v ^= arith_band(a);
    } else {
      return v;
    }
  }
}

static long
arith_bor(struct arith *a)
{
  long v = arith_bxor(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '|' && a->s[1] != '|') {
      a->s++;
      v |= arith_bxor(a);
    } else {
      return v;
    }
  }
}

static long
arith_and(struct arith *a)
{
  long v = arith_bor(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '&' && a->s[1] == '&') {
      a->s += 2;
      long r = arith_bor(a);
      v = v && r;
    } else {
      return v;
    }
  }
}

static long
arith_or(struct arith *a)
{
  long v = arith_and(a);

  for (;;) {
    arith_skip(a);
    if (a->s[0] == '|' && a->s[1] == '|') {
      a->s += 2;
      long r = arith_and(a);
      v = v || r;
    } else {
      return v;
    }
  }
}

static long
arith_expr(struct arith *a)
{
  long v = arith_or(a);

  arith_skip(a);
  if (*a->s == '?') {
    a->s++;
    long t = arith_expr(a);
    arith_skip(a);
    if (*a->s == ':') {
      a->s++;
      long f = arith_expr(a);
      return v ? t : f;
    }
    a->err = 1;
  }
  return v;
}

static char *
do_arith(const char *text)
{
  struct arith a = {text, 0};
  long v = arith_expr(&a);
  char buf[32];

  arith_skip(&a);
  if (*a.s || a.err) {
    sh_error("arithmetic error in `%s'", text);
    return xstrdup("");
  }
  snprintf(buf, sizeof(buf), "%ld", v);
  return xstrdup(buf);
}

// ---- command substitution ----
//
// the text runs in a child whose output comes back through a pipe,
// with the trailing newlines dropped, as POSIX requires.

static char *
do_command_sub(const char *text)
{
  int fds[2];

  if (pipe(fds) < 0) {
    sh_error("pipe: %s", strerror(errno));
    return xstrdup("");
  }
  fflush(0);
  int pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    sh_error("fork: %s", strerror(errno));
    return xstrdup("");
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 1);
    if (fds[1] != 1)
      close(fds[1]);
    trap_reset_for_child();
    interactive = 0;
    exit(run_string(text, 1));
  }
  close(fds[1]);

  int cap = 256, len = 0;
  char *out = xmalloc(cap);
  for (;;) {
    if (len + 256 > cap) {
      cap *= 2;
      char *t = xmalloc(cap);
      memmove(t, out, len);
      free(out);
      out = t;
    }
    int n = read(fds[0], out + len, cap - len - 1);
    if (n <= 0)
      break;
    len += n;
  }
  close(fds[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  last_status = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
  while (len > 0 && out[len - 1] == '\n')
    len--;
  out[len] = 0;
  return out;
}

// ---- parameter expansion ----

// ${name op word}: apply the operator to the value
static char *
apply_op(const char *name, const char *value, int set, char op, char colon, const char *word)
{
  int unset_or_null = !set || (colon && value && *value == 0);

  switch (op) {
  case '-':
    return xstrdup(unset_or_null ? word : value);
  case '=':
    if (unset_or_null) {
      var_set(name, word, 0);
      return xstrdup(word);
    }
    return xstrdup(value);
  case '?':
    if (unset_or_null) {
      sh_error("%s: %s", name, *word ? word : "parameter not set");
      if (!interactive)
        exit(1);
      return xstrdup("");
    }
    return xstrdup(value);
  case '+':
    return xstrdup(unset_or_null ? "" : word);
  default:
    return xstrdup(value ? value : "");
  }
}

// ${name#pattern} and its relatives: strip a matching prefix or suffix
static char *
strip_affix(const char *value, const char *pattern, int from_end, int longest)
{
  int n = strlen(value);
  char *copy = xmalloc(n + 1);

  if (!from_end) {
    for (int len = longest ? n : 0; longest ? len >= 0 : len <= n; len += longest ? -1 : 1) {
      memmove(copy, value, len);
      copy[len] = 0;
      if (fnmatch(pattern, copy, 0) == 0) {
        char *r = xstrdup(value + len);
        free(copy);
        return r;
      }
    }
  } else {
    for (int start = longest ? 0 : n; longest ? start <= n : start >= 0; start += longest ? 1 : -1) {
      if (fnmatch(pattern, value + start, 0) == 0) {
        memmove(copy, value, start);
        copy[start] = 0;
        return copy;
      }
    }
  }
  free(copy);
  return xstrdup(value);
}

static void expand_into(struct buf *out, const char *text, const char *quoted, int len,
                        struct wordlist *fields, int in_quotes);

// expand ${...}, whose text (without the braces) is `spec`
static char *
expand_braced(const char *spec, int *is_at_out)
{
  int len = strlen(spec);

  *is_at_out = 0;
  if (len == 0)
    return xstrdup("");

  // ${#name} is the length, unless the name is # itself
  if (spec[0] == '#' && len > 1) {
    const char *name = spec + 1;
    char buf[32];
    if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
      snprintf(buf, sizeof(buf), "%d", nscript_args);
      return xstrdup(buf);
    }
    const char *v = isdigit((unsigned char)name[0]) ? 0 : var_get(name);
    char *pv = 0;
    if (isdigit((unsigned char)name[0]))
      v = pv = positional(atoi(name));
    snprintf(buf, sizeof(buf), "%d", v ? (int)strlen(v) : 0);
    free(pv);
    return xstrdup(buf);
  }

  // the name, then the operator
  int i = 0;
  char name[256];
  if (spec[0] == '@' || spec[0] == '*' || spec[0] == '?' || spec[0] == '$' || spec[0] == '!' ||
      spec[0] == '#' || spec[0] == '-') {
    name[0] = spec[0];
    name[1] = 0;
    i = 1;
  } else {
    while (i < len && (isalnum((unsigned char)spec[i]) || spec[i] == '_') && i < 255) {
      name[i] = spec[i];
      i++;
    }
    name[i] = 0;
  }
  if (i == 0) {
    sh_error("bad substitution: ${%s}", spec);
    return xstrdup("");
  }

  char *value = 0;
  int set = 0;
  if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
    *is_at_out = (name[0] == '@');
    return 0; // the caller joins the arguments
  }
  if (isdigit((unsigned char)name[0])) {
    value = positional(atoi(name));
    set = atoi(name) <= nscript_args;
  } else {
    int dummy;
    char *sp = special_param(name[0], &dummy);
    if (sp && !isalpha((unsigned char)name[0]) && name[0] != '_') {
      value = sp;
      set = 1;
    } else {
      free(sp);
      const char *v = var_get(name);
      set = v != 0;
      value = xstrdup(v ? v : "");
    }
  }

  if (i >= len) {
    if (!set && opt_nounset && !isdigit((unsigned char)name[0])) {
      sh_error("%s: parameter not set", name);
      if (!interactive)
        exit(1);
    }
    return value;
  }

  // the operator, which may be prefixed with a colon
  char colon = 0, op = spec[i];
  if (op == ':') {
    colon = 1;
    i++;
    op = i < len ? spec[i] : 0;
  }
  i++;

  if (op == '-' || op == '=' || op == '?' || op == '+') {
    // the word may itself contain expansions
    struct buf wb;
    buf_init(&wb);
    char *qz = xmalloc(len - i + 1);
    memset(qz, 0, len - i + 1);
    expand_into(&wb, spec + i, qz, len - i, 0, 1);
    buf_add(&wb, 0, 0);
    free(qz);
    char *r = apply_op(name, value, set, op, colon, wb.text);
    buf_free(&wb);
    free(value);
    return r;
  }
  if (op == '#' || op == '%') {
    int longest = (i < len && spec[i] == op);
    if (longest)
      i++;
    struct buf wb;
    buf_init(&wb);
    char *qz = xmalloc(len - i + 1);
    memset(qz, 0, len - i + 1);
    expand_into(&wb, spec + i, qz, len - i, 0, 1);
    buf_add(&wb, 0, 0);
    free(qz);
    char *r = strip_affix(value ? value : "", wb.text, op == '%', longest);
    buf_free(&wb);
    free(value);
    return r;
  }

  sh_error("bad substitution: ${%s}", spec);
  free(value);
  return xstrdup("");
}

// ---- the main pass ----
//
// walks the word, copying ordinary characters and expanding the rest.
// when `fields` is given, $@ and an unquoted expansion may split into
// more than one field, and the splitting happens here.

// "$@" with no positional parameters expands to no fields at all, not
// to one empty one -- so a word that is nothing but "$@" disappears.
// the caller needs to know that happened, because an empty word it
// built for any other reason is a real, empty argument.
static int at_expanded_empty;

static void
add_arg_fields(struct buf *out, struct wordlist *fields, int in_quotes, int is_at)
{
  const char *ifs = var_get("IFS");
  char sep = (ifs && *ifs) ? ifs[0] : ' ';

  if (is_at && in_quotes && nscript_args == 0) {
    at_expanded_empty = 1;
    return;
  }
  if (is_at && in_quotes && fields) {
    // "$@" is one field per argument, however they are spelled
    for (int i = 0; i < nscript_args; i++) {
      if (i > 0) {
        buf_add(out, 0, 0);
        wl_add(fields, xstrdup(out->text));
        out->len = 0;
      }
      buf_addstr(out, script_args[i], 1);
    }
    return;
  }
  for (int i = 0; i < nscript_args; i++) {
    if (i > 0)
      buf_add(out, sep, in_quotes);
    buf_addstr(out, script_args[i], in_quotes);
  }
}

// the closing parenthesis of a $( ) or a $(( )), found by counting
// the ones that are really nesting. a parenthesis inside quotes or
// behind a backslash is a character in a word, not a bracket -- so
// `$(grep -c 'a(' file)` has to end at the last one and not at the
// one in the pattern.
//
// the quoting that matters is the quoting inside the substitution,
// not the quoting around it: the body of a $( ) is parsed as a
// command of its own, and an expansion happens inside double quotes
// as readily as outside them. so the `quoted` array the caller
// carries -- which says what was in quotes in the enclosing word --
// is deliberately not consulted here.
static int
find_close_paren(const char *text, int len, int start)
{
  int depth = 0;

  for (int j = start; j < len; j++) {
    if (text[j] == '\\' && j + 1 < len) {
      j++;
      continue;
    }
    if (text[j] == '\'' || text[j] == '"') {
      int qc = text[j];
      for (j++; j < len; j++) {
        if (qc == '"' && text[j] == '\\' && j + 1 < len) {
          j++;
          continue;
        }
        if (text[j] == qc)
          break;
      }
      continue;
    }
    if (text[j] == '(')
      depth++;
    else if (text[j] == ')' && --depth == 0)
      return j;
  }
  return len;
}

static void
expand_into(struct buf *out, const char *text, const char *quoted, int len, struct wordlist *fields,
            int in_quotes)
{
  for (int i = 0; i < len; i++) {
    int c = (unsigned char)text[i];
    int q = quoted[i] || in_quotes;

    if (quoted[i] & Q_LITERAL) {
      buf_add(out, c, q);
      continue;
    }
    if (c == '`') {
      // the older spelling of command substitution
      int j = i + 1;
      while (j < len && text[j] != '`')
        j++;
      int inner_len = j - (i + 1);
      char *inner = xmalloc(inner_len + 1);
      memmove(inner, text + i + 1, inner_len);
      inner[inner_len] = 0;
      char *value = do_command_sub(inner);
      free(inner);
      if (q || fields == 0) {
        buf_addstr(out, value, q);
      } else {
        const char *ifs = var_get("IFS");
        if (ifs == 0)
          ifs = " \t\n";
        char *p = value;
        int first = 1;
        while (*p) {
          while (*p && strchr(ifs, *p))
            p++;
          if (*p == 0)
            break;
          if (!first) {
            buf_add(out, 0, 0);
            wl_add(fields, xstrdup(out->text));
            out->len = 0;
          }
          while (*p && !strchr(ifs, *p))
            buf_add(out, *p++, 0);
          first = 0;
        }
      }
      free(value);
      i = j;
      continue;
    }
    if (c == '~' && !q && i == 0 && (len == 1 || text[1] == '/')) {
      const char *home = var_get("HOME");
      buf_addstr(out, home ? home : "/", 0);
      continue;
    }
    if (c != '$' || i + 1 >= len) {
      buf_add(out, c, q);
      continue;
    }

    int next = (unsigned char)text[i + 1];
    if (next == '(' && i + 2 < len && text[i + 2] == '(') {
      // $((expression))
      int j = find_close_paren(text, len, i + 1);
      int inner_start = i + 3, inner_len = j - 1 - inner_start;
      if (inner_len < 0)
        inner_len = 0;
      char *inner = xmalloc(inner_len + 1);
      memmove(inner, text + inner_start, inner_len);
      inner[inner_len] = 0;
      struct buf eb;
      buf_init(&eb);
      char *qz = xmalloc(inner_len + 1);
      memset(qz, 0, inner_len + 1);
      expand_into(&eb, inner, qz, inner_len, 0, 1);
      buf_add(&eb, 0, 0);
      char *value = do_arith(eb.text);
      buf_addstr(out, value, q);
      free(value);
      free(inner);
      free(qz);
      buf_free(&eb);
      // j is the second ')'; the loop's own i++ moves past it, and
      // adding one here as well ate the character after $(( ))
      i = j;
      continue;
    }
    if (next == '(') {
      // $(command)
      int j = find_close_paren(text, len, i + 1);
      int inner_start = i + 2, inner_len = j - inner_start;
      if (inner_len < 0)
        inner_len = 0;
      char *inner = xmalloc(inner_len + 1);
      memmove(inner, text + inner_start, inner_len);
      inner[inner_len] = 0;
      char *value = do_command_sub(inner);
      free(inner);
      if (q || fields == 0) {
        buf_addstr(out, value, q);
      } else {
        // the output is split into fields like any other expansion
        const char *ifs = var_get("IFS");
        if (ifs == 0)
          ifs = " \t\n";
        char *p = value;
        int first = 1;
        while (*p) {
          while (*p && strchr(ifs, *p))
            p++;
          if (*p == 0)
            break;
          if (!first) {
            buf_add(out, 0, 0);
            wl_add(fields, xstrdup(out->text));
            out->len = 0;
          }
          while (*p && !strchr(ifs, *p))
            buf_add(out, *p++, 0);
          first = 0;
        }
      }
      free(value);
      i = j;
      continue;
    }
    if (next == '{') {
      int depth = 0, j = i + 1;
      for (; j < len; j++) {
        if (text[j] == '{')
          depth++;
        else if (text[j] == '}' && --depth == 0)
          break;
      }
      int inner_len = j - (i + 2);
      if (inner_len < 0)
        inner_len = 0;
      char *inner = xmalloc(inner_len + 1);
      memmove(inner, text + i + 2, inner_len);
      inner[inner_len] = 0;
      int is_at = 0;
      char *value = expand_braced(inner, &is_at);
      if (value == 0) {
        add_arg_fields(out, fields, q, is_at);
      } else {
        if (q || fields == 0) {
          buf_addstr(out, value, q);
        } else {
          const char *ifs = var_get("IFS");
          if (ifs == 0)
            ifs = " \t\n";
          char *p = value;
          int first = 1;
          while (*p) {
            while (*p && strchr(ifs, *p))
              p++;
            if (*p == 0)
              break;
            if (!first) {
              buf_add(out, 0, 0);
              wl_add(fields, xstrdup(out->text));
              out->len = 0;
            }
            while (*p && !strchr(ifs, *p))
              buf_add(out, *p++, 0);
            first = 0;
          }
        }
        free(value);
      }
      free(inner);
      i = j;
      continue;
    }
    if (next == '@' || next == '*') {
      add_arg_fields(out, fields, q, next == '@');
      i++;
      continue;
    }
    if (isdigit(next)) {
      char *v = positional(next - '0');
      buf_addstr(out, v, q);
      free(v);
      i++;
      continue;
    }
    if (isalpha(next) || next == '_') {
      char name[256];
      int n = 0;
      int j = i + 1;
      while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_') && n < 255)
        name[n++] = text[j++];
      name[n] = 0;
      const char *v = var_get(name);
      if (v == 0 && opt_nounset) {
        sh_error("%s: parameter not set", name);
        if (!interactive)
          exit(1);
      }
      if (v) {
        if (q || fields == 0) {
          buf_addstr(out, v, q);
        } else {
          const char *ifs = var_get("IFS");
          if (ifs == 0)
            ifs = " \t\n";
          const char *p = v;
          int first = 1;
          while (*p) {
            while (*p && strchr(ifs, *p))
              p++;
            if (*p == 0)
              break;
            if (!first) {
              buf_add(out, 0, 0);
              wl_add(fields, xstrdup(out->text));
              out->len = 0;
            }
            while (*p && !strchr(ifs, *p))
              buf_add(out, *p++, 0);
            first = 0;
          }
        }
      }
      i = j - 1;
      continue;
    }
    {
      int dummy;
      char *v = special_param(next, &dummy);
      if (v) {
        buf_addstr(out, v, q);
        free(v);
        i++;
        continue;
      }
    }
    buf_add(out, c, q);
  }
}

// ---- pathname expansion ----

// does the field hold an unquoted pattern character?
static int
has_pattern(const char *text, const char *quoted, int len)
{
  for (int i = 0; i < len; i++)
    if (!(quoted[i] & Q_QUOTED) && (text[i] == '*' || text[i] == '?' || text[i] == '['))
      return 1;
  return 0;
}

// quote the characters that were quoted in the field, so that glob
// treats them as themselves
static char *
pattern_of(const char *text, const char *quoted, int len)
{
  char *p = xmalloc(len * 2 + 1);
  int n = 0;

  for (int i = 0; i < len; i++) {
    if ((quoted[i] & Q_QUOTED) &&
        (text[i] == '*' || text[i] == '?' || text[i] == '[' || text[i] == '\\'))
      p[n++] = '\\';
    p[n++] = text[i];
  }
  p[n] = 0;
  return p;
}

// ---- the entry points ----

void
expand_word_list(struct word *w, struct wordlist *out, int for_assign)
{
  for (; w; w = w->next) {
    struct buf b;
    buf_init(&b);
    int before = out->n;
    at_expanded_empty = 0;
    expand_into(&b, w->text, w->quoted, w->len, for_assign ? 0 : out, 0);
    buf_add(&b, 0, 0);
    b.len--;

    // pathname expansion, unless the shell was told not to
    if (!for_assign && !opt_noglob && has_pattern(b.text, b.quoted, b.len)) {
      char *pattern = pattern_of(b.text, b.quoted, b.len);
      glob_t g;
      memset(&g, 0, sizeof(g));
      if (glob(pattern, 0, 0, &g) == 0 && g.gl_pathc > 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
          wl_add(out, xstrdup(g.gl_pathv[i]));
        globfree(&g);
        free(pattern);
        buf_free(&b);
        continue;
      }
      globfree(&g);
      free(pattern);
    }

    // an unquoted expansion that produced nothing at all disappears,
    // unless the word had other text in it -- and so does a word that
    // was only a "$@" with nothing in it
    if (b.len == 0 && (out->n > before || at_expanded_empty)) {
      buf_free(&b);
      continue;
    }
    wl_add(out, xstrdup(b.text));
    buf_free(&b);
  }
}

char *
expand_to_string(struct word *w, int quoted_context)
{
  struct buf b;

  buf_init(&b);
  for (; w; w = w->next)
    expand_into(&b, w->text, w->quoted, w->len, 0, quoted_context);
  buf_add(&b, 0, 0);
  char *r = xstrdup(b.text);
  buf_free(&b);
  return r;
}

// a here-document's text, which is expanded unless its delimiter was
// quoted (the caller decides that and only calls this when it must)
char *
expand_here(const char *text)
{
  struct buf b;
  int len = strlen(text);
  char *q = xmalloc(len + 1);

  memset(q, 0, len + 1);
  buf_init(&b);
  expand_into(&b, text, q, len, 0, 1);
  buf_add(&b, 0, 0);
  free(q);
  char *r = xstrdup(b.text);
  buf_free(&b);
  return r;
}

int
pattern_match(const char *pattern, const char *string)
{
  return fnmatch(pattern, string, 0) == 0;
}
