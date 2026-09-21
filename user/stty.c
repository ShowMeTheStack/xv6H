// stty, reset and clear: the terminal's settings, and putting them back.
//
//   stty [-a] [-g] [SETTING...]
//   reset          put the terminal back the way it should be
//   clear          empty the screen
//
// the settings are the familiar ones: the speed, the character size,
// the flags (echo, icanon, isig, and the rest, each with a `-' to turn
// it off), the control characters (intr, erase, kill, eof, ...), the
// window size (rows, columns), and the two names for a sensible set,
// `sane' and `raw'.
#include "user/user.h"
#include <termios.h>
#include <sys/ioctl.h>

static int status;

struct flagname {
  const char *name;
  int which;      // 0 input, 1 output, 2 control, 3 local
  unsigned bit;
};

static const struct flagname flags[] = {
  // input
  {"ignbrk", 0, IGNBRK}, {"brkint", 0, BRKINT}, {"ignpar", 0, IGNPAR},
  {"parmrk", 0, PARMRK}, {"inpck", 0, INPCK},   {"istrip", 0, ISTRIP},
  {"inlcr", 0, INLCR},   {"igncr", 0, IGNCR},   {"icrnl", 0, ICRNL},
  {"ixon", 0, IXON},     {"ixoff", 0, IXOFF},   {"ixany", 0, IXANY},
  // output
  {"opost", 1, OPOST},   {"onlcr", 1, ONLCR},
  // control
  {"cstopb", 2, CSTOPB}, {"cread", 2, CREAD},   {"parenb", 2, PARENB},
  {"parodd", 2, PARODD}, {"hupcl", 2, HUPCL},   {"clocal", 2, CLOCAL},
  // local
  {"isig", 3, ISIG},     {"icanon", 3, ICANON}, {"echo", 3, ECHO},
  {"echoe", 3, ECHOE},   {"echok", 3, ECHOK},   {"echonl", 3, ECHONL},
  {"noflsh", 3, NOFLSH}, {"tostop", 3, TOSTOP}, {"iexten", 3, IEXTEN},
  {0, 0, 0}
};

struct ccname {
  const char *name;
  int index;
};

static const struct ccname controls[] = {
  {"intr", VINTR}, {"quit", VQUIT}, {"erase", VERASE}, {"kill", VKILL},
  {"eof", VEOF},       {"start", VSTART}, {"stop", VSTOP},
  {"susp", VSUSP},     {"werase", VWERASE}, {"rprnt", VREPRINT},
  {"lnext", VLNEXT},   {"min", VMIN},     {"time", VTIME},   {0, 0}
};

static unsigned *
field(struct termios *t, int which)
{
  switch (which) {
  case 0: return &t->c_iflag;
  case 1: return &t->c_oflag;
  case 2: return &t->c_cflag;
  }
  return &t->c_lflag;
}

// a control character as stty writes it: ^C, or `undef' when it is off
static void
print_cc(const char *name, unsigned char c)
{
  if (c == 0 || c == 0377)
    printf("%s = <undef>; ", name);
  else if (c < 32)
    printf("%s = ^%c; ", name, c + '@');
  else if (c == 127)
    printf("%s = ^?; ", name);
  else
    printf("%s = %c; ", name, c);
}

// the other direction: ^C, \0, undef, or a plain character
static int
parse_cc(const char *s)
{
  if (strcmp(s, "undef") == 0 || strcmp(s, "^-") == 0)
    return 0;
  if (s[0] == '^' && s[1]) {
    if (s[1] == '?')
      return 127;
    return toupper((unsigned char)s[1]) - '@';
  }
  if (s[0] == '\\' && s[1] == '0')
    return 0;
  return (unsigned char)s[0];
}

static void
show(const struct termios *t, int all)
{
  struct winsize ws;

  if (ioctl(0, TIOCGWINSZ, &ws) == 0)
    printf("rows %d; columns %d;\n", ws.ws_row, ws.ws_col);
  printf("speed 38400 baud; line = 0;\n");

  for (int i = 0; controls[i].name; i++) {
    if (!all && (controls[i].index == VMIN || controls[i].index == VTIME))
      continue;
    print_cc(controls[i].name, t->c_cc[controls[i].index]);
  }
  printf("\n");

  if (!all) {
    // the short form names only what differs from the usual settings
    struct termios s;
    memset(&s, 0, sizeof(s));
    s.c_iflag = ICRNL | IXON | BRKINT;
    s.c_oflag = OPOST | ONLCR;
    s.c_cflag = CREAD | CS8;
    s.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    int printed = 0;
    for (int i = 0; flags[i].name; i++) {
      const unsigned *have = field((struct termios *)t, flags[i].which);
      const unsigned *want = field(&s, flags[i].which);
      int on = (*have & flags[i].bit) != 0;
      int usual = (*want & flags[i].bit) != 0;
      if (on != usual) {
        printf("%s%s ", on ? "" : "-", flags[i].name);
        printed = 1;
      }
    }
    if (printed)
      printf("\n");
    return;
  }

  for (int which = 0; which < 4; which++) {
    int n = 0;
    for (int i = 0; flags[i].name; i++) {
      if (flags[i].which != which)
        continue;
      const unsigned *have = field((struct termios *)t, which);
      printf("%s%s ", (*have & flags[i].bit) ? "" : "-", flags[i].name);
      n++;
    }
    if (n)
      printf("\n");
  }
}

// the settings a terminal should have when nothing has gone wrong
static void
make_sane(struct termios *t)
{
  t->c_iflag = BRKINT | ICRNL | IXON;
  t->c_oflag = OPOST | ONLCR;
  t->c_cflag = (t->c_cflag & ~CSIZE) | CS8 | CREAD;
  t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
  t->c_cc[VINTR] = 3;     // ^C
  t->c_cc[VQUIT] = 28;    // control-backslash
  t->c_cc[VERASE] = 127;  // del
  t->c_cc[VKILL] = 21;    // ^U
  t->c_cc[VEOF] = 4;      // ^D
  t->c_cc[VSTART] = 17;   // ^Q
  t->c_cc[VSTOP] = 19;    // ^S
  t->c_cc[VSUSP] = 26;    // ^Z
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

static void
make_raw(struct termios *t)
{
  t->c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  struct termios t;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  if (strcmp(me, "clear") == 0) {
    printf("\033[H\033[2J\033[3J");
    fflush(stdout);
    exit(0);
  }

  if (strcmp(me, "reset") == 0) {
    // put the terminal back: the settings, then the screen
    if (tcgetattr(0, &t) == 0) {
      make_sane(&t);
      tcsetattr(0, TCSANOW, &t);
    }
    // leave any alternate screen, show the cursor, plain attributes
    printf("\033c\033[?1049l\033[?25h\033[m\033[H\033[2J");
    fflush(stdout);
    exit(0);
  }

  int fd = 0;
  int i = 1;
  int all = 0, save = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
      all = 1;
      continue;
    }
    if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--save") == 0) {
      save = 1;
      continue;
    }
    if ((strcmp(argv[i], "-F") == 0 || strcmp(argv[i], "--file") == 0) &&
        i + 1 < argc) {
      fd = open(argv[++i], O_RDONLY);
      if (fd < 0) {
        fprintf(stderr, "stty: %s: %s\n", argv[i], strerror(errno));
        exit(1);
      }
      continue;
    }
    break;   // a leading dash here turns a flag off
  }

  if (tcgetattr(fd, &t) < 0) {
    fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
    exit(1);
  }

  if (i >= argc) {
    if (save) {
      // the form stty itself reads back
      printf("%x:%x:%x:%x", t.c_iflag, t.c_oflag, t.c_cflag, t.c_lflag);
      for (int k = 0; k < NCCS; k++)
        printf(":%x", t.c_cc[k]);
      printf("\n");
    } else {
      show(&t, all);
    }
    exit(0);
  }

  int changed = 0;
  for (; i < argc; i++) {
    const char *a = argv[i];
    int off = 0;
    if (a[0] == '-' && a[1]) {
      off = 1;
      a++;
    }

    if (strcmp(a, "sane") == 0 || strcmp(a, "cooked") == 0) {
      make_sane(&t);
      changed = 1;
      continue;
    }
    if (strcmp(a, "raw") == 0) {
      if (off)
        make_sane(&t);
      else
        make_raw(&t);
      changed = 1;
      continue;
    }
    if (strcmp(a, "rows") == 0 || strcmp(a, "columns") == 0 ||
        strcmp(a, "cols") == 0) {
      if (i + 1 >= argc)
        break;
      struct winsize ws;
      if (ioctl(fd, TIOCGWINSZ, &ws) < 0)
        memset(&ws, 0, sizeof(ws));
      if (a[0] == 'r')
        ws.ws_row = atoi(argv[++i]);
      else
        ws.ws_col = atoi(argv[++i]);
      if (ioctl(fd, TIOCSWINSZ, &ws) < 0) {
        fprintf(stderr, "stty: cannot set the size: %s\n", strerror(errno));
        status = 1;
      }
      continue;
    }
    if (strcmp(a, "speed") == 0 || strcmp(a, "ispeed") == 0 ||
        strcmp(a, "ospeed") == 0) {
      if (i + 1 < argc)
        i++;   // the console has one speed and it is not settable
      continue;
    }
    if (strcmp(a, "cs5") == 0 || strcmp(a, "cs6") == 0 ||
        strcmp(a, "cs7") == 0 || strcmp(a, "cs8") == 0) {
      t.c_cflag &= ~CSIZE;
      t.c_cflag |= a[2] == '5' ? CS5 : a[2] == '6' ? CS6
                   : a[2] == '7' ? CS7 : CS8;
      changed = 1;
      continue;
    }

    int done = 0;
    for (int k = 0; flags[k].name; k++)
      if (strcmp(a, flags[k].name) == 0) {
        unsigned *f = field(&t, flags[k].which);
        if (off)
          *f &= ~flags[k].bit;
        else
          *f |= flags[k].bit;
        changed = 1;
        done = 1;
        break;
      }
    if (done)
      continue;

    for (int k = 0; controls[k].name; k++)
      if (strcmp(a, controls[k].name) == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "stty: %s wants a character\n", a);
          exit(1);
        }
        const char *v = argv[++i];
        if (controls[k].index == VMIN || controls[k].index == VTIME)
          t.c_cc[controls[k].index] = (unsigned char)atoi(v);
        else
          t.c_cc[controls[k].index] = (unsigned char)parse_cc(v);
        changed = 1;
        done = 1;
        break;
      }
    if (done)
      continue;

    fprintf(stderr, "stty: cannot make sense of '%s'\n", argv[i]);
    exit(1);
  }

  if (changed && tcsetattr(fd, TCSANOW, &t) < 0) {
    fprintf(stderr, "stty: cannot set the terminal: %s\n", strerror(errno));
    exit(1);
  }
  exit(status);
}
