// mv: give a file another name.
//
//   mv SOURCE TARGET
//   mv SOURCE... DIRECTORY
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

static int
isdir(const char *p)
{
  struct stat st;

  return stat(p, &st) == 0 && st.type == T_DIR;
}

static int
move(const char *from, const char *to)
{
  if (rename(from, to) == 0)
    return 0;
  fprintf(stderr, "mv: %s -> %s: %s\n", from, to, strerror(errno));
  return 1;
}

int
main(int argc, char *argv[])
{
  if (argc < 3) {
    fprintf(stderr, "usage: mv source... target\n");
    return 1;
  }
  const char *target = argv[argc - 1];

  if (argc == 3 && !isdir(target))
    return move(argv[1], target);

  if (!isdir(target)) {
    fprintf(stderr, "mv: %s is not a directory\n", target);
    return 1;
  }
  int bad = 0;
  for (int i = 1; i < argc - 1; i++) {
    const char *base = argv[i], *p;
    char path[PATH_MAX];

    for (p = argv[i]; *p; p++)
      if (*p == '/')
        base = p + 1;
    snprintf(path, sizeof(path), "%s/%s", target, base);
    bad |= move(argv[i], path);
  }
  return bad;
}
