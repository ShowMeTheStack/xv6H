#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/signal.h" // the numbers, sigset_t, struct sigaction, siginfo_t

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

int kill(pid_t pid, int sig);
int killpg(pid_t pgid, int sig);
int raise(int sig);
int tkill(pid_t tid, int sig);
int tgkill(pid_t tgid, pid_t tid, int sig);
sighandler_t signal(int sig, sighandler_t handler);
sighandler_t bsd_signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);
int sigwait(const sigset_t *set, int *sig);
int sigaltstack(const stack_t *ss, stack_t *old);
int sigqueue(pid_t pid, int sig, int value);
int siginterrupt(int sig, int flag);
int sigemptyset(sigset_t *s);
int sigfillset(sigset_t *s);
int sigaddset(sigset_t *s, int sig);
int sigdelset(sigset_t *s, int sig);
int sigismember(const sigset_t *s, int sig);
int sigisemptyset(const sigset_t *s);
void psignal(int sig, const char *prefix);
char *strsignal(int sig);
extern const char *const sys_siglist[];
#endif
