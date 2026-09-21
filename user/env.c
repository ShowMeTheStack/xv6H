// env: the environment, or a command run in a changed one.
//
//   env                       print it
//   env NAME=VALUE... CMD...  run CMD with those set
//   env -i ...                start from an empty environment
//   env -u NAME ...           without that one
#include "user/user.h"

extern char **environ;

int
main(int argc, char *argv[])
{
  int i = 1;
  const char *me = argv[0];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  if (strcmp(me, "printenv") == 0) {
    if (argc > 1) {
      for (int k = 1; k < argc; k++) {
        char *v = getenv(argv[k]);
        if (v)
          printf("%s\n", v);
      }
      exit(0);
    }
    for (char **e = environ; *e; e++)
      printf("%s\n", *e);
    exit(0);
  }

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-i") == 0)
      clearenv();
    else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
      unsetenv(argv[++i]);
    else {
      fprintf(stderr, "usage: env [-i] [-u NAME] [NAME=VALUE...] [COMMAND...]\n");
      exit(1);
    }
  }
  for (; i < argc && strchr(argv[i], '='); i++) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", argv[i]);
    char *eq = strchr(copy, '=');
    *eq = 0;
    setenv(copy, eq + 1, 1);
  }
  if (i >= argc) {
    for (char **e = environ; *e; e++)
      printf("%s\n", *e);
    exit(0);
  }
  execvp(argv[i], &argv[i]);
  fprintf(stderr, "env: %s: %s\n", argv[i], strerror(errno));
  exit(127);
}
