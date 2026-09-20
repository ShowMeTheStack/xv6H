//
// the shell's lexer and parser.
//
// the two are together because the shell's grammar cannot be lexed
// without knowing where the parser is: whether a word is a reserved
// word depends on its position, and a here-document's text is read
// from the lines that follow the line its << appeared on. the lexer
// therefore hands out one token at a time and the parser tells it what
// it is willing to see.
//
// quoting is resolved here, not during expansion: every character of a
// word is marked as quoted or not, so that expansion can tell "$x"
// from $x without looking at the original text again.
//
#include "sh.h"

enum token {
  T_EOF,
  T_WORD,
  T_ASSIGN,      // NAME=value in the command's first position
  T_NEWLINE,
  T_SEMI,        // ;
  T_DSEMI,       // ;;
  T_AMP,         // &
  T_AND,         // &&
  T_PIPE,        // |
  T_OR,          // ||
  T_LPAREN,
  T_RPAREN,
  T_LBRACE,      // {
  T_RBRACE,      // }
  T_LT,          // <
  T_GT,          // >
  T_DGT,         // >>
  T_LTGT,        // <>
  T_GTPIPE,      // >|
  T_DLT,         // <<
  T_DLTDASH,     // <<-
  T_TLT,         // <<<
  T_LTAMP,       // <&
  T_GTAMP,       // >&
  T_IF, T_THEN, T_ELIF, T_ELSE, T_FI,
  T_WHILE, T_UNTIL, T_DO, T_DONE,
  T_FOR, T_IN,
  T_CASE, T_ESAC,
  T_BANG,
};

struct parser {
  const char *text;
  int pos;
  int len;
  int err;
  int interactive_input;

  enum token tok;
  struct word *tokword;   // the word T_WORD or T_ASSIGN carries
  int tok_fd;             // the descriptor a redirection named, or -1
  int pushed;             // a token the parser put back

  struct redir *heredocs[MAXHEREDOC]; // waiting for their text
  int nheredoc;
};

static const struct {
  const char *name;
  enum token tok;
} reserved[] = {
    {"if", T_IF},     {"then", T_THEN}, {"elif", T_ELIF},   {"else", T_ELSE},
    {"fi", T_FI},     {"while", T_WHILE}, {"until", T_UNTIL}, {"do", T_DO},
    {"done", T_DONE}, {"for", T_FOR},   {"in", T_IN},       {"case", T_CASE},
    {"esac", T_ESAC}, {"{", T_LBRACE},  {"}", T_RBRACE},    {"!", T_BANG},
    {0, T_EOF},
};

struct parser *
parser_new(const char *text, int interactive_input)
{
  struct parser *p = xmalloc(sizeof(*p));

  memset(p, 0, sizeof(*p));
  p->text = text;
  p->len = strlen(text);
  p->tok_fd = -1;
  p->interactive_input = interactive_input;
  return p;
}

void
parser_free(struct parser *p)
{
  free(p);
}

int
parser_error(struct parser *p)
{
  return p->err;
}

// ---- words ----

struct word *
word_new(const char *text, const char *quoted, int len)
{
  struct word *w = xmalloc(sizeof(*w));

  w->text = xmalloc(len + 1);
  w->quoted = xmalloc(len + 1);
  memmove(w->text, text, len);
  w->text[len] = 0;
  if (quoted)
    memmove(w->quoted, quoted, len);
  else
    memset(w->quoted, 0, len);
  w->quoted[len] = 0;
  w->len = len;
  w->next = 0;
  return w;
}

void
word_free(struct word *w)
{
  while (w) {
    struct word *next = w->next;
    free(w->text);
    free(w->quoted);
    free(w);
    w = next;
  }
}

char *
word_text(struct word *w)
{
  return w ? w->text : 0;
}

// ---- characters ----

static int
peek(struct parser *p, int ahead)
{
  return p->pos + ahead < p->len ? (unsigned char)p->text[p->pos + ahead] : 0;
}

static int
isblankc(int c)
{
  return c == ' ' || c == '\t';
}

static int
isname_start(int c)
{
  return isalpha(c) || c == '_';
}

static int
isname_char(int c)
{
  return isalnum(c) || c == '_';
}

// the characters that end an unquoted word
static int
isopchar(int c)
{
  return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

// skip blanks, comments and escaped newlines
static void
skip_blanks(struct parser *p)
{
  for (;;) {
    while (p->pos < p->len && isblankc(p->text[p->pos]))
      p->pos++;
    if (peek(p, 0) == '\\' && peek(p, 1) == '\n') {
      p->pos += 2;
      continue;
    }
    if (peek(p, 0) == '#') {
      while (p->pos < p->len && p->text[p->pos] != '\n')
        p->pos++;
      continue;
    }
    break;
  }
}

// ---- reading one word ----
//
// the word is built into two buffers at once: the characters, and a
// mark beside each saying whether it was quoted. a $(...) or `...` or
// $((...)) is copied through with its own quoting kept, because it is
// expanded later by running it as its own script.

struct wbuf {
  char *text;
  char *quoted;
  int len, cap;
};

static void
wbuf_init(struct wbuf *b)
{
  b->cap = 64;
  b->text = xmalloc(b->cap);
  b->quoted = xmalloc(b->cap);
  b->len = 0;
}

static void
wbuf_add(struct wbuf *b, int c, int q)
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
wbuf_free(struct wbuf *b)
{
  free(b->text);
  free(b->quoted);
}

// copy a $(...) or `...` or ${...} or $((...)) through, keeping track
// of nesting and of quotes inside it.
static void
copy_nested(struct parser *p, struct wbuf *b, int open, int close, int quoted)
{
  int depth = 1;

  wbuf_add(b, p->text[p->pos++], quoted); // the opening character
  while (p->pos < p->len && depth > 0) {
    int c = p->text[p->pos];
    if (c == '\\' && p->pos + 1 < p->len) {
      wbuf_add(b, c, quoted);
      wbuf_add(b, p->text[p->pos + 1], quoted);
      p->pos += 2;
      continue;
    }
    if (c == '\'' || c == '"') {
      // a quoted stretch inside: copy it through unchanged
      int q = c;
      wbuf_add(b, c, quoted);
      p->pos++;
      while (p->pos < p->len && p->text[p->pos] != q) {
        if (q == '"' && p->text[p->pos] == '\\' && p->pos + 1 < p->len) {
          wbuf_add(b, p->text[p->pos], quoted);
          p->pos++;
        }
        wbuf_add(b, p->text[p->pos], quoted);
        p->pos++;
      }
      if (p->pos < p->len) {
        wbuf_add(b, p->text[p->pos], quoted);
        p->pos++;
      }
      continue;
    }
    if (c == open)
      depth++;
    else if (c == close)
      depth--;
    wbuf_add(b, c, quoted);
    p->pos++;
    if (depth == 0)
      return;
  }
  if (depth > 0)
    p->err = 1;
}

// read one word, stopping at an operator or blank outside quotes.
// returns 0 if there was no word here.
static struct word *
read_word(struct parser *p, int *was_assign)
{
  struct wbuf b;
  int any = 0;

  wbuf_init(&b);
  *was_assign = 0;

  for (;;) {
    int c = peek(p, 0);
    if (c == 0 || c == '\n' || isblankc(c))
      break;
    if (isopchar(c))
      break;

    if (c == '\\') {
      if (peek(p, 1) == '\n') {
        p->pos += 2;
        continue;
      }
      if (p->pos + 1 < p->len) {
        wbuf_add(&b, p->text[p->pos + 1], Q_QUOTED | Q_LITERAL);
        p->pos += 2;
        any = 1;
        continue;
      }
      p->pos++;
      continue;
    }
    if (c == '\'') {
      p->pos++;
      any = 1;
      while (p->pos < p->len && p->text[p->pos] != '\'') {
        wbuf_add(&b, p->text[p->pos], Q_QUOTED | Q_LITERAL);
        p->pos++;
      }
      if (p->pos >= p->len) {
        p->err = 1;
        break;
      }
      p->pos++;
      continue;
    }
    if (c == '"') {
      p->pos++;
      any = 1;
      while (p->pos < p->len && p->text[p->pos] != '"') {
        int d = p->text[p->pos];
        if (d == '\\' && p->pos + 1 < p->len) {
          int e = p->text[p->pos + 1];
          // inside double quotes a backslash only escapes these
          if (e == '$' || e == '`' || e == '"' || e == '\\' || e == '\n') {
            if (e != '\n')
              wbuf_add(&b, e, Q_QUOTED | Q_LITERAL);
            p->pos += 2;
            continue;
          }
          wbuf_add(&b, d, Q_QUOTED);
          p->pos++;
          continue;
        }
        if (d == '$' && (p->text[p->pos + 1] == '(' || p->text[p->pos + 1] == '{')) {
          wbuf_add(&b, '$', Q_QUOTED);
          p->pos++;
          copy_nested(p, &b, p->text[p->pos], p->text[p->pos] == '(' ? ')' : '}', Q_QUOTED);
          continue;
        }
        if (d == '`') {
          copy_nested(p, &b, 0, '`', Q_QUOTED);
          continue;
        }
        wbuf_add(&b, d, Q_QUOTED);
        p->pos++;
      }
      if (p->pos >= p->len) {
        p->err = 1;
        break;
      }
      p->pos++;
      continue;
    }
    if (c == '$' && (peek(p, 1) == '(' || peek(p, 1) == '{')) {
      wbuf_add(&b, '$', 0);
      p->pos++;
      copy_nested(p, &b, p->text[p->pos], p->text[p->pos] == '(' ? ')' : '}', 0);
      any = 1;
      continue;
    }
    if (c == '`') {
      copy_nested(p, &b, 0, '`', 0);
      any = 1;
      continue;
    }
    wbuf_add(&b, c, 0);
    p->pos++;
    any = 1;
  }

  if (!any && b.len == 0) {
    wbuf_free(&b);
    return 0;
  }

  // is it NAME=value, with the name unquoted?
  if (b.len > 0 && isname_start((unsigned char)b.text[0]) && !b.quoted[0]) {
    for (int i = 1; i < b.len; i++) {
      if (b.text[i] == '=' && !b.quoted[i]) {
        *was_assign = 1;
        break;
      }
      if (!isname_char((unsigned char)b.text[i]))
        break;
    }
  }

  struct word *w = word_new(b.text, b.quoted, b.len);
  wbuf_free(&b);
  return w;
}

// ---- tokens ----

static enum token
reserved_word(struct word *w)
{
  if (w == 0)
    return T_WORD;
  for (int i = 0; w->len && reserved[i].name; i++)
    if (strcmp(w->text, reserved[i].name) == 0) {
      // a reserved word must be unquoted to count
      for (int k = 0; k < w->len; k++)
        if (w->quoted[k])
          return T_WORD;
      return reserved[i].tok;
    }
  return T_WORD;
}

// read the next token. `want_reserved` says whether a reserved word
// would be recognised here, which depends on the position in the
// grammar and is why the parser has to say.
static void
next_token(struct parser *p, int want_reserved)
{
  if (p->pushed) {
    p->pushed = 0;
    return;
  }
  if (p->tokword) {
    word_free(p->tokword);
    p->tokword = 0;
  }
  p->tok_fd = -1;
  skip_blanks(p);

  int c = peek(p, 0);
  if (c == 0) {
    p->tok = T_EOF;
    return;
  }
  if (c == '\n') {
    p->pos++;
    p->tok = T_NEWLINE;
    return;
  }

  // a redirection may begin with a digit naming the descriptor
  if (isdigit(c)) {
    int i = 0;
    while (isdigit(peek(p, i)))
      i++;
    if (peek(p, i) == '<' || peek(p, i) == '>') {
      int fd = 0;
      for (int k = 0; k < i; k++)
        fd = fd * 10 + (p->text[p->pos + k] - '0');
      p->pos += i;
      p->tok_fd = fd;
      c = peek(p, 0);
    }
  }

  switch (c) {
  case '|':
    p->pos++;
    if (peek(p, 0) == '|') {
      p->pos++;
      p->tok = T_OR;
    } else {
      p->tok = T_PIPE;
    }
    return;
  case '&':
    p->pos++;
    if (peek(p, 0) == '&') {
      p->pos++;
      p->tok = T_AND;
    } else {
      p->tok = T_AMP;
    }
    return;
  case ';':
    p->pos++;
    if (peek(p, 0) == ';') {
      p->pos++;
      p->tok = T_DSEMI;
    } else {
      p->tok = T_SEMI;
    }
    return;
  case '(':
    p->pos++;
    p->tok = T_LPAREN;
    return;
  case ')':
    p->pos++;
    p->tok = T_RPAREN;
    return;
  case '<':
    p->pos++;
    if (peek(p, 0) == '<') {
      p->pos++;
      if (peek(p, 0) == '-') {
        p->pos++;
        p->tok = T_DLTDASH;
      } else if (peek(p, 0) == '<') {
        p->pos++;
        p->tok = T_TLT;
      } else {
        p->tok = T_DLT;
      }
    } else if (peek(p, 0) == '&') {
      p->pos++;
      p->tok = T_LTAMP;
    } else if (peek(p, 0) == '>') {
      p->pos++;
      p->tok = T_LTGT;
    } else {
      p->tok = T_LT;
    }
    return;
  case '>':
    p->pos++;
    if (peek(p, 0) == '>') {
      p->pos++;
      p->tok = T_DGT;
    } else if (peek(p, 0) == '&') {
      p->pos++;
      p->tok = T_GTAMP;
    } else if (peek(p, 0) == '|') {
      p->pos++;
      p->tok = T_GTPIPE;
    } else {
      p->tok = T_GT;
    }
    return;
  default:
    break;
  }

  int assign = 0;
  struct word *w = read_word(p, &assign);
  if (w == 0) {
    p->tok = T_EOF;
    return;
  }
  p->tokword = w;
  if (want_reserved) {
    enum token t = reserved_word(w);
    if (t != T_WORD) {
      p->tok = t;
      return;
    }
  }
  p->tok = assign ? T_ASSIGN : T_WORD;
}

static void
push_back(struct parser *p)
{
  p->pushed = 1;
}

// ---- here-documents ----
//
// the text follows the line the << was on, so it is collected when the
// lexer reaches the end of that line.

static void
collect_heredocs(struct parser *p)
{
  for (int i = 0; i < p->nheredoc; i++) {
    struct redir *r = p->heredocs[i];
    const char *delim = r->word->text;
    int strip = (r->kind == R_HEREDOC && r->fd == -2); // <<- was marked this way
    int cap = 256, len = 0;
    char *text = xmalloc(cap);

    // the delimiter counts as quoted if any of it was
    r->heredoc_quoted = 0;
    for (int k = 0; k < r->word->len; k++)
      if (r->word->quoted[k])
        r->heredoc_quoted = 1;

    for (;;) {
      int start = p->pos;
      while (p->pos < p->len && p->text[p->pos] != '\n')
        p->pos++;
      int linelen = p->pos - start;
      const char *line = p->text + start;
      if (p->pos < p->len)
        p->pos++; // the newline

      if (strip) {
        while (linelen > 0 && *line == '\t') {
          line++;
          linelen--;
        }
      }
      if ((int)strlen(delim) == linelen && memcmp(line, delim, linelen) == 0)
        break;
      if (start >= p->len) {
        sh_error("here-document delimited by end of file (wanted `%s')", delim);
        break;
      }
      while (len + linelen + 2 > cap) {
        cap *= 2;
        char *t = xmalloc(cap);
        memmove(t, text, len);
        free(text);
        text = t;
      }
      memmove(text + len, line, linelen);
      len += linelen;
      text[len++] = '\n';
    }
    text[len] = 0;
    r->heredoc = text;
    if (r->fd == -2)
      r->fd = 0;
  }
  p->nheredoc = 0;
}

// ---- the parser ----

static struct cmd *parse_list(struct parser *p);
static struct cmd *parse_and_or(struct parser *p);
static struct cmd *parse_pipeline(struct parser *p);
static struct cmd *parse_command_inner(struct parser *p);

static struct cmd *
cmd_new(enum cmd_kind kind)
{
  struct cmd *c = xmalloc(sizeof(*c));

  memset(c, 0, sizeof(*c));
  c->kind = kind;
  return c;
}

void
cmd_free(struct cmd *c)
{
  if (c == 0)
    return;
  word_free(c->words);
  word_free(c->assigns);
  word_free(c->items);
  word_free(c->subject);
  for (struct redir *r = c->redirs; r;) {
    struct redir *next = r->next;
    word_free(r->word);
    free(r->heredoc);
    free(r);
    r = next;
  }
  for (struct case_item *it = c->items_case; it;) {
    struct case_item *next = it->next;
    word_free(it->patterns);
    cmd_free(it->body);
    free(it);
    it = next;
  }
  cmd_free(c->left);
  cmd_free(c->right);
  cmd_free(c->cond);
  cmd_free(c->then_part);
  cmd_free(c->else_part);
  cmd_free(c->body);
  free(c->var);
  free(c->fname);
  free(c);
}

static void
add_redir(struct cmd *c, struct redir *r)
{
  struct redir **pp = &c->redirs;

  while (*pp)
    pp = &(*pp)->next;
  *pp = r;
}

// a redirection, if the current token is one. returns 0 otherwise.
static int
parse_redir(struct parser *p, struct cmd *c)
{
  enum redir_kind kind;
  int fd = p->tok_fd;

  switch (p->tok) {
  case T_LT: kind = R_IN; if (fd < 0) fd = 0; break;
  case T_GT: kind = R_OUT; if (fd < 0) fd = 1; break;
  case T_DGT: kind = R_APPEND; if (fd < 0) fd = 1; break;
  case T_GTPIPE: kind = R_CLOBBER; if (fd < 0) fd = 1; break;
  case T_LTGT: kind = R_RDWR; if (fd < 0) fd = 0; break;
  case T_LTAMP: kind = R_DUPIN; if (fd < 0) fd = 0; break;
  case T_GTAMP: kind = R_DUPOUT; if (fd < 0) fd = 1; break;
  case T_DLT: kind = R_HEREDOC; if (fd < 0) fd = 0; break;
  case T_DLTDASH: kind = R_HEREDOC; fd = -2; break;
  case T_TLT: kind = R_HERESTR; if (fd < 0) fd = 0; break;
  default:
    return 0;
  }

  next_token(p, 0);
  if (p->tok != T_WORD && p->tok != T_ASSIGN) {
    sh_error("syntax error near unexpected token after redirection");
    p->err = 1;
    return 1;
  }
  struct redir *r = xmalloc(sizeof(*r));
  memset(r, 0, sizeof(*r));
  r->kind = kind;
  r->fd = fd;
  r->word = p->tokword;
  p->tokword = 0;
  add_redir(c, r);

  if (kind == R_HEREDOC && p->nheredoc < MAXHEREDOC)
    p->heredocs[p->nheredoc++] = r;
  return 1;
}

// a simple command: assignments, then words, with redirections
// anywhere among them.
static struct cmd *
parse_simple(struct parser *p)
{
  struct cmd *c = cmd_new(C_SIMPLE);
  struct word **wtail = &c->words;
  struct word **atail = &c->assigns;
  int seen_word = 0;

  for (;;) {
    if (parse_redir(p, c)) {
      if (p->err) {
        cmd_free(c);
        return 0;
      }
      next_token(p, 0);
      continue;
    }
    if (p->tok == T_ASSIGN && !seen_word) {
      *atail = p->tokword;
      atail = &p->tokword->next;
      p->tokword = 0;
      next_token(p, 0);
      continue;
    }
    if (p->tok == T_WORD || p->tok == T_ASSIGN) {
      *wtail = p->tokword;
      wtail = &p->tokword->next;
      p->tokword = 0;
      seen_word = 1;
      next_token(p, 0);
      continue;
    }
    break;
  }

  if (c->words == 0 && c->assigns == 0 && c->redirs == 0) {
    cmd_free(c);
    return 0;
  }
  return c;
}

// the words that can end the body of a compound command. a body stops
// at any of them and the caller checks that it found the one it wanted,
// because a body may hold compound commands of its own and the parser
// must not mistake their terminators for its own.
static int
is_terminator(enum token t)
{
  switch (t) {
  case T_FI:
  case T_THEN:
  case T_ELSE:
  case T_ELIF:
  case T_DO:
  case T_DONE:
  case T_ESAC:
  case T_DSEMI:
  case T_RBRACE:
  case T_RPAREN:
  case T_EOF:
    return 1;
  default:
    return 0;
  }
}

// the body of a compound command, up to one of the words that end it
static struct cmd *
parse_until(struct parser *p, enum token end1, enum token end2)
{
  (void)end1;
  (void)end2;
  struct cmd *list = 0, **tail = &list;

  for (;;) {
    while (p->tok == T_NEWLINE || p->tok == T_SEMI) {
      if (p->tok == T_NEWLINE)
        collect_heredocs(p);
      next_token(p, 1);
    }
    if (is_terminator(p->tok))
      break;

    struct cmd *c = parse_and_or(p);
    if (c == 0 || p->err) {
      cmd_free(list);
      return 0;
    }
    if (*tail == 0) {
      *tail = c;
    } else {
      struct cmd *l = cmd_new(C_LIST);
      l->left = *tail;
      l->right = c;
      *tail = l;
    }

    if (p->tok == T_AMP) {
      // the command before the & runs in the background
      struct cmd *b = *tail;
      while (b->kind == C_LIST && b->right)
        b = b->right;
      b->background = 1;
      next_token(p, 1);
    } else if (p->tok == T_SEMI || p->tok == T_NEWLINE) {
      if (p->tok == T_NEWLINE)
        collect_heredocs(p);
      next_token(p, 1);
    } else if (is_terminator(p->tok)) {
      break;
    } else {
      sh_error("syntax error in command list");
      p->err = 1;
      cmd_free(list);
      return 0;
    }
  }
  return list;
}

static int
expect(struct parser *p, enum token t, const char *what)
{
  while (p->tok == T_NEWLINE) {
    collect_heredocs(p);
    next_token(p, 1);
  }
  if (p->tok != t) {
    sh_error("syntax error: expected `%s'", what);
    p->err = 1;
    return 0;
  }
  return 1;
}

static struct cmd *
parse_if(struct parser *p)
{
  struct cmd *c = cmd_new(C_IF);

  next_token(p, 1);
  c->cond = parse_until(p, T_THEN, T_EOF);
  if (p->err || !expect(p, T_THEN, "then"))
    goto bad;
  next_token(p, 1);
  c->then_part = parse_until(p, T_ELSE, T_ELIF);
  if (p->err)
    goto bad;
  if (p->tok == T_FI) {
    next_token(p, 0);
    return c;
  }
  if (p->tok == T_ELIF) {
    c->else_part = parse_if(p); // an elif is an if inside the else
    return c;
  }
  if (p->tok == T_ELSE) {
    next_token(p, 1);
    c->else_part = parse_until(p, T_FI, T_EOF);
    if (p->err || !expect(p, T_FI, "fi"))
      goto bad;
    next_token(p, 0);
    return c;
  }
  if (!expect(p, T_FI, "fi"))
    goto bad;
  next_token(p, 0);
  return c;

bad:
  cmd_free(c);
  return 0;
}

static struct cmd *
parse_while(struct parser *p, int until)
{
  struct cmd *c = cmd_new(until ? C_UNTIL : C_WHILE);

  next_token(p, 1);
  c->cond = parse_until(p, T_DO, T_EOF);
  if (p->err || !expect(p, T_DO, "do"))
    goto bad;
  next_token(p, 1);
  c->body = parse_until(p, T_DONE, T_EOF);
  if (p->err || !expect(p, T_DONE, "done"))
    goto bad;
  next_token(p, 0);
  return c;

bad:
  cmd_free(c);
  return 0;
}

static struct cmd *
parse_for(struct parser *p)
{
  struct cmd *c = cmd_new(C_FOR);

  next_token(p, 0);
  if (p->tok != T_WORD) {
    sh_error("syntax error: for needs a variable name");
    p->err = 1;
    goto bad;
  }
  c->var = xstrdup(p->tokword->text);
  next_token(p, 1);

  while (p->tok == T_NEWLINE) {
    collect_heredocs(p);
    next_token(p, 1);
  }
  if (p->tok == T_IN) {
    c->has_items = 1;
    struct word **tail = &c->items;
    next_token(p, 0);
    while (p->tok == T_WORD || p->tok == T_ASSIGN) {
      *tail = p->tokword;
      tail = &p->tokword->next;
      p->tokword = 0;
      next_token(p, 0);
    }
    if (p->tok == T_SEMI || p->tok == T_NEWLINE) {
      if (p->tok == T_NEWLINE)
        collect_heredocs(p);
      next_token(p, 1);
    }
  }
  while (p->tok == T_NEWLINE || p->tok == T_SEMI) {
    if (p->tok == T_NEWLINE)
      collect_heredocs(p);
    next_token(p, 1);
  }
  if (!expect(p, T_DO, "do"))
    goto bad;
  next_token(p, 1);
  c->body = parse_until(p, T_DONE, T_EOF);
  if (p->err || !expect(p, T_DONE, "done"))
    goto bad;
  next_token(p, 0);
  return c;

bad:
  cmd_free(c);
  return 0;
}

static struct cmd *
parse_case(struct parser *p)
{
  struct cmd *c = cmd_new(C_CASE);
  struct case_item **tail = &c->items_case;

  next_token(p, 0);
  if (p->tok != T_WORD && p->tok != T_ASSIGN) {
    sh_error("syntax error: case needs a word");
    p->err = 1;
    goto bad;
  }
  c->subject = p->tokword;
  p->tokword = 0;
  next_token(p, 1);
  while (p->tok == T_NEWLINE) {
    collect_heredocs(p);
    next_token(p, 1);
  }
  if (!expect(p, T_IN, "in"))
    goto bad;
  next_token(p, 1);

  for (;;) {
    while (p->tok == T_NEWLINE) {
      collect_heredocs(p);
      next_token(p, 1);
    }
    if (p->tok == T_ESAC || p->tok == T_EOF)
      break;

    struct case_item *it = xmalloc(sizeof(*it));
    memset(it, 0, sizeof(*it));
    struct word **ptail = &it->patterns;

    if (p->tok == T_LPAREN)
      next_token(p, 0);
    for (;;) {
      if (p->tok != T_WORD && p->tok != T_ASSIGN && p->tok != T_ESAC) {
        sh_error("syntax error in case pattern");
        p->err = 1;
        free(it);
        goto bad;
      }
      *ptail = p->tokword;
      ptail = &p->tokword->next;
      p->tokword = 0;
      next_token(p, 0);
      if (p->tok == T_PIPE) {
        next_token(p, 0);
        continue;
      }
      break;
    }
    if (p->tok != T_RPAREN) {
      sh_error("syntax error: case pattern needs )");
      p->err = 1;
      word_free(it->patterns);
      free(it);
      goto bad;
    }
    next_token(p, 1);
    it->body = parse_until(p, T_DSEMI, T_ESAC);
    if (p->err) {
      word_free(it->patterns);
      cmd_free(it->body);
      free(it);
      goto bad;
    }
    *tail = it;
    tail = &it->next;
    if (p->tok == T_DSEMI)
      next_token(p, 1);
  }
  if (!expect(p, T_ESAC, "esac"))
    goto bad;
  next_token(p, 0);
  return c;

bad:
  cmd_free(c);
  return 0;
}

// a command: compound, function definition, or simple
static struct cmd *
parse_command_inner(struct parser *p)
{
  struct cmd *c = 0;

  switch (p->tok) {
  case T_IF:
    c = parse_if(p);
    break;
  case T_WHILE:
    c = parse_while(p, 0);
    break;
  case T_UNTIL:
    c = parse_while(p, 1);
    break;
  case T_FOR:
    c = parse_for(p);
    break;
  case T_CASE:
    c = parse_case(p);
    break;
  case T_LBRACE:
    c = cmd_new(C_GROUP);
    next_token(p, 1);
    c->body = parse_until(p, T_RBRACE, T_EOF);
    if (p->err || !expect(p, T_RBRACE, "}")) {
      cmd_free(c);
      return 0;
    }
    next_token(p, 0);
    break;
  case T_LPAREN:
    c = cmd_new(C_SUBSHELL);
    next_token(p, 1);
    c->body = parse_until(p, T_RPAREN, T_EOF);
    if (p->err || !expect(p, T_RPAREN, ")")) {
      cmd_free(c);
      return 0;
    }
    next_token(p, 0);
    break;
  case T_WORD:
  case T_ASSIGN: {
    // a function definition is NAME ( ) followed by a compound command
    struct word *name = p->tokword;
    int saved_pos = p->pos;
    enum token saved_tok = p->tok; // T_WORD or T_ASSIGN: the kind matters
    p->tokword = 0;
    next_token(p, 0);
    if (p->tok == T_LPAREN) {
      next_token(p, 0);
      if (p->tok == T_RPAREN) {
        next_token(p, 1);
        while (p->tok == T_NEWLINE) {
          collect_heredocs(p);
          next_token(p, 1);
        }
        struct cmd *body = parse_command_inner(p);
        if (body == 0) {
          word_free(name);
          return 0;
        }
        c = cmd_new(C_FUNCTION);
        c->fname = xstrdup(name->text);
        c->body = body;
        word_free(name);
        return c;
      }
      sh_error("syntax error: expected ) in function definition");
      p->err = 1;
      word_free(name);
      return 0;
    }
    // not a function: put the word back at the front of a simple
    // command, with the kind it had, because an assignment that was
    // read as a word would be run as a command instead.
    p->pos = saved_pos;
    if (p->tokword) {
      word_free(p->tokword);
      p->tokword = 0;
    }
    p->tok = saved_tok;
    p->tokword = name;
    c = parse_simple(p);
    break;
  }
  default:
    return 0;
  }

  // redirections may follow a compound command
  if (c && c->kind != C_SIMPLE) {
    while (parse_redir(p, c)) {
      if (p->err) {
        cmd_free(c);
        return 0;
      }
      next_token(p, 0);
    }
  }
  return c;
}

static struct cmd *
parse_pipeline(struct parser *p)
{
  int bang = 0;

  while (p->tok == T_BANG) {
    bang = !bang;
    next_token(p, 1);
  }

  struct cmd *c = parse_command_inner(p);
  if (c == 0)
    return 0;

  while (p->tok == T_PIPE) {
    next_token(p, 1);
    while (p->tok == T_NEWLINE) {
      collect_heredocs(p);
      next_token(p, 1);
    }
    struct cmd *right = parse_command_inner(p);
    if (right == 0) {
      sh_error("syntax error after |");
      p->err = 1;
      cmd_free(c);
      return 0;
    }
    struct cmd *pipe = cmd_new(C_PIPE);
    pipe->left = c;
    pipe->right = right;
    c = pipe;
  }

  if (bang) {
    struct cmd *n = cmd_new(C_NOT);
    n->body = c;
    c = n;
  }
  return c;
}

static struct cmd *
parse_and_or(struct parser *p)
{
  struct cmd *c = parse_pipeline(p);

  if (c == 0)
    return 0;
  for (;;) {
    enum cmd_kind kind;
    if (p->tok == T_AND)
      kind = C_AND;
    else if (p->tok == T_OR)
      kind = C_OR;
    else
      break;
    next_token(p, 1);
    while (p->tok == T_NEWLINE) {
      collect_heredocs(p);
      next_token(p, 1);
    }
    struct cmd *right = parse_pipeline(p);
    if (right == 0) {
      sh_error("syntax error after %s", kind == C_AND ? "&&" : "||");
      p->err = 1;
      cmd_free(c);
      return 0;
    }
    struct cmd *j = cmd_new(kind);
    j->left = c;
    j->right = right;
    c = j;
  }
  return c;
}

static struct cmd *
parse_list(struct parser *p)
{
  return parse_and_or(p);
}

// one complete command: everything up to a newline that is not inside
// a compound command. returns 0 at the end of the input, with *eof set.
struct cmd *
parse_command(struct parser *p, int *eof)
{
  *eof = 0;
  next_token(p, 1);
  while (p->tok == T_NEWLINE || p->tok == T_SEMI) {
    if (p->tok == T_NEWLINE)
      collect_heredocs(p);
    next_token(p, 1);
  }
  if (p->tok == T_EOF) {
    *eof = 1;
    return 0;
  }

  struct cmd *c = parse_list(p);
  if (c == 0 || p->err) {
    cmd_free(c);
    return 0;
  }
  if (p->tok == T_AMP) {
    c->background = 1;
    next_token(p, 1);
  } else if (p->tok == T_SEMI || p->tok == T_NEWLINE) {
    if (p->tok == T_NEWLINE)
      collect_heredocs(p);
  } else if (p->tok != T_EOF) {
    sh_error("syntax error: unexpected token");
    p->err = 1;
    cmd_free(c);
    return 0;
  }
  push_back(p);
  if (p->tok == T_NEWLINE || p->tok == T_SEMI || p->tok == T_AMP)
    p->pushed = 0;
  return c;
}
