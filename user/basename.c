// basename and dirname: the two halves of a path.
#include "user/user.h"
#include <libgen.h>

int
main(int argc, char *argv[])
{
  const char *me = argv[0];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  if (argc < 2) {
    fprintf(stderr, "usage: %s PATH [SUFFIX]\n", me);
    exit(1);
  }
  char copy[PATH_MAX];
  snprintf(copy, sizeof(copy), "%s", argv[1]);
  if (strcmp(me, "dirname") == 0) {
    printf("%s\n", dirname(copy));
    exit(0);
  }
  char *b = basename(copy);
  if (argc > 2) {
    int n = strlen(b), m = strlen(argv[2]);
    if (m < n && strcmp(b + n - m, argv[2]) == 0)
      b[n - m] = 0;
  }
  printf("%s\n", b);
  exit(0);
}
