//
// awk: patterns and the actions they call for.
//
//   awk [-F SEP] [-v NAME=VALUE] 'PROGRAM' [FILE...]
//   awk [-F SEP] [-v NAME=VALUE] -f PROGFILE [FILE...]
//
// a program is a list of rules: an optional pattern, an optional
// action in braces, and the rule fires when the pattern is true of the
// record being read. BEGIN and END are patterns that are true before
// the first record and after the last. A rule with no action prints
// the record; a rule with no pattern fires for every one.
//
// the language here is POSIX awk: numbers and strings that convert
// into one another as they are used, fields $1..$NF and $0 for the
// whole record, associative arrays, the control statements, user
// functions, and the built-ins. what is left out is what POSIX leaves
// out: no regular expressions as first-class values, no local scope
// but a function's own parameters.
//
// the shape is the usual one for a language this size: a lexer, a
// recursive-descent parser building a tree, and a walker that runs it.
//
#include "user/user.h"
#include <regex.h>
#include <math.h>

// ---- values ----
//
// a value is a number, a string, or both: awk decides which when it is
// used, and a field read from input is "a string that may be a number"
// until something asks.

struct cell {
  double num;
  char *str;      // 0 when only the number is valid
  int isnum;      // the number is valid
  int strnum;     // it came from input and looks like a number
};

struct array;

struct var {
  struct cell v;
  struct array *arr; // when it is used as one
};

#define HASHSIZE 101

struct anode {
  char *key;
  struct var val;
  struct anode *next;
};

struct array {
  struct anode *tab[HASHSIZE];
  int n;
};

// ---- the tree ----

enum ntype {
  N_NUM, N_STR, N_REGEX, N_VAR, N_FIELD, N_INDEX,
  N_ASSIGN, N_BINOP, N_UNARY, N_POSTFIX, N_PREFIX, N_TERNARY,
  N_AND, N_OR, N_NOT, N_MATCH, N_IN, N_CONCAT,
  N_CALL, N_BUILTIN, N_GETLINE,
  N_PRINT, N_PRINTF, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN,
  N_BLOCK, N_NEXT, N_EXIT, N_RETURN, N_BREAK, N_CONTINUE, N_DELETE,
  N_RULE, N_EMPTY,
};

enum builtin {
  B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF,
  B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT, B_RAND, B_SRAND,
  B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE,
};

struct node {
  enum ntype type;
  double num;
  char *str;
  int op;              // for N_BINOP, N_ASSIGN, N_UNARY: the token
  struct node *a, *b, *c, *d;
  struct node *next;   // in a list
  regex_t *re;         // compiled, for N_REGEX and N_MATCH
};

// ---- the lexer ----

enum token {
  T_EOF = 0, T_NUMBER = 256, T_STRING, T_REGEX, T_NAME, T_FUNC_NAME, T_BUILTIN,
  T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK,
  T_CONTINUE, T_NEXT, T_EXIT, T_RETURN, T_DELETE, T_IN, T_GETLINE,
  T_PRINT, T_PRINTF,
  T_ADD_ASSIGN, T_SUB_ASSIGN, T_MUL_ASSIGN, T_DIV_ASSIGN, T_MOD_ASSIGN, T_POW_ASSIGN,
  T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR, T_INCR, T_DECR, T_MATCH, T_NOMATCH,
  T_APPEND, T_POW,
};

static const char *src;      // the program text
static int tok;              // the token just read
static double tok_num;
static char tok_str[4096];
static int tok_builtin;
static int prev_tok;         // to tell division from a regex

static void
fatal(const char *fmt, ...)
{
  va_list ap;

  fprintf(stderr, "awk: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(2);
}

static void *
xmalloc(size_t n)
{
  void *p = malloc(n);

  if (p == 0)
    fatal("out of memory");
  memset(p, 0, n);
  return p;
}

static char *
xstrdup(const char *s)
{
  char *p = xmalloc(strlen(s) + 1);

  strcpy(p, s);
  return p;
}

static const struct {
  const char *name;
  int tok;
  int builtin;
} keywords[] = {
    {"BEGIN", T_BEGIN, 0},     {"END", T_END, 0},         {"function", T_FUNCTION, 0},
    {"func", T_FUNCTION, 0},   {"if", T_IF, 0},           {"else", T_ELSE, 0},
    {"while", T_WHILE, 0},     {"for", T_FOR, 0},         {"do", T_DO, 0},
    {"break", T_BREAK, 0},     {"continue", T_CONTINUE, 0}, {"next", T_NEXT, 0},
    {"exit", T_EXIT, 0},       {"return", T_RETURN, 0},   {"delete", T_DELETE, 0},
    {"in", T_IN, 0},           {"getline", T_GETLINE, 0}, {"print", T_PRINT, 0},
    {"printf", T_PRINTF, 0},
    {"length", T_BUILTIN, B_LENGTH},   {"substr", T_BUILTIN, B_SUBSTR},
    {"index", T_BUILTIN, B_INDEX},     {"split", T_BUILTIN, B_SPLIT},
    {"sub", T_BUILTIN, B_SUB},         {"gsub", T_BUILTIN, B_GSUB},
    {"match", T_BUILTIN, B_MATCH},     {"sprintf", T_BUILTIN, B_SPRINTF},
    {"sin", T_BUILTIN, B_SIN},         {"cos", T_BUILTIN, B_COS},
    {"atan2", T_BUILTIN, B_ATAN2},     {"exp", T_BUILTIN, B_EXP},
    {"log", T_BUILTIN, B_LOG},         {"sqrt", T_BUILTIN, B_SQRT},
    {"int", T_BUILTIN, B_INT},         {"rand", T_BUILTIN, B_RAND},
    {"srand", T_BUILTIN, B_SRAND},     {"tolower", T_BUILTIN, B_TOLOWER},
    {"toupper", T_BUILTIN, B_TOUPPER}, {"system", T_BUILTIN, B_SYSTEM},
    {"close", T_BUILTIN, B_CLOSE},
    {0, 0, 0},
};

// a '/' starts a regex unless the last token could end an expression
static int
regex_ok(void)
{
  switch (prev_tok) {
  case T_NUMBER: case T_STRING: case T_NAME: case ')': case ']': case '$':
  case T_INCR: case T_DECR: case T_BUILTIN:
    return 0;
  default:
    return 1;
  }
}

static int
lex(void)
{
  prev_tok = tok;
  for (;;) {
    while (*src == ' ' || *src == '\t')
      src++;
    if (*src == '\\' && src[1] == '\n') {
      src += 2;
      continue;
    }
    if (*src == '#') {
      while (*src && *src != '\n')
        src++;
      continue;
    }
    break;
  }
  if (*src == 0)
    return tok = T_EOF;

  if (*src == '\n') {
    src++;
    return tok = '\n';
  }
  if (isdigit((unsigned char)*src) ||
      (*src == '.' && isdigit((unsigned char)src[1]))) {
    char *end;
    tok_num = strtod(src, &end);
    src = end;
    return tok = T_NUMBER;
  }
  if (isalpha((unsigned char)*src) || *src == '_') {
    int n = 0;
    while (isalnum((unsigned char)*src) || *src == '_')
      if (n < (int)sizeof(tok_str) - 1)
        tok_str[n++] = *src++;
      else
        src++;
    tok_str[n] = 0;
    for (int i = 0; keywords[i].name; i++)
      if (strcmp(keywords[i].name, tok_str) == 0) {
        tok_builtin = keywords[i].builtin;
        return tok = keywords[i].tok;
      }
    // a name followed at once by '(' is a call
    if (*src == '(')
      return tok = T_FUNC_NAME;
    return tok = T_NAME;
  }
  if (*src == '"') {
    int n = 0;
    src++;
    while (*src && *src != '"') {
      int c = *src++;
      if (c == '\\' && *src) {
        switch (*src++) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case '\\': c = '\\'; break;
        case '"': c = '"'; break;
        case '/': c = '/'; break;
        case 'a': c = 7; break;
        case 'b': c = 8; break;
        case 'f': c = 12; break;
        case 'v': c = 11; break;
        default: c = src[-1]; break;
        }
      }
      if (n < (int)sizeof(tok_str) - 1)
        tok_str[n++] = (char)c;
    }
    if (*src == '"')
      src++;
    tok_str[n] = 0;
    return tok = T_STRING;
  }
  if (*src == '/' && regex_ok()) {
    int n = 0;
    src++;
    while (*src && *src != '/') {
      if (*src == '\\' && src[1] == '/') {
        src++;
        if (n < (int)sizeof(tok_str) - 1)
          tok_str[n++] = *src++;
        continue;
      }
      if (n < (int)sizeof(tok_str) - 1)
        tok_str[n++] = *src++;
      else
        src++;
    }
    if (*src == '/')
      src++;
    tok_str[n] = 0;
    return tok = T_REGEX;
  }

  int c = *src++;
  switch (c) {
  case '+':
    if (*src == '+') { src++; return tok = T_INCR; }
    if (*src == '=') { src++; return tok = T_ADD_ASSIGN; }
    return tok = '+';
  case '-':
    if (*src == '-') { src++; return tok = T_DECR; }
    if (*src == '=') { src++; return tok = T_SUB_ASSIGN; }
    return tok = '-';
  case '*':
    if (*src == '*') {
      src++;
      if (*src == '=') { src++; return tok = T_POW_ASSIGN; }
      return tok = T_POW;
    }
    if (*src == '=') { src++; return tok = T_MUL_ASSIGN; }
    return tok = '*';
  case '/':
    if (*src == '=') { src++; return tok = T_DIV_ASSIGN; }
    return tok = '/';
  case '%':
    if (*src == '=') { src++; return tok = T_MOD_ASSIGN; }
    return tok = '%';
  case '^':
    if (*src == '=') { src++; return tok = T_POW_ASSIGN; }
    return tok = T_POW;
  case '=':
    if (*src == '=') { src++; return tok = T_EQ; }
    return tok = '=';
  case '!':
    if (*src == '=') { src++; return tok = T_NE; }
    if (*src == '~') { src++; return tok = T_NOMATCH; }
    return tok = '!';
  case '<':
    if (*src == '=') { src++; return tok = T_LE; }
    return tok = '<';
  case '>':
    if (*src == '=') { src++; return tok = T_GE; }
    if (*src == '>') { src++; return tok = T_APPEND; }
    return tok = '>';
  case '&':
    if (*src == '&') { src++; return tok = T_AND; }
    return tok = '&';
  case '|':
    if (*src == '|') { src++; return tok = T_OR; }
    return tok = '|';
  case '~':
    return tok = T_MATCH;
  default:
    return tok = c;
  }
}

// ---- the parser ----

static struct node *parse_expr(void);
static struct node *parse_statement(void);
static struct node *parse_statement_list(int stop);

// inside a print's expression list '>' redirects rather than
// compares -- except inside parentheses, where it compares again
static int in_print_expr;

static struct node *
newnode(enum ntype t)
{
  struct node *n = xmalloc(sizeof(*n));

  n->type = t;
  return n;
}

static regex_t *
compile_regex(const char *text)
{
  regex_t *re = xmalloc(sizeof(*re));
  int r = regcomp(re, text, REG_EXTENDED);

  if (r != 0) {
    char msg[128];
    regerror(r, re, msg, sizeof(msg));
    fatal("%s: %s", text, msg);
  }
  return re;
}

static void
expect(int t, const char *what)
{
  if (tok != t)
    fatal("expected %s", what);
  lex();
}

static void
skip_newlines(void)
{
  while (tok == '\n' || tok == ';')
    lex();
}

static void
optional_newlines(void)
{
  while (tok == '\n')
    lex();
}

static struct node *
parse_primary(void)
{
  struct node *n;

  switch (tok) {
  case T_NUMBER:
    n = newnode(N_NUM);
    n->num = tok_num;
    lex();
    return n;
  case T_STRING:
    n = newnode(N_STR);
    n->str = xstrdup(tok_str);
    lex();
    return n;
  case T_REGEX:
    n = newnode(N_REGEX);
    n->str = xstrdup(tok_str);
    n->re = compile_regex(tok_str);
    lex();
    return n;
  case '$':
    lex();
    n = newnode(N_FIELD);
    n->a = parse_primary();
    return n;
  case T_INCR:
  case T_DECR: {
    int op = tok;
    lex();
    n = newnode(N_PREFIX);
    n->op = op;
    n->a = parse_primary();
    return n;
  }
  case '!':
    lex();
    n = newnode(N_NOT);
    n->a = parse_primary();
    return n;
  case '-':
  case '+': {
    int op = tok;
    lex();
    n = newnode(N_UNARY);
    n->op = op;
    n->a = parse_primary();
    return n;
  }
  case '(': {
    // inside parentheses '>' is a comparison again, even in a print:
    // print (a > b) ? "x" : "y" compares, print a > b redirects
    int saved = in_print_expr;
    in_print_expr = 0;
    lex();
    n = parse_expr();
    // (a, b) in arr
    if (tok == ',') {
      struct node *list = n;
      struct node *tail = n;
      while (tok == ',') {
        lex();
        optional_newlines();
        tail->next = parse_expr();
        tail = tail->next;
      }
      expect(')', ")");
      in_print_expr = saved;
      if (tok == T_IN) {
        lex();
        struct node *in = newnode(N_IN);
        in->a = list;
        in->str = xstrdup(tok_str);
        expect(T_NAME, "an array name");
        return in;
      }
      return list;
    }
    expect(')', ")");
    in_print_expr = saved;
    return n;
  }
  case T_BUILTIN: {
    int which = tok_builtin;
    lex();
    n = newnode(N_BUILTIN);
    n->op = which;
    if (tok == '(') {
      lex();
      if (tok != ')') {
        struct node *tail = 0;
        for (;;) {
          struct node *arg = parse_expr();
          if (tail)
            tail->next = arg;
          else
            n->a = arg;
          tail = arg;
          if (tok != ',')
            break;
          lex();
          optional_newlines();
        }
      }
      expect(')', ")");
    }
    return n;
  }
  case T_FUNC_NAME: {
    n = newnode(N_CALL);
    n->str = xstrdup(tok_str);
    lex();
    expect('(', "(");
    if (tok != ')') {
      struct node *tail = 0;
      for (;;) {
        struct node *arg = parse_expr();
        if (tail)
          tail->next = arg;
        else
          n->a = arg;
        tail = arg;
        if (tok != ',')
          break;
        lex();
        optional_newlines();
      }
    }
    expect(')', ")");
    return n;
  }
  case T_NAME: {
    char name[256];
    snprintf(name, sizeof(name), "%s", tok_str);
    lex();
    if (tok == '[') {
      lex();
      n = newnode(N_INDEX);
      n->str = xstrdup(name);
      struct node *tail = 0;
      for (;;) {
        struct node *e = parse_expr();
        if (tail)
          tail->next = e;
        else
          n->a = e;
        tail = e;
        if (tok != ',')
          break;
        lex();
      }
      expect(']', "]");
      return n;
    }
    n = newnode(N_VAR);
    n->str = xstrdup(name);
    return n;
  }
  case T_GETLINE: {
    lex();
    n = newnode(N_GETLINE);
    if (tok == '$' || tok == T_NAME)
      n->a = parse_primary();
    if (tok == '<') {
      lex();
      n->b = parse_primary();
    }
    return n;
  }
  default:
    fatal("unexpected token in an expression");
    return 0;
  }
}

static struct node *
parse_postfix(void)
{
  struct node *n = parse_primary();

  while (tok == T_INCR || tok == T_DECR) {
    struct node *p = newnode(N_POSTFIX);
    p->op = tok;
    p->a = n;
    n = p;
    lex();
  }
  return n;
}

static struct node *
parse_power(void)
{
  struct node *n = parse_postfix();

  if (tok == T_POW) {
    lex();
    struct node *b = newnode(N_BINOP);
    b->op = T_POW;
    b->a = n;
    b->b = parse_power(); // right associative
    return b;
  }
  return n;
}

static struct node *
parse_mul(void)
{
  struct node *n = parse_power();

  while (tok == '*' || tok == '/' || tok == '%') {
    int op = tok;
    lex();
    struct node *b = newnode(N_BINOP);
    b->op = op;
    b->a = n;
    b->b = parse_power();
    n = b;
  }
  return n;
}

static struct node *
parse_add(void)
{
  struct node *n = parse_mul();

  while (tok == '+' || tok == '-') {
    int op = tok;
    lex();
    struct node *b = newnode(N_BINOP);
    b->op = op;
    b->a = n;
    b->b = parse_mul();
    n = b;
  }
  return n;
}

// juxtaposition is concatenation: the awkward part of awk's grammar.
// a token that can start an expression, and is not an operator,
// continues the concatenation.
static int
starts_expression(void)
{
  switch (tok) {
  case T_NUMBER: case T_STRING: case T_REGEX: case T_NAME: case T_FUNC_NAME:
  case T_BUILTIN: case '$': case '(': case '!': case T_INCR: case T_DECR:
    return 1;
  default:
    return 0;
  }
}

static struct node *
parse_concat(void)
{
  struct node *n = parse_add();

  while (starts_expression()) {
    struct node *c = newnode(N_CONCAT);
    c->a = n;
    c->b = parse_add();
    n = c;
  }
  return n;
}



static struct node *
parse_compare(void)
{
  struct node *n = parse_concat();

  if (tok == '<' || tok == T_LE || tok == T_NE || tok == T_EQ || tok == T_GE ||
      (tok == '>' && !in_print_expr)) {
    int op = tok;
    lex();
    struct node *b = newnode(N_BINOP);
    b->op = op;
    b->a = n;
    b->b = parse_concat();
    return b; // non-associative, as POSIX says
  }
  return n;
}

static struct node *
parse_match(void)
{
  struct node *n = parse_compare();

  while (tok == T_MATCH || tok == T_NOMATCH) {
    int negate = tok == T_NOMATCH;
    lex();
    struct node *m = newnode(N_MATCH);
    m->op = negate;
    m->a = n;
    m->b = parse_compare();
    n = m;
  }
  return n;
}

static struct node *
parse_in(void)
{
  struct node *n = parse_match();

  while (tok == T_IN) {
    lex();
    struct node *in = newnode(N_IN);
    in->a = n;
    in->str = xstrdup(tok_str);
    expect(T_NAME, "an array name");
    n = in;
  }
  return n;
}

static struct node *
parse_and(void)
{
  struct node *n = parse_in();

  while (tok == T_AND) {
    lex();
    optional_newlines();
    struct node *a = newnode(N_AND);
    a->a = n;
    a->b = parse_in();
    n = a;
  }
  return n;
}

static struct node *
parse_or(void)
{
  struct node *n = parse_and();

  while (tok == T_OR) {
    lex();
    optional_newlines();
    struct node *o = newnode(N_OR);
    o->a = n;
    o->b = parse_and();
    n = o;
  }
  return n;
}

static struct node *
parse_ternary(void)
{
  struct node *n = parse_or();

  if (tok == '?') {
    lex();
    optional_newlines();
    struct node *t = newnode(N_TERNARY);
    t->a = n;
    t->b = parse_ternary();
    expect(':', ":");
    optional_newlines();
    t->c = parse_ternary();
    return t;
  }
  return n;
}

static int
is_lvalue(struct node *n)
{
  return n && (n->type == N_VAR || n->type == N_FIELD || n->type == N_INDEX);
}

static struct node *
parse_expr(void)
{
  struct node *n = parse_ternary();

  if ((tok == '=' || tok == T_ADD_ASSIGN || tok == T_SUB_ASSIGN || tok == T_MUL_ASSIGN ||
       tok == T_DIV_ASSIGN || tok == T_MOD_ASSIGN || tok == T_POW_ASSIGN) &&
      is_lvalue(n)) {
    int op = tok;
    lex();
    optional_newlines();
    struct node *a = newnode(N_ASSIGN);
    a->op = op;
    a->a = n;
    a->b = parse_expr();
    return a;
  }
  return n;
}

// print and printf: a list of expressions, then an optional redirection
static struct node *
parse_print(int is_printf)
{
  struct node *n = newnode(is_printf ? N_PRINTF : N_PRINT);

  lex();
  in_print_expr = 1;
  if (tok != '\n' && tok != ';' && tok != '}' && tok != '>' && tok != T_APPEND &&
      tok != '|' && tok != T_EOF) {
    struct node *tail = 0;
    for (;;) {
      struct node *e = parse_expr();
      if (tail)
        tail->next = e;
      else
        n->a = e;
      tail = e;
      if (tok != ',')
        break;
      lex();
      optional_newlines();
    }
  }
  in_print_expr = 0;
  if (tok == '>' || tok == T_APPEND || tok == '|') {
    n->op = tok;
    lex();
    n->b = parse_concat();
  }
  return n;
}

static struct node *
parse_simple_statement(void)
{
  struct node *n;

  switch (tok) {
  case T_PRINT:
    return parse_print(0);
  case T_PRINTF:
    return parse_print(1);
  case T_NEXT:
    lex();
    return newnode(N_NEXT);
  case T_EXIT:
    lex();
    n = newnode(N_EXIT);
    if (tok != '\n' && tok != ';' && tok != '}')
      n->a = parse_expr();
    return n;
  case T_RETURN:
    lex();
    n = newnode(N_RETURN);
    if (tok != '\n' && tok != ';' && tok != '}')
      n->a = parse_expr();
    return n;
  case T_BREAK:
    lex();
    return newnode(N_BREAK);
  case T_CONTINUE:
    lex();
    return newnode(N_CONTINUE);
  case T_DELETE: {
    lex();
    n = newnode(N_DELETE);
    n->str = xstrdup(tok_str);
    expect(T_NAME, "an array name");
    if (tok == '[') {
      lex();
      struct node *tail = 0;
      for (;;) {
        struct node *e = parse_expr();
        if (tail)
          tail->next = e;
        else
          n->a = e;
        tail = e;
        if (tok != ',')
          break;
        lex();
      }
      expect(']', "]");
    }
    return n;
  }
  default:
    return parse_expr();
  }
}

static struct node *
parse_statement(void)
{
  struct node *n;

  skip_newlines();
  switch (tok) {
  case '{':
    lex();
    n = parse_statement_list('}');
    expect('}', "}");
    return n;
  case T_IF: {
    lex();
    expect('(', "(");
    n = newnode(N_IF);
    n->a = parse_expr();
    expect(')', ")");
    optional_newlines();
    n->b = parse_statement();
    // the else, if there is one, may be on the next line
    const char *save_src = src;
    int save_tok = tok;
    double save_num = tok_num;
    char save_str[256];
    snprintf(save_str, sizeof(save_str), "%s", tok_str);
    while (tok == '\n' || tok == ';')
      lex();
    if (tok == T_ELSE) {
      lex();
      optional_newlines();
      n->c = parse_statement();
    } else {
      src = save_src;
      tok = save_tok;
      tok_num = save_num;
      snprintf(tok_str, sizeof(tok_str), "%s", save_str);
    }
    return n;
  }
  case T_WHILE:
    lex();
    expect('(', "(");
    n = newnode(N_WHILE);
    n->a = parse_expr();
    expect(')', ")");
    optional_newlines();
    n->b = parse_statement();
    return n;
  case T_DO:
    lex();
    optional_newlines();
    n = newnode(N_DO);
    n->b = parse_statement();
    skip_newlines();
    expect(T_WHILE, "while");
    expect('(', "(");
    n->a = parse_expr();
    expect(')', ")");
    return n;
  case T_FOR: {
    lex();
    expect('(', "(");
    // for (name in array)
    if (tok == T_NAME) {
      const char *save_src = src;
      int save_tok = tok;
      char save_str[256];
      snprintf(save_str, sizeof(save_str), "%s", tok_str);
      char name[256];
      snprintf(name, sizeof(name), "%s", tok_str);
      lex();
      if (tok == T_IN) {
        lex();
        n = newnode(N_FORIN);
        n->str = xstrdup(name);
        char *arr = xstrdup(tok_str);
        expect(T_NAME, "an array name");
        expect(')', ")");
        n->a = newnode(N_VAR);
        n->a->str = arr;
        optional_newlines();
        n->b = parse_statement();
        return n;
      }
      src = save_src;
      tok = save_tok;
      snprintf(tok_str, sizeof(tok_str), "%s", save_str);
    }
    n = newnode(N_FOR);
    if (tok != ';')
      n->a = parse_simple_statement();
    expect(';', ";");
    optional_newlines();
    if (tok != ';')
      n->b = parse_expr();
    expect(';', ";");
    optional_newlines();
    if (tok != ')')
      n->c = parse_simple_statement();
    expect(')', ")");
    optional_newlines();
    n->d = parse_statement();
    return n;
  }
  case ';':
    lex();
    return newnode(N_EMPTY);
  default:
    n = parse_simple_statement();
    while (tok == ';')
      lex();
    return n;
  }
}

static struct node *
parse_statement_list(int stop)
{
  struct node *head = 0, *tail = 0;

  for (;;) {
    skip_newlines();
    if (tok == stop || tok == T_EOF)
      break;
    struct node *s = parse_statement();
    if (tail)
      tail->next = s;
    else
      head = s;
    tail = s;
  }
  struct node *b = newnode(N_BLOCK);
  b->a = head;
  return b;
}

// ---- functions and rules ----

#define MAXFUNC 64
#define MAXPARAM 16

struct function {
  char *name;
  char *params[MAXPARAM];
  int nparams;
  struct node *body;
};

static struct function functions[MAXFUNC];
static int nfunctions;

static struct node *rules;      // the ordinary rules, in order
static struct node *begin_rules;
static struct node *end_rules;

static void
add_rule(struct node **list, struct node *r)
{
  if (*list == 0) {
    *list = r;
    return;
  }
  struct node *p = *list;
  while (p->next)
    p = p->next;
  p->next = r;
}

static void
parse_program(void)
{
  lex();
  for (;;) {
    skip_newlines();
    if (tok == T_EOF)
      break;
    if (tok == T_FUNCTION) {
      lex();
      if (nfunctions >= MAXFUNC)
        fatal("too many functions");
      struct function *f = &functions[nfunctions++];
      f->name = xstrdup(tok_str);
      if (tok != T_NAME && tok != T_FUNC_NAME)
        fatal("a function needs a name");
      lex();
      expect('(', "(");
      while (tok != ')') {
        if (f->nparams >= MAXPARAM)
          fatal("too many parameters");
        f->params[f->nparams++] = xstrdup(tok_str);
        expect(T_NAME, "a parameter name");
        if (tok == ',') {
          lex();
          optional_newlines();
        }
      }
      expect(')', ")");
      optional_newlines();
      expect('{', "{");
      f->body = parse_statement_list('}');
      expect('}', "}");
      continue;
    }

    struct node *rule = newnode(N_RULE);
    if (tok == T_BEGIN) {
      lex();
      optional_newlines();
      expect('{', "{");
      rule->b = parse_statement_list('}');
      expect('}', "}");
      add_rule(&begin_rules, rule);
      continue;
    }
    if (tok == T_END) {
      lex();
      optional_newlines();
      expect('{', "{");
      rule->b = parse_statement_list('}');
      expect('}', "}");
      add_rule(&end_rules, rule);
      continue;
    }
    if (tok != '{') {
      rule->a = parse_expr();
      // a range pattern: pattern, pattern
      if (tok == ',') {
        lex();
        optional_newlines();
        rule->c = parse_expr();
      }
    }
    if (tok == '{') {
      lex();
      rule->b = parse_statement_list('}');
      expect('}', "}");
    }
    add_rule(&rules, rule);
  }
}

// ---- the values at run time ----

static struct array globals;
static struct array *locals;      // the frame of the function running
static char *local_names[MAXPARAM];
static int nlocals;

static unsigned
hash(const char *s)
{
  unsigned h = 5381;

  while (*s)
    h = h * 33 + (unsigned char)*s++;
  return h % HASHSIZE;
}

static struct var *
array_find(struct array *a, const char *key, int create)
{
  unsigned h = hash(key);

  for (struct anode *n = a->tab[h]; n; n = n->next)
    if (strcmp(n->key, key) == 0)
      return &n->val;
  if (!create)
    return 0;
  struct anode *n = xmalloc(sizeof(*n));
  n->key = xstrdup(key);
  n->next = a->tab[h];
  a->tab[h] = n;
  a->n++;
  return &n->val;
}

static void
array_delete(struct array *a, const char *key)
{
  unsigned h = hash(key);
  struct anode **pp = &a->tab[h];

  while (*pp) {
    if (strcmp((*pp)->key, key) == 0) {
      struct anode *dead = *pp;
      *pp = dead->next;
      free(dead->key);
      free(dead);
      a->n--;
      return;
    }
    pp = &(*pp)->next;
  }
}

static void
array_clear(struct array *a)
{
  for (int i = 0; i < HASHSIZE; i++) {
    struct anode *n = a->tab[i];
    while (n) {
      struct anode *next = n->next;
      free(n->key);
      free(n);
      n = next;
    }
    a->tab[i] = 0;
  }
  a->n = 0;
}

// a name: a parameter of the function running, or a global
static struct var *
lookup(const char *name)
{
  if (locals) {
    for (int i = 0; i < nlocals; i++)
      if (strcmp(local_names[i], name) == 0)
        return array_find(locals, name, 1);
  }
  return array_find(&globals, name, 1);
}

// ---- cells ----

static char numbuf[64];

static void
set_num(struct cell *c, double v)
{
  free(c->str);
  c->str = 0;
  c->num = v;
  c->isnum = 1;
  c->strnum = 0;
}

static void
set_str(struct cell *c, const char *s)
{
  char *copy = xstrdup(s);

  free(c->str);
  c->str = copy;
  c->isnum = 0;
  c->strnum = 0;
}

// a value read from input: a string that counts as a number if it
// looks like one, which is what makes "10" < "9" false in awk
static int
looks_numeric(const char *s)
{
  char *end;

  while (*s == ' ' || *s == '\t')
    s++;
  if (*s == 0)
    return 0;
  strtod(s, &end);
  if (end == s)
    return 0;
  while (*end == ' ' || *end == '\t' || *end == '\n')
    end++;
  return *end == 0;
}

static void
set_input(struct cell *c, const char *s)
{
  set_str(c, s);
  if (looks_numeric(s)) {
    c->strnum = 1;
    c->num = strtod(s, 0);
    c->isnum = 1;
  }
}

static double
to_num(struct cell *c)
{
  if (c->isnum)
    return c->num;
  if (c->str)
    return strtod(c->str, 0);
  return 0;
}

static const char *convfmt = "%.6g";

static const char *
to_str(struct cell *c)
{
  if (c->str)
    return c->str;
  if (c->isnum) {
    // an integer prints as one, whatever CONVFMT says
    if (c->num == (long long)c->num && fabs(c->num) < 1e16)
      snprintf(numbuf, sizeof(numbuf), "%lld", (long long)c->num);
    else
      snprintf(numbuf, sizeof(numbuf), convfmt, c->num);
    return numbuf;
  }
  return "";
}

static int
is_true(struct cell *c)
{
  if (c->str && !c->strnum)
    return c->str[0] != 0;
  return to_num(c) != 0;
}

// ---- the record and its fields ----

#define MAXFIELD 256

static char record[65536];
static char *fields[MAXFIELD];
static int nfields;
static int fields_valid;
static int record_valid;
static struct cell field_cells[MAXFIELD];

static const char *
getvar_str(const char *name)
{
  struct var *v = array_find(&globals, name, 1);

  return to_str(&v->v);
}

static double
getvar_num(const char *name)
{
  struct var *v = array_find(&globals, name, 1);

  return to_num(&v->v);
}

static void
setvar_num(const char *name, double d)
{
  struct var *v = array_find(&globals, name, 1);

  set_num(&v->v, d);
}

static void
setvar_str(const char *name, const char *s)
{
  struct var *v = array_find(&globals, name, 1);

  set_str(&v->v, s);
}

// split the record into fields on FS, which is a single space for the
// default (runs of blanks, ignoring leading and trailing), a single
// character, or a regular expression
static void
split_record(void)
{
  static char work[sizeof(record)];
  const char *fs = getvar_str("FS");

  snprintf(work, sizeof(work), "%s", record);
  for (int i = 1; i < MAXFIELD; i++) {
    free(field_cells[i].str);
    memset(&field_cells[i], 0, sizeof(field_cells[i]));
  }
  nfields = 0;

  char *p = work;
  if (strcmp(fs, " ") == 0) {
    while (*p && nfields < MAXFIELD - 1) {
      while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
      if (*p == 0)
        break;
      fields[++nfields] = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n')
        p++;
      if (*p)
        *p++ = 0;
    }
  } else if (strlen(fs) == 1 && strchr("^$.[]|()*+?{}\\", fs[0]) == 0) {
    char sep = fs[0];
    fields[++nfields] = p;
    while (*p && nfields < MAXFIELD - 1) {
      if (*p == sep) {
        *p++ = 0;
        fields[++nfields] = p;
        continue;
      }
      p++;
    }
  } else {
    regex_t re;
    if (regcomp(&re, fs, REG_EXTENDED) != 0) {
      fields[++nfields] = p;
    } else {
      regmatch_t m;
      fields[++nfields] = p;
      while (*p && nfields < MAXFIELD - 1) {
        if (regexec(&re, p, 1, &m, p == work ? 0 : REG_NOTBOL) != 0 || m.rm_eo == m.rm_so)
          break;
        p[m.rm_so] = 0;
        p += m.rm_eo;
        fields[++nfields] = p;
      }
      regfree(&re);
    }
  }
  for (int i = 1; i <= nfields; i++)
    set_input(&field_cells[i], fields[i]);
  setvar_num("NF", nfields);
  fields_valid = 1;
}

// put the fields back together, which an assignment to one asks for
static void
rebuild_record(void)
{
  const char *ofs = getvar_str("OFS");
  char out[sizeof(record)];
  int n = 0;

  for (int i = 1; i <= nfields; i++) {
    if (i > 1)
      n += snprintf(out + n, sizeof(out) - n, "%s", ofs);
    n += snprintf(out + n, sizeof(out) - n, "%s", to_str(&field_cells[i]));
  }
  out[n] = 0;
  snprintf(record, sizeof(record), "%s", out);
  record_valid = 1;
}

static struct cell *
field(int i)
{
  static struct cell zero;

  if (i == 0) {
    if (!record_valid)
      rebuild_record();
    set_input(&zero, record);
    return &zero;
  }
  if (!fields_valid)
    split_record();
  if (i < 0 || i >= MAXFIELD)
    fatal("field %d is out of range", i);
  if (i > nfields) {
    static struct cell empty;
    free(empty.str);
    memset(&empty, 0, sizeof(empty));
    set_str(&empty, "");
    return &empty;
  }
  return &field_cells[i];
}

static void
set_field(int i, struct cell *value)
{
  if (i == 0) {
    snprintf(record, sizeof(record), "%s", to_str(value));
    record_valid = 1;
    fields_valid = 0;
    split_record();
    return;
  }
  if (!fields_valid)
    split_record();
  if (i < 1 || i >= MAXFIELD)
    fatal("field %d is out of range", i);
  for (int k = nfields + 1; k <= i; k++) {
    free(field_cells[k].str);
    memset(&field_cells[k], 0, sizeof(field_cells[k]));
    set_str(&field_cells[k], "");
  }
  if (i > nfields) {
    nfields = i;
    setvar_num("NF", nfields);
  }
  set_str(&field_cells[i], to_str(value));
  record_valid = 0;
}

// ---- running ----

enum flow { F_NORMAL, F_BREAK, F_CONTINUE, F_NEXT, F_EXIT, F_RETURN };

static enum flow flow;
static struct cell return_value;
static int exit_code;

static void eval(struct node *n, struct cell *out);
static void exec_node(struct node *n);

// the files and pipes print writes to
#define MAXSTREAM 16
static struct {
  char *name;
  FILE *f;
  int is_pipe;
} streams[MAXSTREAM];

static FILE *
stream_for(const char *name, int mode)
{
  for (int i = 0; i < MAXSTREAM; i++)
    if (streams[i].name && strcmp(streams[i].name, name) == 0)
      return streams[i].f;
  for (int i = 0; i < MAXSTREAM; i++)
    if (streams[i].name == 0) {
      FILE *f;
      if (mode == '|')
        f = popen(name, "w");
      else
        f = fopen(name, mode == T_APPEND ? "a" : "w");
      if (f == 0)
        fatal("cannot write %s: %s", name, strerror(errno));
      streams[i].name = xstrdup(name);
      streams[i].f = f;
      streams[i].is_pipe = mode == '|';
      return f;
    }
  fatal("too many output files");
  return 0;
}

static void
close_streams(void)
{
  for (int i = 0; i < MAXSTREAM; i++)
    if (streams[i].name) {
      if (streams[i].is_pipe)
        pclose(streams[i].f);
      else
        fclose(streams[i].f);
      free(streams[i].name);
      streams[i].name = 0;
    }
}

// awk's printf: the C one, with awk's idea of which argument is which.
// the conversion letter decides whether the argument is taken as a
// number or a string, which is the only difference from C's.
static void
awk_printf(FILE *out, const char *fmt, struct node *args)
{
  char spec[64];

  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      fputc(*p, out);
      continue;
    }
    if (p[1] == '%') {
      fputc('%', out);
      p++;
      continue;
    }
    int n = 0;
    spec[n++] = *p++;
    // the flags, width and precision, with * taking an argument each
    while (*p && strchr("-+ #0123456789.", *p) && n < (int)sizeof(spec) - 4)
      spec[n++] = *p++;
    while (*p == '*' && n < (int)sizeof(spec) - 8) {
      struct cell w;
      memset(&w, 0, sizeof(w));
      if (args) {
        eval(args, &w);
        args = args->next;
      }
      n += snprintf(spec + n, sizeof(spec) - n, "%d", (int)to_num(&w));
      free(w.str);
      p++;
      while (*p && strchr("-+ #0123456789.", *p) && n < (int)sizeof(spec) - 4)
        spec[n++] = *p++;
    }
    if (*p == 0)
      break;
    char conv = *p;

    struct cell c;
    memset(&c, 0, sizeof(c));
    if (args) {
      eval(args, &c);
      args = args->next;
    }
    switch (conv) {
    case 'd':
    case 'i':
      // the length modifier goes in before the letter, because the
      // value is passed as a long long
      snprintf(spec + n, sizeof(spec) - n, "lld");
      fprintf(out, spec, (long long)to_num(&c));
      break;
    case 'o': case 'x': case 'X': case 'u':
      snprintf(spec + n, sizeof(spec) - n, "ll%c", conv);
      fprintf(out, spec, (long long)to_num(&c));
      break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
      spec[n++] = conv;
      spec[n] = 0;
      fprintf(out, spec, to_num(&c));
      break;
    case 'c': {
      spec[n++] = 's';
      spec[n] = 0;
      char one[8];
      if (c.str && c.str[0] && !c.strnum) {
        one[0] = c.str[0];
        one[1] = 0;
      } else {
        one[0] = (char)(int)to_num(&c);
        one[1] = 0;
      }
      fprintf(out, spec, one);
      break;
    }
    case 's':
      spec[n++] = 's';
      spec[n] = 0;
      fprintf(out, spec, to_str(&c));
      break;
    default:
      // an unknown conversion prints as it was written
      fputc('%', out);
      fputc(conv, out);
      break;
    }
    free(c.str);
  }
}

// ---- evaluation ----

static void
copy_cell(struct cell *dst, struct cell *src)
{
  free(dst->str);
  dst->num = src->num;
  dst->isnum = src->isnum;
  dst->strnum = src->strnum;
  dst->str = src->str ? xstrdup(src->str) : 0;
}

static struct array *
array_of(const char *name)
{
  struct var *v = lookup(name);

  if (v->arr == 0)
    v->arr = xmalloc(sizeof(*v->arr));
  return v->arr;
}

// the key a subscript list makes: the parts joined with SUBSEP
static void
subscript(struct node *list, char *out, int max)
{
  const char *subsep = getvar_str("SUBSEP");
  struct cell c;
  int n = 0;

  for (struct node *e = list; e; e = e->next) {
    memset(&c, 0, sizeof(c));
    eval(e, &c);
    n += snprintf(out + n, max - n, "%s%s", n ? subsep : "", to_str(&c));
    free(c.str);
  }
  out[n < max ? n : max - 1] = 0;
}

static void call_function(struct node *n, struct cell *out);
static void call_builtin(struct node *n, struct cell *out);

static void
assign_to(struct node *target, struct cell *value)
{
  if (target->type == N_VAR) {
    struct var *v = lookup(target->str);
    copy_cell(&v->v, value);
    if (strcmp(target->str, "NF") == 0) {
      int want = (int)to_num(value);
      if (!fields_valid)
        split_record();
      if (want < nfields) {
        for (int i = want + 1; i <= nfields; i++) {
          free(field_cells[i].str);
          memset(&field_cells[i], 0, sizeof(field_cells[i]));
        }
      } else {
        for (int i = nfields + 1; i <= want && i < MAXFIELD; i++)
          set_str(&field_cells[i], "");
      }
      nfields = want;
      record_valid = 0;
    }
    if (strcmp(target->str, "CONVFMT") == 0)
      convfmt = to_str(&v->v);
  } else if (target->type == N_FIELD) {
    struct cell idx;
    memset(&idx, 0, sizeof(idx));
    eval(target->a, &idx);
    set_field((int)to_num(&idx), value);
    free(idx.str);
  } else if (target->type == N_INDEX) {
    char key[1024];
    subscript(target->a, key, sizeof(key));
    struct var *v = array_find(array_of(target->str), key, 1);
    copy_cell(&v->v, value);
  } else {
    fatal("that is not something to assign to");
  }
}

static void
eval_lvalue(struct node *n, struct cell *out)
{
  if (n->type == N_VAR) {
    struct var *v = lookup(n->str);
    if (strcmp(n->str, "NF") == 0 && !fields_valid)
      split_record();
    copy_cell(out, &v->v);
  } else if (n->type == N_FIELD) {
    struct cell idx;
    memset(&idx, 0, sizeof(idx));
    eval(n->a, &idx);
    copy_cell(out, field((int)to_num(&idx)));
    free(idx.str);
  } else if (n->type == N_INDEX) {
    char key[1024];
    subscript(n->a, key, sizeof(key));
    struct var *v = array_find(array_of(n->str), key, 1);
    copy_cell(out, &v->v);
  } else {
    eval(n, out);
  }
}

static int
match_regex(struct node *pattern, const char *text)
{
  if (pattern->type == N_REGEX)
    return regexec(pattern->re, text, 0, 0, 0) == 0;
  struct cell c;
  memset(&c, 0, sizeof(c));
  eval(pattern, &c);
  regex_t re;
  int ok = 0;
  if (regcomp(&re, to_str(&c), REG_EXTENDED) == 0) {
    ok = regexec(&re, text, 0, 0, 0) == 0;
    regfree(&re);
  }
  free(c.str);
  return ok;
}

static int getline_into(struct node *target, struct node *from);

static void
eval(struct node *n, struct cell *out)
{
  struct cell a, b;

  memset(out, 0, sizeof(*out));
  if (n == 0)
    return;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));

  switch (n->type) {
  case N_NUM:
    set_num(out, n->num);
    return;
  case N_STR:
    set_str(out, n->str);
    return;
  case N_REGEX:
    // a bare regex is a match against the whole record
    set_num(out, regexec(n->re, to_str(field(0)), 0, 0, 0) == 0);
    return;
  case N_VAR:
  case N_FIELD:
  case N_INDEX:
    eval_lvalue(n, out);
    return;
  case N_ASSIGN: {
    eval(n->b, &b);
    if (n->op == '=') {
      assign_to(n->a, &b);
      copy_cell(out, &b);
    } else {
      eval_lvalue(n->a, &a);
      double x = to_num(&a), y = to_num(&b), r = 0;
      switch (n->op) {
      case T_ADD_ASSIGN: r = x + y; break;
      case T_SUB_ASSIGN: r = x - y; break;
      case T_MUL_ASSIGN: r = x * y; break;
      case T_DIV_ASSIGN:
        if (y == 0)
          fatal("division by zero");
        r = x / y;
        break;
      case T_MOD_ASSIGN:
        if (y == 0)
          fatal("division by zero");
        r = fmod(x, y);
        break;
      case T_POW_ASSIGN: r = pow(x, y); break;
      }
      set_num(out, r);
      assign_to(n->a, out);
    }
    free(a.str);
    free(b.str);
    return;
  }
  case N_BINOP: {
    eval(n->a, &a);
    eval(n->b, &b);
    switch (n->op) {
    case '+': set_num(out, to_num(&a) + to_num(&b)); break;
    case '-': set_num(out, to_num(&a) - to_num(&b)); break;
    case '*': set_num(out, to_num(&a) * to_num(&b)); break;
    case '/':
      if (to_num(&b) == 0)
        fatal("division by zero");
      set_num(out, to_num(&a) / to_num(&b));
      break;
    case '%':
      if (to_num(&b) == 0)
        fatal("division by zero");
      set_num(out, fmod(to_num(&a), to_num(&b)));
      break;
    case T_POW: set_num(out, pow(to_num(&a), to_num(&b))); break;
    default: {
      // a comparison: numeric when both sides are numbers or came
      // from input looking like them, and a string comparison
      // otherwise -- which is the rule that makes awk awk
      int numeric = (a.isnum && !a.str) || (b.isnum && !b.str) ? 1 : 0;
      if ((a.strnum || (a.isnum && !a.str)) && (b.strnum || (b.isnum && !b.str)))
        numeric = 1;
      else if (a.str && b.str)
        numeric = 0;
      int cmp;
      if (numeric) {
        double x = to_num(&a), y = to_num(&b);
        cmp = x < y ? -1 : x > y ? 1 : 0;
      } else {
        const char *x = to_str(&a);
        char xs[4096];
        snprintf(xs, sizeof(xs), "%s", x);
        cmp = strcmp(xs, to_str(&b));
      }
      int r = 0;
      switch (n->op) {
      case '<': r = cmp < 0; break;
      case T_LE: r = cmp <= 0; break;
      case '>': r = cmp > 0; break;
      case T_GE: r = cmp >= 0; break;
      case T_EQ: r = cmp == 0; break;
      case T_NE: r = cmp != 0; break;
      }
      set_num(out, r);
      break;
    }
    }
    free(a.str);
    free(b.str);
    return;
  }
  case N_CONCAT: {
    eval(n->a, &a);
    char left[8192];
    snprintf(left, sizeof(left), "%s", to_str(&a));
    eval(n->b, &b);
    char both[16384];
    snprintf(both, sizeof(both), "%s%s", left, to_str(&b));
    set_str(out, both);
    free(a.str);
    free(b.str);
    return;
  }
  case N_UNARY:
    eval(n->a, &a);
    set_num(out, n->op == '-' ? -to_num(&a) : to_num(&a));
    free(a.str);
    return;
  case N_NOT:
    eval(n->a, &a);
    set_num(out, !is_true(&a));
    free(a.str);
    return;
  case N_AND:
    eval(n->a, &a);
    if (!is_true(&a)) {
      set_num(out, 0);
    } else {
      eval(n->b, &b);
      set_num(out, is_true(&b));
      free(b.str);
    }
    free(a.str);
    return;
  case N_OR:
    eval(n->a, &a);
    if (is_true(&a)) {
      set_num(out, 1);
    } else {
      eval(n->b, &b);
      set_num(out, is_true(&b));
      free(b.str);
    }
    free(a.str);
    return;
  case N_TERNARY:
    eval(n->a, &a);
    if (is_true(&a))
      eval(n->b, out);
    else
      eval(n->c, out);
    free(a.str);
    return;
  case N_MATCH: {
    eval(n->a, &a);
    char text[8192];
    snprintf(text, sizeof(text), "%s", to_str(&a));
    int m = match_regex(n->b, text);
    set_num(out, n->op ? !m : m);
    free(a.str);
    return;
  }
  case N_IN: {
    char key[1024];
    subscript(n->a, key, sizeof(key));
    set_num(out, array_find(array_of(n->str), key, 0) != 0);
    return;
  }
  case N_PREFIX:
  case N_POSTFIX: {
    eval_lvalue(n->a, &a);
    double old = to_num(&a);
    double nw = n->op == T_INCR ? old + 1 : old - 1;
    struct cell v;
    memset(&v, 0, sizeof(v));
    set_num(&v, nw);
    assign_to(n->a, &v);
    set_num(out, n->type == N_PREFIX ? nw : old);
    free(v.str);
    free(a.str);
    return;
  }
  case N_CALL:
    call_function(n, out);
    return;
  case N_BUILTIN:
    call_builtin(n, out);
    return;
  case N_GETLINE:
    set_num(out, getline_into(n->a, n->b));
    return;
  default:
    fatal("that is not an expression");
  }
}

// ---- the built-ins ----

static void
substitute(struct node *n, struct cell *out, int global)
{
  struct node *pattern = n->a;
  struct node *repl = pattern ? pattern->next : 0;
  struct node *target = repl ? repl->next : 0;
  struct cell rc;
  char text[8192], result[16384];
  int count = 0;

  memset(&rc, 0, sizeof(rc));
  if (repl)
    eval(repl, &rc);
  char replacement[4096];
  snprintf(replacement, sizeof(replacement), "%s", to_str(&rc));
  free(rc.str);

  struct cell tc;
  memset(&tc, 0, sizeof(tc));
  if (target)
    eval_lvalue(target, &tc);
  else
    copy_cell(&tc, field(0));
  snprintf(text, sizeof(text), "%s", to_str(&tc));

  regex_t local;
  regex_t *re;
  if (pattern->type == N_REGEX) {
    re = pattern->re;
  } else {
    struct cell pc;
    memset(&pc, 0, sizeof(pc));
    eval(pattern, &pc);
    if (regcomp(&local, to_str(&pc), REG_EXTENDED) != 0)
      fatal("%s: not a regular expression", to_str(&pc));
    free(pc.str);
    re = &local;
  }

  const char *p = text;
  int n2 = 0;
  regmatch_t m;
  while (regexec(re, p, 1, &m, p == text ? 0 : REG_NOTBOL) == 0) {
    for (int i = 0; i < m.rm_so && n2 < (int)sizeof(result) - 1; i++)
      result[n2++] = p[i];
    for (const char *r = replacement; *r; r++) {
      if (*r == '\\' && r[1] == '&') {
        r++;
        if (n2 < (int)sizeof(result) - 1)
          result[n2++] = '&';
      } else if (*r == '&') {
        for (int i = m.rm_so; i < m.rm_eo && n2 < (int)sizeof(result) - 1; i++)
          result[n2++] = p[i];
      } else if (n2 < (int)sizeof(result) - 1) {
        result[n2++] = *r;
      }
    }
    count++;
    if (m.rm_eo == m.rm_so) {
      if (p[m.rm_eo] == 0) {
        p += m.rm_eo;
        break;
      }
      if (n2 < (int)sizeof(result) - 1)
        result[n2++] = p[m.rm_eo];
      p += m.rm_eo + 1;
    } else {
      p += m.rm_eo;
    }
    if (!global)
      break;
  }
  while (*p && n2 < (int)sizeof(result) - 1)
    result[n2++] = *p++;
  result[n2] = 0;
  if (re == &local)
    regfree(&local);

  if (count > 0) {
    struct cell nc;
    memset(&nc, 0, sizeof(nc));
    set_str(&nc, result);
    if (target)
      assign_to(target, &nc);
    else
      set_field(0, &nc);
    free(nc.str);
  }
  free(tc.str);
  set_num(out, count);
}

static void
call_builtin(struct node *n, struct cell *out)
{
  struct cell a, b, c;

  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  memset(&c, 0, sizeof(c));
  struct node *arg = n->a;

  switch (n->op) {
  case B_LENGTH:
    if (arg == 0) {
      set_num(out, strlen(to_str(field(0))));
    } else if (arg->type == N_VAR && lookup(arg->str)->arr) {
      set_num(out, lookup(arg->str)->arr->n);
    } else {
      eval(arg, &a);
      set_num(out, strlen(to_str(&a)));
    }
    break;
  case B_SUBSTR: {
    eval(arg, &a);
    char s[8192];
    snprintf(s, sizeof(s), "%s", to_str(&a));
    int len = strlen(s);
    eval(arg->next, &b);
    int start = (int)(to_num(&b) + 0.5);
    int count = len;
    if (arg->next->next) {
      eval(arg->next->next, &c);
      count = (int)(to_num(&c) + 0.5);
    }
    if (start < 1) {
      count += start - 1;
      start = 1;
    }
    if (start > len || count <= 0) {
      set_str(out, "");
    } else {
      if (start - 1 + count > len)
        count = len - (start - 1);
      char piece[8192];
      memmove(piece, s + start - 1, count);
      piece[count] = 0;
      set_str(out, piece);
    }
    break;
  }
  case B_INDEX: {
    eval(arg, &a);
    char s[8192];
    snprintf(s, sizeof(s), "%s", to_str(&a));
    eval(arg->next, &b);
    const char *at = strstr(s, to_str(&b));
    set_num(out, at ? (double)(at - s + 1) : 0);
    break;
  }
  case B_SPLIT: {
    eval(arg, &a);
    char s[8192];
    snprintf(s, sizeof(s), "%s", to_str(&a));
    struct node *arrnode = arg->next;
    if (arrnode == 0 || arrnode->type != N_VAR)
      fatal("split needs an array");
    struct array *arr = array_of(arrnode->str);
    array_clear(arr);
    char sep[64];
    if (arg->next->next) {
      struct node *sn = arg->next->next;
      if (sn->type == N_REGEX) {
        snprintf(sep, sizeof(sep), "%s", sn->str);
      } else {
        eval(sn, &c);
        snprintf(sep, sizeof(sep), "%s", to_str(&c));
      }
    } else {
      snprintf(sep, sizeof(sep), "%s", getvar_str("FS"));
    }
    int count = 0;
    char key[32];
    char *p = s;
    if (strcmp(sep, " ") == 0) {
      while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
          p++;
        if (*p == 0)
          break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
          p++;
        if (*p)
          *p++ = 0;
        snprintf(key, sizeof(key), "%d", ++count);
        set_input(&array_find(arr, key, 1)->v, start);
      }
    } else if (strlen(sep) == 1 && strchr("^$.[]|()*+?{}\\", sep[0]) == 0) {
      char *start = p;
      for (;;) {
        if (*p == sep[0] || *p == 0) {
          int done = *p == 0;
          *p = 0;
          snprintf(key, sizeof(key), "%d", ++count);
          set_input(&array_find(arr, key, 1)->v, start);
          if (done)
            break;
          start = ++p;
          continue;
        }
        p++;
      }
    } else {
      regex_t re;
      if (regcomp(&re, sep, REG_EXTENDED) == 0) {
        regmatch_t m;
        char *start = p;
        while (*p && regexec(&re, p, 1, &m, p == s ? 0 : REG_NOTBOL) == 0 && m.rm_eo > m.rm_so) {
          p[m.rm_so] = 0;
          snprintf(key, sizeof(key), "%d", ++count);
          set_input(&array_find(arr, key, 1)->v, start);
          p += m.rm_eo;
          start = p;
        }
        snprintf(key, sizeof(key), "%d", ++count);
        set_input(&array_find(arr, key, 1)->v, start);
        regfree(&re);
      }
    }
    set_num(out, count);
    break;
  }
  case B_SUB:
    substitute(n, out, 0);
    break;
  case B_GSUB:
    substitute(n, out, 1);
    break;
  case B_MATCH: {
    eval(arg, &a);
    char s[8192];
    snprintf(s, sizeof(s), "%s", to_str(&a));
    regex_t local;
    regex_t *re;
    if (arg->next && arg->next->type == N_REGEX) {
      re = arg->next->re;
    } else {
      eval(arg->next, &b);
      if (regcomp(&local, to_str(&b), REG_EXTENDED) != 0)
        fatal("match: not a regular expression");
      re = &local;
    }
    regmatch_t m;
    if (regexec(re, s, 1, &m, 0) == 0) {
      setvar_num("RSTART", m.rm_so + 1);
      setvar_num("RLENGTH", m.rm_eo - m.rm_so);
      set_num(out, m.rm_so + 1);
    } else {
      setvar_num("RSTART", 0);
      setvar_num("RLENGTH", -1);
      set_num(out, 0);
    }
    if (re == &local)
      regfree(&local);
    break;
  }
  case B_SPRINTF: {
    eval(arg, &a);
    char fmt[4096];
    snprintf(fmt, sizeof(fmt), "%s", to_str(&a));
    char *buf = 0;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (mem == 0)
      fatal("sprintf: out of memory");
    awk_printf(mem, fmt, arg->next);
    fclose(mem);
    set_str(out, buf ? buf : "");
    free(buf);
    break;
  }
  case B_SIN: eval(arg, &a); set_num(out, sin(to_num(&a))); break;
  case B_COS: eval(arg, &a); set_num(out, cos(to_num(&a))); break;
  case B_ATAN2:
    eval(arg, &a);
    eval(arg->next, &b);
    set_num(out, atan2(to_num(&a), to_num(&b)));
    break;
  case B_EXP: eval(arg, &a); set_num(out, exp(to_num(&a))); break;
  case B_LOG: eval(arg, &a); set_num(out, log(to_num(&a))); break;
  case B_SQRT: eval(arg, &a); set_num(out, sqrt(to_num(&a))); break;
  case B_INT: eval(arg, &a); set_num(out, (double)(long long)to_num(&a)); break;
  case B_RAND: set_num(out, (double)(random() % 1000000) / 1000000.0); break;
  case B_SRAND: {
    static double last_seed = 0;
    double seed = 0;
    if (arg) {
      eval(arg, &a);
      seed = to_num(&a);
    } else {
      seed = (double)time(0);
    }
    srandom((unsigned)seed);
    set_num(out, last_seed);
    last_seed = seed;
    break;
  }
  case B_TOLOWER:
  case B_TOUPPER: {
    eval(arg, &a);
    char s[8192];
    snprintf(s, sizeof(s), "%s", to_str(&a));
    for (char *p = s; *p; p++)
      *p = n->op == B_TOLOWER ? tolower((unsigned char)*p) : toupper((unsigned char)*p);
    set_str(out, s);
    break;
  }
  case B_SYSTEM:
    eval(arg, &a);
    fflush(stdout);
    set_num(out, system(to_str(&a)));
    break;
  case B_CLOSE: {
    eval(arg, &a);
    const char *name = to_str(&a);
    int r = -1;
    for (int i = 0; i < MAXSTREAM; i++)
      if (streams[i].name && strcmp(streams[i].name, name) == 0) {
        r = streams[i].is_pipe ? pclose(streams[i].f) : fclose(streams[i].f);
        free(streams[i].name);
        streams[i].name = 0;
      }
    set_num(out, r);
    break;
  }
  }
  free(a.str);
  free(b.str);
  free(c.str);
}

static void
call_function(struct node *n, struct cell *out)
{
  struct function *f = 0;

  for (int i = 0; i < nfunctions; i++)
    if (strcmp(functions[i].name, n->str) == 0) {
      f = &functions[i];
      break;
    }
  if (f == 0)
    fatal("%s: no such function", n->str);

  // the arguments are evaluated in the caller's frame, then the new
  // frame is put in place. an array passed by name is shared, which is
  // what awk does and what makes a function able to fill one.
  struct array *frame = xmalloc(sizeof(*frame));
  struct node *arg = n->a;
  struct array *shared[MAXPARAM];
  int nshared = 0;

  for (int i = 0; i < f->nparams; i++) {
    if (arg && arg->type == N_VAR && lookup(arg->str)->arr) {
      shared[i] = lookup(arg->str)->arr;
      nshared++;
    } else {
      shared[i] = 0;
    }
    if (arg) {
      struct cell v;
      memset(&v, 0, sizeof(v));
      if (shared[i] == 0)
        eval(arg, &v);
      struct var *p = array_find(frame, f->params[i], 1);
      copy_cell(&p->v, &v);
      p->arr = shared[i];
      free(v.str);
      arg = arg->next;
    } else {
      array_find(frame, f->params[i], 1);
    }
  }
  (void)nshared;

  struct array *saved_locals = locals;
  char *saved_names[MAXPARAM];
  int saved_n = nlocals;
  for (int i = 0; i < nlocals; i++)
    saved_names[i] = local_names[i];

  locals = frame;
  nlocals = f->nparams;
  for (int i = 0; i < f->nparams; i++)
    local_names[i] = f->params[i];

  memset(&return_value, 0, sizeof(return_value));
  exec_node(f->body);
  if (flow == F_RETURN)
    flow = F_NORMAL;

  locals = saved_locals;
  nlocals = saved_n;
  for (int i = 0; i < saved_n; i++)
    local_names[i] = saved_names[i];

  copy_cell(out, &return_value);
  free(return_value.str);
  memset(&return_value, 0, sizeof(return_value));
  // an array passed in is the caller's: the frame lets go of the
  // pointer rather than freeing what it points at
  for (int i = 0; i < f->nparams; i++) {
    struct var *p = array_find(frame, f->params[i], 0);
    if (p && shared[i])
      p->arr = 0;
  }
  array_clear(frame);
  free(frame);
}

// ---- input ----

static char **input_files;
static int ninput, input_index;
static FILE *input;
static int used_stdin;

static int
next_input(void)
{
  while (input_index < ninput) {
    char *name = input_files[input_index++];
    // NAME=VALUE among the file names is an assignment, as POSIX says
    char *eq = strchr(name, '=');
    if (eq && eq != name) {
      *eq = 0;
      setvar_str(name, eq + 1);
      *eq = '=';
      continue;
    }
    if (strcmp(name, "-") == 0) {
      input = stdin;
      setvar_str("FILENAME", "-");
      return 1;
    }
    input = fopen(name, "r");
    if (input == 0) {
      fprintf(stderr, "awk: %s: %s\n", name, strerror(errno));
      exit_code = 2;
      continue;
    }
    setvar_str("FILENAME", name);
    return 1;
  }
  if (!used_stdin && ninput == 0) {
    used_stdin = 1;
    input = stdin;
    return 1;
  }
  return 0;
}

// one record, honouring RS: a single character, or "" for paragraphs
static int
read_record(FILE *f, char *buf, int max)
{
  const char *rs = getvar_str("RS");
  int n = 0, c;

  if (rs[0] == 0) {
    // paragraph mode: records are separated by blank lines
    while ((c = fgetc(f)) != EOF && (c == '\n'))
      ;
    if (c == EOF)
      return 0;
    ungetc(c, f);
    int newlines = 0;
    while ((c = fgetc(f)) != EOF) {
      if (c == '\n') {
        if (++newlines == 2)
          break;
      } else {
        newlines = 0;
      }
      if (n < max - 1)
        buf[n++] = (char)c;
    }
    while (n > 0 && buf[n - 1] == '\n')
      n--;
    buf[n] = 0;
    return 1;
  }
  char sep = rs[0];
  while ((c = fgetc(f)) != EOF && c != sep)
    if (n < max - 1)
      buf[n++] = (char)c;
  buf[n] = 0;
  return n > 0 || c != EOF;
}

static int
next_record(void)
{
  for (;;) {
    if (input == 0 && !next_input())
      return 0;
    if (read_record(input, record, sizeof(record))) {
      record_valid = 1;
      fields_valid = 0;
      setvar_num("NR", getvar_num("NR") + 1);
      setvar_num("FNR", getvar_num("FNR") + 1);
      return 1;
    }
    if (input != stdin)
      fclose(input);
    input = 0;
    setvar_num("FNR", 0);
    if (input_index >= ninput)
      return 0;
  }
}

// getline, in the forms POSIX defines that make sense without pipes
static int
getline_into(struct node *target, struct node *from)
{
  char buf[sizeof(record)];
  FILE *f = input;

  if (from) {
    struct cell c;
    memset(&c, 0, sizeof(c));
    eval(from, &c);
    static FILE *open_file;
    static char open_name[256];
    if (open_file == 0 || strcmp(open_name, to_str(&c)) != 0) {
      if (open_file)
        fclose(open_file);
      open_file = fopen(to_str(&c), "r");
      snprintf(open_name, sizeof(open_name), "%s", to_str(&c));
    }
    free(c.str);
    if (open_file == 0)
      return -1;
    f = open_file;
    if (!read_record(f, buf, sizeof(buf)))
      return 0;
  } else {
    if (f == 0 && !next_input())
      return 0;
    f = input;
    if (!read_record(f, buf, sizeof(buf)))
      return 0;
    setvar_num("NR", getvar_num("NR") + 1);
    setvar_num("FNR", getvar_num("FNR") + 1);
  }
  struct cell v;
  memset(&v, 0, sizeof(v));
  set_input(&v, buf);
  if (target)
    assign_to(target, &v);
  else
    set_field(0, &v);
  free(v.str);
  return 1;
}

// ---- statements ----

static void
do_print(struct node *n)
{
  FILE *out = stdout;
  struct cell c;

  memset(&c, 0, sizeof(c));
  if (n->b) {
    struct cell name;
    memset(&name, 0, sizeof(name));
    eval(n->b, &name);
    out = stream_for(to_str(&name), n->op);
    free(name.str);
  }
  if (n->type == N_PRINTF) {
    if (n->a == 0)
      fatal("printf needs a format");
    eval(n->a, &c);
    char fmt[4096];
    snprintf(fmt, sizeof(fmt), "%s", to_str(&c));
    free(c.str);
    awk_printf(out, fmt, n->a->next);
    return;
  }
  const char *ofs = getvar_str("OFS");
  char ofs_copy[64];
  snprintf(ofs_copy, sizeof(ofs_copy), "%s", ofs);
  if (n->a == 0) {
    fputs(to_str(field(0)), out);
  } else {
    for (struct node *e = n->a; e; e = e->next) {
      memset(&c, 0, sizeof(c));
      eval(e, &c);
      if (e != n->a)
        fputs(ofs_copy, out);
      fputs(to_str(&c), out);
      free(c.str);
    }
  }
  fputs(getvar_str("ORS"), out);
}

static void
exec_node(struct node *n)
{
  struct cell c;

  if (n == 0 || flow != F_NORMAL)
    return;
  memset(&c, 0, sizeof(c));

  switch (n->type) {
  case N_BLOCK:
    for (struct node *s = n->a; s && flow == F_NORMAL; s = s->next)
      exec_node(s);
    return;
  case N_PRINT:
  case N_PRINTF:
    do_print(n);
    return;
  case N_IF:
    eval(n->a, &c);
    if (is_true(&c))
      exec_node(n->b);
    else
      exec_node(n->c);
    free(c.str);
    return;
  case N_WHILE:
    for (;;) {
      eval(n->a, &c);
      int go = is_true(&c);
      free(c.str);
      memset(&c, 0, sizeof(c));
      if (!go)
        break;
      exec_node(n->b);
      if (flow == F_BREAK) {
        flow = F_NORMAL;
        break;
      }
      if (flow == F_CONTINUE)
        flow = F_NORMAL;
      if (flow != F_NORMAL)
        break;
    }
    return;
  case N_DO:
    for (;;) {
      exec_node(n->b);
      if (flow == F_BREAK) {
        flow = F_NORMAL;
        break;
      }
      if (flow == F_CONTINUE)
        flow = F_NORMAL;
      if (flow != F_NORMAL)
        break;
      eval(n->a, &c);
      int go = is_true(&c);
      free(c.str);
      memset(&c, 0, sizeof(c));
      if (!go)
        break;
    }
    return;
  case N_FOR:
    if (n->a)
      exec_node(n->a);
    for (;;) {
      if (n->b) {
        eval(n->b, &c);
        int go = is_true(&c);
        free(c.str);
        memset(&c, 0, sizeof(c));
        if (!go)
          break;
      }
      exec_node(n->d);
      if (flow == F_BREAK) {
        flow = F_NORMAL;
        break;
      }
      if (flow == F_CONTINUE)
        flow = F_NORMAL;
      if (flow != F_NORMAL)
        break;
      if (n->c)
        exec_node(n->c);
    }
    return;
  case N_FORIN: {
    struct array *arr = array_of(n->a->str);
    // the keys are copied first: the body may add or remove entries
    char *keys[1024];
    int nk = 0;
    for (int i = 0; i < HASHSIZE && nk < 1024; i++)
      for (struct anode *a = arr->tab[i]; a && nk < 1024; a = a->next)
        keys[nk++] = xstrdup(a->key);
    struct node target;
    memset(&target, 0, sizeof(target));
    target.type = N_VAR;
    target.str = n->str;
    for (int i = 0; i < nk; i++) {
      struct cell k;
      memset(&k, 0, sizeof(k));
      set_input(&k, keys[i]);
      assign_to(&target, &k);
      free(k.str);
      exec_node(n->b);
      if (flow == F_BREAK) {
        flow = F_NORMAL;
        break;
      }
      if (flow == F_CONTINUE)
        flow = F_NORMAL;
      if (flow != F_NORMAL)
        break;
    }
    for (int i = 0; i < nk; i++)
      free(keys[i]);
    return;
  }
  case N_NEXT:
    flow = F_NEXT;
    return;
  case N_EXIT:
    if (n->a) {
      eval(n->a, &c);
      exit_code = (int)to_num(&c);
      free(c.str);
    }
    flow = F_EXIT;
    return;
  case N_RETURN:
    if (n->a) {
      struct cell v;
      memset(&v, 0, sizeof(v));
      eval(n->a, &v);
      copy_cell(&return_value, &v);
      free(v.str);
    }
    flow = F_RETURN;
    return;
  case N_BREAK:
    flow = F_BREAK;
    return;
  case N_CONTINUE:
    flow = F_CONTINUE;
    return;
  case N_DELETE: {
    struct array *arr = array_of(n->str);
    if (n->a == 0) {
      array_clear(arr);
    } else {
      char key[1024];
      subscript(n->a, key, sizeof(key));
      array_delete(arr, key);
    }
    return;
  }
  case N_EMPTY:
    return;
  default:
    eval(n, &c);
    free(c.str);
    return;
  }
}

// ---- the main loop ----

static void
run_rules(void)
{
  for (struct node *r = rules; r && flow == F_NORMAL; r = r->next) {
    int fire;
    struct cell c;
    memset(&c, 0, sizeof(c));

    if (r->a == 0) {
      fire = 1;
    } else if (r->c) {
      // a range: it opens on the first and closes on the second
      if (!r->num) {
        eval(r->a, &c);
        fire = is_true(&c);
        free(c.str);
        if (fire) {
          r->num = 1;
          memset(&c, 0, sizeof(c));
          eval(r->c, &c);
          if (is_true(&c))
            r->num = 0;
          free(c.str);
        }
      } else {
        fire = 1;
        memset(&c, 0, sizeof(c));
        eval(r->c, &c);
        if (is_true(&c))
          r->num = 0;
        free(c.str);
      }
    } else {
      eval(r->a, &c);
      fire = is_true(&c);
      free(c.str);
    }
    if (!fire)
      continue;
    if (r->b == 0) {
      fputs(to_str(field(0)), stdout);
      fputs(getvar_str("ORS"), stdout);
    } else {
      exec_node(r->b);
    }
    if (flow == F_NEXT) {
      flow = F_NORMAL;
      return;
    }
  }
}

int
main(int argc, char *argv[])
{
  static char program[65536];
  int have_program = 0, i = 1;

  setvar_str("FS", " ");
  setvar_str("OFS", " ");
  setvar_str("ORS", "\n");
  setvar_str("RS", "\n");
  setvar_str("SUBSEP", "\034");
  setvar_str("CONVFMT", "%.6g");
  setvar_str("OFMT", "%.6g");
  setvar_num("NR", 0);
  setvar_num("FNR", 0);
  setvar_num("RSTART", 0);
  setvar_num("RLENGTH", -1);

  // -v assignments, which happen before BEGIN
  char *vassign[32];
  int nvassign = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strncmp(argv[i], "-F", 2) == 0) {
      const char *fs = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : " ");
      // awk -F'\t' is a tab, as everyone writes it
      if (strcmp(fs, "\\t") == 0)
        fs = "\t";
      setvar_str("FS", fs);
    } else if (strncmp(argv[i], "-v", 2) == 0) {
      const char *a = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "");
      if (nvassign < 32)
        vassign[nvassign++] = (char *)a;
    } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
      FILE *f = fopen(argv[++i], "r");
      if (f == 0)
        fatal("%s: %s", argv[i], strerror(errno));
      int n = strlen(program);
      n += fread(program + n, 1, sizeof(program) - n - 1, f);
      program[n] = 0;
      fclose(f);
      have_program = 1;
    } else if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    } else {
      fprintf(stderr, "usage: awk [-F SEP] [-v NAME=VALUE] 'PROGRAM' [FILE...]\n");
      exit(2);
    }
  }
  if (!have_program) {
    if (i >= argc) {
      fprintf(stderr, "usage: awk [-F SEP] [-v NAME=VALUE] 'PROGRAM' [FILE...]\n");
      exit(2);
    }
    snprintf(program, sizeof(program), "%s", argv[i++]);
  }

  for (int k = 0; k < nvassign; k++) {
    char *eq = strchr(vassign[k], '=');
    if (eq) {
      *eq = 0;
      setvar_str(vassign[k], eq + 1);
      *eq = '=';
    }
  }

  src = program;
  parse_program();

  input_files = &argv[i];
  ninput = argc - i;

  for (struct node *r = begin_rules; r && flow == F_NORMAL; r = r->next)
    exec_node(r->b);

  if (flow != F_EXIT && (rules || end_rules)) {
    while (next_record()) {
      flow = F_NORMAL;
      run_rules();
      if (flow == F_EXIT)
        break;
    }
  }
  if (flow == F_EXIT)
    flow = F_NORMAL;
  for (struct node *r = end_rules; r && flow == F_NORMAL; r = r->next)
    exec_node(r->b);

  fflush(stdout);
  close_streams();
  exit(exit_code);
}
