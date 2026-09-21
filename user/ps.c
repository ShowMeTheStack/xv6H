//
// ps: what is running.
//
//   ps            this terminal's processes
//   ps -e, ps ax  every process
//   ps -f, ps u   the long form (user, times, command)
//   ps -u USER    one user's
//   ps -o LIST    the columns to print
//
// everything comes from /proc: one directory per process, with the
// numbers in status and stat.
//
#include "user/user.h"
#include <pwd.h>

struct entry {
  int pid, ppid, pgid, sid, tty, threads;
  unsigned uid, euid, gid;
  long utime, stime, vmkb;
  char state[8];
  char name[64];
};

static struct entry procs[1024];
static int nprocs;
static int longform, everyone, filter_uid = (unsigned)-1;
static const char *columns;

static long
readfield(const char *text, const char *key)
{
  const char *p = text;
  int n = strlen(key);

  while (*p) {
    if (strncmp(p, key, n) == 0 && p[n] == ':')
      return atol(p + n + 1);
    while (*p && *p != '\n')
      p++;
    if (*p)
      p++;
  }
  return -1;
}

static int
read_entry(int pid, struct entry *e)
{
  char path[64], text[1024];
  int fd, n;

  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  if ((fd = open(path, O_RDONLY)) < 0)
    return -1;
  n = read(fd, text, sizeof(text) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  text[n] = 0;

  memset(e, 0, sizeof(*e));
  e->pid = pid;
  // the name is the first line: "Name:\tsh"
  const char *nl = strchr(text, '\n');
  const char *tab = strchr(text, '\t');
  if (tab && nl && tab < nl) {
    int len = (int)(nl - tab - 1);
    if (len > (int)sizeof(e->name) - 1)
      len = sizeof(e->name) - 1;
    memmove(e->name, tab + 1, len);
    e->name[len] = 0;
  }
  e->ppid = (int)readfield(text, "PPid");
  e->threads = (int)readfield(text, "Threads");
  e->vmkb = readfield(text, "VmSize");
  e->tty = (int)readfield(text, "Tty");
  e->utime = readfield(text, "Utime");
  e->stime = readfield(text, "Stime");
  // "Uid:\treal\teffective"
  const char *u = strstr(text, "Uid:");
  if (u) {
    e->uid = (unsigned)atol(u + 4);
    const char *second = strchr(u + 4, '\t');
    second = second ? strchr(second + 1, '\t') : 0;
    e->euid = second ? (unsigned)atol(second + 1) : e->uid;
  }
  const char *gg = strstr(text, "Gid:");
  if (gg)
    e->gid = (unsigned)atol(gg + 4);
  const char *st = strstr(text, "State:");
  if (st) {
    const char *p = st + 6;
    while (*p == '\t' || *p == ' ')
      p++;
    e->state[0] = *p;
    e->state[1] = 0;
  }
  return 0;
}

static const char *
username(unsigned uid)
{
  struct passwd *pw = getpwuid((uid_t)uid);
  static char buf[16];

  if (pw)
    return pw->pw_name;
  snprintf(buf, sizeof(buf), "%u", uid);
  return buf;
}

static void
print_header(void)
{
  if (columns)
    return;
  if (longform)
    printf("%-8s %5s %5s %3s %5s %6s %s\n", "USER", "PID", "PPID", "TTY", "TIME", "VSZ", "CMD");
  else
    printf("%5s %3s %5s %s\n", "PID", "TTY", "TIME", "CMD");
}

static void
print_one(const struct entry *e)
{
  char ttyname[8];
  char timestr[16];
  long ticks = e->utime + e->stime;

  if (e->tty > 0)
    snprintf(ttyname, sizeof(ttyname), "%d", e->tty);
  else
    snprintf(ttyname, sizeof(ttyname), "?");
  snprintf(timestr, sizeof(timestr), "%ld:%02ld", ticks / 600, (ticks / 10) % 60);

  if (columns) {
    // -o: the named columns, in the order given
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", columns);
    int first = 1;
    for (char *p = copy; p && *p;) {
      char *comma = strchr(p, ',');
      if (comma)
        *comma = 0;
      if (!first)
        printf(" ");
      if (strcmp(p, "pid") == 0)
        printf("%d", e->pid);
      else if (strcmp(p, "ppid") == 0)
        printf("%d", e->ppid);
      else if (strcmp(p, "user") == 0)
        printf("%s", username(e->euid));
      else if (strcmp(p, "uid") == 0)
        printf("%u", e->euid);
      else if (strcmp(p, "tty") == 0)
        printf("%s", ttyname);
      else if (strcmp(p, "time") == 0)
        printf("%s", timestr);
      else if (strcmp(p, "vsz") == 0)
        printf("%ld", e->vmkb);
      else if (strcmp(p, "stat") == 0 || strcmp(p, "state") == 0)
        printf("%s", e->state);
      else if (strcmp(p, "nlwp") == 0)
        printf("%d", e->threads);
      else if (strcmp(p, "comm") == 0 || strcmp(p, "cmd") == 0 || strcmp(p, "args") == 0)
        printf("%s", e->name);
      else
        printf("-");
      first = 0;
      p = comma ? comma + 1 : 0;
    }
    printf("\n");
    return;
  }
  if (longform)
    printf("%-8s %5d %5d %3s %5s %6ld %s\n", username(e->euid), e->pid, e->ppid, ttyname,
           timestr, e->vmkb, e->name);
  else
    printf("%5d %3s %5s %s\n", e->pid, ttyname, timestr, e->name);
}

int
main(int argc, char *argv[])
{
  int mytty = -1;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-') {
      for (const char *p = a + 1; *p; p++)
        switch (*p) {
        case 'e': case 'A': everyone = 1; break;
        case 'f': case 'l': longform = 1; break;
        case 'u':
          if (i + 1 < argc && argv[i + 1][0] != '-') {
            struct passwd *pw = getpwnam(argv[++i]);
            filter_uid = pw ? pw->pw_uid : (unsigned)atol(argv[i]);
            everyone = 1;
          } else {
            longform = 1;
          }
          break;
        case 'o':
          if (i + 1 < argc)
            columns = argv[++i];
          break;
        case 'a': everyone = 1; break;
        case 'x': everyone = 1; break;
        default:
          fprintf(stderr, "usage: ps [-e] [-f] [-u USER] [-o LIST]\n");
          exit(1);
        }
    } else {
      // the BSD form: ps ax, ps aux
      for (const char *p = a; *p; p++)
        switch (*p) {
        case 'a': case 'x': everyone = 1; break;
        case 'u': longform = 1; break;
        default: break;
        }
    }
  }

  // which terminal this process is on, so that a bare ps shows its own
  {
    struct entry me;
    if (read_entry(getpid(), &me) == 0)
      mytty = me.tty;
  }

  DIR *d = opendir("/proc");
  if (d == 0) {
    fprintf(stderr, "ps: /proc: %s\n", strerror(errno));
    exit(1);
  }
  struct dirent *e;
  while ((e = readdir(d)) != 0 && nprocs < 1024) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    if (read_entry(atoi(e->d_name), &procs[nprocs]) == 0)
      nprocs++;
  }
  closedir(d);

  // in order of pid, which is the order people expect
  for (int i = 1; i < nprocs; i++) {
    struct entry t = procs[i];
    int k = i - 1;
    while (k >= 0 && procs[k].pid > t.pid) {
      procs[k + 1] = procs[k];
      k--;
    }
    procs[k + 1] = t;
  }

  print_header();
  for (int i = 0; i < nprocs; i++) {
    if (filter_uid != (unsigned)-1 && procs[i].euid != filter_uid)
      continue;
    if (!everyone && filter_uid == (unsigned)-1 && procs[i].tty != mytty)
      continue;
    print_one(&procs[i]);
  }
  exit(0);
}
