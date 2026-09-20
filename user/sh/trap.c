//
// traps.
//
// a handler set with trap(1) cannot run inside the signal handler: the
// shell would have to be re-entrant, and it is not. the handler here
// only records that the signal arrived; the shell runs the action at
// the next safe point, which is between commands and after any system
// call that a signal interrupted.
//
#include "sh.h"

static char *actions[NSIG];
static volatile int pending[NSIG];
static int exit_running;

static const struct {
  const char *name;
  int sig;
} signames[] = {
    {"HUP", SIGHUP},     {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ILL", SIGILL},
    {"TRAP", SIGTRAP},   {"ABRT", SIGABRT}, {"BUS", SIGBUS},   {"FPE", SIGFPE},
    {"KILL", SIGKILL},   {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2},
    {"PIPE", SIGPIPE},   {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD},
    {"CONT", SIGCONT},   {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
    {"TTOU", SIGTTOU},   {"URG", SIGURG},   {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ},
    {"VTALRM", SIGVTALRM}, {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"SYS", SIGSYS},
    {"EXIT", 0},         {0, 0},
};

int
trap_signal_number(const char *name)
{
  if (isdigit((unsigned char)name[0]))
    return atoi(name);
  if (strncasecmp(name, "SIG", 3) == 0)
    name += 3;
  for (int i = 0; signames[i].name; i++)
    if (strcasecmp(name, signames[i].name) == 0)
      return signames[i].sig;
  return -1;
}

static const char *
signal_name(int sig)
{
  for (int i = 0; signames[i].name; i++)
    if (signames[i].sig == sig)
      return signames[i].name;
  return "?";
}

static void
handler(int sig)
{
  if (sig >= 0 && sig < NSIG)
    pending[sig] = 1;
}

void
trap_set(int sig, const char *action)
{
  if (sig < 0 || sig >= NSIG)
    return;
  free(actions[sig]);
  actions[sig] = 0;

  if (action == 0) { // back to what it was when the shell started
    if (sig > 0)
      signal(sig, SIG_DFL);
    return;
  }
  actions[sig] = xstrdup(action);
  if (sig == 0)
    return; // EXIT is not a signal
  if (*action == 0)
    signal(sig, SIG_IGN);
  else
    signal(sig, handler);
}

const char *
trap_get(int sig)
{
  return (sig >= 0 && sig < NSIG) ? actions[sig] : 0;
}

void
trap_list(void)
{
  for (int sig = 0; sig < NSIG; sig++) {
    if (actions[sig] == 0)
      continue;
    if (*actions[sig])
      printf("trap -- '%s' %s\n", actions[sig], sig ? signal_name(sig) : "EXIT");
    else
      printf("trap -- '' %s\n", sig ? signal_name(sig) : "EXIT");
  }
}

// run whatever arrived while we were busy
void
trap_run_pending(void)
{
  for (int sig = 1; sig < NSIG; sig++) {
    if (!pending[sig])
      continue;
    pending[sig] = 0;
    char *action = actions[sig];
    if (action && *action) {
      int saved = last_status;
      run_string(action, 1);
      last_status = saved;
    }
  }
}

void
trap_on_exit(void)
{
  if (exit_running)
    return;
  exit_running = 1;
  if (actions[0] && *actions[0]) {
    int saved = last_status;
    run_string(actions[0], 1);
    last_status = saved;
  }
}

// a child starts with the traps the shell was given, not the shell's
// own handlers: an ignored signal stays ignored, everything else goes
// back to its default.
void
trap_reset_for_child(void)
{
  for (int sig = 1; sig < NSIG; sig++) {
    if (actions[sig] && *actions[sig] == 0)
      continue; // ignored, and stays ignored
    signal(sig, SIG_DFL);
  }
  for (int sig = 0; sig < NSIG; sig++) {
    free(actions[sig]);
    actions[sig] = 0;
  }
}
