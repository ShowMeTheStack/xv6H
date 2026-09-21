// less and more: read a file a screen at a time.
//
//   less [-N] [-S] [-e] [+N] [FILE...]
//   more [-N] [-S] [-e] [+N] [FILE...]
//
//   -N  number the lines
//   -S  cut long lines off rather than folding them
//   -e  leave as soon as the end is reached
//   +N  start at line N
//
// what the keys do:
//
//   space, f, page down   the next screenful
//   b, page up            the one before
//   return, j, down       one line on
//   k, up                 one line back
//   d / u                 half a screen on or back
//   g / G                 the start or the end
//   /PATTERN              look forward for it, n and N to repeat
//   ?PATTERN              look backward
//   :n / :p               the next or previous file
//   -N                    turn line numbers on or off
//   q                     leave
//
// more is the same program: it starts without the screen being
// cleared and leaves at the end of the last file, which is what more
// does and less does not.
#include "user/user.h"
#include <regex.h>
#include <sys/ioctl.h>
#include <termios.h>

static int rows = 24, cols = 80;
static int numbers, chop, quit_at_eof, is_more;
static struct termios saved;
static int raw_on;

static char **line;
static int nlines, maxlines;
static char *text;

static char pattern[256];
static regex_t re;
static int have_pattern;

static void
raw_mode(int on)
{
  struct termios t;

  if (on) {
    if (tcgetattr(0, &saved) < 0)
      return;
    t = saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    raw_on = 1;
  } else if (raw_on) {
    tcsetattr(0, TCSANOW, &saved);
    raw_on = 0;
  }
}

static void
leave(int code)
{
  raw_mode(0);
  printf("\r\033[K");
  fflush(stdout);
  exit(code);
}

static void
on_signal(int sig)
{
  (void)sig;
  leave(0);
}

static void
get_size(void)
{
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 0) {
    rows = ws.ws_row;
    cols = ws.ws_col;
  } else {
    const char *l = getenv("LINES"), *c = getenv("COLUMNS");
    if (l && atoi(l) > 2)
      rows = atoi(l);
    if (c && atoi(c) > 0)
      cols = atoi(c);
  }
}

static void
push(char *s)
{
  if (nlines == maxlines) {
    maxlines = maxlines ? maxlines * 2 : 1024;
    line = realloc(line, maxlines * sizeof(char *));
    if (line == 0) {
      fprintf(stderr, "less: out of memory\n");
      leave(2);
    }
  }
  line[nlines++] = s;
}

static int
load(const char *path)
{
  int cap = 1 << 16, n = 0;
  FILE *f = path ? fopen(path, "r") : stdin;

  if (f == 0)
    return -1;
  free(text);
  text = malloc(cap);
  if (text == 0) {
    fprintf(stderr, "less: out of memory\n");
    leave(2);
  }
  size_t got;
  while ((got = fread(text + n, 1, cap - n - 1, f)) > 0) {
    n += got;
    if (n + 1 >= cap) {
      cap *= 2;
      text = realloc(text, cap);
      if (text == 0) {
        fprintf(stderr, "less: out of memory\n");
        leave(2);
      }
    }
  }
  if (f != stdin)
    fclose(f);
  text[n] = 0;

  nlines = 0;
  char *p = text;
  while (*p) {
    push(p);
    char *e = strchr(p, '\n');
    if (e == 0)
      break;
    *e = 0;
    p = e + 1;
  }
  return 0;
}

// one screenful from `top`, and the number of screen rows it used
static void
draw(int top, const char *name, int nfiles)
{
  int used = 0;

  printf("\033[H\033[2J");
  for (int i = top; i < nlines && used < rows - 1; i++) {
    int width = cols;
    char prefix[16];
    int plen = 0;
    if (numbers) {
      plen = snprintf(prefix, sizeof(prefix), "%6d ", i + 1);
      printf("%s", prefix);
      width -= plen;
    }
    const char *s = line[i];
    int len = strlen(s);
    if (chop) {
      printf("%.*s\r\n", len > width ? width : len, s);
      used++;
    } else {
      // a long line takes as many screen rows as it needs
      int at = 0;
      do {
        int take = len - at;
        if (take > width)
          take = width;
        if (at > 0 && numbers)
          printf("%*s", plen, "");
        printf("%.*s\r\n", take, s + at);
        at += take ? take : 1;
        used++;
      } while (at < len && used < rows - 1);
    }
  }
  for (; used < rows - 1; used++)
    printf("~\r\n");

  int last = top + rows - 1 >= nlines;
  if (name && nfiles > 1)
    printf("\033[7m%s (%d/%d) %s\033[m", name, top + 1, nlines,
           last ? "(END)" : "");
  else if (name)
    printf("\033[7m%s line %d/%d %s\033[m", name, top + 1, nlines,
           last ? "(END)" : "");
  else
    printf("\033[7m:\033[m");
  fflush(stdout);
}

// a line typed on the bottom row, for / and ?
static int
read_line_prompt(int ch, char *out, int size)
{
  int n = 0;

  printf("\r\033[K%c", ch);
  fflush(stdout);
  for (;;) {
    int c = fgetc(stdin);
    if (c == EOF || c == '\r' || c == '\n')
      break;
    if (c == 27) {
      out[0] = 0;
      return -1;
    }
    if (c == 127 || c == 8) {
      if (n > 0) {
        n--;
        printf("\b \b");
        fflush(stdout);
      }
      continue;
    }
    if (n < size - 1) {
      out[n++] = (char)c;
      putchar(c);
      fflush(stdout);
    }
  }
  out[n] = 0;
  return n;
}

static int
search(int from, int backward)
{
  if (!have_pattern)
    return -1;
  if (backward) {
    for (int i = from; i >= 0; i--)
      if (regexec(&re, line[i], 0, 0, 0) == 0)
        return i;
  } else {
    for (int i = from; i < nlines; i++)
      if (regexec(&re, line[i], 0, 0, 0) == 0)
        return i;
  }
  return -1;
}

// an escape sequence from an arrow or page key, turned into a letter
static int
read_key(void)
{
  int c = fgetc(stdin);

  if (c != 27)
    return c;
  int a = fgetc(stdin);
  if (a != '[' && a != 'O')
    return a;
  int b = fgetc(stdin);
  switch (b) {
  case 'A': return 'k';   // up
  case 'B': return 'j';   // down
  case '5': fgetc(stdin); return 'b';  // page up
  case '6': fgetc(stdin); return ' ';  // page down
  case 'H': return 'g';
  case 'F': return 'G';
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *files[256];
  int nfiles = 0, start_line = 0, i = 1;
  const char *me = argv[0];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  is_more = strcmp(me, "more") == 0;
  if (is_more)
    quit_at_eof = 1;

  for (; i < argc; i++) {
    if (argv[i][0] == '+') {
      start_line = atoi(argv[i] + 1) - 1;
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1]) {
      for (const char *p = argv[i] + 1; *p; p++) {
        switch (*p) {
        case 'N': numbers = 1; break;
        case 'S': chop = 1; break;
        case 'e': case 'E': quit_at_eof = 1; break;
        case 'c': case 'r': case 'R': case 'X': case 'F': break;
        default:
          if (*p >= '0' && *p <= '9') {
            rows = atoi(p);
            while (p[1] >= '0' && p[1] <= '9')
              p++;
            break;
          }
          fprintf(stderr, "usage: %s [-N] [-S] [-e] [+N] [FILE...]\n", me);
          exit(1);
        }
      }
      continue;
    }
    if (nfiles < 256)
      files[nfiles++] = argv[i];
  }
  if (start_line < 0)
    start_line = 0;

  get_size();

  // with no terminal there is nothing to page against, so the files
  // simply go out -- which is what makes `less` usable in a pipeline
  if (!isatty(1)) {
    for (int k = 0; k < nfiles || (k == 0 && nfiles == 0); k++) {
      if (load(nfiles ? files[k] : 0) < 0) {
        fprintf(stderr, "%s: %s: %s\n", me, nfiles ? files[k] : "-",
                strerror(errno));
        continue;
      }
      for (int j = 0; j < nlines; j++)
        if (numbers)
          printf("%6d %s\n", j + 1, line[j]);
        else
          printf("%s\n", line[j]);
    }
    exit(0);
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  raw_mode(1);

  int at_file = 0;
  for (;;) {
    const char *name = nfiles ? files[at_file] : 0;
    if (load(name) < 0) {
      raw_mode(0);
      fprintf(stderr, "%s: %s: %s\n", me, name ? name : "-", strerror(errno));
      if (nfiles <= 1)
        exit(1);
      if (++at_file >= nfiles)
        leave(1);
      raw_mode(1);
      continue;
    }

    int top = start_line;
    start_line = 0;
    int page = rows - 1;
    int last_search_back = 0;

    for (;;) {
      if (top > nlines - 1)
        top = nlines - 1;
      if (top < 0)
        top = 0;
      draw(top, name ? name : "(standard input)", nfiles);

      int c = read_key();
      if (c == EOF)
        break;
      switch (c) {
      case 'q': case 'Q':
        leave(0);
      case ' ': case 'f': case 6:
        if (top + page >= nlines) {
          if (quit_at_eof) {
            if (at_file + 1 >= nfiles)
              leave(0);
            goto next_file;
          }
        } else {
          top += page;
        }
        break;
      case 'b': case 2:
        top -= page;
        break;
      case '\r': case '\n': case 'j': case 14:
        if (top + 1 < nlines)
          top++;
        else if (quit_at_eof && at_file + 1 >= nfiles)
          leave(0);
        break;
      case 'k': case 16:
        top--;
        break;
      case 'd': case 4:
        top += page / 2;
        break;
      case 'u': case 21:
        top -= page / 2;
        break;
      case 'g': case '<':
        top = 0;
        break;
      case 'G': case '>':
        top = nlines - page;
        break;
      case 'N':
        numbers = !numbers;
        break;
      case '-': {
        int o = fgetc(stdin);
        if (o == 'N')
          numbers = !numbers;
        else if (o == 'S')
          chop = !chop;
        break;
      }
      case '/': case '?': {
        char buf[256];
        if (read_line_prompt(c, buf, sizeof(buf)) < 0)
          break;
        if (buf[0]) {
          if (have_pattern)
            regfree(&re);
          have_pattern = 0;
          if (regcomp(&re, buf, REG_EXTENDED | REG_NOSUB) == 0) {
            snprintf(pattern, sizeof(pattern), "%s", buf);
            have_pattern = 1;
          } else {
            printf("\r\033[Kbad pattern");
            fflush(stdout);
            fgetc(stdin);
            break;
          }
        }
        last_search_back = (c == '?');
        int hit = search(last_search_back ? top - 1 : top + 1, last_search_back);
        if (hit < 0) {
          printf("\r\033[Knot found");
          fflush(stdout);
          fgetc(stdin);
        } else {
          top = hit;
        }
        break;
      }
      case 'n': case 'p': {
        int back = c == 'p' ? !last_search_back : last_search_back;
        int hit = search(back ? top - 1 : top + 1, back);
        if (hit >= 0)
          top = hit;
        break;
      }
      case ':': {
        int o = fgetc(stdin);
        if (o == 'n' && at_file + 1 < nfiles)
          goto next_file;
        if (o == 'p' && at_file > 0) {
          at_file -= 2;
          goto next_file;
        }
        if (o == 'q')
          leave(0);
        break;
      }
      case 'h': case 'H':
        printf("\033[H\033[2Jspace forward  b back  j/k a line  d/u half\r\n"
               "g start  G end  /pat search  n next  :n next file  q quit\r\n"
               "\033[7many key\033[m");
        fflush(stdout);
        fgetc(stdin);
        break;
      }
    }

  next_file:
    if (++at_file >= nfiles)
      leave(0);
  }
}
