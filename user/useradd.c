//
// the account tools: one program under six names, because they are
// six views of the same two files.
//
//   useradd  [-u UID] [-g GROUP] [-G GROUPS] [-d HOME] [-s SHELL] [-m] NAME
//   userdel  [-r] NAME
//   usermod  [-u UID] [-g GROUP] [-G GROUPS] [-d HOME] [-s SHELL] [-L] [-U] NAME
//   groupadd [-g GID] NAME
//   groupdel NAME
//   groupmod [-g GID] [-n NEWNAME] NAME
//
// every change rewrites the file through a temporary and renames it,
// so a crash leaves the old file rather than half of the new one, and
// /etc/shadow keeps its 0600.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <shadow.h>

#define PASSWD "/etc/passwd"
#define GROUP  "/etc/group"
#define SHADOW "/etc/shadow"

static const char *me;

static void
die(const char *fmt, ...)
{
  va_list ap;

  fprintf(stderr, "%s: ", me);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

// rewrite `path`, replacing the line whose first colon-field is `key`
// with `replacement`, or removing it when that is null. with `append`
// the line is added when the key was not there.
static void
rewrite(const char *path, const char *key, const char *replacement, int append, int mode)
{
  char tmp[128], line[512];
  FILE *in, *out;
  int found = 0;

  snprintf(tmp, sizeof(tmp), "%s.new", path);
  in = fopen(path, "r");
  out = fopen(tmp, "w");
  if (out == 0)
    die("cannot write %s: %s", tmp, strerror(errno));
  while (in && fgets(line, sizeof(line), in)) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", line);
    char *colon = strchr(copy, ':');
    if (colon) {
      *colon = 0;
      if (strcmp(copy, key) == 0) {
        found = 1;
        if (replacement)
          fprintf(out, "%s\n", replacement);
        continue;
      }
    }
    fputs(line, out);
  }
  if (in)
    fclose(in);
  if (!found && append && replacement)
    fprintf(out, "%s\n", replacement);
  if (fclose(out) != 0)
    die("cannot write %s: %s", tmp, strerror(errno));
  chmod(tmp, mode);
  if (rename(tmp, path) < 0)
    die("cannot replace %s: %s", path, strerror(errno));
}

// the first free id at or above `from`
static int
free_uid(int from)
{
  for (int u = from; u < 60000; u++)
    if (getpwuid((uid_t)u) == 0)
      return u;
  die("no free user id");
  return -1;
}

static int
free_gid(int from)
{
  for (int g = from; g < 60000; g++)
    if (getgrgid((gid_t)g) == 0)
      return g;
  die("no free group id");
  return -1;
}

// add a user to the member list of each named group, and take them out
// of every other group they are not meant to be in
static void
set_supplementary(const char *name, const char *list)
{
  struct group *g;
  char wanted[256];

  snprintf(wanted, sizeof(wanted), "%s", list ? list : "");
  setgrent();
  // collect the names first: the rewrite below reopens the file
  char names[64][32];
  gid_t gids[64];
  char members[64][256];
  int n = 0;
  while ((g = getgrent()) != 0 && n < 64) {
    snprintf(names[n], sizeof(names[n]), "%s", g->gr_name);
    gids[n] = g->gr_gid;
    members[n][0] = 0;
    for (char **m = g->gr_mem; *m; m++) {
      if (members[n][0])
        strcat(members[n], ",");
      strcat(members[n], *m);
    }
    n++;
  }
  endgrent();

  for (int i = 0; i < n; i++) {
    // is this group in the wanted list?
    int want = 0;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", wanted);
    for (char *p = copy; *p;) {
      char *comma = strchr(p, ',');
      if (comma)
        *comma = 0;
      if (strcmp(p, names[i]) == 0)
        want = 1;
      if (comma == 0)
        break;
      p = comma + 1;
    }
    // is the user in it now?
    int have = 0;
    char mcopy[256];
    snprintf(mcopy, sizeof(mcopy), "%s", members[i]);
    char rebuilt[256];
    rebuilt[0] = 0;
    for (char *p = mcopy; *p;) {
      char *comma = strchr(p, ',');
      if (comma)
        *comma = 0;
      if (strcmp(p, name) == 0)
        have = 1;
      else if (*p) {
        if (rebuilt[0])
          strcat(rebuilt, ",");
        strcat(rebuilt, p);
      }
      if (comma == 0)
        break;
      p = comma + 1;
    }
    if (want == have && !(want && !have))
      continue;
    if (want) {
      if (rebuilt[0])
        strcat(rebuilt, ",");
      strcat(rebuilt, name);
    }
    char line[512];
    snprintf(line, sizeof(line), "%s:x:%u:%s", names[i], (unsigned)gids[i], rebuilt);
    rewrite(GROUP, names[i], line, 0, 0644);
  }
}

static void
make_home(const char *dir, uid_t uid, gid_t gid)
{
  if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
    fprintf(stderr, "%s: cannot make %s: %s\n", me, dir, strerror(errno));
    return;
  }
  if (chown(dir, uid, gid) < 0)
    fprintf(stderr, "%s: cannot give %s to %u: %s\n", me, dir, (unsigned)uid, strerror(errno));
}

int
main(int argc, char *argv[])
{
  me = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  int uid = -1, gid = -1, makehome = 0, removehome = 0, lock = 0, unlock = 0;
  const char *group = 0, *groups = 0, *home = 0, *shell = 0, *newname = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
      uid = atoi(argv[++i]);
    else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
      group = argv[++i];
    else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc)
      groups = argv[++i];
    else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
      home = argv[++i];
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
      shell = argv[++i];
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      newname = argv[++i];
    else if (strcmp(argv[i], "-m") == 0)
      makehome = 1;
    else if (strcmp(argv[i], "-r") == 0)
      removehome = 1;
    else if (strcmp(argv[i], "-L") == 0)
      lock = 1;
    else if (strcmp(argv[i], "-U") == 0)
      unlock = 1;
    else
      die("unknown option %s", argv[i]);
  }
  if (i >= argc)
    die("a name is needed");
  const char *name = argv[i];

  if (geteuid() != 0)
    die("only root may change accounts");

  if (strcmp(me, "groupadd") == 0) {
    if (getgrnam(name))
      die("group %s exists", name);
    if (gid < 0)
      gid = free_gid(1000);
    char line[256];
    snprintf(line, sizeof(line), "%s:x:%u:", name, (unsigned)gid);
    rewrite(GROUP, name, line, 1, 0644);
    exit(0);
  }
  if (strcmp(me, "groupdel") == 0) {
    if (getgrnam(name) == 0)
      die("group %s does not exist", name);
    rewrite(GROUP, name, 0, 0, 0644);
    exit(0);
  }
  if (strcmp(me, "groupmod") == 0) {
    struct group *g = getgrnam(name);
    if (g == 0)
      die("group %s does not exist", name);
    char members[256];
    members[0] = 0;
    for (char **m = g->gr_mem; *m; m++) {
      if (members[0])
        strcat(members, ",");
      strcat(members, *m);
    }
    unsigned newgid = gid >= 0 ? (unsigned)gid : (unsigned)g->gr_gid;
    char line[512];
    snprintf(line, sizeof(line), "%s:x:%u:%s", newname ? newname : name, newgid, members);
    rewrite(GROUP, name, line, 0, 0644);
    exit(0);
  }

  if (strcmp(me, "userdel") == 0) {
    struct passwd *pw = getpwnam(name);
    if (pw == 0)
      die("user %s does not exist", name);
    char dir[128];
    snprintf(dir, sizeof(dir), "%s", pw->pw_dir);
    rewrite(PASSWD, name, 0, 0, 0644);
    rewrite(SHADOW, name, 0, 0, 0600);
    set_supplementary(name, "");
    if (removehome && dir[0] == '/' && strcmp(dir, "/") != 0) {
      char cmd[256];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
      system(cmd);
    }
    exit(0);
  }

  if (strcmp(me, "usermod") == 0) {
    struct passwd *pw = getpwnam(name);
    if (pw == 0)
      die("user %s does not exist", name);
    unsigned newuid = uid >= 0 ? (unsigned)uid : (unsigned)pw->pw_uid;
    unsigned newgid = (unsigned)pw->pw_gid;
    if (group) {
      struct group *g = getgrnam(group);
      if (g == 0)
        die("group %s does not exist", group);
      newgid = (unsigned)g->gr_gid;
    }
    char line[512];
    snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s", name, newuid, newgid, pw->pw_gecos,
             home ? home : pw->pw_dir, shell ? shell : pw->pw_shell);
    rewrite(PASSWD, name, line, 0, 0644);
    if (groups)
      set_supplementary(name, groups);
    if (lock || unlock) {
      struct spwd *sp = getspnam(name);
      if (sp) {
        char hash[256], sline[512];
        if (lock)
          snprintf(hash, sizeof(hash), "!%s", sp->sp_pwdp[0] == '!' ? sp->sp_pwdp + 1 : sp->sp_pwdp);
        else
          snprintf(hash, sizeof(hash), "%s",
                   sp->sp_pwdp[0] == '!' ? sp->sp_pwdp + 1 : sp->sp_pwdp);
        snprintf(sline, sizeof(sline), "%s:%s:19000:0:99999:7:::", name, hash);
        rewrite(SHADOW, name, sline, 0, 0600);
      }
    }
    exit(0);
  }

  // useradd
  if (getpwnam(name))
    die("user %s exists", name);
  if (uid < 0)
    uid = free_uid(1000);
  if (gid < 0) {
    if (group) {
      struct group *g = getgrnam(group);
      if (g == 0)
        die("group %s does not exist", group);
      gid = (int)g->gr_gid;
    } else {
      // a group of the user's own, as every unix makes
      gid = free_gid(uid);
      char gline[256];
      snprintf(gline, sizeof(gline), "%s:x:%u:", name, (unsigned)gid);
      rewrite(GROUP, name, gline, 1, 0644);
    }
  }
  char dir[128];
  snprintf(dir, sizeof(dir), "%s", home ? home : "");
  if (dir[0] == 0)
    snprintf(dir, sizeof(dir), "/home/%s", name);
  char line[512];
  snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s", name, (unsigned)uid, (unsigned)gid, name,
           dir, shell ? shell : "/bin/sh");
  rewrite(PASSWD, name, line, 1, 0644);
  // an account with no password is one nobody can log into until
  // passwd is run for it, which is what "!" means
  char sline[512];
  snprintf(sline, sizeof(sline), "%s:!:19000:0:99999:7:::", name);
  rewrite(SHADOW, name, sline, 1, 0600);
  if (groups)
    set_supplementary(name, groups);
  if (makehome)
    make_home(dir, (uid_t)uid, (gid_t)gid);
  exit(0);
}
