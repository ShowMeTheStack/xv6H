//
// <editline.h>: reading a line from a terminal, with editing.
//
// the editor holds the line in a buffer that grows, an offset into it
// for the cursor, and the width of the terminal. a key either changes
// the buffer or moves the cursor, and afterwards one function draws
// the line again. a terminal is a stream and not a window, so every
// drawing decision is really about how few bytes will do: typing a
// character at the end of a line that still fits on its row sends that
// one character and nothing else, while anything else rewrites the row
// from the prompt and clears to its end.
//
// a line wider than the terminal is shown one of two ways. scrolling
// keeps it on a single row and draws the window that contains the
// cursor, which costs nothing to undo. wrapping spreads it over as
// many rows as it needs, and there the redraw has to count: a terminal
// will not say where its cursor is, so the editor remembers how many
// rows it drew last time and which of them the cursor was on, walks
// down to the last, and clears its way back up.
//
// while it reads, the terminal is raw: ICANON, ECHO and ISIG are off,
// so every key arrives here, including ctrl-C and the escape sequences
// that the arrow keys send. the old settings are put back when the
// read ends, and also from an atexit handler and from a handler for
// the signals that would otherwise end the program with the terminal
// still raw.
//
// when the input is not a terminal there is nothing to edit and
// nothing to draw: the line is read with fgets and handed back, which
// is how the shell runs a script through the same call.
//
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <signal.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <editline.h>

// the control keys the editor answers to, named so that the switch
// below reads as a list of bindings.
enum {
  K_CTRL_A = 1, K_CTRL_B = 2, K_CTRL_C = 3, K_CTRL_D = 4, K_CTRL_E = 5,
  K_CTRL_F = 6, K_CTRL_G = 7, K_CTRL_H = 8, K_TAB = 9, K_NEWLINE = 10,
  K_CTRL_K = 11, K_CTRL_L = 12, K_ENTER = 13, K_CTRL_N = 14, K_CTRL_P = 16,
  K_CTRL_R = 18, K_CTRL_T = 20, K_CTRL_U = 21, K_CTRL_W = 23, K_ESC = 27,
  K_BACKSPACE = 127
};

// the keys that arrive as an escape sequence rather than a byte, given
// numbers no byte can take. zero is not a key: it is how the code says
// "nothing is waiting", and -1 is the end of the input.
enum {
  K_EOF = -1,
  K_NONE = 256, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DELETE,
  K_WORDLEFT, K_WORDRIGHT, K_DELWORD, K_DELWORDBACK
};

#define EL_DEFAULT_COLS 80
#define EL_MIN_COLS 4
#define EL_QUERY_MAX 128

// one line being edited. it lives on the stack of the read that owns
// it, so two reads never share anything but the history.
struct el {
  int ifd;          // where the keys come from
  int ofd;          // where the drawing goes, or -1 to draw nothing
  const char *prompt;
  size_t plen;
  char *buf;        // the line, always nul-terminated
  size_t len;       // how much of it is in use
  size_t cap;
  size_t pos;       // the cursor, an offset into buf
  int cols;         // the terminal's width
  size_t oldpos;    // where the cursor was when the line was last drawn
  int oldrows;      // how many rows it then occupied
  int hpos;         // which history entry is showing, hcount for none
  char *hsaved;     // the line that was being typed before browsing began
  char *out;        // a refresh is gathered here and written in one call
  size_t olen;
  size_t ocap;
};

// the history, oldest first, and the completer, shared by every read.
static char **hist;
static int hcount;
static int hcap;
static int hlimit = 500;
static el_completer completer;
static int multiline;

// ---- the buffers ----

// make room for a line of n characters, plus its terminator.
static int
reserve(struct el *e, size_t n)
{
  size_t cap = e->cap ? e->cap : 64;
  char *p;

  if (n + 1 <= e->cap)
    return 0;
  while (cap < n + 1)
    cap *= 2;
  p = realloc(e->buf, cap);
  if (p == 0)
    return -1;
  e->buf = p;
  e->cap = cap;
  return 0;
}

// drawing is gathered rather than written a piece at a time, so that a
// redraw reaches the terminal as one write and cannot be seen half
// done. when there is nowhere to draw, or no memory to gather in, the
// drawing is simply dropped: it must never fail the read.
static void
emit(struct el *e, const char *s, size_t n)
{
  if (e->ofd < 0 || n == 0)
    return;
  if (e->olen + n > e->ocap) {
    size_t cap = e->ocap ? e->ocap : 128;
    char *p;
    while (cap < e->olen + n)
      cap *= 2;
    p = realloc(e->out, cap);
    if (p == 0)
      return;
    e->out = p;
    e->ocap = cap;
  }
  memcpy(e->out + e->olen, s, n);
  e->olen += n;
}

static void
emits(struct el *e, const char *s)
{
  emit(e, s, strlen(s));
}

// a control sequence with one count, such as "up three rows". a count
// of zero is left out: terminals read it as one.
static void
emitcsi(struct el *e, int n, char final)
{
  char seq[24];

  if (n <= 0)
    return;
  snprintf(seq, sizeof(seq), "\x1b[%d%c", n, final);
  emits(e, seq);
}

static void
flush_out(struct el *e)
{
  size_t off = 0;

  while (e->ofd >= 0 && off < e->olen) {
    ssize_t n = write(e->ofd, e->out + off, e->olen - off);
    if (n <= 0)
      break;
    off += n;
  }
  e->olen = 0;
}

// ---- the terminal ----

// the width, or the traditional 80 when the terminal will not say. a
// width of one or two would make the arithmetic below meaningless, so
// anything that small is treated as unusable.
static int
term_cols(int fd)
{
  struct winsize ws;

  if (fd < 0 || ioctl(fd, TIOCGWINSZ, &ws) < 0 || ws.ws_col < EL_MIN_COLS)
    return EL_DEFAULT_COLS;
  return ws.ws_col;
}

static struct termios saved_mode;
static int raw_fd = -1;
static const int fatal_sigs[] = {SIGINT, SIGQUIT, SIGHUP, SIGTERM};
static sighandler_t saved_sig[sizeof(fatal_sigs) / sizeof(fatal_sigs[0])];

static void
leave_raw(void)
{
  size_t i;

  if (raw_fd < 0)
    return;
  // TCSADRAIN, not TCSAFLUSH: anything typed while the last command
  // was running is still waiting, and it belongs to the next line.
  tcsetattr(raw_fd, TCSADRAIN, &saved_mode);
  raw_fd = -1;
  for (i = 0; i < sizeof(fatal_sigs) / sizeof(fatal_sigs[0]); i++)
    signal(fatal_sigs[i], saved_sig[i]);
}

// a signal that ends the program must not leave the terminal raw, so
// the settings go back and the signal is then let through as it would
// have been had nobody been listening.
static void
on_fatal(int sig)
{
  leave_raw();
  signal(sig, SIG_DFL);
  raise(sig);
}

static int
enter_raw(int fd)
{
  static int registered;
  struct termios t;
  size_t i;

  if (tcgetattr(fd, &saved_mode) < 0)
    return -1;
  t = saved_mode;
  // no line discipline at all: no echo, no gathering into lines, and
  // no signals from the keyboard, since ctrl-C is the editor's own.
  t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
  t.c_iflag &= ~(IXON | ICRNL | INLCR | ISTRIP | BRKINT);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  // and the same on the way in: a shell that threw away what was typed
  // ahead of the prompt would be the only program on the system to do
  // it, and a test harness that types a command and then another would
  // lose the second.
  if (tcsetattr(fd, TCSADRAIN, &t) < 0)
    return -1;
  raw_fd = fd;
  if (!registered) {
    atexit(leave_raw);
    registered = 1;
  }
  for (i = 0; i < sizeof(fatal_sigs) / sizeof(fatal_sigs[0]); i++)
    saved_sig[i] = signal(fatal_sigs[i], on_fatal);
  return 0;
}

// ---- reading keys ----

// one byte, retrying when a signal cuts the read short. the answer is
// 1 for a byte, 0 at the end of the input and -1 for an error.
static int
read_byte(struct el *e, char *c)
{
  for (;;) {
    ssize_t n = read(e->ifd, c, 1);
    if (n == 1)
      return 1;
    if (n == 0)
      return 0;
    if (errno == EINTR)
      continue;
    return -1;
  }
}

// what a control sequence means, given everything after the "esc [".
// only the final byte distinguishes the arrows and the vt220 keys are
// a number and a tilde, so a sequence with parameters we do not use,
// such as the shifted arrows, still lands on the right key.
static int
decode_csi(const char *s)
{
  size_t n = strlen(s);

  switch (n ? s[n - 1] : 0) {
  case 'A': return K_UP;
  case 'B': return K_DOWN;
  case 'C': return K_RIGHT;
  case 'D': return K_LEFT;
  case 'H': return K_HOME;
  case 'F': return K_END;
  case '~':
    switch (atoi(s)) {
    case 1: case 7: return K_HOME;
    case 3: return K_DELETE;
    case 4: case 8: return K_END;
    }
    return K_NONE;
  }
  return K_NONE;
}

// the rest of an escape sequence. the bytes of a real sequence follow
// the escape immediately, so the terminal is asked to wait a tenth of
// a second for them and to give up after that: the difference between
// the escape key and the start of a sequence is only in the timing. a
// file or a pipe needs none of this, since it simply ends.
static int
read_escape(struct el *e)
{
  struct termios old;
  char seq[12];
  int timed = 0, key = K_NONE, n = 0;

  if (raw_fd == e->ifd && tcgetattr(e->ifd, &old) == 0) {
    struct termios w = old;
    w.c_cc[VMIN] = 0;
    w.c_cc[VTIME] = 1;
    timed = tcsetattr(e->ifd, TCSANOW, &w) == 0;
  }
  if (read_byte(e, &seq[0]) <= 0) {
    key = K_ESC; // the escape key by itself
  } else if (seq[0] == '[') {
    // a control sequence: parameters, then a byte from '@' to '~'
    for (n = 1; n < (int)sizeof(seq) - 1; n++) {
      if (read_byte(e, &seq[n]) <= 0)
        break;
      if (seq[n] >= 0x40 && seq[n] <= 0x7e) {
        n++;
        break;
      }
    }
    seq[n] = 0;
    key = decode_csi(seq + 1);
  } else if (seq[0] == 'O') {
    // the vt100 form, which a terminal in application mode sends
    if (read_byte(e, &seq[1]) > 0) {
      seq[2] = 0;
      key = decode_csi(seq + 1);
    }
  } else {
    // esc followed by a letter is what alt does to that letter
    switch (seq[0]) {
    case 'b': case 'B': key = K_WORDLEFT; break;
    case 'f': case 'F': key = K_WORDRIGHT; break;
    case 'd': case 'D': key = K_DELWORD; break;
    case K_BACKSPACE: case K_CTRL_H: key = K_DELWORDBACK; break;
    default: key = K_NONE; break; // anything unknown is dropped, not printed
    }
  }
  if (timed)
    tcsetattr(e->ifd, TCSANOW, &old);
  return key;
}

static int
read_key(struct el *e)
{
  char c;

  if (read_byte(e, &c) <= 0)
    return K_EOF;
  if (c == K_ESC)
    return read_escape(e);
  return (unsigned char)c;
}

// ---- drawing ----

// how much of the prompt is drawn. a prompt as wide as the terminal
// would leave nowhere for the line, so a very long one is cut short
// rather than allowed to push the line off the row.
static size_t
prompt_width(struct el *e)
{
  size_t max = (size_t)e->cols - 1;

  return e->plen < max ? e->plen : max;
}

// scrolling: the line stays on one row, and the part drawn is the
// window that holds the cursor.
static void
refresh_single(struct el *e)
{
  size_t plen = prompt_width(e), start = 0, len = e->len, pos = e->pos;

  while (plen + pos >= (size_t)e->cols) {
    start++;
    pos--;
    len--;
  }
  while (plen + len > (size_t)e->cols)
    len--;

  emits(e, "\r");
  emit(e, e->prompt, plen);
  emit(e, e->buf + start, len);
  emits(e, "\x1b[0K");
  emits(e, "\r");
  emitcsi(e, (int)(plen + pos), 'C');
  e->oldpos = e->pos;
  e->oldrows = 1;
  flush_out(e);
}

// wrapping: the line covers as many rows as it needs. the terminal
// cannot be asked where its cursor is, so the rows drawn last time and
// the row the cursor was on are what the walk back up is counted from.
static void
refresh_multi(struct el *e)
{
  size_t plen = prompt_width(e);
  size_t cols = (size_t)e->cols;
  int rows = (int)((plen + e->len + cols - 1) / cols);
  int oldrow = (int)((plen + e->oldpos) / cols);
  int currow, j;

  if (rows < 1)
    rows = 1;
  if (e->oldrows < 1)
    e->oldrows = 1;
  emitcsi(e, e->oldrows - 1 - oldrow, 'B');
  for (j = 0; j < e->oldrows - 1; j++)
    emits(e, "\r\x1b[0K\x1b[1A");
  emits(e, "\r\x1b[0K");

  emit(e, e->prompt, plen);
  emit(e, e->buf, e->len);
  // a cursor sitting just past the last column has not wrapped yet, so
  // make it wrap now yourself: otherwise the next character appears on
  // the row it has just left.
  if (e->pos == e->len && (plen + e->len) % cols == 0) {
    emits(e, "\r\n");
    rows++;
  }
  currow = (int)((plen + e->pos) / cols);
  emitcsi(e, rows - 1 - currow, 'A');
  emits(e, "\r");
  emitcsi(e, (int)((plen + e->pos) % cols), 'C');

  e->oldpos = e->pos;
  e->oldrows = rows;
  flush_out(e);
}

// the width is asked for again here rather than once at the start, so
// that a terminal resized in the middle of a line is drawn correctly
// from the next key onwards. it costs one ioctl per redraw, and none
// at all on the path that just appends a character.
static void
refresh(struct el *e)
{
  e->cols = term_cols(e->ofd);
  if (multiline)
    refresh_multi(e);
  else
    refresh_single(e);
}

// ---- changing the line ----

static void
set_line(struct el *e, const char *s)
{
  size_t n = strlen(s);

  if (reserve(e, n) < 0)
    return;
  memcpy(e->buf, s, n + 1);
  e->len = n;
  e->pos = n;
}

// take out the characters in [from, to). every caller leaves the
// cursor where the hole is, which is what all of these keys do.
static void
delete_range(struct el *e, size_t from, size_t to)
{
  if (to > e->len)
    to = e->len;
  if (to <= from)
    return;
  memmove(e->buf + from, e->buf + to, e->len - to);
  e->len -= to - from;
  e->pos = from;
  e->buf[e->len] = 0;
}

// insert one character. when it lands at the end of a line that still
// has room on its row, the character itself is the whole redraw, which
// is the case that matters: it is what typing is.
static int
insert_char(struct el *e, int c)
{
  size_t plen = prompt_width(e);
  char ch = c;

  if (reserve(e, e->len + 1) < 0)
    return -1;
  if (e->pos != e->len)
    memmove(e->buf + e->pos + 1, e->buf + e->pos, e->len - e->pos);
  e->buf[e->pos++] = ch;
  e->len++;
  e->buf[e->len] = 0;

  if (e->pos == e->len && (plen + e->len) % (size_t)e->cols != 0 &&
      (multiline || plen + e->len < (size_t)e->cols)) {
    emit(e, &ch, 1);
    e->oldpos = e->pos;
    e->oldrows = (int)((plen + e->len + (size_t)e->cols - 1) / (size_t)e->cols);
    flush_out(e);
  } else {
    refresh(e);
  }
  return 0;
}

// alt-b, alt-f and alt-d step over runs of letters and digits, which
// is what a word means in prose; ctrl-w takes what a shell means by
// one, everything back to the last blank.
static size_t
word_start(struct el *e, size_t pos)
{
  while (pos > 0 && !isalnum((unsigned char)e->buf[pos - 1]))
    pos--;
  while (pos > 0 && isalnum((unsigned char)e->buf[pos - 1]))
    pos--;
  return pos;
}

static size_t
word_end(struct el *e, size_t pos)
{
  while (pos < e->len && !isalnum((unsigned char)e->buf[pos]))
    pos++;
  while (pos < e->len && isalnum((unsigned char)e->buf[pos]))
    pos++;
  return pos;
}

// ---- the history ----

// drop the oldest entries when the list is over its limit.
static void
hist_trim(void)
{
  int drop, i;

  if (hcount <= hlimit)
    return;
  drop = hcount - hlimit;
  for (i = 0; i < drop; i++)
    free(hist[i]);
  memmove(hist, hist + drop, (size_t)(hcount - drop) * sizeof(char *));
  hcount -= drop;
}

int
el_history_add(const char *line)
{
  char *copy;

  // an empty line, and a line just like the one before it, say nothing
  // worth keeping: a repeated command should not push out the history.
  if (line == 0 || *line == 0 || hlimit < 1)
    return 0;
  if (hcount > 0 && strcmp(hist[hcount - 1], line) == 0)
    return 0;
  copy = strdup(line);
  if (copy == 0)
    return -1;
  if (hcount == hcap) {
    int cap = hcap ? hcap * 2 : 16;
    char **p = realloc(hist, (size_t)cap * sizeof(char *));
    if (p == 0) {
      free(copy);
      return -1;
    }
    hist = p;
    hcap = cap;
  }
  hist[hcount++] = copy;
  hist_trim();
  return 1;
}

int
el_history_load(const char *path)
{
  FILE *f = fopen(path, "r");
  char *line = 0;
  size_t cap = 0;
  int added = 0;
  ssize_t n;

  if (f == 0)
    return -1;
  while ((n = getline(&line, &cap, f)) > 0) {
    if (line[n - 1] == '\n')
      line[n - 1] = 0;
    if (el_history_add(line) == 1)
      added++;
  }
  free(line);
  fclose(f);
  return added;
}

int
el_history_save(const char *path)
{
  FILE *f = fopen(path, "w");
  int i;

  if (f == 0)
    return -1;
  for (i = 0; i < hcount; i++) {
    fputs(hist[i], f);
    fputc('\n', f);
  }
  return fclose(f) == 0 ? 0 : -1;
}

void
el_history_clear(void)
{
  int i;

  for (i = 0; i < hcount; i++)
    free(hist[i]);
  free(hist);
  hist = 0;
  hcount = 0;
  hcap = 0;
}

int
el_history_count(void)
{
  return hcount;
}

const char *
el_history_get(int i)
{
  return i >= 0 && i < hcount ? hist[i] : 0;
}

void
el_history_max(int n)
{
  if (n < 1)
    return;
  hlimit = n;
  hist_trim();
}

// ctrl-p and ctrl-n walk the list. the line being typed is kept aside
// so that walking back down the way you came returns to it.
static void
history_move(struct el *e, int dir)
{
  int want = e->hpos + dir;

  if (hcount == 0 || want < 0 || want > hcount)
    return;
  if (e->hpos == hcount) {
    free(e->hsaved);
    e->hsaved = strdup(e->buf);
  }
  e->hpos = want;
  set_line(e, want == hcount ? (e->hsaved ? e->hsaved : "") : hist[want]);
}

// the newest entry at or below `from` that contains the query.
static int
hist_find(const char *query, int from)
{
  int i;

  if (from >= hcount)
    from = hcount - 1;
  for (i = from; i >= 0; i--)
    if (strstr(hist[i], query))
      return i;
  return -1;
}

// the search line is kept to one row, so that the redraw afterwards
// knows exactly how much it has to undo.
static void
draw_search(struct el *e, const char *query)
{
  char head[EL_QUERY_MAX + 32];
  size_t room = (size_t)e->cols, n;

  snprintf(head, sizeof(head), "(reverse-i-search)`%s': ", query);
  emits(e, "\r\x1b[0K");
  n = strlen(head);
  if (n > room)
    n = room;
  emit(e, head, n);
  room -= n;
  n = e->len < room ? e->len : room;
  emit(e, e->buf, n);
  e->oldpos = 0;
  e->oldrows = 1;
  flush_out(e);
}

// ctrl-r: walk back through the history for an entry that contains
// what has been typed so far. the key that ends the search is handed
// back to be acted on, so that enter accepts the line it found and
// ctrl-a moves the cursor in it rather than being swallowed.
static int
reverse_search(struct el *e)
{
  char query[EL_QUERY_MAX];
  char *orig = strdup(e->buf);
  size_t origpos = e->pos, qlen = 0;
  int match = hcount - 1;

  query[0] = 0;
  for (;;) {
    int c, m;

    draw_search(e, query);
    c = read_key(e);
    if (c == K_ESC || c == K_CTRL_G || c == K_EOF) {
      // give up: the line goes back to what it was before the search
      if (orig)
        set_line(e, orig);
      e->pos = origpos < e->len ? origpos : e->len;
      free(orig);
      e->oldpos = 0;
      e->oldrows = 1;
      refresh(e);
      return c == K_EOF ? K_EOF : 0;
    }
    if (c == K_BACKSPACE || c == K_CTRL_H) {
      if (qlen > 0)
        query[--qlen] = 0;
      m = hist_find(query, hcount - 1);
      if (m >= 0) {
        match = m;
        set_line(e, hist[m]);
      }
      continue;
    }
    if (c == K_CTRL_R) {
      m = hist_find(query, match - 1);
      if (m >= 0) {
        match = m;
        set_line(e, hist[m]);
      }
      continue;
    }
    if (c >= 32 && c < 256 && c != K_BACKSPACE) {
      if (qlen + 1 < sizeof(query)) {
        query[qlen++] = c;
        query[qlen] = 0;
      }
      m = hist_find(query, match);
      if (m >= 0) {
        match = m;
        set_line(e, hist[m]);
      }
      continue;
    }
    // any other key ends the search and keeps what it found
    free(orig);
    e->oldpos = 0;
    e->oldrows = 1;
    refresh(e);
    return c;
  }
}

// ---- completion ----

static void
free_matches(char **m, int n)
{
  int i;

  for (i = 0; m && i < n; i++)
    free(m[i]);
  free(m);
}

// put `text` in place of the word that starts at `start` and ends at
// the cursor, leaving the cursor after it.
static int
replace_word(struct el *e, size_t start, const char *text)
{
  size_t tlen = strlen(text), tail = e->len - e->pos;

  if (reserve(e, start + tlen + tail) < 0)
    return -1;
  memmove(e->buf + start + tlen, e->buf + e->pos, tail);
  memcpy(e->buf + start, text, tlen);
  e->len = start + tlen + tail;
  e->pos = start + tlen;
  e->buf[e->len] = 0;
  return 0;
}

// several matches are printed the way ls prints names, down the
// columns, and the prompt is drawn again underneath them.
static void
print_columns(struct el *e, char **m, int n)
{
  int width = 0, ncols, rows, r, c, i;

  for (i = 0; i < n; i++) {
    int l = (int)strlen(m[i]);
    if (l > width)
      width = l;
  }
  width += 2;
  ncols = e->cols / width;
  if (ncols < 1)
    ncols = 1;
  rows = (n + ncols - 1) / ncols;
  emits(e, "\r\n");
  for (r = 0; r < rows; r++) {
    for (c = 0; c < ncols; c++) {
      int pad;
      i = c * rows + r;
      if (i >= n)
        continue;
      emits(e, m[i]);
      // the last name on a row needs no padding after it
      if (i + rows < n)
        for (pad = (int)strlen(m[i]); pad < width; pad++)
          emits(e, " ");
    }
    emits(e, "\r\n");
  }
  e->oldpos = 0;
  e->oldrows = 1;
  flush_out(e);
}

// tab: hand the line to the completer and act on what comes back.
static void
complete(struct el *e)
{
  char **m = 0;
  int n = 0, i;
  size_t start = e->pos;

  if (completer == 0)
    return;
  completer(e->buf, (int)e->pos, &m, &n);
  if (m == 0 || n < 1) {
    free(m);
    return;
  }
  while (start > 0 && !isspace((unsigned char)e->buf[start - 1]))
    start--;
  if (n == 1) {
    replace_word(e, start, m[0]);
  } else {
    // with several, fill in as much as they all agree on and show the
    // list, which is as far as the editor can guess on its own.
    size_t common = strlen(m[0]);
    for (i = 1; i < n; i++) {
      size_t j = 0;
      while (j < common && m[i][j] == m[0][j])
        j++;
      common = j;
    }
    if (common > e->pos - start) {
      char cut = m[0][common];
      m[0][common] = 0;
      replace_word(e, start, m[0]);
      m[0][common] = cut;
    }
    print_columns(e, m, n);
  }
  free_matches(m, n);
  refresh(e);
}

// the line has been accepted: put the cursor at its end and leave the
// terminal on a fresh row. a full redraw would do that too, but it
// would also print the prompt and the line a second time -- invisible
// on a terminal, where it overwrites what is already there, and very
// visible to anything reading the output, which is how a test harness
// reads a command's reply. so the cursor is moved instead, and the
// line is only redrawn when the window has to scroll to show its end.
static void
accept_line(struct el *e)
{
  size_t plen = prompt_width(e);

  e->cols = term_cols(e->ofd);
  if (multiline) {
    size_t cols = (size_t)e->cols;
    int lastrow = (int)((plen + e->len) / cols);
    int currow = (int)((plen + e->pos) / cols);

    emitcsi(e, lastrow - currow, 'B');
    emits(e, "\r");
    emitcsi(e, (int)((plen + e->len) % cols), 'C');
  } else if (plen + e->len < (size_t)e->cols) {
    emitcsi(e, (int)(e->len - e->pos), 'C');
  } else {
    e->pos = e->len;
    refresh(e);
  }
  emits(e, "\r\n");
  flush_out(e);
}

// ---- the editor ----

// give back everything the edit allocated except the answer itself.
static char *
finish(struct el *e, char *result)
{
  free(e->out);
  free(e->hsaved);
  if (result != e->buf)
    free(e->buf);
  return result;
}

static char *
edit(int ifd, int ofd, const char *prompt)
{
  struct el e;
  int c, pending = 0;

  memset(&e, 0, sizeof(e));
  e.ifd = ifd;
  e.ofd = ofd;
  e.prompt = prompt ? prompt : "";
  e.plen = strlen(e.prompt);
  e.cols = term_cols(ofd);
  e.oldrows = 1;
  e.hpos = hcount;
  if (reserve(&e, 0) < 0)
    return 0;
  e.buf[0] = 0;
  emit(&e, e.prompt, prompt_width(&e));
  flush_out(&e);

  for (;;) {
    if (pending) {
      c = pending;
      pending = 0;
    } else {
      c = read_key(&e);
    }

    if (c == K_EOF) {
      // the input ran out: an empty line is the end of it, and
      // anything already typed is taken as though enter had come.
      if (e.len == 0)
        return finish(&e, 0);
      break;
    }
    if (c == K_ENTER || c == K_NEWLINE)
      break;

    switch (c) {
    case K_CTRL_C:
      // abandon the line; the caller is handed an empty one so that it
      // can tell this apart from the end of the input.
      e.len = 0;
      e.pos = 0;
      e.buf[0] = 0;
      emits(&e, "^C\r\n");
      flush_out(&e);
      return finish(&e, e.buf);
    case K_CTRL_D:
      // on an empty line this is the end of the input; otherwise it is
      // the delete key, which is how every unix shell reads it.
      if (e.len == 0)
        return finish(&e, 0);
      if (e.pos < e.len) {
        delete_range(&e, e.pos, e.pos + 1);
        refresh(&e);
      }
      break;
    case K_CTRL_A:
    case K_HOME:
      e.pos = 0;
      refresh(&e);
      break;
    case K_CTRL_E:
    case K_END:
      e.pos = e.len;
      refresh(&e);
      break;
    case K_CTRL_B:
    case K_LEFT:
      if (e.pos > 0) {
        e.pos--;
        refresh(&e);
      }
      break;
    case K_CTRL_F:
    case K_RIGHT:
      if (e.pos < e.len) {
        e.pos++;
        refresh(&e);
      }
      break;
    case K_WORDLEFT:
      e.pos = word_start(&e, e.pos);
      refresh(&e);
      break;
    case K_WORDRIGHT:
      e.pos = word_end(&e, e.pos);
      refresh(&e);
      break;
    case K_CTRL_H:
    case K_BACKSPACE:
      if (e.pos > 0) {
        delete_range(&e, e.pos - 1, e.pos);
        refresh(&e);
      }
      break;
    case K_DELETE:
      if (e.pos < e.len) {
        delete_range(&e, e.pos, e.pos + 1);
        refresh(&e);
      }
      break;
    case K_CTRL_W: {
      // the shell's idea of a word: back over blanks, then back over
      // everything that is not one.
      size_t s = e.pos;
      while (s > 0 && isspace((unsigned char)e.buf[s - 1]))
        s--;
      while (s > 0 && !isspace((unsigned char)e.buf[s - 1]))
        s--;
      delete_range(&e, s, e.pos);
      refresh(&e);
      break;
    }
    case K_DELWORDBACK:
      delete_range(&e, word_start(&e, e.pos), e.pos);
      refresh(&e);
      break;
    case K_DELWORD:
      delete_range(&e, e.pos, word_end(&e, e.pos));
      refresh(&e);
      break;
    case K_CTRL_U:
      delete_range(&e, 0, e.pos);
      refresh(&e);
      break;
    case K_CTRL_K:
      delete_range(&e, e.pos, e.len);
      refresh(&e);
      break;
    case K_CTRL_T:
      // swap the two characters around the cursor and step past them,
      // which is how a transposition of two typed letters is undone.
      if (e.len >= 2) {
        size_t at = e.pos == e.len ? e.pos - 1 : e.pos;
        if (at > 0) {
          char t = e.buf[at - 1];
          e.buf[at - 1] = e.buf[at];
          e.buf[at] = t;
          e.pos = at + 1;
          refresh(&e);
        }
      }
      break;
    case K_CTRL_P:
    case K_UP:
      history_move(&e, -1);
      refresh(&e);
      break;
    case K_CTRL_N:
    case K_DOWN:
      history_move(&e, 1);
      refresh(&e);
      break;
    case K_CTRL_R:
      pending = reverse_search(&e);
      break;
    case K_CTRL_L:
      emits(&e, "\x1b[H\x1b[2J");
      e.oldpos = 0;
      e.oldrows = 1;
      refresh(&e);
      break;
    case K_TAB:
      complete(&e);
      break;
    default:
      // printable characters go in; anything else, including an escape
      // sequence nobody here knows, is dropped rather than shown.
      if (c >= 32 && c < 256)
        insert_char(&e, c);
      break;
    }
  }

  accept_line(&e);
  return finish(&e, e.buf);
}

// not a terminal: fgets into a buffer that grows until the newline
// arrives. a script is read this way, by the same call the person at
// the keyboard uses.
static char *
read_plain(void)
{
  size_t cap = 128, len = 0;
  char *buf = malloc(cap);
  int got = 0;

  if (buf == 0)
    return 0;
  for (;;) {
    char *p;
    if (fgets(buf + len, (int)(cap - len), stdin) == 0)
      break;
    got = 1;
    len += strlen(buf + len);
    if (len > 0 && buf[len - 1] == '\n') {
      buf[--len] = 0;
      break;
    }
    if (len + 1 < cap)
      break; // fgets stopped for want of input, not of room
    p = realloc(buf, cap * 2);
    if (p == 0)
      break;
    buf = p;
    cap *= 2;
  }
  if (!got) {
    free(buf);
    return 0;
  }
  buf[len] = 0;
  return buf;
}

char *
el_readline(const char *prompt)
{
  char *line;

  if (!isatty(0) || enter_raw(0) < 0)
    return read_plain();
  line = edit(0, 1, prompt);
  leave_raw();
  return line;
}

char *
__el_edit_from(int fd, const char *prompt)
{
  return edit(fd, -1, prompt);
}

void
el_free(char *line)
{
  free(line);
}

void
el_set_completer(el_completer fn)
{
  completer = fn;
}

void
el_set_multiline(int on)
{
  multiline = on;
}
