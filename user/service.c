//
// service NAME ACTION, and update-rc.d.
//
//   service NAME start|stop|restart|reload|force-reload|status
//   service --status-all
//
//   update-rc.d NAME defaults|remove|enable|disable
//
// a service is /etc/init.d/NAME; the runlevel links in /etc/rc.d/rcN.d
// say which ones start and in what order, and update-rc.d makes and
// removes them. the order comes from the script's LSB header: a
// service that requires another starts after it.
//
#include "user/user.h"

#define INITD "/etc/init.d"
#define RCDIR "/etc/rc.d"

static const char *me;

static int
run_script(const char *name, const char *action)
{
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", INITD, name);
  if (access(path, X_OK) < 0) {
    fprintf(stderr, "%s: %s: no such service\n", me, name);
    return 1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "%s: fork: %s\n", me, strerror(errno));
    return 1;
  }
  if (pid == 0) {
    char *argv[] = {(char *)path, (char *)action, 0};
    execv(path, argv);
    fprintf(stderr, "%s: %s: %s\n", me, path, strerror(errno));
    exit(1);
  }
  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// the "Required-Start:" line of a script, which says what has to be
// running before it
static void
required_start(const char *name, char *out, int max)
{
  char path[128], line[256];

  out[0] = 0;
  snprintf(path, sizeof(path), "%s/%s", INITD, name);
  FILE *f = fopen(path, "r");
  if (f == 0)
    return;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "# Required-Start:", 17) == 0) {
      char *p = line + 17;
      while (*p == ' ' || *p == '\t')
        p++;
      int n = strlen(p);
      while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r'))
        p[--n] = 0;
      snprintf(out, max, "%s", p);
      break;
    }
    if (strncmp(line, "### END INIT INFO", 17) == 0)
      break;
  }
  fclose(f);
}

// the order number a service gets: one more than the highest of what
// it requires, so a topological order comes out of a simple pass
static int
order_of(const char *name, int depth)
{
  char req[256];
  int order = 10;

  if (depth > 8)
    return order;
  required_start(name, req, sizeof(req));
  for (char *p = req; p && *p;) {
    char *space = strchr(p, ' ');
    if (space)
      *space = 0;
    if (*p) {
      int o = order_of(p, depth + 1) + 10;
      if (o > order)
        order = o;
    }
    p = space ? space + 1 : 0;
  }
  return order > 99 ? 99 : order;
}

static int
update_rcd(const char *name, const char *what)
{
  char script[128], link[128];

  snprintf(script, sizeof(script), "%s/%s", INITD, name);
  if (access(script, X_OK) < 0) {
    fprintf(stderr, "%s: %s: no such service\n", me, name);
    return 1;
  }

  // every runlevel link this service might have
  for (int level = 0; level <= 6; level++) {
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/rc%d.d", RCDIR, level);
    DIR *d = opendir(dir);
    if (d == 0)
      continue;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      const char *p = e->d_name;
      if ((p[0] != 'S' && p[0] != 'K') || !isdigit((unsigned char)p[1]))
        continue;
      const char *base = p + 1;
      while (isdigit((unsigned char)*base))
        base++;
      if (strcmp(base, name) != 0)
        continue;
      snprintf(link, sizeof(link), "%s/%s", dir, e->d_name);
      if (strcmp(what, "remove") == 0 || strcmp(what, "defaults") == 0 ||
          strcmp(what, "disable") == 0 || strcmp(what, "enable") == 0)
        unlink(link);
    }
    closedir(d);
  }
  if (strcmp(what, "remove") == 0 || strcmp(what, "disable") == 0)
    return 0;

  int order = order_of(name, 0);
  char dir[128];
  snprintf(dir, sizeof(dir), "%s/rc2.d", RCDIR);
  mkdir(RCDIR, 0755);
  mkdir(dir, 0755);
  snprintf(link, sizeof(link), "%s/S%02d%s", dir, order, name);
  char target[128];
  snprintf(target, sizeof(target), "../../init.d/%s", name);
  if (symlink(target, link) < 0 && errno != EEXIST) {
    fprintf(stderr, "%s: %s: %s\n", me, link, strerror(errno));
    return 1;
  }
  printf("%s: %s starts at %s\n", me, name, link);
  return 0;
}

static void
status_all(void)
{
  DIR *d = opendir(INITD);
  struct dirent *e;

  if (d == 0) {
    fprintf(stderr, "%s: %s: %s\n", me, INITD, strerror(errno));
    exit(1);
  }
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.' || strcmp(e->d_name, "rcS") == 0)
      continue;
    run_script(e->d_name, "status");
  }
  closedir(d);
}

int
main(int argc, char *argv[])
{
  me = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  if (strcmp(me, "update-rc.d") == 0) {
    if (argc < 3) {
      fprintf(stderr, "usage: update-rc.d NAME defaults|remove|enable|disable\n");
      exit(1);
    }
    exit(update_rcd(argv[1], argv[2]));
  }

  if (argc == 2 && strcmp(argv[1], "--status-all") == 0) {
    status_all();
    exit(0);
  }
  if (argc < 3) {
    fprintf(stderr, "usage: service NAME start|stop|restart|reload|status\n");
    exit(1);
  }
  exit(run_script(argv[1], argv[2]));
}
