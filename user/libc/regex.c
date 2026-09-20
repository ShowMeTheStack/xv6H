//
// <regex.h>: POSIX basic and extended regular expressions.
//
// a pattern is parsed once into a small syntax tree, and the tree is
// compiled into a program for a virtual machine in Thompson's form:
// the only instructions are "match this byte", "match this set of
// bytes", "split into two branches", "jump", "record an offset" and
// "stop". a split is where the automaton stops being deterministic, so
// running the program means running every branch at once. the
// simulation below keeps the list of states the machine could be in
// after the bytes read so far, steps every one of them on the next
// byte, and refuses to put the same state into a list twice. a list is
// therefore never longer than the program, the work per byte is
// bounded by the program's size, and no pattern -- not even the
// (a*)*b that makes a backtracking matcher take exponential time --
// can make matching run away.
//
// submatches ride along with the states: each one carries the offsets
// recorded so far, shared between states by reference counting and
// copied only when a state writes one. POSIX asks for the leftmost
// longest match, so the simulation does not stop at the first match it
// sees; it keeps the best one (earliest start, then longest) and runs
// until every state has died.
//
// back-references are the exception, and the reason this file has a
// second matcher. "\(a*\)\1" is not a regular language, no automaton
// recognises it, and the linear simulation cannot express it. so a
// pattern that uses one is matched by bt() instead, which walks the
// same program with an explicit stack of the branches it has yet to
// try and an undo log for the offsets it has written. that matcher is
// bounded twice over -- it gives up after BT_STEPS steps and after
// BT_STACK pending branches -- and reports REG_ESPACE rather than
// running for ever, which is the price of asking for a back-reference.
//
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stddef.h>

#define RE_DUP_MAX  255        // the largest bound an interval may name
#define MAX_INSTS   200000     // a compiled program larger than this is REG_ESPACE
#define MAX_DEPTH   128        // nesting of \( \), which bounds the parser's recursion
#define BT_STEPS    4000000L   // the backtracker's step budget
#define BT_STACK    65536      // ... and its pending-branch budget

// ---- character sets ----

// a bracket expression, and the shorthands, become one of these: a bit
// per byte, so testing a byte is two instructions however long the
// expression was.
struct cset {
  unsigned char bits[32];
};

static void
set_add(struct cset *s, int c)
{
  s->bits[(unsigned char)c >> 3] |= 1 << (c & 7);
}

static int
set_has(const struct cset *s, int c)
{
  return (s->bits[(unsigned char)c >> 3] >> (c & 7)) & 1;
}

static void
set_del(struct cset *s, int c)
{
  s->bits[(unsigned char)c >> 3] &= ~(1 << (c & 7));
}

// the named classes of the C locale, in the order in_ctype() tests them
static const char *const ctype_names[] = {
  "alpha", "upper", "lower", "digit", "xdigit", "alnum",
  "punct", "space", "print", "graph", "cntrl", "blank",
};

static int
in_ctype(int which, int c)
{
  switch (which) {
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

// ---- the syntax tree ----

enum {
  N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP, N_BOL, N_EOL, N_BREF,
};

struct node {
  int type;
  int c;              // N_CHAR: the byte
  int set;            // N_CLASS: which of the parser's sets
  int min, max;       // N_REP: max is -1 when unbounded
  int grp;            // N_GROUP: which pair of offsets; N_BREF: which group
  struct node *l, *r;
};

struct parser {
  const char *p;      // where the parse has got to
  int cflags;
  int ere;            // REG_EXTENDED
  int ngroup;         // groups opened so far
  int depth;          // groups currently open, so recursion stays bounded
  int err;
  struct node **nodes;  // every node allocated, so a failed parse frees them all
  int nnodes, cnodes;
  struct cset *sets;
  int nsets, csets;
  char closed[10];    // group n has been closed, so \n is a legal back-reference
};

static struct node *
newnode(struct parser *p, int type)
{
  if (p->nnodes == p->cnodes) {
    int nc = p->cnodes ? p->cnodes * 2 : 32;
    struct node **nv = realloc(p->nodes, nc * sizeof(*nv));
    if (nv == 0) {
      p->err = REG_ESPACE;
      return 0;
    }
    p->nodes = nv;
    p->cnodes = nc;
  }
  struct node *n = calloc(1, sizeof(*n));
  if (n == 0) {
    p->err = REG_ESPACE;
    return 0;
  }
  n->type = type;
  n->min = 0;
  n->max = -1;
  p->nodes[p->nnodes++] = n;
  return n;
}

static int
newset(struct parser *p)
{
  if (p->nsets == p->csets) {
    int nc = p->csets ? p->csets * 2 : 8;
    struct cset *ns = realloc(p->sets, nc * sizeof(*ns));
    if (ns == 0) {
      p->err = REG_ESPACE;
      return -1;
    }
    p->sets = ns;
    p->csets = nc;
  }
  memset(&p->sets[p->nsets], 0, sizeof(struct cset));
  return p->nsets++;
}

// a one-byte literal, folded here when the match is to ignore case so
// that neither matcher has to think about it again
static struct node *
charnode(struct parser *p, int c)
{
  struct node *n = newnode(p, N_CHAR);

  if (n)
    n->c = (p->cflags & REG_ICASE) ? tolower((unsigned char)c) : (unsigned char)c;
  return n;
}

// a set as an atom, with the newline taken out of a negated one when
// REG_NEWLINE says a newline ends a line
static struct node *
setnode(struct parser *p, int idx)
{
  struct node *n = newnode(p, N_CLASS);

  if (n)
    n->set = idx;
  return n;
}

// ---- the parser ----

static struct node *parse_alt(struct parser *p);

static int
at_bar(struct parser *p)
{
  if (p->ere)
    return p->p[0] == '|';
  return p->p[0] == '\\' && p->p[1] == '|';  // the GNU spelling in a BRE
}

static int
at_close(struct parser *p)
{
  if (p->ere)
    return p->p[0] == ')';
  return p->p[0] == '\\' && p->p[1] == ')';
}

// the twelve [:name:] classes, plus the [.x.] and [=x=] forms, which
// without other locales can only ever name one character
static int
bracket_special(struct parser *p, struct cset *s, int *single)
{
  char kind = p->p[1];
  const char *q = p->p + 2;
  const char *e = q;

  *single = -1;
  while (*e && !(e[0] == kind && e[1] == ']'))
    e++;
  if (*e == 0) {
    p->err = (kind == ':') ? REG_ECTYPE : REG_ECOLLATE;
    return -1;
  }
  size_t len = e - q;
  if (kind == ':') {
    for (int i = 0; i < (int)(sizeof(ctype_names) / sizeof(ctype_names[0])); i++) {
      if (strlen(ctype_names[i]) == len && memcmp(q, ctype_names[i], len) == 0) {
        for (int c = 0; c < 256; c++)
          if (in_ctype(i, c))
            set_add(s, c);
        p->p = e + 2;
        return 0;
      }
    }
    p->err = REG_ECTYPE;
    return -1;
  }
  // a collating element or an equivalence class: one character, or nothing this locale knows
  if (len != 1) {
    p->err = REG_ECOLLATE;
    return -1;
  }
  *single = (unsigned char)*q;
  p->p = e + 2;
  return 0;
}

static struct node *
parse_bracket(struct parser *p)
{
  int idx = newset(p);

  if (idx < 0)
    return 0;
  struct cset *s = &p->sets[idx];
  int neg = 0;

  p->p++;  // the '['
  if (*p->p == '^') {
    neg = 1;
    p->p++;
  }
  // a ']' first in the list is the character itself, not the end of the list
  int first = 1;
  for (;;) {
    if (*p->p == 0) {
      p->err = REG_EBRACK;
      return 0;
    }
    if (*p->p == ']' && !first)
      break;
    first = 0;

    int lo;
    if (p->p[0] == '[' && (p->p[1] == ':' || p->p[1] == '.' || p->p[1] == '=')) {
      int single;
      if (bracket_special(p, s, &single) < 0)
        return 0;
      if (single < 0) {
        // a named class cannot be one end of a range
        if (p->p[0] == '-' && p->p[1] != ']' && p->p[1] != 0) {
          p->err = REG_ERANGE;
          return 0;
        }
        continue;
      }
      lo = single;
    } else {
      // POSIX gives a backslash no meaning inside a bracket expression
      lo = (unsigned char)*p->p++;
    }

    if (p->p[0] == '-' && p->p[1] != ']' && p->p[1] != 0) {
      p->p++;
      int hi;
      if (p->p[0] == '[' && (p->p[1] == ':' || p->p[1] == '.' || p->p[1] == '=')) {
        int single;
        if (bracket_special(p, s, &single) < 0)
          return 0;
        if (single < 0) {
          p->err = REG_ERANGE;
          return 0;
        }
        hi = single;
      } else {
        hi = (unsigned char)*p->p++;
      }
      if (hi < lo) {
        p->err = REG_ERANGE;
        return 0;
      }
      for (int c = lo; c <= hi; c++)
        set_add(s, c);
    } else {
      set_add(s, lo);
    }
  }
  p->p++;  // the ']'

  // case folding and negation both work on the finished set, which is
  // the only order that makes [^a] ignore case the way it should.
  if (p->cflags & REG_ICASE) {
    for (int c = 0; c < 256; c++) {
      if (set_has(s, c)) {
        set_add(s, tolower(c));
        set_add(s, toupper(c));
      }
    }
  }
  if (neg) {
    for (int i = 0; i < 32; i++)
      s->bits[i] = ~s->bits[i];
  }
  if ((p->cflags & REG_NEWLINE) && neg)
    set_del(s, '\n');
  set_del(s, 0);  // the subject is a C string, so a NUL is never there to match
  return setnode(p, idx);
}

// \w, \W, \s and \S: not POSIX, but every program that writes patterns
// by hand expects them, and they cost one set each.
static struct node *
shorthand(struct parser *p, int c)
{
  int idx = newset(p);

  if (idx < 0)
    return 0;
  struct cset *s = &p->sets[idx];
  int word = (c == 'w' || c == 'W');
  for (int ch = 1; ch < 256; ch++) {
    if (word ? (isalnum(ch) || ch == '_') : isspace(ch) != 0)
      set_add(s, ch);
  }
  if (c == 'W' || c == 'S') {
    for (int i = 0; i < 32; i++)
      s->bits[i] = ~s->bits[i];
    set_del(s, 0);
  }
  return setnode(p, idx);
}

// in a BRE a '$' is an anchor only at the very end of the pattern or of
// a branch; anywhere else it is the character itself.
static int
bre_dollar_anchors(struct parser *p)
{
  const char *q = p->p + 1;

  if (*q == 0)
    return 1;
  return q[0] == '\\' && (q[1] == ')' || q[1] == '|');
}

static struct node *
parse_escape(struct parser *p)
{
  int c = p->p[1];

  if (c == 0) {
    p->err = REG_EESCAPE;
    return 0;
  }
  if (c >= '1' && c <= '9') {
    int d = c - '0';
    if (!p->closed[d]) {
      p->err = REG_ESUBREG;
      return 0;
    }
    p->p += 2;
    struct node *n = newnode(p, N_BREF);
    if (n)
      n->grp = d;
    return n;
  }
  if (c == 'w' || c == 'W' || c == 's' || c == 'S') {
    p->p += 2;
    return shorthand(p, c);
  }
  p->p += 2;
  switch (c) {
  case 'n': return charnode(p, '\n');
  case 't': return charnode(p, '\t');
  case 'r': return charnode(p, '\r');
  case 'f': return charnode(p, '\f');
  case 'v': return charnode(p, '\v');
  }
  return charnode(p, c);
}

// `first` says this atom begins the pattern or a branch, which is what
// decides whether a BRE's '^' and '*' are operators or characters.
static struct node *
parse_atom(struct parser *p, int first)
{
  int c = (unsigned char)*p->p;

  switch (c) {
  case '.':
    p->p++;
    return newnode(p, N_ANY);
  case '[':
    return parse_bracket(p);
  case '^':
    if (p->ere || first) {
      p->p++;
      return newnode(p, N_BOL);
    }
    p->p++;
    return charnode(p, '^');
  case '$':
    if (p->ere || bre_dollar_anchors(p)) {
      p->p++;
      return newnode(p, N_EOL);
    }
    p->p++;
    return charnode(p, '$');
  case '*':
    // reachable only where nothing precedes: an ERE calls that an error
    // and a BRE calls it a literal asterisk.
    if (p->ere) {
      p->err = REG_BADRPT;
      return 0;
    }
    p->p++;
    return charnode(p, '*');
  case '+':
  case '?':
    if (p->ere) {
      p->err = REG_BADRPT;
      return 0;
    }
    p->p++;
    return charnode(p, c);
  case '{':
    if (p->ere && isdigit((unsigned char)p->p[1])) {
      p->err = REG_BADRPT;
      return 0;
    }
    p->p++;
    return charnode(p, '{');
  case '(':
    if (!p->ere) {
      p->p++;
      return charnode(p, '(');
    }
    break;
  case '\\':
    if (!p->ere && p->p[1] == '(')
      break;
    return parse_escape(p);
  default:
    p->p++;
    return charnode(p, c);
  }

  // what is left is a group: '(' in an ERE, '\(' in a BRE.
  p->p += p->ere ? 1 : 2;
  if (++p->depth > MAX_DEPTH) {
    p->err = REG_ESPACE;
    return 0;
  }
  int idx = ++p->ngroup;
  struct node *body = parse_alt(p);
  p->depth--;
  if (body == 0)
    return 0;
  if (!at_close(p)) {
    p->err = REG_EPAREN;
    return 0;
  }
  p->p += p->ere ? 1 : 2;
  if (idx < 10)
    p->closed[idx] = 1;
  struct node *n = newnode(p, N_GROUP);
  if (n == 0)
    return 0;
  n->grp = idx;
  n->l = body;
  return n;
}

// {n}, {n,} or {n,m}, spelled \{n,m\} in a BRE. returns 1 having
// consumed one, 0 when this is not an interval at all, -1 on an error.
static int
parse_interval(struct parser *p, int *min, int *max)
{
  const char *q = p->p;

  if (p->ere) {
    if (*q != '{')
      return 0;
    q++;
  } else {
    if (q[0] != '\\' || q[1] != '{')
      return 0;
    q += 2;
  }
  if (!isdigit((unsigned char)*q)) {
    // an ERE's brace that starts no interval is an ordinary character
    if (p->ere)
      return 0;
    p->err = REG_BADBR;
    return -1;
  }
  long lo = 0, hi;
  while (isdigit((unsigned char)*q)) {
    lo = lo * 10 + (*q++ - '0');
    if (lo > RE_DUP_MAX) {
      p->err = REG_BADBR;
      return -1;
    }
  }
  hi = lo;
  if (*q == ',') {
    q++;
    if (isdigit((unsigned char)*q)) {
      hi = 0;
      while (isdigit((unsigned char)*q)) {
        hi = hi * 10 + (*q++ - '0');
        if (hi > RE_DUP_MAX) {
          p->err = REG_BADBR;
          return -1;
        }
      }
    } else {
      hi = -1;
    }
  }
  if (p->ere) {
    if (*q != '}') {
      p->err = REG_EBRACE;
      return -1;
    }
    q++;
  } else {
    if (q[0] != '\\' || q[1] != '}') {
      p->err = REG_EBRACE;
      return -1;
    }
    q += 2;
  }
  if (hi >= 0 && hi < lo) {
    p->err = REG_BADBR;
    return -1;
  }
  *min = (int)lo;
  *max = (int)hi;
  p->p = q;
  return 1;
}

// an atom and every repetition operator stacked on it
static struct node *
parse_piece(struct parser *p, int first)
{
  struct node *a = parse_atom(p, first);

  if (a == 0)
    return 0;
  // a BRE never repeats its leading '^': "^*" is an anchor and then a
  // literal asterisk, which is what every other implementation does.
  if (!p->ere && a->type == N_BOL)
    return a;
  for (;;) {
    int min, max;
    int c = (unsigned char)*p->p;

    if (p->ere && (c == '*' || c == '+' || c == '?')) {
      min = (c == '+');
      max = (c == '?') ? 1 : -1;
      p->p++;
    } else if (!p->ere && c == '*') {
      min = 0;
      max = -1;
      p->p++;
    } else if (!p->ere && c == '\\' && (p->p[1] == '+' || p->p[1] == '?')) {
      min = (p->p[1] == '+');
      max = (p->p[1] == '?') ? 1 : -1;
      p->p += 2;
    } else {
      int r = parse_interval(p, &min, &max);
      if (r < 0)
        return 0;
      if (r == 0)
        break;
    }
    struct node *n = newnode(p, N_REP);
    if (n == 0)
      return 0;
    n->l = a;
    n->min = min;
    n->max = max;
    a = n;
  }
  return a;
}

// a concatenation, folded to the right so that the compiler can walk it
// with a loop instead of one stack frame per character.
static struct node *
parse_cat(struct parser *p)
{
  struct node **v = 0;
  int n = 0, cap = 0;
  struct node *r = 0;

  while (*p->p && !at_bar(p) && !at_close(p)) {
    struct node *piece = parse_piece(p, n == 0);
    if (piece == 0)
      goto out;
    if (n == cap) {
      int nc = cap ? cap * 2 : 8;
      struct node **nv = realloc(v, nc * sizeof(*nv));
      if (nv == 0) {
        p->err = REG_ESPACE;
        goto out;
      }
      v = nv;
      cap = nc;
    }
    v[n++] = piece;
  }
  if (n == 0) {
    r = newnode(p, N_EMPTY);
  } else {
    r = v[n - 1];
    for (int i = n - 2; i >= 0; i--) {
      struct node *c = newnode(p, N_CAT);
      if (c == 0) {
        r = 0;
        break;
      }
      c->l = v[i];
      c->r = r;
      r = c;
    }
  }
out:
  free(v);
  return r;
}

// alternation, folded to the right for the same reason
static struct node *
parse_alt(struct parser *p)
{
  struct node **v = 0;
  int n = 0, cap = 0;
  struct node *r = 0;

  for (;;) {
    struct node *br = parse_cat(p);
    if (br == 0)
      goto out;
    if (n == cap) {
      int nc = cap ? cap * 2 : 8;
      struct node **nv = realloc(v, nc * sizeof(*nv));
      if (nv == 0) {
        p->err = REG_ESPACE;
        goto out;
      }
      v = nv;
      cap = nc;
    }
    v[n++] = br;
    if (!at_bar(p))
      break;
    p->p += p->ere ? 1 : 2;
  }
  r = v[n - 1];
  for (int i = n - 2; i >= 0; i--) {
    struct node *a = newnode(p, N_ALT);
    if (a == 0) {
      r = 0;
      break;
    }
    a->l = v[i];
    a->r = r;
    r = a;
  }
out:
  free(v);
  return r;
}

// ---- the program ----

enum {
  I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_MARK, I_BOL, I_EOL, I_BREF, I_MATCH,
};

struct inst {
  int op;
  int c;      // I_CHAR: the byte
  int x, y;   // the targets of a split or a jump; the slot of a save or a mark;
              // the set of a class; the group of a back-reference
  int loop;   // the empty-iteration guard a back-edge belongs to, or -1
};

struct prog {
  struct inst *ins;
  int n;
  struct cset *sets;
  int nsets;
  int nsave;    // 2 * (groups + 1)
  int nloop;
  int icase, newline;
  int hasbref;
};

struct comp {
  struct prog *pr;
  int cap;
  int err;
};

static int
emit(struct comp *c, int op, int x, int y)
{
  struct prog *pr = c->pr;

  if (c->err)
    return -1;
  if (pr->n == c->cap) {
    int nc = c->cap ? c->cap * 2 : 64;
    if (nc > MAX_INSTS) {
      c->err = REG_ESPACE;
      return -1;
    }
    struct inst *ni = realloc(pr->ins, nc * sizeof(*ni));
    if (ni == 0) {
      c->err = REG_ESPACE;
      return -1;
    }
    pr->ins = ni;
    c->cap = nc;
  }
  struct inst *in = &pr->ins[pr->n];
  in->op = op;
  in->c = 0;
  in->x = x;
  in->y = y;
  in->loop = -1;
  return pr->n++;
}

static void compile(struct comp *c, struct node *n);

// the optional tail of an interval: (body (body (body)?)?)? , nested so
// that the greedy branch of each copy is tried before the next one is
// even reached.
static void
compile_opt(struct comp *c, struct node *body, int count)
{
  if (count <= 0 || c->err)
    return;
  int sp = emit(c, I_SPLIT, 0, 0);
  if (sp < 0)
    return;
  c->pr->ins[sp].x = c->pr->n;
  compile(c, body);
  compile_opt(c, body, count - 1);
  if (c->err)
    return;
  c->pr->ins[sp].y = c->pr->n;
}

// a repetition: the mandatory copies first, then either an unbounded
// loop or the nested optional copies. a loop gets a mark at the top of
// its body and a guard on its back edge, which is what stops the
// backtracker from spinning on a body that can match nothing.
static void
compile_rep(struct comp *c, struct node *n)
{
  for (int i = 0; i < n->min && !c->err; i++)
    compile(c, n->l);
  if (c->err)
    return;
  if (n->max < 0) {
    int k = c->pr->nloop++;
    int sp = emit(c, I_SPLIT, 0, 0);
    if (sp < 0)
      return;
    c->pr->ins[sp].x = c->pr->n;
    emit(c, I_MARK, k, 0);
    compile(c, n->l);
    int jp = emit(c, I_JMP, sp, 0);
    if (jp < 0)
      return;
    c->pr->ins[jp].loop = k;
    c->pr->ins[sp].y = c->pr->n;
  } else {
    compile_opt(c, n->l, n->max - n->min);
  }
}

static void
compile(struct comp *c, struct node *n)
{
  // concatenation and alternation are both right-leaning chains, and
  // both are walked here rather than recursed into, so that a long
  // pattern costs no stack.
  for (;;) {
    if (c->err || n == 0)
      return;
    switch (n->type) {
    case N_EMPTY:
      return;
    case N_CHAR:
      {
        int pc = emit(c, I_CHAR, 0, 0);
        if (pc >= 0)
          c->pr->ins[pc].c = n->c;
      }
      return;
    case N_ANY:
      emit(c, I_ANY, 0, 0);
      return;
    case N_CLASS:
      emit(c, I_CLASS, n->set, 0);
      return;
    case N_BOL:
      emit(c, I_BOL, 0, 0);
      return;
    case N_EOL:
      emit(c, I_EOL, 0, 0);
      return;
    case N_BREF:
      c->pr->hasbref = 1;
      emit(c, I_BREF, n->grp, 0);
      return;
    case N_GROUP:
      emit(c, I_SAVE, 2 * n->grp, 0);
      compile(c, n->l);
      emit(c, I_SAVE, 2 * n->grp + 1, 0);
      return;
    case N_REP:
      compile_rep(c, n);
      return;
    case N_CAT:
      compile(c, n->l);
      n = n->r;
      continue;
    case N_ALT:
      {
        // one split per branch, and every branch jumps to the same end
        int jmps[8], njmp = 0;
        int *more = 0, nmore = 0, cmore = 0;
        while (n->type == N_ALT && !c->err) {
          int sp = emit(c, I_SPLIT, 0, 0);
          if (sp < 0)
            break;
          c->pr->ins[sp].x = c->pr->n;
          compile(c, n->l);
          int jp = emit(c, I_JMP, 0, 0);
          if (jp < 0)
            break;
          if (njmp < 8) {
            jmps[njmp++] = jp;
          } else {
            if (nmore == cmore) {
              int nc = cmore ? cmore * 2 : 16;
              int *nv = realloc(more, nc * sizeof(*nv));
              if (nv == 0) {
                c->err = REG_ESPACE;
                break;
              }
              more = nv;
              cmore = nc;
            }
            more[nmore++] = jp;
          }
          if (c->err)
            break;
          c->pr->ins[sp].y = c->pr->n;
          n = n->r;
        }
        if (!c->err)
          compile(c, n);
        if (!c->err) {
          for (int i = 0; i < njmp; i++)
            c->pr->ins[jmps[i]].x = c->pr->n;
          for (int i = 0; i < nmore; i++)
            c->pr->ins[more[i]].x = c->pr->n;
        }
        free(more);
      }
      return;
    default:
      return;
    }
  }
}

// ---- regcomp ----

static void
free_parser(struct parser *p)
{
  for (int i = 0; i < p->nnodes; i++)
    free(p->nodes[i]);
  free(p->nodes);
}

int
regcomp(regex_t *re, const char *pattern, int cflags)
{
  struct parser p;
  struct comp c;

  if (re == 0 || pattern == 0)
    return REG_BADPAT;
  memset(&p, 0, sizeof(p));
  memset(&c, 0, sizeof(c));
  re->re_nsub = 0;
  re->__prog = 0;
  re->__cflags = cflags;

  p.p = pattern;
  p.cflags = cflags;
  p.ere = (cflags & REG_EXTENDED) != 0;

  struct node *root = parse_alt(&p);
  if (root == 0 || *p.p != 0) {
    int err = p.err;
    if (err == 0)
      err = at_close(&p) ? REG_EPAREN : REG_BADPAT;
    free_parser(&p);
    free(p.sets);
    return err;
  }

  struct prog *pr = calloc(1, sizeof(*pr));
  if (pr == 0) {
    free_parser(&p);
    free(p.sets);
    return REG_ESPACE;
  }
  pr->sets = p.sets;
  pr->nsets = p.nsets;
  pr->nsave = 2 * (p.ngroup + 1);
  pr->icase = (cflags & REG_ICASE) != 0;
  pr->newline = (cflags & REG_NEWLINE) != 0;
  c.pr = pr;

  // the whole pattern is one group, so that the overall match records
  // its own offsets exactly the way a subexpression does.
  emit(&c, I_SAVE, 0, 0);
  compile(&c, root);
  emit(&c, I_SAVE, 1, 0);
  emit(&c, I_MATCH, 0, 0);
  free_parser(&p);
  if (c.err) {
    free(pr->ins);
    free(pr->sets);
    free(pr);
    return c.err;
  }
  re->re_nsub = p.ngroup;
  re->__prog = pr;
  return 0;
}

void
regfree(regex_t *re)
{
  if (re == 0 || re->__prog == 0)
    return;
  struct prog *pr = re->__prog;
  free(pr->ins);
  free(pr->sets);
  free(pr);
  re->__prog = 0;
  re->re_nsub = 0;
}

static const char *const errmsg[] = {
  "Success",
  "No match",
  "Invalid regular expression",
  "Invalid collation character",
  "Invalid character class name",
  "Trailing backslash",
  "Invalid back reference",
  "Unmatched [, [^, [:, [. or [=",
  "Unmatched ( or \\(",
  "Unmatched \\{",
  "Invalid content of \\{\\}",
  "Invalid range end",
  "Out of memory",
  "Invalid preceding regular expression",
};

size_t
regerror(int code, const regex_t *re, char *errbuf, size_t errbuf_size)
{
  const char *msg = "Unknown error";

  (void)re;
  if (code >= 0 && code < (int)(sizeof(errmsg) / sizeof(errmsg[0])))
    msg = errmsg[code];
  size_t len = strlen(msg) + 1;
  if (errbuf && errbuf_size) {
    size_t copy = len < errbuf_size ? len : errbuf_size;
    memcpy(errbuf, msg, copy - 1);
    errbuf[copy - 1] = 0;
  }
  return len;
}

// ---- what both matchers share ----

static int
inst_matches(const struct prog *pr, const struct inst *in, unsigned char ch)
{
  switch (in->op) {
  case I_CHAR:
    return pr->icase ? tolower(ch) == in->c : (int)ch == in->c;
  case I_ANY:
    return !(pr->newline && ch == '\n');
  case I_CLASS:
    return set_has(&pr->sets[in->x], ch);
  }
  return 0;
}

static int
at_bol(const struct prog *pr, const char *s, const char *sp, int eflags)
{
  if (sp == s)
    return !(eflags & REG_NOTBOL);
  return pr->newline && sp[-1] == '\n';
}

static int
at_eol(const struct prog *pr, const char *sp, const char *end, int eflags)
{
  if (sp == end)
    return !(eflags & REG_NOTEOL);
  return pr->newline && *sp == '\n';
}

// ---- the linear simulation ----

// the offsets a state has recorded. states share one of these until one
// of them writes, which keeps the copying down to what really differs.
struct caps {
  int ref;
  struct caps *next;   // only while on the free list
  regoff_t s[];
};

struct thread {
  int pc;
  struct caps *c;
};

struct list {
  struct thread *t;
  int n;
};

struct vm {
  const struct prog *pr;
  const char *start, *end;
  int eflags;
  int nsave;
  int *gen;            // the step each program counter was last added in
  int cur;
  struct thread *stk;  // the stack the epsilon closure walks with
  struct caps *freelist;
  int oom;
};

static struct caps *
capalloc(struct vm *m)
{
  struct caps *c = m->freelist;

  if (c) {
    m->freelist = c->next;
  } else {
    c = malloc(sizeof(*c) + m->nsave * sizeof(regoff_t));
    if (c == 0) {
      m->oom = 1;
      return 0;
    }
  }
  c->ref = 1;
  c->next = 0;
  return c;
}

static void
capunref(struct vm *m, struct caps *c)
{
  if (c && --c->ref == 0) {
    c->next = m->freelist;
    m->freelist = c;
  }
}

// about to write: take a private copy unless this state is the only
// owner already.
static struct caps *
capunshare(struct vm *m, struct caps *c)
{
  if (c->ref == 1)
    return c;
  struct caps *n = capalloc(m);
  if (n == 0)
    return 0;
  memcpy(n->s, c->s, m->nsave * sizeof(regoff_t));
  c->ref--;
  return n;
}

// follow every epsilon edge out of pc and put the states that want a
// byte (or that have finished) into l, in priority order. the gen array
// makes sure a state is added at most once per position, which is what
// bounds the whole simulation.
static void
addthread(struct vm *m, struct list *l, int pc, const char *sp, struct caps *c)
{
  int top = 0;

  m->stk[top].pc = pc;
  m->stk[top].c = c;
  top++;
  while (top > 0) {
    top--;
    int p = m->stk[top].pc;
    struct caps *cc = m->stk[top].c;

    if (m->gen[p] == m->cur) {
      capunref(m, cc);
      continue;
    }
    m->gen[p] = m->cur;
    const struct inst *in = &m->pr->ins[p];
    switch (in->op) {
    case I_JMP:
      m->stk[top].pc = in->x;
      m->stk[top].c = cc;
      top++;
      break;
    case I_SPLIT:
      // the second branch goes on first, so the first one comes off
      // first and keeps the higher priority
      cc->ref++;
      m->stk[top].pc = in->y;
      m->stk[top].c = cc;
      top++;
      m->stk[top].pc = in->x;
      m->stk[top].c = cc;
      top++;
      break;
    case I_MARK:
      m->stk[top].pc = p + 1;
      m->stk[top].c = cc;
      top++;
      break;
    case I_SAVE: {
      struct caps *nc = capunshare(m, cc);
      if (nc == 0) {
        capunref(m, cc);   // out of memory: drop this state rather than lose it
        break;
      }
      nc->s[in->x] = sp - m->start;
      m->stk[top].pc = p + 1;
      m->stk[top].c = nc;
      top++;
      break;
    }
    case I_BOL:
      if (at_bol(m->pr, m->start, sp, m->eflags)) {
        m->stk[top].pc = p + 1;
        m->stk[top].c = cc;
        top++;
      } else {
        capunref(m, cc);
      }
      break;
    case I_EOL:
      if (at_eol(m->pr, sp, m->end, m->eflags)) {
        m->stk[top].pc = p + 1;
        m->stk[top].c = cc;
        top++;
      } else {
        capunref(m, cc);
      }
      break;
    default:
      l->t[l->n].pc = p;
      l->t[l->n].c = cc;
      l->n++;
      break;
    }
  }
}

// earlier start wins; then the longer match, which is what POSIX means
// by leftmost longest.
static int
better(const struct caps *a, const struct caps *b)
{
  if (a->s[0] != b->s[0])
    return a->s[0] < b->s[0];
  return a->s[1] > b->s[1];
}

static int
pike(const struct prog *pr, const char *s, int eflags, regoff_t *out)
{
  struct vm m;
  struct list la, lb;
  struct list *cl = &la, *nl = &lb;
  struct caps *best = 0;
  int rc = REG_NOMATCH;

  memset(&m, 0, sizeof(m));
  m.pr = pr;
  m.start = s;
  m.end = s + strlen(s);
  m.eflags = eflags;
  m.nsave = pr->nsave;
  m.gen = malloc(pr->n * sizeof(int));
  la.t = malloc(pr->n * sizeof(struct thread));
  lb.t = malloc(pr->n * sizeof(struct thread));
  m.stk = malloc((2 * pr->n + 2) * sizeof(struct thread));
  if (m.gen == 0 || la.t == 0 || lb.t == 0 || m.stk == 0) {
    rc = REG_ESPACE;
    goto done;
  }
  for (int i = 0; i < pr->n; i++)
    m.gen[i] = -1;
  la.n = lb.n = 0;
  m.cur = 0;

  for (const char *sp = s;; sp++) {
    // a new state starting here, but only while no match is in hand:
    // once one is, nothing starting later can beat it.
    if (best == 0 && !m.oom) {
      struct caps *c = capalloc(&m);
      if (c == 0)
        break;
      for (int i = 0; i < m.nsave; i++)
        c->s[i] = -1;
      addthread(&m, cl, 0, sp, c);
    }
    // with a match in hand and nothing left alive there is nothing more
    // to learn; without one, an empty list only means this position
    // starts nothing, and the next one still might.
    if (cl->n == 0 && best)
      break;

    m.cur++;
    nl->n = 0;
    for (int i = 0; i < cl->n; i++) {
      int pc = cl->t[i].pc;
      struct caps *c = cl->t[i].c;
      const struct inst *in = &pr->ins[pc];

      if (best && c->s[0] > best->s[0]) {
        capunref(&m, c);   // it started too late to ever win
        continue;
      }
      if (in->op == I_MATCH) {
        if (best == 0 || better(c, best)) {
          capunref(&m, best);
          best = c;        // the thread's reference becomes the winner's
        } else {
          capunref(&m, c);
        }
        continue;
      }
      if (sp < m.end && inst_matches(pr, in, (unsigned char)*sp))
        addthread(&m, nl, pc + 1, sp + 1, c);
      else
        capunref(&m, c);
    }
    cl->n = 0;
    struct list *tmp = cl;
    cl = nl;
    nl = tmp;
    if (sp >= m.end)
      break;
  }

  for (int i = 0; i < cl->n; i++)
    capunref(&m, cl->t[i].c);
  for (int i = 0; i < nl->n; i++)
    capunref(&m, nl->t[i].c);
  if (m.oom) {
    rc = REG_ESPACE;
  } else if (best) {
    memcpy(out, best->s, m.nsave * sizeof(regoff_t));
    rc = 0;
  }
  capunref(&m, best);

done:
  while (m.freelist) {
    struct caps *c = m.freelist;
    m.freelist = c->next;
    free(c);
  }
  free(m.gen);
  free(la.t);
  free(lb.t);
  free(m.stk);
  return rc;
}

// ---- the bounded backtracker, for patterns with back-references ----

struct btframe {
  int pc;
  const char *sp;
  int un;          // how much of the undo log belongs to this branch
};

struct btundo {
  int slot;        // an offset slot, or ~k for the guard of loop k
  regoff_t old;
};

struct btctx {
  const struct prog *pr;
  const char *start, *end;
  int eflags;
  regoff_t *caps, *best;
  regoff_t *loopsp;
  struct btframe *fr;
  int nfr, cfr;
  struct btundo *un;
  int nun, cun;
  long steps;
  int over;        // a budget ran out
};

static int
push_frame(struct btctx *b, int pc, const char *sp)
{
  if (b->nfr == b->cfr) {
    int nc = b->cfr ? b->cfr * 2 : 128;
    if (nc > BT_STACK)
      return 0;
    struct btframe *nf = realloc(b->fr, nc * sizeof(*nf));
    if (nf == 0)
      return 0;
    b->fr = nf;
    b->cfr = nc;
  }
  b->fr[b->nfr].pc = pc;
  b->fr[b->nfr].sp = sp;
  b->fr[b->nfr].un = b->nun;
  b->nfr++;
  return 1;
}

static int
push_undo(struct btctx *b, int slot, regoff_t old)
{
  if (b->nun == b->cun) {
    int nc = b->cun ? b->cun * 2 : 128;
    if (nc > BT_STACK * 2)
      return 0;
    struct btundo *nu = realloc(b->un, nc * sizeof(*nu));
    if (nu == 0)
      return 0;
    b->un = nu;
    b->cun = nc;
  }
  b->un[b->nun].slot = slot;
  b->un[b->nun].old = old;
  b->nun++;
  return 1;
}

static void
unwind(struct btctx *b, int to)
{
  while (b->nun > to) {
    struct btundo *u = &b->un[--b->nun];
    if (u->slot >= 0)
      b->caps[u->slot] = u->old;
    else
      b->loopsp[~u->slot] = u->old;
  }
}

// try every way the program can match starting at `from`, keeping the
// longest. returns 1 with b->best filled in, 0 for no match, -1 when a
// budget ran out before either answer was certain.
static int
bt(struct btctx *b, const char *from)
{
  const struct prog *pr = b->pr;
  const char *sp = from;
  int pc = 0;
  int found = 0;

  b->nfr = 0;
  b->nun = 0;
  for (int i = 0; i < pr->nsave; i++)
    b->caps[i] = -1;
  for (int i = 0; i < pr->nloop; i++)
    b->loopsp[i] = -1;

  for (;;) {
    if (b->steps-- <= 0) {
      b->over = 1;
      return found ? 1 : -1;
    }
    const struct inst *in = &pr->ins[pc];
    switch (in->op) {
    case I_CHAR:
    case I_ANY:
    case I_CLASS:
      if (sp < b->end && inst_matches(pr, in, (unsigned char)*sp)) {
        sp++;
        pc++;
        continue;
      }
      break;
    case I_BOL:
      if (at_bol(pr, b->start, sp, b->eflags)) {
        pc++;
        continue;
      }
      break;
    case I_EOL:
      if (at_eol(pr, sp, b->end, b->eflags)) {
        pc++;
        continue;
      }
      break;
    case I_SAVE:
      if (!push_undo(b, in->x, b->caps[in->x])) {
        b->over = 1;
        return found ? 1 : -1;
      }
      b->caps[in->x] = sp - b->start;
      pc++;
      continue;
    case I_MARK:
      if (!push_undo(b, ~in->x, b->loopsp[in->x])) {
        b->over = 1;
        return found ? 1 : -1;
      }
      b->loopsp[in->x] = sp - b->start;
      pc++;
      continue;
    case I_JMP:
      // a back edge whose body matched nothing would spin for ever
      if (in->loop >= 0 && b->loopsp[in->loop] == sp - b->start)
        break;
      pc = in->x;
      continue;
    case I_SPLIT:
      if (in->loop >= 0 && b->loopsp[in->loop] == sp - b->start) {
        pc = in->y;
        continue;
      }
      if (!push_frame(b, in->y, sp)) {
        b->over = 1;
        return found ? 1 : -1;
      }
      pc = in->x;
      continue;
    case I_BREF: {
      regoff_t so = b->caps[2 * in->x], eo = b->caps[2 * in->x + 1];
      if (so < 0 || eo < 0)
        break;   // a group that never matched cannot be referred to
      regoff_t len = eo - so;
      if (b->end - sp < len)
        break;
      int ok = 1;
      for (regoff_t i = 0; i < len && ok; i++) {
        unsigned char a = b->start[so + i], c = sp[i];
        ok = pr->icase ? tolower(a) == tolower(c) : a == c;
      }
      if (!ok)
        break;
      sp += len;
      pc++;
      continue;
    }
    case I_MATCH:
      // POSIX wants the longest, so record this one and carry on
      if (!found || b->caps[1] > b->best[1]) {
        memcpy(b->best, b->caps, pr->nsave * sizeof(regoff_t));
        found = 1;
      }
      break;
    }

    // this branch failed, or was taken as far as it goes: go back to
    // the most recent split and try its other side.
    if (b->nfr == 0)
      return found;
    struct btframe *f = &b->fr[--b->nfr];
    unwind(b, f->un);
    pc = f->pc;
    sp = f->sp;
  }
}

static int
backtrack(const struct prog *pr, const char *s, int eflags, regoff_t *out)
{
  struct btctx b;
  const char *end = s + strlen(s);
  int rc = REG_NOMATCH;

  memset(&b, 0, sizeof(b));
  b.pr = pr;
  b.start = s;
  b.end = end;
  b.eflags = eflags;
  b.steps = BT_STEPS;
  b.caps = malloc(pr->nsave * sizeof(regoff_t));
  b.best = malloc(pr->nsave * sizeof(regoff_t));
  b.loopsp = malloc((pr->nloop ? pr->nloop : 1) * sizeof(regoff_t));
  if (b.caps == 0 || b.best == 0 || b.loopsp == 0) {
    rc = REG_ESPACE;
    goto done;
  }
  // leftmost: the first start that matches at all is the right one
  for (const char *from = s;; from++) {
    int r = bt(&b, from);
    if (r > 0) {
      memcpy(out, b.best, pr->nsave * sizeof(regoff_t));
      rc = 0;
      break;
    }
    if (r < 0) {
      rc = REG_ESPACE;
      break;
    }
    if (from >= end)
      break;
  }

done:
  free(b.caps);
  free(b.best);
  free(b.loopsp);
  free(b.fr);
  free(b.un);
  return rc;
}

int
regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t *pmatch, int eflags)
{
  if (re == 0 || re->__prog == 0 || string == 0)
    return REG_BADPAT;
  const struct prog *pr = re->__prog;
  regoff_t *caps = malloc(pr->nsave * sizeof(regoff_t));
  if (caps == 0)
    return REG_ESPACE;
  for (int i = 0; i < pr->nsave; i++)
    caps[i] = -1;

  int rc = pr->hasbref ? backtrack(pr, string, eflags, caps)
                       : pike(pr, string, eflags, caps);
  if (rc == 0 && pmatch && !(re->__cflags & REG_NOSUB)) {
    for (size_t i = 0; i < nmatch; i++) {
      if (2 * i + 1 < (size_t)pr->nsave) {
        pmatch[i].rm_so = caps[2 * i];
        pmatch[i].rm_eo = caps[2 * i + 1];
      } else {
        pmatch[i].rm_so = -1;
        pmatch[i].rm_eo = -1;
      }
    }
  }
  free(caps);
  return rc;
}
