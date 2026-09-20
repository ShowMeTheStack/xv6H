// kill [-s SIGNAL | -SIGNAL] pid...
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/signal.h"
#include "user/user.h"

static const struct {
  const char *name;
  int sig;
} names[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ILL", SIGILL},   {"TRAP", SIGTRAP},
    {"ABRT", SIGABRT}, {"BUS", SIGBUS},   {"FPE", SIGFPE},   {"KILL", SIGKILL}, {"USR1", SIGUSR1},
    {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
    {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
    {"TTOU", SIGTTOU}, {"WINCH", SIGWINCH}, {0, 0},
};

static int
parse_sig(char *s)
{
  if (*s >= '0' && *s <= '9')
    return atoi(s);
  if (strcmp(s, "SIG") > 0 && memcmp(s, "SIG", 3) == 0)
    s += 3;
  for (int i = 0; names[i].name; i++)
    if (strcmp(names[i].name, s) == 0)
      return names[i].sig;
  return -1;
}

int
main(int argc, char **argv)
{
  int sig = SIGTERM, i = 1;

  if (argc > 2 && strcmp(argv[1], "-s") == 0) {
    sig = parse_sig(argv[2]);
    i = 3;
  } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] != 0) {
    sig = parse_sig(argv[1] + 1);
    i = 2;
  }
  if (sig < 0 || i >= argc) {
    fprintf(stderr, "usage: kill [-s SIGNAL] pid...\n");
    exit(1);
  }
  int bad = 0;
  for (; i < argc; i++) {
    if (kill(atoi(argv[i]), sig) < 0) {
      fprintf(stderr, "kill: %s: no such process\n", argv[i]);
      bad = 1;
    }
  }
  exit(bad);
}
