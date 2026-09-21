// chown: who a file belongs to.
//
//   chown USER FILE...        chown USER:GROUP FILE...
//   chgrp GROUP FILE...       (the same program under another name)
#include "user/user.h"
#include <pwd.h>
#include <grp.h>

static int recursive, status, asgroup;

static void
apply(int uid, int gid, const char *path)
{
  struct stat st;

  if (lstat(path, &st) < 0) {
    fprintf(stderr, "chown: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  if (lchown(path, (uid_t)uid, (gid_t)gid) < 0) {
    fprintf(stderr, "chown: %s: %s\n", path, strerror(errno));
    status = 1;
  }
  if (recursive && st.type == T_DIR) {
    DIR *d = opendir(path);
    if (d == 0)
      return;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s%s%s", path,
               path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
      apply(uid, gid, child);
    }
    closedir(d);
  }
}

// a name or a number
static int
user_id(const char *s)
{
  struct passwd *pw = getpwnam(s);

  if (pw)
    return (int)pw->pw_uid;
  if (s[0] >= '0' && s[0] <= '9')
    return atoi(s);
  return -1;
}

static int
group_id(const char *s)
{
  struct group *gr = getgrnam(s);

  if (gr)
    return (int)gr->gr_gid;
  if (s[0] >= '0' && s[0] <= '9')
    return atoi(s);
  return -1;
}

int
main(int argc, char *argv[])
{
  int i = 1, uid = -1, gid = -1;
  const char *me = argv[0];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  asgroup = strcmp(me, "chgrp") == 0;

  for (; i < argc && argv[i][0] == '-' && (argv[i][1] == 'R' || argv[i][1] == 'r'); i++)
    recursive = 1;
  if (argc - i < 2) {
    fprintf(stderr, "usage: %s [-R] %s FILE...\n", me, asgroup ? "GROUP" : "USER[:GROUP]");
    exit(1);
  }

  char spec[128];
  snprintf(spec, sizeof(spec), "%s", argv[i++]);
  if (asgroup) {
    gid = group_id(spec);
    if (gid < 0) {
      fprintf(stderr, "chgrp: %s: no such group\n", spec);
      exit(1);
    }
  } else {
    char *colon = strchr(spec, ':');
    if (colon) {
      *colon = 0;
      gid = group_id(colon + 1);
      if (gid < 0) {
        fprintf(stderr, "chown: %s: no such group\n", colon + 1);
        exit(1);
      }
    }
    if (spec[0]) {
      uid = user_id(spec);
      if (uid < 0) {
        fprintf(stderr, "chown: %s: no such user\n", spec);
        exit(1);
      }
    }
  }
  for (; i < argc; i++)
    apply(uid, gid, argv[i]);
  exit(status);
}
