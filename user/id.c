// id: who this process is. with a name, who that user is.
#include "user/user.h"
#include <pwd.h>
#include <grp.h>

static void
show(uid_t u, gid_t g, const gid_t *groups, int n)
{
  struct passwd *pw = getpwuid(u);
  struct group *gr = getgrgid(g);

  printf("uid=%d", (int)u);
  if (pw)
    printf("(%s)", pw->pw_name);
  printf(" gid=%d", (int)g);
  if (gr)
    printf("(%s)", gr->gr_name);
  if (n > 0) {
    printf(" groups=");
    for (int i = 0; i < n; i++) {
      struct group *x = getgrgid(groups[i]);
      printf("%s%d", i ? "," : "", (int)groups[i]);
      if (x)
        printf("(%s)", x->gr_name);
    }
  }
  printf("\n");
}

int
main(int argc, char *argv[])
{
  int nameonly = 0, useronly = 0, grouponly = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *p = argv[i] + 1; *p; p++) {
      if (*p == 'n')
        nameonly = 1;
      else if (*p == 'u')
        useronly = 1;
      else if (*p == 'g')
        grouponly = 1;
      else {
        fprintf(stderr, "usage: id [-ngu] [USER]\n");
        exit(1);
      }
    }

  uid_t u = geteuid();
  gid_t g = getegid();
  gid_t groups[32];
  int n = getgroups(32, groups);

  if (i < argc) {
    struct passwd *pw = getpwnam(argv[i]);
    if (pw == 0) {
      fprintf(stderr, "id: %s: no such user\n", argv[i]);
      exit(1);
    }
    u = pw->pw_uid;
    g = pw->pw_gid;
    n = 0;
  }
  if (useronly || grouponly) {
    unsigned v = useronly ? (unsigned)u : (unsigned)g;
    if (nameonly) {
      struct passwd *pw = useronly ? getpwuid(u) : 0;
      struct group *gr = grouponly ? getgrgid(g) : 0;
      printf("%s\n", pw ? pw->pw_name : gr ? gr->gr_name : "");
    } else
      printf("%u\n", v);
    exit(0);
  }
  show(u, g, groups, n < 0 ? 0 : n);
  exit(0);
}
