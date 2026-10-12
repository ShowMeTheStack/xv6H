// expr: evaluate one expression and print what it comes to.
//
//   expr ARG...
//
// the grammar POSIX gives, loosest binding first:
//
//   |   the left side if it is neither empty nor zero, else the right
//   &   the left side when neither is empty or zero, else 0
//   = > >= < <= != !=   comparison, numeric when both sides are numbers
//   + -
//   * / %
//   :   the left side matched against a basic regular expression
//
// the exit status is 0 when the result is neither empty nor zero, 1
// when it is, 2 when the expression is malformed and 3 on an error.
#include "user/user.h"
#include <regex.h>

static char **words;
static int nwords, at;

static void
die(const char *msg)
{
  fprintf(stderr, "expr: %s\n", msg);
  exit(2);
}

static const char *
peek(void)
{
  return at < nwords ? words[at] : 0;
}

static const char *
take(void)
{
  return at < nwords ? words[at++] : 0;
}

// a value is a string; whether it is also a number is asked when it
// matters, which is what expr does
struct value {
  char s[256];
};

static struct value
from_string(const char *s)
{
  struct value v;

  snprintf(v.s, sizeof(v.s), "%s", s);
  return v;
}

static struct value
from_number(long long n)
{
  struct value v;

  snprintf(v.s, sizeof(v.s), "%lld", n);
  return v;
}

static int
is_number(const struct value *v, long long *out)
{
  const char *p = v->s;
  long long n = 0;
  int sign = 1;

  if (*p == 0)
    return 0;
  if (*p == '-' || *p == '+') {
    sign = *p == '-' ? -1 : 1;
    p++;
    if (*p == 0)
      return 0;
  }
  for (; *p; p++) {
    if (*p < '0' || *p > '9')
      return 0;
    n = n * 10 + (*p - '0');
  }
  *out = n * sign;
  return 1;
}

static int
is_true(const struct value *v)
{
  long long n;

  if (v->s[0] == 0)
    return 0;
  if (is_number(v, &n))
    return n != 0;
  return 1;
}

static struct value parse_or(void);

// the : operator: the left side is matched from its start against a
// basic regular expression. with a subexpression the match is what it
// caught, otherwise how many characters matched.
static struct value
do_match(const struct value *a, const struct value *b)
{
  regex_t re;
  regmatch_t m[2];
  char anchored[512];

  // the match is always from the beginning, whether or not it says ^
  if (b->s[0] == '^')
    snprintf(anchored, sizeof(anchored), "%s", b->s);
  else
    snprintf(anchored, sizeof(anchored), "^%s", b->s);
  if (regcomp(&re, anchored, 0) != 0) {
    fprintf(stderr, "expr: %s: bad regular expression\n", b->s);
    exit(2);
  }
  int has_group = re.re_nsub > 0;
  struct value out;
  if (regexec(&re, a->s, 2, m, 0) != 0) {
    out = has_group ? from_string("") : from_number(0);
  } else if (has_group) {
    if (m[1].rm_so < 0)
      out = from_string("");
    else
      out = from_string(""), snprintf(out.s, sizeof(out.s), "%.*s",
                                      (int)(m[1].rm_eo - m[1].rm_so),
                                      a->s + m[1].rm_so);
  } else {
    out = from_number(m[0].rm_eo);
  }
  regfree(&re);
  return out;
}

static struct value
parse_primary(void)
{
  const char *w = take();

  if (w == 0)
    die("the expression ends too soon");
  if (strcmp(w, "(") == 0) {
    struct value v = parse_or();
    const char *close = take();
    if (close == 0 || strcmp(close, ")") != 0)
      die("expected )");
    return v;
  }
  // the four named operators POSIX gives, which take the place of a
  // value when they lead
  if (strcmp(w, "length") == 0 && peek())
    return from_number((long long)strlen(take()));
  if (strcmp(w, "substr") == 0 && peek()) {
    const char *s = take();
    const char *p = take();
    const char *l = take();
    if (s == 0 || p == 0 || l == 0)
      die("substr wants three arguments");
    long long pos = atol(p), len = atol(l);
    long long n = (long long)strlen(s);
    if (pos < 1 || pos > n || len <= 0)
      return from_string("");
    if (pos - 1 + len > n)
      len = n - (pos - 1);
    struct value v;
    snprintf(v.s, sizeof(v.s), "%.*s", (int)len, s + pos - 1);
    return v;
  }
  if (strcmp(w, "index") == 0 && peek()) {
    const char *s = take();
    const char *set = take();
    if (s == 0 || set == 0)
      die("index wants two arguments");
    for (int i = 0; s[i]; i++)
      if (strchr(set, s[i]))
        return from_number(i + 1);
    return from_number(0);
  }
  if (strcmp(w, "match") == 0 && peek()) {
    const char *s = take();
    const char *p = take();
    if (s == 0 || p == 0)
      die("match wants two arguments");
    struct value a = from_string(s), b = from_string(p);
    return do_match(&a, &b);
  }
  if (strcmp(w, "+") == 0 && peek())
    return from_string(take());   // + makes the next word a plain value
  return from_string(w);
}

static struct value
parse_match(void)
{
  struct value left = parse_primary();

  while (peek() && strcmp(peek(), ":") == 0) {
    at++;
    struct value right = parse_primary();
    left = do_match(&left, &right);
  }
  return left;
}

static struct value
parse_mul(void)
{
  struct value left = parse_match();

  for (;;) {
    const char *op = peek();
    if (op == 0 || (strcmp(op, "*") != 0 && strcmp(op, "/") != 0 &&
                    strcmp(op, "%") != 0))
      return left;
    at++;
    struct value right = parse_match();
    long long a, b;
    if (!is_number(&left, &a) || !is_number(&right, &b))
      die("a numeric operand is expected");
    if ((op[0] == '/' || op[0] == '%') && b == 0) {
      fprintf(stderr, "expr: division by zero\n");
      exit(2);
    }
    left = from_number(op[0] == '*' ? a * b : op[0] == '/' ? a / b : a % b);
  }
}

static struct value
parse_add(void)
{
  struct value left = parse_mul();

  for (;;) {
    const char *op = peek();
    if (op == 0 || (strcmp(op, "+") != 0 && strcmp(op, "-") != 0))
      return left;
    at++;
    struct value right = parse_mul();
    long long a, b;
    if (!is_number(&left, &a) || !is_number(&right, &b))
      die("a numeric operand is expected");
    left = from_number(op[0] == '+' ? a + b : a - b);
  }
}

static struct value
parse_compare(void)
{
  struct value left = parse_add();

  for (;;) {
    const char *op = peek();
    if (op == 0)
      return left;
    if (strcmp(op, "=") != 0 && strcmp(op, "==") != 0 &&
        strcmp(op, "!=") != 0 && strcmp(op, "<") != 0 &&
        strcmp(op, "<=") != 0 && strcmp(op, ">") != 0 &&
        strcmp(op, ">=") != 0)
      return left;
    at++;
    struct value right = parse_add();
    long long a, b;
    int cmp;
    // numbers compare as numbers and anything else as text, which is
    // what every expr does
    if (is_number(&left, &a) && is_number(&right, &b))
      cmp = a < b ? -1 : a > b ? 1 : 0;
    else
      cmp = strcmp(left.s, right.s);
    int r;
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) r = cmp == 0;
    else if (strcmp(op, "!=") == 0) r = cmp != 0;
    else if (strcmp(op, "<") == 0) r = cmp < 0;
    else if (strcmp(op, "<=") == 0) r = cmp <= 0;
    else if (strcmp(op, ">") == 0) r = cmp > 0;
    else r = cmp >= 0;
    left = from_number(r);
  }
}

static struct value
parse_and(void)
{
  struct value left = parse_compare();

  while (peek() && strcmp(peek(), "&") == 0) {
    at++;
    struct value right = parse_compare();
    if (!is_true(&left) || !is_true(&right))
      left = from_number(0);
  }
  return left;
}

static struct value
parse_or(void)
{
  struct value left = parse_and();

  while (peek() && strcmp(peek(), "|") == 0) {
    at++;
    struct value right = parse_and();
    if (!is_true(&left))
      left = is_true(&right) ? right : from_number(0);
  }
  return left;
}

int
main(int argc, char *argv[])
{
  int start = 1;

  if (argc > 1 && strcmp(argv[1], "--") == 0)
    start = 2;
  if (argc <= start) {
    fprintf(stderr, "usage: expr EXPRESSION\n");
    exit(2);
  }
  words = argv + start;
  nwords = argc - start;
  at = 0;

  struct value v = parse_or();
  if (at < nwords)
    die("the expression has something left over");
  printf("%s\n", v.s);
  exit(is_true(&v) ? 0 : 1);
}
