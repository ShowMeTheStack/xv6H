//
// --help, answered from the manual.
//
// Every program passes through start() in crt.c, and every program is
// asked `--help' sooner or later. Rather than a hundred and sixty-four
// copies of the same paragraph, one of them here: the manual page's
// NAME, SYNOPSIS and OPTIONS sections, printed before main() sees the
// argument. A program with no manual entry keeps its own behaviour --
// this returns and main() runs as it always did.
//
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "libc.h"

#define MANDB "/usr/share/man/man.db"

static void
put(const char *s, int n)
{
  while (n > 0) {
    int w = write(1, s, n);
    if (w <= 0)
      return;
    s += w;
    n -= w;
  }
}

// the sections a --help prints: what it is, how it is called, and what
// the options are. DESCRIPTION and the rest are what `man' is for.
static int
wanted(const char *line)
{
  return strncmp(line, "NAME", 4) == 0 || strncmp(line, "SYNOPSIS", 8) == 0 ||
         strncmp(line, "OPTIONS", 7) == 0;
}

// returns 1 when it printed something, and the caller exits.
int
__libc_help(const char *name)
{
  char buf[1024];
  int fd = open(MANDB, O_RDONLY);
  int n, held = 0, in = 0, show = 0, printed = 0;

  if (fd < 0)
    return 0;
  // one line at a time, without stdio: this runs before main and must
  // not decide how the program's streams are set up.
  while ((n = read(fd, buf + held, sizeof(buf) - held - 1)) > 0) {
    n += held;
    int start = 0;
    for (int i = 0; i < n; i++) {
      if (buf[i] != '\n')
        continue;
      buf[i] = 0;
      char *line = buf + start;
      if (line[0] == '@' && line[1] == '@') {
        in = strcmp(line + 2, name) == 0;
        show = 0;
      } else if (in) {
        if (line[0] && line[0] != ' ' && line[0] != '\t')
          show = wanted(line);          // a section heading
        if (show) {
          put(line, strlen(line));
          put("\n", 1);
          printed = 1;
        }
      }
      start = i + 1;
    }
    held = n - start;
    if (held > 0)
      memmove(buf, buf + start, held);
    if (held >= (int)sizeof(buf) - 1)
      held = 0;                          // a line longer than the buffer
  }
  close(fd);
  return printed;
}
