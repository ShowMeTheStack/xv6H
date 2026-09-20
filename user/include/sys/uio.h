#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/posix.h" // struct iovec
ssize_t readv(int fd, const struct iovec *iov, int n);
ssize_t writev(int fd, const struct iovec *iov, int n);
ssize_t preadv(int fd, const struct iovec *iov, int n, off_t off);
ssize_t pwritev(int fd, const struct iovec *iov, int n, off_t off);
#endif
