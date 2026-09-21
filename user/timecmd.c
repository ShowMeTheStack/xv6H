// time, timeout and nohup: three ways of running a command.
//
//   time [-p] COMMAND [ARG...]        how long it took
//   timeout [-s SIG] [-k SEC] SEC COMMAND [ARG...]   stop it if it runs on
//   nohup COMMAND [ARG...]            let it outlive the terminal
//
// all three run the command and hand back what it says, which is why
// they are one program: the difference is what happens around the
// wait.
#include "user/user.h"
#include <sys/times.h>

static const char *me = "time";

// the signals timeout knows by name
static int
signal_by_name(const char *s)
{
  static const struct {
    const char *name;
    int sig;
  } names[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
    {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
    {"ALRM", SIGALRM}, {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {0, 0}
  };

  if (*s >= '0' && *s <= '9')
    return atoi(s);
  if (strncasecmp(s, "SIG", 3) == 0)
    s += 3;
  for (int i = 0; names[i].name; i++)
    if (strcasecmp(s, names[i].name) == 0)
      return names[i].sig;
  return -1;
}

static void
print_time(const char *what, uint64 ticks_used, int posix)
{
  // the clock runs at a hundred ticks a second
  long long ms = (long long)ticks_used * 10;

  if (posix)
    printf("%s %lld.%02lld\n", what, ms / 1000, (ms % 1000) / 10);
  else
    printf("%s\t%lldm%lld.%03llds\n", what, ms / 60000, (ms % 60000) / 1000,
           ms % 1000);
}

static int
run_time(int argc, char *argv[], int i)
{
  int posix = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-p") == 0)
      posix = 1;
    else if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    } else {
      fprintf(stderr, "usage: time [-p] COMMAND [ARG...]\n");
      return 1;
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: time [-p] COMMAND [ARG...]\n");
    return 1;
  }

  struct tms before, after;
  uint64 start = times(&before);
  int pid = fork();
  if (pid < 0) {
    fprintf(stderr, "time: fork: %s\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    execvp(argv[i], argv + i);
    fprintf(stderr, "time: %s: %s\n", argv[i], strerror(errno));
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  uint64 end = times(&after);

  // the child's own time is in the c* fields once it has been waited for
  print_time(posix ? "real" : "real", end - start, posix);
  print_time("user", after.tms_cutime - before.tms_cutime, posix);
  print_time("sys", after.tms_cstime - before.tms_cstime, posix);
  if (WIFSIGNALED(st))
    return 128 + WTERMSIG(st);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static int child_pid;
static int timed_out;

static void
on_alarm(int sig)
{
  (void)sig;
  timed_out = 1;
}

static int
run_timeout(int argc, char *argv[], int i)
{
  int sig = SIGTERM, kill_after = 0, foreground = 0;
  long long seconds;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv[i], "--foreground") == 0) {
      foreground = 1;
      continue;
    }
    if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--signal") == 0) &&
        i + 1 < argc) {
      sig = signal_by_name(argv[++i]);
      if (sig < 0) {
        fprintf(stderr, "timeout: unknown signal\n");
        return 125;
      }
      continue;
    }
    if ((strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--kill-after") == 0) &&
        i + 1 < argc) {
      kill_after = atoi(argv[++i]);
      continue;
    }
    fprintf(stderr, "usage: timeout [-s SIG] [-k SEC] SECONDS COMMAND"
                    " [ARG...]\n");
    return 125;
  }
  if (i + 1 >= argc) {
    fprintf(stderr, "usage: timeout [-s SIG] [-k SEC] SECONDS COMMAND"
                    " [ARG...]\n");
    return 125;
  }
  seconds = atoll(argv[i++]);
  (void)foreground;

  child_pid = fork();
  if (child_pid < 0) {
    fprintf(stderr, "timeout: fork: %s\n", strerror(errno));
    return 125;
  }
  if (child_pid == 0) {
    execvp(argv[i], argv + i);
    fprintf(stderr, "timeout: %s: %s\n", argv[i], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }

  // signal(3) asks for SA_RESTART, which would put waitpid straight
  // back to sleep after the alarm; this handler must interrupt it
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_alarm;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, 0);
  alarm((unsigned)seconds);

  int st = 0;
  for (;;) {
    int r = waitpid(child_pid, &st, 0);
    if (r == child_pid)
      break;
    if (r < 0 && errno == EINTR) {
      if (timed_out) {
        kill(child_pid, sig);
        timed_out = 0;
        if (kill_after > 0) {
          alarm((unsigned)kill_after);
          sig = SIGKILL;
          continue;
        }
        // wait for it to go
        continue;
      }
      continue;
    }
    if (r < 0)
      break;
  }
  alarm(0);
  if (WIFSIGNALED(st)) {
    int s = WTERMSIG(st);
    if (s == SIGTERM || s == SIGKILL)
      return 124;   // what timeout returns when it had to step in
    return 128 + s;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static int
run_nohup(int argc, char *argv[], int i)
{
  if (i < argc && strcmp(argv[i], "--") == 0)
    i++;
  if (i >= argc) {
    fprintf(stderr, "usage: nohup COMMAND [ARG...]\n");
    return 127;
  }

  signal(SIGHUP, SIG_IGN);

  // with a terminal on standard output, what it writes goes to a file
  // instead, so that nothing is lost when the terminal goes
  if (isatty(1)) {
    const char *name = "nohup.out";
    int fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
      const char *home = getenv("HOME");
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/nohup.out", home ? home : "/tmp");
      fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
      name = path;
    }
    if (fd < 0) {
      fprintf(stderr, "nohup: cannot open nohup.out: %s\n", strerror(errno));
      return 127;
    }
    fprintf(stderr, "nohup: ignoring input and appending output to '%s'\n",
            name);
    dup2(fd, 1);
    if (isatty(2))
      dup2(fd, 2);
    if (fd > 2)
      close(fd);
  } else if (isatty(2)) {
    dup2(1, 2);
  }
  if (isatty(0)) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
      dup2(fd, 0);
      if (fd > 2)
        close(fd);
    }
  }

  execvp(argv[i], argv + i);
  fprintf(stderr, "nohup: %s: %s\n", argv[i], strerror(errno));
  return errno == ENOENT ? 127 : 126;
}

int
main(int argc, char *argv[])
{
  me = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  if (strcmp(me, "timeout") == 0)
    exit(run_timeout(argc, argv, 1));
  if (strcmp(me, "nohup") == 0)
    exit(run_nohup(argc, argv, 1));
  exit(run_time(argc, argv, 1));
}
