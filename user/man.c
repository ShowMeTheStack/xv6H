//
// man -- the manual.
//
// The pages live in one file, /usr/share/man/man.db, with a marker line
// before each (tools/mkman.py writes it). This finds the marker and
// prints what follows it up to the next one. `man -k' looks in the
// @@whatis entry instead, which is one line per page.
//
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MANDB "/usr/share/man/man.db"

static void
usage(void)
{
  fprintf(stderr, "usage: man [-k] [NAME...]\n");
  exit(1);
}

// print the entry called `name', and return 1 if it was there. lines
// beginning with @@ are markers and are not part of any page.
static int
show(const char *name)
{
  FILE *f = fopen(MANDB, "r");
  char line[512];
  int in = 0, found = 0;

  if (f == 0) {
    fprintf(stderr, "man: %s: no manual on this system\n", MANDB);
    exit(1);
  }
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '@' && line[1] == '@') {
      char *nl = strchr(line, '\n');
      if (nl)
        *nl = 0;
      in = strcmp(line + 2, name) == 0;
      if (in)
        found = 1;
      continue;
    }
    if (in)
      fputs(line, stdout);
  }
  fclose(f);
  return found;
}

int
main(int argc, char *argv[])
{
  int k = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-k") == 0)
      k = 1;
    else
      usage();
  }
  if (i == argc) {
    // with nothing to look up: what there is
    if (!show("whatis"))
      return 1;
    return 0;
  }
  if (k) {
    // the index, filtered: `man -k net' is every page whose one-line
    // description mentions it
    FILE *f = fopen(MANDB, "r");
    char line[512];
    int in = 0, hits = 0;
    if (f == 0) {
      fprintf(stderr, "man: %s: no manual on this system\n", MANDB);
      return 1;
    }
    while (fgets(line, sizeof(line), f)) {
      if (line[0] == '@' && line[1] == '@') {
        char *nl = strchr(line, '\n');
        if (nl)
          *nl = 0;
        in = strcmp(line + 2, "whatis") == 0;
        continue;
      }
      if (!in)
        continue;
      for (int j = i; j < argc; j++)
        if (strstr(line, argv[j])) {
          fputs(line, stdout);
          hits++;
          break;
        }
    }
    fclose(f);
    if (hits == 0) {
      fprintf(stderr, "man: nothing appropriate\n");
      return 1;
    }
    return 0;
  }
  int bad = 0;
  for (; i < argc; i++) {
    if (!show(argv[i])) {
      fprintf(stderr, "man: no manual entry for %s\n", argv[i]);
      bad = 1;
    }
  }
  return bad;
}
