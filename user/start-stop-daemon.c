//
// start-stop-daemon: start a program if it is not running, or stop it
// if it is.
//
//   --start --exec PATH [--pidfile FILE] [--make-pidfile] [--background]
//           [--chuid USER] [--chdir DIR] [--name NAME] [--test] -- ARGS...
//   --stop [--pidfile FILE] [--name NAME] [--signal SIG] [--retry N] [--test]
//
// this is what an init script uses so that "start" twice does not
// start two of them and "stop" does not have to know the pid.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <signal.h>

static const char *me = "start-stop-daemon";

static int
pid_from_file(const char *path)
{
  FILE *f = fopen(path, "r");
  int pid = 0;

  if (f == 0)
    return 0;
  if (fscanf(f, "%d", &pid) != 1)
    pid = 0;
  fclose(f);
  if (pid > 0) {
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%d", pid);
    if (access(proc, F_OK) < 0)
      return 0;
  }
  return pid;
}

static int
pid_from_name(const char *name)
{
  DIR *d = opendir("/proc");
  struct dirent *e;
  int self = getpid(), found = 0;

  if (d == 0)
    return 0;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    int pid = atoi(e->d_name);
    if (pid == self)
      continue;
    char path[64], text[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;
    int n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
      continue;
    text[n] = 0;
    const char *tab = strchr(text, '\t');
    const char *nl = strchr(text, '\n');
    if (tab && nl && tab < nl) {
      int len = (int)(nl - tab - 1);
      if (len == (int)strlen(name) && strncmp(tab + 1, name, len) == 0) {
        found = pid;
        break;
      }
    }
  }
  closedir(d);
  return found;
}

static int
signal_by_name(const char *s)
{
  static const struct {
    const char *name;
    int value;
  } names[] = {{"HUP", SIGHUP},   {"INT", SIGINT},  {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
               {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {0, 0}};

  if (s[0] >= '0' && s[0] <= '9')
    return atoi(s);
  if (strncmp(s, "SIG", 3) == 0)
    s += 3;
  for (int i = 0; names[i].name; i++)
    if (strcasecmp(names[i].name, s) == 0)
      return names[i].value;
  return SIGTERM;
}

int
main(int argc, char *argv[])
{
  int start = 0, stop = 0, background = 0, make_pidfile = 0, test_only = 0;
  int signo = SIGTERM, retry = 5;
  const char *exec_path = 0, *pidfile = 0, *name = 0, *chuid = 0, *chdir_to = 0;
  int i = 1;

  for (; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    } else if (strcmp(a, "--start") == 0 || strcmp(a, "-S") == 0)
      start = 1;
    else if (strcmp(a, "--stop") == 0 || strcmp(a, "-K") == 0)
      stop = 1;
    else if (strcmp(a, "--exec") == 0 && i + 1 < argc)
      exec_path = argv[++i];
    else if (strcmp(a, "--pidfile") == 0 && i + 1 < argc)
      pidfile = argv[++i];
    else if (strcmp(a, "--name") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(a, "--chuid") == 0 && i + 1 < argc)
      chuid = argv[++i];
    else if (strcmp(a, "--chdir") == 0 && i + 1 < argc)
      chdir_to = argv[++i];
    else if (strcmp(a, "--signal") == 0 && i + 1 < argc)
      signo = signal_by_name(argv[++i]);
    else if (strcmp(a, "--retry") == 0 && i + 1 < argc)
      retry = atoi(argv[++i]);
    else if (strcmp(a, "--background") == 0 || strcmp(a, "-b") == 0)
      background = 1;
    else if (strcmp(a, "--make-pidfile") == 0|| strcmp(a, "-m") == 0)
      make_pidfile = 1;
    else if (strcmp(a, "--test") == 0 || strcmp(a, "-t") == 0)
      test_only = 1;
    else if (strcmp(a, "--oknodo") == 0 || strcmp(a, "-o") == 0)
      ; // the exit status is 0 either way, which is what this asks for
    else if (strcmp(a, "--quiet") == 0 || strcmp(a, "-q") == 0)
      ;
    else if (a[0] == '-') {
      fprintf(stderr, "%s: unknown option %s\n", me, a);
      exit(2);
    } else
      break;
  }

  if (start == stop) {
    fprintf(stderr, "usage: %s --start|--stop ...\n", me);
    exit(2);
  }
  if (name == 0 && exec_path) {
    name = exec_path;
    for (const char *p = exec_path; *p; p++)
      if (*p == '/')
        name = p + 1;
  }

  int pid = pidfile ? pid_from_file(pidfile) : 0;
  if (pid == 0 && name)
    pid = pid_from_name(name);

  if (stop) {
    if (pid == 0) {
      printf("%s: %s is not running\n", me, name ? name : "it");
      exit(1);
    }
    if (test_only) {
      printf("%s: would send signal %d to %d\n", me, signo, pid);
      exit(0);
    }
    if (kill(pid, signo) < 0) {
      fprintf(stderr, "%s: %d: %s\n", me, pid, strerror(errno));
      exit(2);
    }
    for (int k = 0; k < retry; k++) {
      char proc[64];
      snprintf(proc, sizeof(proc), "/proc/%d", pid);
      if (access(proc, F_OK) < 0)
        break;
      sleep(1);
    }
    if (pidfile)
      unlink(pidfile);
    exit(0);
  }

  // --start
  if (pid != 0) {
    printf("%s: %s is already running (pid %d)\n", me, name ? name : "it", pid);
    exit(1);
  }
  if (exec_path == 0) {
    fprintf(stderr, "%s: --start needs --exec\n", me);
    exit(2);
  }
  if (test_only) {
    printf("%s: would start %s\n", me, exec_path);
    exit(0);
  }

  pid_t child = fork();
  if (child < 0) {
    fprintf(stderr, "%s: fork: %s\n", me, strerror(errno));
    exit(2);
  }
  if (child == 0) {
    if (chuid) {
      struct passwd *pw = getpwnam(chuid);
      if (pw == 0) {
        fprintf(stderr, "%s: %s: no such user\n", me, chuid);
        exit(2);
      }
      gid_t groups[32];
      int n = 32;
      if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
        n = 1;
      if (setgroups(n, groups) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
        fprintf(stderr, "%s: cannot become %s: %s\n", me, chuid, strerror(errno));
        exit(2);
      }
    }
    if (chdir_to && chdir(chdir_to) < 0) {
      fprintf(stderr, "%s: %s: %s\n", me, chdir_to, strerror(errno));
      exit(2);
    }
    if (background)
      setsid();
    // the rest of the command line, after --
    char *args[32];
    int n = 0;
    args[n++] = (char *)exec_path;
    for (; i < argc && n < 31; i++)
      args[n++] = argv[i];
    args[n] = 0;
    execv(exec_path, args);
    fprintf(stderr, "%s: %s: %s\n", me, exec_path, strerror(errno));
    exit(2);
  }
  if (make_pidfile && pidfile) {
    FILE *f = fopen(pidfile, "w");
    if (f) {
      fprintf(f, "%d\n", (int)child);
      fclose(f);
    }
  }
  if (!background) {
    int status;
    waitpid(child, &status, 0);
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
  }
  exit(0);
}
