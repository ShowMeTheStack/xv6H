// groups: the groups a user is in.
#include "user/user.h"
#include <pwd.h>
#include <grp.h>

int
main(int argc, char *argv[])
{
  const char *name;
  gid_t list[32];
  int n = 32;

  if (argc > 1) {
    name = argv[1];
    struct passwd *pw = getpwnam(name);
    if (pw == 0) {
      fprintf(stderr, "groups: %s: no such user\n", name);
      exit(1);
    }
    if (getgrouplist(name, pw->pw_gid, list, &n) < 0)
      n = 1;
  } else {
    struct passwd *pw = getpwuid(getuid());
    name = pw ? pw->pw_name : "";
    n = getgroups(30, list + 1);
    if (n < 0)
      n = 0;
    // the effective group is one of the groups you are in, whether or
    // not it is in the supplementary list
    list[0] = getegid();
    int dup = 0;
    for (int i = 0; i < n; i++)
      if (list[1 + i] == list[0])
        dup = 1;
    if (dup) {
      memmove(list, list + 1, (size_t)n * sizeof(list[0]));
    } else {
      n++;
    }
  }
  for (int i = 0; i < n; i++) {
    struct group *g = getgrgid(list[i]);
    printf("%s%s", i ? " " : "", g ? g->gr_name : "?");
  }
  printf("\n");
  exit(0);
}
