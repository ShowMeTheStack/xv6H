#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/posix.h" // fd_set and the FD_* macros, struct timeval
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *t);
#endif
