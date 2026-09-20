#ifndef _POLL_H
#define _POLL_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/posix.h" // struct pollfd and the POLL* bits
typedef unsigned long nfds_t;
int poll(struct pollfd *fds, nfds_t n, int timeout);
#endif
