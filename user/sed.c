//
// sed: the stream editor.
//
//   sed [-n] [-E] [-i] [-e SCRIPT] [-f FILE] [SCRIPT] [FILE...]
//
// the POSIX command set: s, p, d, a, i, c, y, q, =, l, n, N, P, D, h,
// H, g, G, x, b, t, :, r, w and { }, with addresses that are line
// numbers, $, /regex/ or a pair of either, optionally negated with !.
//
// the two spaces sed works with are the pattern space, which holds the
// line being worked on, and the hold space, which holds whatever was
// put there. that is the whole model, and every command below is a
// sentence about one or the other.
//
#include "user/user.h"
#include <regex.h>

#define MAXCMD 256
#define SPACE 8192

enum addrkind { A_NONE, A_LINE, A_LAST, A_REGEX, A_STEP };

struct addr {
  enum addrkind kind;
  long line;
  long step;     // for first~step
  regex_t re;
  int have_re;
};

struct command {
  char name;
  struct addr a1, a2;
  int naddr;
  int negate;
  int active;    // inside a two-address range
  long range_end;

  // s///
  regex_t re;
  int have_re;
  char *replacement;
  int global, print, nth;
  char *wfile;

  // y///
  char *from, *to;

  // a, i, c, r, w and b, t, :
  char *text;
  char *label;

  int block_end; // for {: the index just past the matching }
};

static struct command cmds[MAXCMD];
static int ncmds;
static int quiet, extended, in_place;
static char pattern[SPACE], hold[SPACE];
static long lineno;
static int sub_made;        // for t
static int quit_requested;
static int exit_status;
static char *append_queue[64];
static int nappend;

// memory that must be there: a sed script is small and a failure to
// hold it is not something to carry on from
static void *
xmalloc(size_t n)
{
  void *p = malloc(n);

  if (p == 0) {
    fprintf(stderr, "sed: out of memory\n");
    exit(1);
  }
  return p;
}

static char *
xstrdup(const char *s)
{
  char *p = xmalloc(strlen(s) + 1);

  strcpy(p, s);
  return p;
}

static void
die(const char *fmt, ...)
{
  va_list ap;

  fprintf(stderr, "sed: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

// ---- parsing ----

// a delimited piece of the script: /.../ or ,X...X, with backslash
// escapes kept as they are for the regex compiler
static char *
take_delimited(char **pp, char delim)
{
  char *p = *pp;
  char *out = xmalloc(strlen(p) + 1);
  int n = 0;

  while (*p && *p != delim) {
    if (*p == '\\' && p[1]) {
      if (p[1] == delim) {
        out[n++] = delim; // \/ inside /.../ is a literal delimiter
        p += 2;
        continue;
      }
      out[n++] = *p++;
    }
    out[n++] = *p++;
  }
  if (*p == delim)
    p++;
  out[n] = 0;
  *pp = p;
  return out;
}

static void
compile(regex_t *re, const char *text)
{
  int flags = extended ? REG_EXTENDED : 0;
  int r = regcomp(re, text, flags);

  if (r != 0) {
    char msg[128];
    regerror(r, re, msg, sizeof(msg));
    die("%s: %s", text, msg);
  }
}

static int
parse_addr(char **pp, struct addr *a)
{
  char *p = *pp;

  a->kind = A_NONE;
  a->have_re = 0;
  if (*p == '$') {
    a->kind = A_LAST;
    p++;
  } else if (*p >= '0' && *p <= '9') {
    a->kind = A_LINE;
    a->line = 0;
    while (*p >= '0' && *p <= '9')
      a->line = a->line * 10 + (*p++ - '0');
    if (*p == '~') {
      p++;
      a->step = 0;
      while (*p >= '0' && *p <= '9')
        a->step = a->step * 10 + (*p++ - '0');
      a->kind = A_STEP;
    }
  } else if (*p == '/' || *p == '\\') {
    char delim = '/';
    if (*p == '\\') {
      p++;
      delim = *p;
    }
    p++;
    char *text = take_delimited(&p, delim);
    compile(&a->re, text);
    free(text);
    a->have_re = 1;
    a->kind = A_REGEX;
  } else {
    return 0;
  }
  *pp = p;
  return 1;
}

// the text of a, i and c: either "a text" or "a\" then the next lines
static char *
take_text(char **pp)
{
  char *p = *pp;
  char *out;
  int n = 0;

  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '\\') {
    p++;
    if (*p == '\n')
      p++;
  }
  out = xmalloc(strlen(p) + 1);
  while (*p && *p != '\n') {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'n') {
        out[n++] = '\n';
        p++;
        continue;
      }
    }
    out[n++] = *p++;
  }
  out[n] = 0;
  *pp = p;
  return out;
}

static void
parse_script(char *script)
{
  char *p = script;

  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')
      p++;
    if (*p == 0)
      break;
    if (*p == '#') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (ncmds >= MAXCMD)
      die("too many commands");
    struct command *c = &cmds[ncmds];
    memset(c, 0, sizeof(*c));
    c->nth = 1;

    if (parse_addr(&p, &c->a1)) {
      c->naddr = 1;
      if (*p == ',') {
        p++;
        if (parse_addr(&p, &c->a2))
          c->naddr = 2;
        else
          die("a comma with no second address");
      }
    }
    while (*p == ' ' || *p == '\t')
      p++;
    while (*p == '!') {
      c->negate = !c->negate;
      p++;
      while (*p == ' ' || *p == '\t')
        p++;
    }
    if (*p == 0)
      die("an address with no command");
    c->name = *p++;

    switch (c->name) {
    case 's': {
      char delim = *p++;
      if (delim == 0)
        die("s needs a delimiter");
      char *re = take_delimited(&p, delim);
      c->replacement = take_delimited(&p, delim);
      compile(&c->re, re);
      free(re);
      c->have_re = 1;
      while (*p && *p != ';' && *p != '\n' && *p != ' ' && *p != '}') {
        if (*p == 'g')
          c->global = 1;
        else if (*p == 'p')
          c->print = 1;
        else if (*p == 'i' || *p == 'I')
          ; // case folding is not in this regex library's POSIX subset
        else if (*p >= '1' && *p <= '9')
          c->nth = *p - '0';
        else if (*p == 'w') {
          p++;
          while (*p == ' ')
            p++;
          char *w = p;
          while (*p && *p != '\n')
            p++;
          c->wfile = xstrdup(w);
          break;
        } else
          die("s: unknown flag %c", *p);
        p++;
      }
      break;
    }
    case 'y': {
      char delim = *p++;
      c->from = take_delimited(&p, delim);
      c->to = take_delimited(&p, delim);
      if (strlen(c->from) != strlen(c->to))
        die("y: the two sets are of different lengths");
      break;
    }
    case 'a':
    case 'i':
    case 'c':
      c->text = take_text(&p);
      break;
    case 'r':
    case 'w':
      while (*p == ' ')
        p++;
      {
        char *f = p;
        while (*p && *p != '\n')
          p++;
        char save = *p;
        *p = 0;
        c->text = xstrdup(f);
        *p = save;
      }
      break;
    case 'b':
    case 't':
    case ':': {
      while (*p == ' ')
        p++;
      char *l = p;
      while (*p && *p != '\n' && *p != ';' && *p != '}')
        p++;
      char save = *p;
      *p = 0;
      c->label = xstrdup(l);
      *p = save;
      break;
    }
    case 'q':
    case 'Q':
      while (*p == ' ')
        p++;
      if (*p >= '0' && *p <= '9')
        exit_status = atoi(p);
      while (*p && *p != '\n' && *p != ';')
        p++;
      break;
    default:
      break;
    }
    ncmds++;
  }

  // match each { with its }
  int stack[MAXCMD], top = 0;
  for (int i = 0; i < ncmds; i++) {
    if (cmds[i].name == '{')
      stack[top++] = i;
    else if (cmds[i].name == '}') {
      if (top == 0)
        die("a } with no {");
      cmds[stack[--top]].block_end = i;
    }
  }
  if (top != 0)
    die("a { with no }");
}

// ---- matching ----

static int
addr_match(struct addr *a, int last_line)
{
  switch (a->kind) {
  case A_LINE:
    return lineno == a->line;
  case A_STEP:
    return a->step > 0 ? (lineno >= a->line && (lineno - a->line) % a->step == 0)
                       : lineno == a->line;
  case A_LAST:
    return last_line;
  case A_REGEX:
    return regexec(&a->re, pattern, 0, 0, 0) == 0;
  default:
    return 0;
  }
}

static int
selected(struct command *c, int last_line)
{
  int yes;

  if (c->naddr == 0) {
    yes = 1;
  } else if (c->naddr == 1) {
    yes = addr_match(&c->a1, last_line);
  } else {
    // a range: it opens on the first address and closes on the second
    if (!c->active) {
      if (addr_match(&c->a1, last_line)) {
        c->active = 1;
        // a numeric end that is not ahead of us closes at once
        if (c->a2.kind == A_LINE && c->a2.line <= lineno)
          c->active = 0;
        yes = 1;
      } else {
        yes = 0;
      }
    } else {
      yes = 1;
      if (addr_match(&c->a2, last_line))
        c->active = 0;
    }
  }
  return c->negate ? !yes : yes;
}

// ---- the substitution ----

static void
append_text(char *dst, int *n, const char *text, int max)
{
  while (*text && *n < max - 1)
    dst[(*n)++] = *text++;
}

static int
substitute(struct command *c)
{
  char out[SPACE];
  regmatch_t m[10];
  const char *p = pattern;
  int n = 0, count = 0, made = 0;

  while (*p || p == pattern) {
    if (regexec(&c->re, p, 10, m, p == pattern ? 0 : REG_NOTBOL) != 0)
      break;
    count++;
    int use = c->global ? count >= c->nth : count == c->nth;
    // what came before the match
    for (int i = 0; i < m[0].rm_so && n < SPACE - 1; i++)
      out[n++] = p[i];
    if (use) {
      made = 1;
      for (const char *r = c->replacement; *r; r++) {
        if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
          int g = r[1] - '0';
          r++;
          if (m[g].rm_so >= 0)
            for (int i = m[g].rm_so; i < m[g].rm_eo && n < SPACE - 1; i++)
              out[n++] = p[i];
        } else if (*r == '\\' && r[1]) {
          r++;
          if (n < SPACE - 1)
            out[n++] = *r == 'n' ? '\n' : *r;
        } else if (*r == '&') {
          for (int i = m[0].rm_so; i < m[0].rm_eo && n < SPACE - 1; i++)
            out[n++] = p[i];
        } else if (n < SPACE - 1) {
          out[n++] = *r;
        }
      }
    } else {
      for (int i = m[0].rm_so; i < m[0].rm_eo && n < SPACE - 1; i++)
        out[n++] = p[i];
    }
    if (m[0].rm_eo == m[0].rm_so) {
      // an empty match: take one character and go on, or this never ends
      if (p[m[0].rm_eo] == 0) {
        p += m[0].rm_eo;
        break;
      }
      if (n < SPACE - 1)
        out[n++] = p[m[0].rm_eo];
      p += m[0].rm_eo + 1;
    } else {
      p += m[0].rm_eo;
    }
    if (!c->global && count >= c->nth)
      break;
  }
  append_text(out, &n, p, SPACE);
  out[n] = 0;
  if (made)
    snprintf(pattern, sizeof(pattern), "%s", out);
  return made;
}

// ---- running ----

static FILE *out_stream;

static void
emit(const char *s)
{
  fputs(s, out_stream);
  fputc('\n', out_stream);
}

static void
flush_appends(void)
{
  for (int i = 0; i < nappend; i++) {
    emit(append_queue[i]);
    free(append_queue[i]);
  }
  nappend = 0;
}

static int
find_label(const char *label)
{
  for (int i = 0; i < ncmds; i++)
    if (cmds[i].name == ':' && strcmp(cmds[i].label, label) == 0)
      return i;
  return -1;
}

// one line through the script. returns 0 normally, 1 for "start the
// next cycle without printing" (d), 2 for "quit".
static int
run_cycle(FILE *in, int last_line)
{
  sub_made = 0;
  for (int i = 0; i < ncmds; i++) {
    struct command *c = &cmds[i];
    if (c->name == ':')
      continue;
    int yes = selected(c, last_line);
    if (c->name == '{') {
      if (!yes)
        i = c->block_end;
      continue;
    }
    if (c->name == '}')
      continue;
    if (!yes)
      continue;

    switch (c->name) {
    case 'p':
      emit(pattern);
      break;
    case 'P': {
      char *nl = strchr(pattern, '\n');
      if (nl) {
        *nl = 0;
        emit(pattern);
        *nl = '\n';
      } else {
        emit(pattern);
      }
      break;
    }
    case 'd':
      return 1;
    case 'D': {
      char *nl = strchr(pattern, '\n');
      if (nl == 0)
        return 1;
      memmove(pattern, nl + 1, strlen(nl + 1) + 1);
      return 3; // start again without reading a new line
    }
    case '=':
      fprintf(out_stream, "%ld\n", lineno);
      break;
    case 'l': {
      for (const char *s = pattern; *s; s++) {
        if (*s == '\\')
          fprintf(out_stream, "\\\\");
        else if (*s == '\t')
          fprintf(out_stream, "\\t");
        else if (*s == '\n')
          fprintf(out_stream, "\\n");
        else if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f)
          fprintf(out_stream, "\\%03o", (unsigned char)*s);
        else
          fputc(*s, out_stream);
      }
      fprintf(out_stream, "$\n");
      break;
    }
    case 's':
      if (substitute(c)) {
        sub_made = 1;
        if (c->print)
          emit(pattern);
        if (c->wfile) {
          FILE *w = fopen(c->wfile, "a");
          if (w) {
            fprintf(w, "%s\n", pattern);
            fclose(w);
          }
        }
      }
      break;
    case 'y': {
      for (char *s = pattern; *s; s++) {
        char *at = strchr(c->from, *s);
        if (at)
          *s = c->to[at - c->from];
      }
      break;
    }
    case 'a':
      if (nappend < 64)
        append_queue[nappend++] = xstrdup(c->text);
      break;
    case 'i':
      emit(c->text);
      break;
    case 'c':
      // a range prints the text once, at its end
      if (c->naddr < 2 || !c->active)
        emit(c->text);
      return 1;
    case 'r': {
      FILE *f = fopen(c->text, "r");
      if (f) {
        char line[4096];
        // the file's contents join the append queue, as POSIX says
        while (fgets(line, sizeof(line), f)) {
          int len = strlen(line);
          if (len && line[len - 1] == '\n')
            line[len - 1] = 0;
          if (nappend < 64)
            append_queue[nappend++] = xstrdup(line);
        }
        fclose(f);
      }
      break;
    }
    case 'w': {
      FILE *f = fopen(c->text, "a");
      if (f) {
        fprintf(f, "%s\n", pattern);
        fclose(f);
      }
      break;
    }
    case 'h':
      snprintf(hold, sizeof(hold), "%s", pattern);
      break;
    case 'H': {
      int n = strlen(hold);
      snprintf(hold + n, sizeof(hold) - n, "\n%s", pattern);
      break;
    }
    case 'g':
      snprintf(pattern, sizeof(pattern), "%s", hold);
      break;
    case 'G': {
      int n = strlen(pattern);
      snprintf(pattern + n, sizeof(pattern) - n, "\n%s", hold);
      break;
    }
    case 'x': {
      char tmp[SPACE];
      snprintf(tmp, sizeof(tmp), "%s", pattern);
      snprintf(pattern, sizeof(pattern), "%s", hold);
      snprintf(hold, sizeof(hold), "%s", tmp);
      break;
    }
    case 'n':
      if (!quiet)
        emit(pattern);
      flush_appends();
      if (fgets(pattern, sizeof(pattern), in) == 0)
        return 2;
      {
        int len = strlen(pattern);
        if (len && pattern[len - 1] == '\n')
          pattern[len - 1] = 0;
      }
      lineno++;
      break;
    case 'N': {
      flush_appends();
      char next[SPACE];
      if (fgets(next, sizeof(next), in) == 0)
        return quiet ? 2 : 4; // POSIX: print the pattern space and end
      int len = strlen(next);
      if (len && next[len - 1] == '\n')
        next[len - 1] = 0;
      int n = strlen(pattern);
      snprintf(pattern + n, sizeof(pattern) - n, "\n%s", next);
      lineno++;
      break;
    }
    case 'b':
      if (c->label[0] == 0)
        return 0;
      {
        int at = find_label(c->label);
        if (at < 0)
          die("b: no label %s", c->label);
        i = at;
      }
      break;
    case 't':
      if (sub_made) {
        sub_made = 0;
        if (c->label[0] == 0)
          return 0;
        int at = find_label(c->label);
        if (at < 0)
          die("t: no label %s", c->label);
        i = at;
      }
      break;
    case 'q':
      quit_requested = 1;
      return 0;
    case 'Q':
      quit_requested = 1;
      return 1;
    default:
      die("unknown command %c", c->name);
    }
  }
  return 0;
}

static void
process(FILE *in, FILE *out)
{
  char peek[SPACE];
  int have_peek = 0;

  out_stream = out;
  for (;;) {
    if (have_peek) {
      snprintf(pattern, sizeof(pattern), "%s", peek);
      have_peek = 0;
    } else if (fgets(pattern, sizeof(pattern), in) == 0) {
      break;
    } else {
      int len = strlen(pattern);
      if (len && pattern[len - 1] == '\n')
        pattern[len - 1] = 0;
    }
    lineno++;

    // is this the last line? sed's $ needs to know before running
    int last_line = 0;
    if (fgets(peek, sizeof(peek), in) == 0) {
      last_line = 1;
    } else {
      int len = strlen(peek);
      if (len && peek[len - 1] == '\n')
        peek[len - 1] = 0;
      have_peek = 1;
    }

    int r;
    do {
      r = run_cycle(in, last_line);
    } while (r == 3);
    if (r == 0 || r == 4) {
      if (!quiet)
        emit(pattern);
    }
    flush_appends();
    if (r == 2 || r == 4 || quit_requested)
      break;
  }
}

int
main(int argc, char *argv[])
{
  char script[8192];
  int have_script = 0, i = 1;

  script[0] = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-n") == 0)
      quiet = 1;
    else if (strcmp(argv[i], "-E") == 0 || strcmp(argv[i], "-r") == 0)
      extended = 1;
    else if (strcmp(argv[i], "-i") == 0)
      in_place = 1;
    else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
      int n = strlen(script);
      snprintf(script + n, sizeof(script) - n, "%s%s", n ? "\n" : "", argv[++i]);
      have_script = 1;
    } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
      FILE *f = fopen(argv[++i], "r");
      if (f == 0)
        die("%s: %s", argv[i], strerror(errno));
      int n = strlen(script);
      n += fread(script + n, 1, sizeof(script) - n - 1, f);
      script[n] = 0;
      fclose(f);
      have_script = 1;
    } else {
      fprintf(stderr, "usage: sed [-n] [-E] [-i] [-e SCRIPT] [-f FILE] [SCRIPT] [FILE...]\n");
      exit(1);
    }
  }
  if (!have_script) {
    if (i >= argc) {
      fprintf(stderr, "usage: sed [-n] [-E] [-i] [-e SCRIPT] [-f FILE] [SCRIPT] [FILE...]\n");
      exit(1);
    }
    snprintf(script, sizeof(script), "%s", argv[i++]);
  }
  parse_script(script);

  if (i >= argc) {
    process(stdin, stdout);
    exit(exit_status);
  }
  for (; i < argc; i++) {
    FILE *f = fopen(argv[i], "r");
    if (f == 0) {
      fprintf(stderr, "sed: %s: %s\n", argv[i], strerror(errno));
      exit_status = 2;
      continue;
    }
    if (in_place) {
      // the result goes to a temporary and replaces the file, so an
      // interrupted run leaves the original rather than half of it
      char tmp[PATH_MAX];
      snprintf(tmp, sizeof(tmp), "%s.sed%d", argv[i], (int)getpid());
      FILE *out = fopen(tmp, "w");
      if (out == 0) {
        fprintf(stderr, "sed: %s: %s\n", tmp, strerror(errno));
        fclose(f);
        exit_status = 2;
        continue;
      }
      lineno = 0;
      quit_requested = 0;
      for (int k = 0; k < ncmds; k++)
        cmds[k].active = 0;
      process(f, out);
      fclose(out);
      fclose(f);
      if (rename(tmp, argv[i]) < 0) {
        fprintf(stderr, "sed: %s: %s\n", argv[i], strerror(errno));
        unlink(tmp);
        exit_status = 2;
      }
    } else {
      process(f, stdout);
      fclose(f);
      if (quit_requested)
        break;
    }
  }
  exit(exit_status);
}
