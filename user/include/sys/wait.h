#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/signal.h" // WIFEXITED and friends, WNOHANG, WUNTRACED
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait3(int *status, int options, void *rusage);
pid_t wait4(pid_t pid, int *status, int options, void *rusage);
#endif
