#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
#include <sys/types.h>
#include "kernel/posix.h" // struct statfs

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
