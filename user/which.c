// which: where a command is on PATH.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int all = 0, i = 1, status = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-a") == 0)
      all = 1;
    else {
      fprintf(stderr, "usage: which [-a] NAME...\n");
      exit(1);
    }
  }
  const char *path = getenv("PATH");
  if (path == 0)
    path = "/bin:/sbin:/usr/bin";

  for (; i < argc; i++) {
    int found = 0;
    if (strchr(argv[i], '/')) {
      if (access(argv[i], X_OK) == 0) {
        printf("%s\n", argv[i]);
        found = 1;
      }
    } else {
      char copy[512];
      snprintf(copy, sizeof(copy), "%s", path);
      for (char *p = copy; p && *p;) {
        char *colon = strchr(p, ':');
        if (colon)
          *colon = 0;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", *p ? p : ".", argv[i]);
        if (access(full, X_OK) == 0) {
          printf("%s\n", full);
          found = 1;
          if (!all)
            break;
        }
        p = colon ? colon + 1 : 0;
      }
    }
    if (!found)
      status = 1;
  }
  exit(status);
}
