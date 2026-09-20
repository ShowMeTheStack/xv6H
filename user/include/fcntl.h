#ifndef _FCNTL_H
#define _FCNTL_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/fcntl.h"  // the O_* flags
#include "kernel/posix.h"  // F_*, FD_CLOEXEC, SEEK_*
int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);
#define AT_FDCWD (-100)
#endif
