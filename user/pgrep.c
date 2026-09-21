//
// pgrep, pkill and killall: processes by name.
//
//   pgrep [-l] [-u USER] PATTERN     print the pids
//   pkill [-SIGNAL] [-u USER] PATTERN
//   killall [-SIGNAL] NAME           an exact name, as killall means
//
// the pattern is a substring for pgrep and pkill, as they are
// everywhere, and an exact name for killall.
//
#include "user/user.h"
#include <pwd.h>
#include <signal.h>

static int list_names, signo = SIGTERM, exact;
static unsigned only_uid = (unsigned)-1;

static int
name_of(int pid, char *out, int max, unsigned *uid)
{
  char path[64], text[512];
  int fd, n;

  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  if ((fd = open(path, O_RDONLY)) < 0)
    return -1;
  n = read(fd, text, sizeof(text) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  text[n] = 0;
  const char *tab = strchr(text, '\t');
  const char *nl = strchr(text, '\n');
  if (tab == 0 || nl == 0 || tab > nl)
    return -1;
  int len = (int)(nl - tab - 1);
  if (len > max - 1)
    len = max - 1;
  memmove(out, tab + 1, len);
  out[len] = 0;
  if (uid) {
    const char *u = strstr(text, "Uid:");
    *uid = u ? (unsigned)atol(u + 4) : 0;
  }
  return 0;
}

static int
signal_by_name(const char *s)
{
  static const struct {
    const char *name;
    int value;
  } names[] = {{"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
               {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"STOP", SIGSTOP},
               {"CONT", SIGCONT}, {0, 0}};

  if (s[0] >= '0' && s[0] <= '9')
    return atoi(s);
  if (strncmp(s, "SIG", 3) == 0)
    s += 3;
  for (int i = 0; names[i].name; i++)
    if (strcasecmp(names[i].name, s) == 0)
      return names[i].value;
  return -1;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0], *pattern = 0;
  int killing = 0, i = 1, found = 0;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  killing = strcmp(me, "pkill") == 0 || strcmp(me, "killall") == 0;
  exact = strcmp(me, "killall") == 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-l") == 0)
      list_names = 1;
    else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
      struct passwd *pw = getpwnam(argv[++i]);
      only_uid = pw ? pw->pw_uid : (unsigned)atol(argv[i]);
    } else if (killing) {
      int s = signal_by_name(argv[i] + 1);
      if (s < 0) {
        fprintf(stderr, "%s: unknown signal %s\n", me, argv[i]);
        exit(1);
      }
      signo = s;
    } else {
      fprintf(stderr, "usage: %s [-l] [-u USER] PATTERN\n", me);
      exit(1);
    }
  }
  if (i < argc)
    pattern = argv[i];
  if (pattern == 0 && only_uid == (unsigned)-1) {
    fprintf(stderr, "usage: %s [-l] [-u USER] PATTERN\n", me);
    exit(1);
  }

  DIR *d = opendir("/proc");
  if (d == 0) {
    fprintf(stderr, "%s: /proc: %s\n", me, strerror(errno));
    exit(1);
  }
  struct dirent *e;
  int self = getpid();
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    int pid = atoi(e->d_name);
    if (pid == self)
      continue;
    char name[64];
    unsigned uid;
    if (name_of(pid, name, sizeof(name), &uid) < 0)
      continue;
    if (only_uid != (unsigned)-1 && uid != only_uid)
      continue;
    if (pattern && (exact ? strcmp(name, pattern) != 0 : strstr(name, pattern) == 0))
      continue;
    found++;
    if (killing) {
      if (kill(pid, signo) < 0)
        fprintf(stderr, "%s: %d: %s\n", me, pid, strerror(errno));
    } else if (list_names)
      printf("%d %s\n", pid, name);
    else
      printf("%d\n", pid);
  }
  closedir(d);
  exit(found ? 0 : 1);
}
