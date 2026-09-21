// bc: an arbitrary precision calculator.
//
//   bc [-l] [-q] [FILE...]
//
//   -l  load the maths library and start with scale 20
//   -q  no banner
//
// the language POSIX describes: expressions with the usual operators
// and the relational ones, named variables and arrays, `scale`,
// `ibase` and `obase`, `if`, `while`, `for`, `break`, `continue`,
// `define` with `auto` locals and `return`, `print`, `length`, `sqrt`
// and `read`, `++` and `--` in both positions, and the compound
// assignments.
//
// the numbers are the point. a value here is a sign, a string of
// decimal digits and a count of how many of them are after the point;
// nothing is a machine integer, so 2^500 and a thousand digits of a
// square root are ordinary results rather than overflow. the
// arithmetic is schoolbook: add and subtract by columns, multiply by
// the long method, divide by trial digits. that is slower than the
// clever methods for very large numbers and it is exactly right,
// which is what a calculator owes the person using it.
#include "user/user.h"

#define MAXDIGITS 4096
#define MAXNAME 32
#define MAXPARAMS 16
#define MAXAUTOS 16

// ---- numbers ----

// digits are held most significant first, one decimal digit per byte
// as a value 0..9. `scale` says how many of them are fractional.
struct num {
  int neg;
  int len;      // how many digits there are
  int scale;    // how many of them are after the point
  signed char d[MAXDIGITS];
};

static void
num_zero(struct num *n)
{
  n->neg = 0;
  n->len = 1;
  n->scale = 0;
  n->d[0] = 0;
}

static int
num_is_zero(const struct num *n)
{
  for (int i = 0; i < n->len; i++)
    if (n->d[i])
      return 0;
  return 1;
}

// drop leading zeros in the whole part and trailing zeros past the
// point, so that every number has one spelling
static void
num_trim(struct num *n)
{
  int whole = n->len - n->scale;

  while (whole > 1 && n->d[0] == 0) {
    memmove(n->d, n->d + 1, n->len - 1);
    n->len--;
    whole--;
  }
  if (whole <= 0) {
    // nothing but fraction: keep one zero in front of the point
    memmove(n->d + 1, n->d, n->len);
    n->d[0] = 0;
    n->len++;
    whole = 1;
  }
  if (num_is_zero(n))
    n->neg = 0;
}

static void
num_from_long(struct num *n, long long v)
{
  char tmp[32];
  int k = 0;

  num_zero(n);
  n->neg = v < 0;
  unsigned long long u = n->neg ? (unsigned long long)(-(v + 1)) + 1
                                : (unsigned long long)v;
  if (u == 0) {
    n->len = 1;
    n->d[0] = 0;
    return;
  }
  while (u) {
    tmp[k++] = (char)(u % 10);
    u /= 10;
  }
  n->len = k;
  for (int i = 0; i < k; i++)
    n->d[i] = tmp[k - 1 - i];
  n->scale = 0;
}

static long long
num_to_long(const struct num *n)
{
  long long v = 0;
  int whole = n->len - n->scale;

  for (int i = 0; i < whole; i++)
    v = v * 10 + n->d[i];
  return n->neg ? -v : v;
}

// compare the magnitudes, ignoring the signs
static int
num_cmp_abs(const struct num *a, const struct num *b)
{
  int aw = a->len - a->scale, bw = b->len - b->scale;

  if (aw != bw)
    return aw > bw ? 1 : -1;
  int scale = a->scale > b->scale ? a->scale : b->scale;
  for (int i = 0; i < aw + scale; i++) {
    int da = i < a->len ? a->d[i] : 0;
    int db = i < b->len ? b->d[i] : 0;
    // past the end of one of them the digits are zero
    if (i >= aw + a->scale)
      da = 0;
    if (i >= bw + b->scale)
      db = 0;
    if (da != db)
      return da > db ? 1 : -1;
  }
  return 0;
}

static int
num_cmp(const struct num *a, const struct num *b)
{
  if (num_is_zero(a) && num_is_zero(b))
    return 0;
  if (a->neg != b->neg)
    return a->neg ? -1 : 1;
  int c = num_cmp_abs(a, b);
  return a->neg ? -c : c;
}

static void add_abs(const struct num *a, const struct num *b, struct num *out);
static void sub_abs(const struct num *a, const struct num *b, struct num *out);

static void
num_add(const struct num *a, const struct num *b, struct num *out)
{
  if (a->neg == b->neg) {
    add_abs(a, b, out);
    out->neg = a->neg;
  } else {
    int c = num_cmp_abs(a, b);
    if (c == 0) {
      num_zero(out);
      return;
    }
    if (c > 0) {
      sub_abs(a, b, out);
      out->neg = a->neg;
    } else {
      sub_abs(b, a, out);
      out->neg = b->neg;
    }
  }
  num_trim(out);
}

static void
num_sub(const struct num *a, const struct num *b, struct num *out)
{
  struct num nb = *b;

  if (!num_is_zero(&nb))
    nb.neg = !nb.neg;
  num_add(a, &nb, out);
}

static void
add_abs(const struct num *a, const struct num *b, struct num *out)
{
  int aw = a->len - a->scale, bw = b->len - b->scale;
  int whole = (aw > bw ? aw : bw) + 1;
  int scale = a->scale > b->scale ? a->scale : b->scale;
  int len = whole + scale;

  if (len > MAXDIGITS)
    len = MAXDIGITS;
  static signed char tmp[MAXDIGITS];
  int carry = 0;
  for (int i = len - 1; i >= 0; i--) {
    // column i counts from the left of the widened layout
    int ai = i - (whole - aw);
    int bi = i - (whole - bw);
    int da = (ai >= 0 && ai < a->len) ? a->d[ai] : 0;
    int db = (bi >= 0 && bi < b->len) ? b->d[bi] : 0;
    int s = da + db + carry;
    tmp[i] = (signed char)(s % 10);
    carry = s / 10;
  }
  out->neg = 0;
  out->len = len;
  out->scale = scale;
  memmove(out->d, tmp, len);
  num_trim(out);
}

// the magnitude of a minus the magnitude of b, where a is the larger
static void
sub_abs(const struct num *a, const struct num *b, struct num *out)
{
  int aw = a->len - a->scale, bw = b->len - b->scale;
  int whole = aw > bw ? aw : bw;
  int scale = a->scale > b->scale ? a->scale : b->scale;
  int len = whole + scale;

  if (len > MAXDIGITS)
    len = MAXDIGITS;
  static signed char tmp[MAXDIGITS];
  int borrow = 0;
  for (int i = len - 1; i >= 0; i--) {
    int ai = i - (whole - aw);
    int bi = i - (whole - bw);
    int da = (ai >= 0 && ai < a->len) ? a->d[ai] : 0;
    int db = (bi >= 0 && bi < b->len) ? b->d[bi] : 0;
    int s = da - db - borrow;
    if (s < 0) {
      s += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    tmp[i] = (signed char)s;
  }
  out->neg = 0;
  out->len = len;
  out->scale = scale;
  memmove(out->d, tmp, len);
  num_trim(out);
}

static void
num_mul(const struct num *a, const struct num *b, struct num *out)
{
  static signed char tmp[MAXDIGITS * 2];
  int n = a->len + b->len;

  if (n > MAXDIGITS * 2)
    n = MAXDIGITS * 2;
  for (int i = 0; i < n; i++)
    tmp[i] = 0;
  for (int i = a->len - 1; i >= 0; i--) {
    int carry = 0;
    for (int j = b->len - 1; j >= 0; j--) {
      int at = i + j + 1;
      if (at >= n)
        continue;
      int s = tmp[at] + a->d[i] * b->d[j] + carry;
      tmp[at] = (signed char)(s % 10);
      carry = s / 10;
    }
    int at = i;
    while (carry > 0 && at >= 0) {
      int s = tmp[at] + carry;
      tmp[at] = (signed char)(s % 10);
      carry = s / 10;
      at--;
    }
  }
  int len = a->len + b->len;
  int scale = a->scale + b->scale;
  if (len > MAXDIGITS) {
    // keep the most significant digits; a result this large is past
    // what this calculator promises
    len = MAXDIGITS;
  }
  out->neg = (a->neg != b->neg) && !(num_is_zero(a) || num_is_zero(b));
  out->len = len;
  out->scale = scale;
  memmove(out->d, tmp + (a->len + b->len - len), len);
  num_trim(out);
}

// shift the point right by `k`, which is multiplying by ten k times
static void
num_shift(struct num *n, int k)
{
  while (k > 0 && n->scale > 0) {
    n->scale--;
    k--;
  }
  while (k > 0 && n->len < MAXDIGITS) {
    n->d[n->len++] = 0;
    k--;
  }
  num_trim(n);
}

// a divided by b, kept to `scale` places after the point.
//
// the two numbers are first made whole. a is A/10^sa and b is B/10^sb,
// so a/b is A*10^sb / (B*10^sa); multiplying the top by another
// 10^scale gives the digits wanted, and the point then goes `scale`
// from the right. the digits come out one at a time by repeated
// subtraction, which needs at most ten tries each.
static int
num_div(const struct num *a, const struct num *b, int scale, struct num *out)
{
  if (num_is_zero(b))
    return -1;

  struct num na = *a, nb = *b;
  na.neg = nb.neg = 0;
  int sa = na.scale, sb = nb.scale;
  na.scale = 0;
  nb.scale = 0;
  num_trim(&na);
  num_trim(&nb);

  // the top gains sb + scale zeros, the bottom gains sa
  for (int i = 0; i < sb + scale && na.len < MAXDIGITS; i++)
    na.d[na.len++] = 0;
  for (int i = 0; i < sa && nb.len < MAXDIGITS; i++)
    nb.d[nb.len++] = 0;
  num_trim(&na);
  num_trim(&nb);

  static signed char q[MAXDIGITS];
  int qlen = 0;
  struct num rem;
  num_zero(&rem);

  for (int i = 0; i < na.len; i++) {
    // bring down one digit
    if (rem.len == 1 && rem.d[0] == 0)
      rem.d[0] = na.d[i];
    else if (rem.len < MAXDIGITS)
      rem.d[rem.len++] = na.d[i];
    rem.scale = 0;
    num_trim(&rem);

    int digit = 0;
    while (digit < 10 && num_cmp_abs(&rem, &nb) >= 0) {
      struct num next;
      sub_abs(&rem, &nb, &next);
      rem = next;
      digit++;
    }
    if (qlen < MAXDIGITS)
      q[qlen++] = (signed char)digit;
  }

  if (qlen == 0) {
    q[0] = 0;
    qlen = 1;
  }
  out->neg = (a->neg != b->neg);
  out->len = qlen;
  out->scale = scale;
  memmove(out->d, q, qlen);
  // a quotient with more places than digits needs a zero in front of
  // the point, which num_trim puts there
  num_trim(out);
  return 0;
}

// the remainder bc defines: a - (a/b truncated to `scale`) * b
static int
num_mod(const struct num *a, const struct num *b, int scale, struct num *out)
{
  struct num q, p;

  if (num_div(a, b, scale, &q) < 0)
    return -1;
  // truncate the quotient to `scale` places, which num_div already did
  num_mul(&q, b, &p);
  num_sub(a, &p, out);
  return 0;
}

// a raised to a whole power, by squaring
static void
num_pow(const struct num *a, long long e, int scale, struct num *out)
{
  struct num base = *a, acc;
  int negexp = e < 0;

  if (negexp)
    e = -e;
  num_from_long(&acc, 1);
  while (e > 0) {
    if (e & 1) {
      struct num t;
      num_mul(&acc, &base, &t);
      acc = t;
    }
    e >>= 1;
    if (e) {
      struct num t;
      num_mul(&base, &base, &t);
      base = t;
    }
  }
  if (negexp) {
    struct num one;
    num_from_long(&one, 1);
    if (num_div(&one, &acc, scale, out) < 0)
      num_zero(out);
    return;
  }
  *out = acc;
}

// the square root, by Newton's method: each step doubles the digits
// that are right, which is why it converges so quickly
static int
num_sqrt(const struct num *a, int scale, struct num *out)
{
  if (a->neg)
    return -1;
  if (num_is_zero(a)) {
    num_zero(out);
    return 0;
  }

  struct num x, two, prev;
  num_from_long(&two, 2);
  // start from something of about the right size
  int whole = a->len - a->scale;
  num_from_long(&x, 1);
  num_shift(&x, (whole + 1) / 2);

  int work = scale + 3;
  for (int i = 0; i < 200; i++) {
    struct num t, s;
    prev = x;
    if (num_div(a, &x, work, &t) < 0)
      return -1;
    num_add(&x, &t, &s);
    if (num_div(&s, &two, work, &x) < 0)
      return -1;
    if (num_cmp(&x, &prev) == 0)
      break;
  }
  // round down to the asked-for scale, which is what bc does
  struct num one;
  num_from_long(&one, 1);
  num_div(&x, &one, scale, out);
  return 0;
}

// ---- reading and writing numbers ----

static int ibase = 10, obase = 10, scale_var = 0;

// the digits above nine are written in capitals, as bc has always
// written them. lowercase letters are names -- without that rule
// `for`, `else`, `break`, `continue`, `define` and `auto` all begin
// with a hex digit and are read as numbers.
static int
digit_value(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// a literal in the current input base
static void
num_parse(struct num *n, const char *s)
{
  num_zero(n);
  struct num base, digit, t;
  num_from_long(&base, ibase);

  int seen_point = 0;
  int frac = 0;
  struct num whole;
  num_zero(&whole);
  for (const char *p = s; *p; p++) {
    if (*p == '.') {
      seen_point = 1;
      continue;
    }
    int v = digit_value(*p);
    if (v < 0)
      break;
    if (v >= ibase && ibase <= 10)
      v = ibase - 1;      // bc clamps a digit too big for the base
    num_mul(&whole, &base, &t);
    num_from_long(&digit, v);
    num_add(&t, &digit, &whole);
    if (seen_point)
      frac++;
  }
  if (ibase == 10) {
    // the usual case: the digits are already decimal, so the point
    // goes back where it was
    *n = whole;
    n->scale = frac;
    if (n->scale > n->len) {
      // .5 and the like: a zero is needed in front of the point
      int need = n->scale - n->len + 1;
      memmove(n->d + need, n->d, n->len);
      for (int i = 0; i < need; i++)
        n->d[i] = 0;
      n->len += need;
    }
    num_trim(n);
    return;
  }
  // another base: divide by the base once per fractional digit
  struct num div;
  num_from_long(&div, 1);
  for (int i = 0; i < frac; i++) {
    num_mul(&div, &base, &t);
    div = t;
  }
  if (frac > 0)
    num_div(&whole, &div, scale_var + frac, n);
  else
    *n = whole;
}

// a number too long for a line is broken with a backslash, as bc has
// always broken them. a line holds sixty-nine characters counting the
// backslash, so sixty-eight digits go on a line that continues and
// sixty-nine on one that does not -- which is why the number is
// gathered first and then written: the last character of a line can
// only be placed knowing whether anything follows it.
//
// only numbers are wrapped; what `print' is given goes out as it is.
static char outbuf[MAXDIGITS * 3];
static int outlen;

static void
out_char(int c)
{
  if (outlen < (int)sizeof(outbuf) - 1)
    outbuf[outlen++] = (char)c;
}

static void
out_string(const char *s)
{
  while (*s)
    out_char(*s++);
}

static void
out_end(void)
{
  int col = 0;

  for (int i = 0; i < outlen; i++) {
    // the 69th character of a line is only written when it is the
    // last, because otherwise the backslash needs that place
    if (col == 68 && outlen - i > 1) {
      putchar('\\');
      putchar('\n');
      col = 0;
    }
    putchar(outbuf[i]);
    col++;
  }
  putchar('\n');
  outlen = 0;
}

static void
print_in_base(const struct num *n, int base)
{
  // the whole part, by repeated division
  struct num whole = *n, frac, b, t;
  whole.neg = 0;
  // split off the fraction
  struct num one;
  num_from_long(&one, 1);
  num_div(&whole, &one, 0, &t);   // truncation to no places
  struct num ipart = t;
  num_sub(&whole, &ipart, &frac);
  num_from_long(&b, base);

  if (n->neg && !num_is_zero(n))
    out_char('-');

  static char out[MAXDIGITS * 2];
  int k = 0;
  if (num_is_zero(&ipart)) {
    out[k++] = '0';
  } else {
    struct num cur = ipart;
    while (!num_is_zero(&cur)) {
      struct num q, r;
      num_div(&cur, &b, 0, &q);
      struct num p;
      num_mul(&q, &b, &p);
      num_sub(&cur, &p, &r);
      long long dv = num_to_long(&r);
      if (base <= 16) {
        out[k++] = "0123456789ABCDEF"[dv];
      } else {
        // a base above sixteen prints each digit as a number, which
        // is what bc does
        k += snprintf(out + k, sizeof(out) - k, "%s%lld",
                      k ? " " : "", dv);
      }
      cur = q;
      if (k >= (int)sizeof(out) - 8)
        break;
    }
    if (base <= 16)
      for (int i = 0; i < k / 2; i++) {
        char c = out[i];
        out[i] = out[k - 1 - i];
        out[k - 1 - i] = c;
      }
  }
  out[k] = 0;
  out_string(out);

  if (!num_is_zero(&frac) && n->scale > 0) {
    out_char('.');
    struct num f = frac;
    for (int i = 0; i < n->scale; i++) {
      struct num p;
      num_mul(&f, &b, &p);
      struct num ip;
      num_div(&p, &one, 0, &ip);
      long long dv = num_to_long(&ip);
      if (base <= 16) {
        out_char("0123456789ABCDEF"[dv < 0 ? -dv : dv]);
      } else {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "%s%lld", i ? " " : "", dv < 0 ? -dv : dv);
        out_string(tmp);
      }
      num_sub(&p, &ip, &f);
      if (num_is_zero(&f))
        break;
    }
  }
}

static void
num_print(const struct num *n)
{
  if (num_is_zero(n)) {
    // zero is written as one digit, however many places the scale
    // asks for -- .00 is not how bc says nothing
    printf("0\n");
    return;
  }
  outlen = 0;
  if (obase == 10) {
    if (n->neg)
      out_char('-');
    int whole = n->len - n->scale;
    // a value below one prints as .5, not 0.5, which is what bc does
    int skip = (whole == 1 && n->d[0] == 0 && n->scale > 0) ? 1 : 0;
    for (int i = skip; i < whole; i++)
      out_char('0' + n->d[i]);
    if (n->scale > 0) {
      out_char('.');
      for (int i = whole; i < n->len; i++)
        out_char('0' + n->d[i]);
    }
  } else {
    print_in_base(n, obase);
  }
  out_end();
}

// ---- the language ----

enum {
  T_EOF, T_NUM, T_NAME, T_STRING,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
  T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ, T_CARETEQ,
  T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
  T_AND, T_OR, T_NOT,
  T_INC, T_DEC,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
  T_SEMI, T_NEWLINE, T_COMMA,
  T_IF, T_ELSE, T_WHILE, T_FOR, T_BREAK, T_CONTINUE, T_RETURN,
  T_DEFINE, T_AUTO, T_PRINT, T_QUIT, T_LENGTH, T_SQRT, T_SCALEF, T_READ,
  T_IBASE, T_OBASE, T_SCALE, T_LAST
};

struct token {
  int kind;
  char text[MAXNAME + 1];
  char numtext[256];
};

static const char *src;
static const char *tok_start;   // where the current token begins
static struct token tok;
static int had_error;

static void
bc_error(const char *msg)
{
  fprintf(stderr, "bc: %s\n", msg);
  had_error = 1;
}

static void
next_token(void)
{
  const char *p = src;

  for (;;) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\\' && p[1] == '\n') {
      p += 2;
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      p += 2;
      while (*p && !(p[0] == '*' && p[1] == '/'))
        p++;
      if (*p)
        p += 2;
      continue;
    }
    if (p[0] == '#') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    break;
  }

  tok_start = p;
  tok.text[0] = 0;
  tok.numtext[0] = 0;
  if (*p == 0) {
    tok.kind = T_EOF;
    src = p;
    return;
  }
  if (*p == '\n') {
    tok.kind = T_NEWLINE;
    src = p + 1;
    return;
  }
  if (*p == '"') {
    p++;
    int k = 0;
    while (*p && *p != '"') {
      if (k < (int)sizeof(tok.numtext) - 1)
        tok.numtext[k++] = *p;
      p++;
    }
    tok.numtext[k] = 0;
    if (*p == '"')
      p++;
    tok.kind = T_STRING;
    src = p;
    return;
  }
  if (digit_value(*p) >= 0 || (*p == '.' && digit_value(p[1]) >= 0)) {
    // a number: decimal digits, the capitals A to F, and one point
    int k = 0;
    while (digit_value(*p) >= 0 || *p == '.') {
      if (k < (int)sizeof(tok.numtext) - 1)
        tok.numtext[k++] = *p;
      p++;
    }
    tok.numtext[k] = 0;
    tok.kind = T_NUM;
    src = p;
    return;
  }
  if ((*p >= 'a' && *p <= 'z') || *p == '_' || (*p >= 'G' && *p <= 'Z')) {
    int k = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
      if (k < MAXNAME)
        tok.text[k++] = *p;
      p++;
    }
    tok.text[k] = 0;
    src = p;
    static const struct {
      const char *word;
      int kind;
    } keywords[] = {
      {"if", T_IF}, {"else", T_ELSE}, {"while", T_WHILE}, {"for", T_FOR},
      {"break", T_BREAK}, {"continue", T_CONTINUE}, {"return", T_RETURN},
      {"define", T_DEFINE}, {"auto", T_AUTO}, {"print", T_PRINT},
      {"quit", T_QUIT}, {"halt", T_QUIT}, {"length", T_LENGTH},
      {"sqrt", T_SQRT}, {"scale", T_SCALE}, {"ibase", T_IBASE},
      {"obase", T_OBASE}, {"read", T_READ}, {"last", T_LAST}, {0, 0}
    };
    for (int i = 0; keywords[i].word; i++)
      if (strcmp(tok.text, keywords[i].word) == 0) {
        tok.kind = keywords[i].kind;
        return;
      }
    tok.kind = T_NAME;
    return;
  }

  int two = 0;
  switch (*p) {
  case '+': tok.kind = p[1] == '+' ? (two = 1, T_INC) : (p[1] == '=' ? (two = 1, T_PLUSEQ) : T_PLUS); break;
  case '-': tok.kind = p[1] == '-' ? (two = 1, T_DEC) : (p[1] == '=' ? (two = 1, T_MINUSEQ) : T_MINUS); break;
  case '*': tok.kind = p[1] == '=' ? (two = 1, T_STAREQ) : T_STAR; break;
  case '/': tok.kind = p[1] == '=' ? (two = 1, T_SLASHEQ) : T_SLASH; break;
  case '%': tok.kind = p[1] == '=' ? (two = 1, T_PERCENTEQ) : T_PERCENT; break;
  case '^': tok.kind = p[1] == '=' ? (two = 1, T_CARETEQ) : T_CARET; break;
  case '=': tok.kind = p[1] == '=' ? (two = 1, T_EQ) : T_ASSIGN; break;
  case '!': tok.kind = p[1] == '=' ? (two = 1, T_NE) : T_NOT; break;
  case '<': tok.kind = p[1] == '=' ? (two = 1, T_LE) : T_LT; break;
  case '>': tok.kind = p[1] == '=' ? (two = 1, T_GE) : T_GT; break;
  case '&': tok.kind = p[1] == '&' ? (two = 1, T_AND) : T_AND; if (p[1] == '&') two = 1; break;
  case '|': tok.kind = p[1] == '|' ? (two = 1, T_OR) : T_OR; if (p[1] == '|') two = 1; break;
  case '(': tok.kind = T_LPAREN; break;
  case ')': tok.kind = T_RPAREN; break;
  case '{': tok.kind = T_LBRACE; break;
  case '}': tok.kind = T_RBRACE; break;
  case '[': tok.kind = T_LBRACK; break;
  case ']': tok.kind = T_RBRACK; break;
  case ';': tok.kind = T_SEMI; break;
  case ',': tok.kind = T_COMMA; break;
  default:
    bc_error("unexpected character");
    tok.kind = T_EOF;
    src = p + 1;
    return;
  }
  src = p + (two ? 2 : 1);
}

// ---- variables ----

struct var {
  char name[MAXNAME + 1];
  struct num value;
  struct num *array;     // an array of MAXARRAY, allocated on first use
  int arraylen;
};

#define MAXVARS 256
#define MAXARRAY 2048

static struct var vars[MAXVARS];
static int nvars;

static struct var *
find_var(const char *name, int make)
{
  for (int i = 0; i < nvars; i++)
    if (strcmp(vars[i].name, name) == 0)
      return &vars[i];
  if (!make || nvars >= MAXVARS)
    return 0;
  struct var *v = &vars[nvars++];
  snprintf(v->name, sizeof(v->name), "%s", name);
  num_zero(&v->value);
  v->array = 0;
  v->arraylen = 0;
  return v;
}

// ---- functions ----

struct func {
  char name[MAXNAME + 1];
  char params[MAXPARAMS][MAXNAME + 1];
  int nparams;
  char autos[MAXAUTOS][MAXNAME + 1];
  int nautos;
  char *body;
  int defined;
};

#define MAXFUNCS 64
static struct func funcs[MAXFUNCS];
static int nfuncs;

static struct func *
find_func(const char *name)
{
  for (int i = 0; i < nfuncs; i++)
    if (strcmp(funcs[i].name, name) == 0)
      return &funcs[i];
  return 0;
}

// ---- evaluating ----

enum { FLOW_NONE, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_QUIT };

static int flow;
static struct num retval;
static int depth;

static void parse_expr(struct num *out);
static void parse_statement(void);
static int is_assignment(void);

static void
expect(int kind, const char *what)
{
  if (tok.kind != kind) {
    bc_error(what);
    return;
  }
  next_token();
}

static void
call_function(struct func *f, struct num *out)
{
  struct num args[MAXPARAMS];
  int nargs = 0;

  expect(T_LPAREN, "expected (");
  if (tok.kind != T_RPAREN) {
    for (;;) {
      if (nargs < MAXPARAMS)
        parse_expr(&args[nargs++]);
      else {
        struct num junk;
        parse_expr(&junk);
      }
      if (tok.kind != T_COMMA)
        break;
      next_token();
    }
  }
  expect(T_RPAREN, "expected )");

  if (f == 0 || !f->defined) {
    bc_error("undefined function");
    num_zero(out);
    return;
  }
  if (++depth > 64) {
    bc_error("too deep");
    depth--;
    num_zero(out);
    return;
  }

  // the parameters and the locals are saved and put back afterwards,
  // which is what `auto` means
  struct num saved[MAXPARAMS + MAXAUTOS];
  struct var *slots[MAXPARAMS + MAXAUTOS];
  int nsaved = 0;
  for (int i = 0; i < f->nparams; i++) {
    struct var *v = find_var(f->params[i], 1);
    slots[nsaved] = v;
    saved[nsaved] = v->value;
    nsaved++;
    if (i < nargs)
      v->value = args[i];
    else
      num_zero(&v->value);
  }
  for (int i = 0; i < f->nautos; i++) {
    struct var *v = find_var(f->autos[i], 1);
    slots[nsaved] = v;
    saved[nsaved] = v->value;
    nsaved++;
    num_zero(&v->value);
  }

  const char *saved_src = src, *saved_start = tok_start;
  struct token saved_tok = tok;
  int saved_flow = flow;

  src = f->body;
  next_token();
  flow = FLOW_NONE;
  num_zero(&retval);
  while (tok.kind != T_EOF && flow != FLOW_RETURN && flow != FLOW_QUIT)
    parse_statement();
  *out = retval;
  if (flow != FLOW_QUIT)
    flow = saved_flow;

  src = saved_src;
  tok_start = saved_start;
  tok = saved_tok;
  for (int i = 0; i < nsaved; i++)
    slots[i]->value = saved[i];
  depth--;
}

// a name may be a variable, an array element or a call
static void
parse_name(struct num *out, struct var **lvalue, int *index)
{
  char name[MAXNAME + 1];

  snprintf(name, sizeof(name), "%s", tok.text);
  next_token();
  if (lvalue)
    *lvalue = 0;
  if (index)
    *index = -1;

  if (tok.kind == T_LPAREN) {
    call_function(find_func(name), out);
    return;
  }
  struct var *v = find_var(name, 1);
  if (v == 0) {
    bc_error("too many variables");
    num_zero(out);
    return;
  }
  if (tok.kind == T_LBRACK) {
    next_token();
    struct num idx;
    parse_expr(&idx);
    expect(T_RBRACK, "expected ]");
    long long i = num_to_long(&idx);
    if (i < 0 || i >= MAXARRAY) {
      bc_error("array index out of range");
      num_zero(out);
      return;
    }
    if (v->array == 0) {
      v->array = malloc(MAXARRAY * sizeof(struct num));
      if (v->array == 0) {
        bc_error("out of memory");
        num_zero(out);
        return;
      }
      for (int k = 0; k < MAXARRAY; k++)
        num_zero(&v->array[k]);
      v->arraylen = MAXARRAY;
    }
    if (lvalue) {
      *lvalue = v;
      *index = (int)i;
    }
    *out = v->array[i];
    return;
  }
  if (lvalue)
    *lvalue = v;
  *out = v->value;
}

static void
store(struct var *v, int index, const struct num *value)
{
  if (v == 0)
    return;
  if (index >= 0 && v->array)
    v->array[index] = *value;
  else
    v->value = *value;
}

static void
parse_primary(struct num *out)
{
  switch (tok.kind) {
  case T_NUM:
    num_parse(out, tok.numtext);
    next_token();
    return;
  case T_MINUS:
    next_token();
    parse_primary(out);
    if (!num_is_zero(out))
      out->neg = !out->neg;
    return;
  case T_NOT: {
    next_token();
    struct num v;
    parse_primary(&v);
    num_from_long(out, num_is_zero(&v) ? 1 : 0);
    return;
  }
  case T_LPAREN:
    next_token();
    parse_expr(out);
    expect(T_RPAREN, "expected )");
    return;
  case T_INC:
  case T_DEC: {
    int inc = tok.kind == T_INC;
    next_token();
    struct var *v = 0;
    int idx = -1;
    struct num cur;
    if (tok.kind == T_NAME) {
      parse_name(&cur, &v, &idx);
    } else if (tok.kind == T_SCALE || tok.kind == T_IBASE ||
               tok.kind == T_OBASE) {
      int which = tok.kind;
      next_token();
      num_from_long(&cur, which == T_SCALE ? scale_var
                          : which == T_IBASE ? ibase : obase);
      struct num one, r;
      num_from_long(&one, 1);
      if (inc)
        num_add(&cur, &one, &r);
      else
        num_sub(&cur, &one, &r);
      long long nv = num_to_long(&r);
      if (which == T_SCALE) scale_var = (int)nv;
      else if (which == T_IBASE) ibase = (int)nv;
      else obase = (int)nv;
      *out = r;
      return;
    } else {
      bc_error("++ wants a variable");
      num_zero(out);
      return;
    }
    struct num one, r;
    num_from_long(&one, 1);
    if (inc)
      num_add(&cur, &one, &r);
    else
      num_sub(&cur, &one, &r);
    store(v, idx, &r);
    *out = r;
    return;
  }
  case T_LENGTH: {
    next_token();
    expect(T_LPAREN, "expected (");
    struct num v;
    parse_expr(&v);
    expect(T_RPAREN, "expected )");
    // the number of significant digits, which is what bc counts
    int whole = v.len - v.scale;
    int n = v.len;
    if (whole == 1 && v.d[0] == 0 && v.scale > 0)
      n = v.scale;   // 0.nnn counts only the fraction
    num_from_long(out, n);
    return;
  }
  case T_SQRT: {
    next_token();
    expect(T_LPAREN, "expected (");
    struct num v;
    parse_expr(&v);
    expect(T_RPAREN, "expected )");
    if (num_sqrt(&v, scale_var, out) < 0) {
      bc_error("square root of a negative number");
      num_zero(out);
    }
    return;
  }
  case T_READ: {
    next_token();
    if (tok.kind == T_LPAREN) {
      next_token();
      expect(T_RPAREN, "expected )");
    }
    char line[256];
    if (fgets(line, sizeof(line), stdin) == 0) {
      num_zero(out);
      return;
    }
    num_parse(out, line);
    return;
  }
  case T_SCALE:
    next_token();
    if (tok.kind == T_LPAREN) {
      // scale(expr): how many places the value has
      next_token();
      struct num v;
      parse_expr(&v);
      expect(T_RPAREN, "expected )");
      num_from_long(out, v.scale);
      return;
    }
    num_from_long(out, scale_var);
    return;
  case T_IBASE:
    next_token();
    num_from_long(out, ibase);
    return;
  case T_OBASE:
    next_token();
    num_from_long(out, obase);
    return;
  case T_LAST:
    next_token();
    {
      struct var *v = find_var("last", 1);
      if (v)
        *out = v->value;
      else
        num_zero(out);
    }
    return;
  case T_NAME: {
    struct var *v = 0;
    int idx = -1;
    struct num cur;
    parse_name(&cur, &v, &idx);
    // a name followed by ++ or -- takes its old value
    if (tok.kind == T_INC || tok.kind == T_DEC) {
      int inc = tok.kind == T_INC;
      next_token();
      struct num one, r;
      num_from_long(&one, 1);
      if (inc)
        num_add(&cur, &one, &r);
      else
        num_sub(&cur, &one, &r);
      store(v, idx, &r);
      *out = cur;
      return;
    }
    *out = cur;
    return;
  }
  default:
    bc_error("a value is expected");
    num_zero(out);
    next_token();
    return;
  }
}

static void
parse_power(struct num *out)
{
  parse_primary(out);
  // ^ groups to the right, so 2^3^2 is 2^(3^2) and not (2^3)^2
  if (tok.kind == T_CARET) {
    next_token();
    struct num e;
    parse_power(&e);
    struct num r;
    num_pow(out, num_to_long(&e), scale_var, &r);
    *out = r;
  }
}

static void
parse_mul(struct num *out)
{
  parse_power(out);
  for (;;) {
    int op = tok.kind;
    if (op != T_STAR && op != T_SLASH && op != T_PERCENT)
      return;
    next_token();
    struct num b, r;
    parse_power(&b);
    if (op == T_STAR) {
      int sa = out->scale, sb = b.scale;
      num_mul(out, &b, &r);
      // a product keeps min(a+b, max(scale, a, b)) places
      int keep = sa + sb;
      int least = scale_var > sa ? scale_var : sa;
      if (least < sb)
        least = sb;
      if (keep > least)
        keep = least;
      struct num one, t;
      num_from_long(&one, 1);
      num_div(&r, &one, keep, &t);
      *out = t;
    } else if (op == T_SLASH) {
      if (num_div(out, &b, scale_var, &r) < 0) {
        bc_error("divide by zero");
        num_zero(&r);
      }
      *out = r;
    } else {
      if (num_mod(out, &b, scale_var, &r) < 0) {
        bc_error("divide by zero");
        num_zero(&r);
      }
      *out = r;
    }
  }
}

static void
parse_add(struct num *out)
{
  parse_mul(out);
  for (;;) {
    int op = tok.kind;
    if (op != T_PLUS && op != T_MINUS)
      return;
    next_token();
    struct num b, r;
    parse_mul(&b);
    if (op == T_PLUS)
      num_add(out, &b, &r);
    else
      num_sub(out, &b, &r);
    *out = r;
  }
}

static void
parse_relational(struct num *out)
{
  parse_add(out);
  int op = tok.kind;
  if (op != T_EQ && op != T_NE && op != T_LT && op != T_LE && op != T_GT &&
      op != T_GE)
    return;
  next_token();
  struct num b;
  parse_add(&b);
  int c = num_cmp(out, &b);
  int r;
  switch (op) {
  case T_EQ: r = c == 0; break;
  case T_NE: r = c != 0; break;
  case T_LT: r = c < 0; break;
  case T_LE: r = c <= 0; break;
  case T_GT: r = c > 0; break;
  default: r = c >= 0; break;
  }
  num_from_long(out, r);
}

static void
parse_and(struct num *out)
{
  parse_relational(out);
  while (tok.kind == T_AND) {
    next_token();
    struct num b;
    parse_relational(&b);
    num_from_long(out, !num_is_zero(out) && !num_is_zero(&b));
  }
}

static void
parse_or_expr(struct num *out)
{
  parse_and(out);
  while (tok.kind == T_OR) {
    next_token();
    struct num b;
    parse_and(&b);
    num_from_long(out, !num_is_zero(out) || !num_is_zero(&b));
  }
}

// an assignment, or anything that is not one
static void
parse_expr(struct num *out)
{
  // a name followed by an assignment operator is a store; anything
  // else is read, which needs one token of lookahead over the name
  if (tok.kind == T_NAME) {
    const char *save_src = src;
    struct token save_tok = tok;

    struct var *v = 0;
    int idx = -1;
    struct num cur;
    parse_name(&cur, &v, &idx);
    int op = tok.kind;
    if (op == T_ASSIGN || op == T_PLUSEQ || op == T_MINUSEQ ||
        op == T_STAREQ || op == T_SLASHEQ || op == T_PERCENTEQ ||
        op == T_CARETEQ) {
      next_token();
      struct num rhs, r;
      parse_expr(&rhs);
      switch (op) {
      case T_ASSIGN: r = rhs; break;
      case T_PLUSEQ: num_add(&cur, &rhs, &r); break;
      case T_MINUSEQ: num_sub(&cur, &rhs, &r); break;
      case T_STAREQ: num_mul(&cur, &rhs, &r); break;
      case T_SLASHEQ:
        if (num_div(&cur, &rhs, scale_var, &r) < 0) {
          bc_error("divide by zero");
          num_zero(&r);
        }
        break;
      case T_PERCENTEQ:
        if (num_mod(&cur, &rhs, scale_var, &r) < 0) {
          bc_error("divide by zero");
          num_zero(&r);
        }
        break;
      default: num_pow(&cur, num_to_long(&rhs), scale_var, &r); break;
      }
      store(v, idx, &r);
      *out = r;
      return;
    }
    // not an assignment after all: read it again as an expression
    src = save_src;
    tok = save_tok;
  }
  if (tok.kind == T_SCALE || tok.kind == T_IBASE || tok.kind == T_OBASE) {
    int which = tok.kind;
    const char *save_src = src;
    struct token save_tok = tok;
    next_token();
    if (tok.kind == T_ASSIGN) {
      next_token();
      struct num rhs;
      parse_expr(&rhs);
      long long v = num_to_long(&rhs);
      if (which == T_SCALE)
        scale_var = v < 0 ? 0 : (int)v;
      else if (which == T_IBASE)
        ibase = (v < 2 ? 2 : (v > 16 ? 16 : (int)v));
      else
        obase = (v < 2 ? 2 : (v > 999 ? 999 : (int)v));
      *out = rhs;
      return;
    }
    src = save_src;
    tok = save_tok;
  }
  parse_or_expr(out);
}

// the text from the current token up to the matching close, with the
// reader left on that close. taking a piece of a loop as text and
// running it again is what lets a condition and a body be evaluated
// as many times as the loop needs, without any of the re-parsing the
// token stream would otherwise have to do.
static char *
take_balanced(int open, int close)
{
  const char *start = tok_start;
  const char *p = start;
  int level = 0;

  while (*p) {
    if (*p == open) {
      level++;
    } else if (*p == close) {
      if (level == 0)
        break;
      level--;
    }
    p++;
  }
  int n = p - start;
  char *text = malloc(n + 1);
  if (text == 0) {
    bc_error("out of memory");
    return 0;
  }
  memmove(text, start, n);
  text[n] = 0;
  src = p;
  next_token();     // the close becomes the current token
  return text;
}

// one statement as text: a braced block, or everything up to the end
// of the line or a semicolon
static char *
take_statement_text(void)
{
  const char *start = tok_start;
  const char *p = start;

  if (tok.kind == T_LBRACE) {
    int level = 0;
    while (*p) {
      if (*p == '{')
        level++;
      else if (*p == '}') {
        level--;
        if (level == 0) {
          p++;
          break;
        }
      }
      p++;
    }
  } else {
    while (*p && *p != '\n' && *p != ';')
      p++;
  }
  int n = p - start;
  char *text = malloc(n + 1);
  if (text == 0) {
    bc_error("out of memory");
    return 0;
  }
  memmove(text, start, n);
  text[n] = 0;
  src = p;
  next_token();
  return text;
}

// run a piece of text as an expression, or as statements, and put the
// reader back where it was
static void
eval_text(const char *text, struct num *out)
{
  const char *save_src = src, *save_start = tok_start;
  struct token save_tok = tok;

  num_zero(out);
  if (text) {
    src = text;
    next_token();
    if (tok.kind != T_EOF)
      parse_expr(out);
  }
  src = save_src;
  tok_start = save_start;
  tok = save_tok;
}

static void
exec_text(const char *text)
{
  const char *save_src = src, *save_start = tok_start;
  struct token save_tok = tok;

  if (text) {
    src = text;
    next_token();
    while (tok.kind != T_EOF && flow == FLOW_NONE)
      parse_statement();
  }
  src = save_src;
  tok_start = save_start;
  tok = save_tok;
}

static void
skip_newlines(void)
{
  while (tok.kind == T_NEWLINE || tok.kind == T_SEMI)
    next_token();
}

// a brace-enclosed body as text, so a function can be run again.
// `src` is just past the {.
static char *
capture_block(void)
{
  const char *start = src;
  int level = 1;

  while (*src && level > 0) {
    if (*src == '{')
      level++;
    else if (*src == '}')
      level--;
    if (level > 0)
      src++;
  }
  int n = src - start;
  char *body = malloc(n + 1);
  if (body == 0)
    return 0;
  memmove(body, start, n);
  body[n] = 0;
  if (*src == '}')
    src++;
  next_token();
  return body;
}

static void
parse_define(void)
{
  next_token();
  if (tok.kind != T_NAME) {
    bc_error("define wants a name");
    return;
  }
  struct func *f = find_func(tok.text);
  if (f == 0) {
    if (nfuncs >= MAXFUNCS) {
      bc_error("too many functions");
      return;
    }
    f = &funcs[nfuncs++];
    snprintf(f->name, sizeof(f->name), "%s", tok.text);
    f->body = 0;
  }
  f->nparams = 0;
  f->nautos = 0;
  next_token();
  expect(T_LPAREN, "expected (");
  while (tok.kind == T_NAME) {
    if (f->nparams < MAXPARAMS)
      snprintf(f->params[f->nparams++], MAXNAME + 1, "%s", tok.text);
    next_token();
    if (tok.kind == T_COMMA)
      next_token();
  }
  expect(T_RPAREN, "expected )");
  skip_newlines();
  if (tok.kind != T_LBRACE) {
    bc_error("expected {");
    return;
  }

  // from here the body is taken as text rather than as tokens: it has
  // to be kept whole so the function can be run again, and a token
  // already read is a character gone from `src`. `src` is just past
  // the {, which is where the scan starts.
  const char *p = src;
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')
      p++;
    if (strncmp(p, "auto", 4) != 0 ||
        ((p[4] >= 'a' && p[4] <= 'z') || (p[4] >= '0' && p[4] <= '9') ||
         p[4] == '_'))
      break;
    p += 4;
    // the names up to the end of the line or a semicolon
    for (;;) {
      while (*p == ' ' || *p == '\t' || *p == ',')
        p++;
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_'))
        break;
      char name[MAXNAME + 1];
      int k = 0;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') || *p == '_') {
        if (k < MAXNAME)
          name[k++] = *p;
        p++;
      }
      name[k] = 0;
      if (f->nautos < MAXAUTOS)
        snprintf(f->autos[f->nautos++], MAXNAME + 1, "%s", name);
    }
  }

  src = p;
  free(f->body);
  f->body = capture_block();
  f->defined = 1;
}

static void
parse_statement(void)
{
  switch (tok.kind) {
  case T_NEWLINE:
  case T_SEMI:
    next_token();
    return;
  case T_EOF:
    return;
  case T_QUIT:
    flow = FLOW_QUIT;
    next_token();
    return;
  case T_BREAK:
    flow = FLOW_BREAK;
    next_token();
    return;
  case T_CONTINUE:
    flow = FLOW_CONTINUE;
    next_token();
    return;
  case T_RETURN: {
    // the value is gathered in a local: retval is one global, and a
    // call inside the expression would otherwise overwrite the very
    // operand the expression is still working on
    struct num v;
    num_zero(&v);
    next_token();
    if (tok.kind == T_LPAREN) {
      next_token();
      if (tok.kind != T_RPAREN)
        parse_expr(&v);
      expect(T_RPAREN, "expected )");
    } else if (tok.kind != T_NEWLINE && tok.kind != T_SEMI &&
               tok.kind != T_RBRACE && tok.kind != T_EOF) {
      parse_expr(&v);
    }
    retval = v;
    flow = FLOW_RETURN;
    return;
  }
  case T_DEFINE:
    parse_define();
    return;
  case T_STRING:
    printf("%s", tok.numtext);
    next_token();
    return;
  case T_PRINT: {
    next_token();
    for (;;) {
      if (tok.kind == T_STRING) {
        // the escapes print rather than stand for themselves
        for (const char *p = tok.numtext; *p; p++) {
          if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case '\\': putchar('\\'); break;
            case 'q': putchar('"'); break;
            default: putchar(*p);
            }
          } else {
            putchar(*p);
          }
        }
        next_token();
      } else {
        struct num v;
        parse_expr(&v);
        num_print(&v);
      }
      if (tok.kind != T_COMMA)
        break;
      next_token();
    }
    return;
  }
  case T_LBRACE: {
    next_token();
    while (tok.kind != T_RBRACE && tok.kind != T_EOF && flow == FLOW_NONE)
      parse_statement();
    if (tok.kind == T_RBRACE)
      next_token();
    return;
  }
  case T_IF: {
    next_token();
    expect(T_LPAREN, "expected (");
    char *cond = take_balanced('(', ')');
    expect(T_RPAREN, "expected )");
    skip_newlines();
    char *then_part = take_statement_text();
    char *else_part = 0;
    // `else` may be on the next line, so look past the newlines --
    // and put them back when there is no else after all
    const char *save_src = src, *save_start = tok_start;
    struct token save_tok = tok;
    skip_newlines();
    if (tok.kind == T_ELSE) {
      next_token();
      skip_newlines();
      else_part = take_statement_text();
    } else {
      src = save_src;
      tok_start = save_start;
      tok = save_tok;
    }

    struct num v;
    eval_text(cond, &v);
    if (!num_is_zero(&v))
      exec_text(then_part);
    else if (else_part)
      exec_text(else_part);
    free(cond);
    free(then_part);
    free(else_part);
    return;
  }
  case T_WHILE: {
    next_token();
    expect(T_LPAREN, "expected (");
    char *cond = take_balanced('(', ')');
    expect(T_RPAREN, "expected )");
    skip_newlines();
    char *body = take_statement_text();

    for (int guard = 0; guard < 100000000; guard++) {
      struct num v;
      eval_text(cond, &v);
      if (num_is_zero(&v))
        break;
      exec_text(body);
      if (flow == FLOW_BREAK) {
        flow = FLOW_NONE;
        break;
      }
      if (flow == FLOW_CONTINUE)
        flow = FLOW_NONE;
      if (flow != FLOW_NONE)
        break;
    }
    free(cond);
    free(body);
    return;
  }
  case T_FOR: {
    next_token();
    expect(T_LPAREN, "expected (");
    // the three pieces, each up to its own separator
    char *init = 0, *cond = 0, *step = 0;
    {
      const char *start = tok_start;
      const char *p = start;
      int level = 0;
      while (*p && !(level == 0 && *p == ';')) {
        if (*p == '(') level++;
        else if (*p == ')') level--;
        p++;
      }
      int n = p - start;
      init = malloc(n + 1);
      if (init) {
        memmove(init, start, n);
        init[n] = 0;
      }
      src = p;
      next_token();          // the ;
    }
    expect(T_SEMI, "expected ;");
    {
      const char *start = tok_start;
      const char *p = start;
      int level = 0;
      while (*p && !(level == 0 && *p == ';')) {
        if (*p == '(') level++;
        else if (*p == ')') level--;
        p++;
      }
      int n = p - start;
      cond = malloc(n + 1);
      if (cond) {
        memmove(cond, start, n);
        cond[n] = 0;
      }
      src = p;
      next_token();
    }
    expect(T_SEMI, "expected ;");
    step = take_balanced('(', ')');
    expect(T_RPAREN, "expected )");
    skip_newlines();
    char *body = take_statement_text();

    struct num v;
    eval_text(init, &v);
    for (int guard = 0; guard < 100000000; guard++) {
      if (cond && cond[0]) {
        eval_text(cond, &v);
        if (num_is_zero(&v))
          break;
      }
      exec_text(body);
      if (flow == FLOW_BREAK) {
        flow = FLOW_NONE;
        break;
      }
      if (flow == FLOW_CONTINUE)
        flow = FLOW_NONE;
      if (flow != FLOW_NONE)
        break;
      eval_text(step, &v);
    }
    free(init);
    free(cond);
    free(step);
    free(body);
    return;
  }
  default: {
    // an expression on its own prints its value, unless it was an
    // assignment -- which is what bc does and why `x = 1` is silent,
    // inside a loop as much as at the top level
    int quiet = is_assignment();
    struct num v;
    parse_expr(&v);
    struct var *last = find_var("last", 1);
    if (last)
      last->value = v;
    if (!quiet)
      num_print(&v);
    return;
  }
  }
}

// an assignment statement prints nothing; this decides which it was by
// looking at the text before running it
static int
is_assignment(void)
{
  const char *p = src;
  struct token t = tok;

  if (t.kind != T_NAME && t.kind != T_SCALE && t.kind != T_IBASE &&
      t.kind != T_OBASE)
    return 0;
  // step over the name and any [index]
  int level = 0;
  while (*p) {
    if (*p == '[')
      level++;
    else if (*p == ']')
      level--;
    else if (level == 0 && *p != ' ' && *p != '\t')
      break;
    p++;
  }
  if (*p == '=' && p[1] != '=')
    return 1;
  if ((*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '%' ||
       *p == '^') && p[1] == '=')
    return 1;
  return 0;
}

static void
run(const char *text)
{
  src = text;
  next_token();
  while (tok.kind != T_EOF && flow != FLOW_QUIT) {
    if (tok.kind == T_NEWLINE || tok.kind == T_SEMI) {
      next_token();
      continue;
    }
    parse_statement();
    if (flow == FLOW_RETURN)
      flow = FLOW_NONE;
  }
}

// the maths library -l loads, written in bc itself
static const char *mathlib =
    "scale = 20\n"
    "define e(x) {\n"
    "  auto a, b, i, s, t\n"
    "  if (x < 0) { return (1 / e(-x)) }\n"
    "  a = 1; b = 1; s = 1; i = 1\n"
    "  while (1) {\n"
    "    a = a * x; b = b * i; t = a / b\n"
    "    if (t == 0) { break }\n"
    "    s = s + t; i = i + 1\n"
    "    if (i > 200) { break }\n"
    "  }\n"
    "  return (s)\n"
    "}\n"
    "define l(x) {\n"
    "  auto y, z, t, i, s\n"
    "  if (x <= 0) { return (0) }\n"
    "  y = (x - 1) / (x + 1); z = y * y; t = y; s = y; i = 3\n"
    "  while (1) {\n"
    "    t = t * z; s = s + t / i; i = i + 2\n"
    "    if (t / i == 0) { break }\n"
    "    if (i > 400) { break }\n"
    "  }\n"
    "  return (2 * s)\n"
    "}\n"
    "define s(x) {\n"
    "  auto t, s, i, sign, p\n"
    "  s = 0; t = x; i = 1; sign = 1; p = x\n"
    "  while (1) {\n"
    "    s = s + sign * p\n"
    "    p = p * x * x / ((i + 1) * (i + 2)); i = i + 2; sign = -sign\n"
    "    if (p == 0) { break }\n"
    "    if (i > 60) { break }\n"
    "  }\n"
    "  return (s)\n"
    "}\n"
    "define c(x) { return (s(x + 2 * a(1))) }\n"
    "define a(x) {\n"
    "  auto s, t, i, sign, z\n"
    "  if (x > 1) { return (2 * a(1) / 2 - a(1 / x)) }\n"
    "  if (x < -1) { return (-2 * a(1) / 2 - a(1 / x)) }\n"
    "  z = x * x; t = x; s = x; i = 3; sign = -1\n"
    "  while (1) {\n"
    "    t = t * z; s = s + sign * t / i; i = i + 2; sign = -sign\n"
    "    if (t / i == 0) { break }\n"
    "    if (i > 400) { break }\n"
    "  }\n"
    "  return (s)\n"
    "}\n"
    "define j(n, x) { return (0) }\n";

int
main(int argc, char *argv[])
{
  int use_mathlib = 0, quiet = 0, i = 1;
  const char *files[32];
  int nfiles = 0;

  for (; i < argc; i++) {
    if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--mathlib") == 0)
      use_mathlib = 1;
    else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0)
      quiet = 1;
    else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-w") == 0)
      ;
    else if (argv[i][0] == '-' && argv[i][1])
      fprintf(stderr, "bc: ignoring %s\n", argv[i]);
    else if (nfiles < 32)
      files[nfiles++] = argv[i];
  }

  struct var *last = find_var("last", 1);
  if (last)
    num_zero(&last->value);

  if (use_mathlib)
    run(mathlib);
  // the banner belongs to an interactive session, not to a script
  if (!quiet && nfiles == 0 && isatty(0))
    printf("bc: an arbitrary precision calculator\n");

  for (int k = 0; k < nfiles && flow != FLOW_QUIT; k++) {
    FILE *f = fopen(files[k], "r");
    if (f == 0) {
      fprintf(stderr, "bc: %s: %s\n", files[k], strerror(errno));
      exit(1);
    }
    static char buf[1 << 16];
    int n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n > 0 ? n : 0] = 0;
    fclose(f);
    run(buf);
  }

  if (nfiles == 0) {
    if (!isatty(0)) {
      // a script arrives whole: a define or an if runs over several
      // lines, and reading one at a time would cut it in half
      static char all[1 << 16];
      int n = fread(all, 1, sizeof(all) - 1, stdin);
      all[n > 0 ? n : 0] = 0;
      run(all);
    } else {
      // typed in: a line at a time, but a line that leaves a brace or
      // a bracket open waits for the rest
      static char buf[1 << 16];
      static char line[4096];
      int held = 0;
      buf[0] = 0;
      while (flow != FLOW_QUIT && fgets(line, sizeof(line), stdin)) {
        int n = strlen(line);
        if (held + n < (int)sizeof(buf) - 1) {
          memmove(buf + held, line, n + 1);
          held += n;
        }
        int depth_braces = 0, depth_parens = 0;
        for (int i = 0; i < held; i++) {
          if (buf[i] == '{') depth_braces++;
          else if (buf[i] == '}') depth_braces--;
          else if (buf[i] == '(') depth_parens++;
          else if (buf[i] == ')') depth_parens--;
        }
        if (depth_braces > 0 || depth_parens > 0)
          continue;
        run(buf);
        fflush(stdout);
        held = 0;
        buf[0] = 0;
      }
    }
  }
  exit(had_error ? 1 : 0);
}
