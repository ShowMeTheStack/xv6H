#ifndef XV6_POSIX_H
#define XV6_POSIX_H
#include "types.h"
//
// structures and constants shared by the kernel and user programs for
// the POSIX-shaped system calls (iovec, pollfd, select, utsname, tms,
// rusage, fcntl, ioctl requests, the auxiliary vector).
//

struct iovec {
  void *iov_base;
  uint64 iov_len;
};

// poll
struct pollfd {
  int fd;
  short events;
  short revents;
};
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

// select: FD_SETSIZE descriptors in 16 words
#define FD_SETSIZE 1024
typedef struct {
  uint64 bits[FD_SETSIZE / 64];
} fd_set;
#define FD_ZERO(s)       memset((s), 0, sizeof(fd_set))
#define FD_SET(fd, s)    ((s)->bits[(fd) / 64] |= 1UL << ((fd) % 64))
#define FD_CLR(fd, s)    ((s)->bits[(fd) / 64] &= ~(1UL << ((fd) % 64)))
#define FD_ISSET(fd, s)  (((s)->bits[(fd) / 64] >> ((fd) % 64)) & 1)

struct timeval {
  long tv_sec;
  long tv_usec;
};

// uname
struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
};

// times, getrusage
struct tms {
  long tms_utime, tms_stime, tms_cutime, tms_cstime; // clock ticks
};
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
struct rusage {
  struct timeval ru_utime, ru_stime;
  long ru_maxrss;   // KB
  long ru_minflt;   // page faults served without I/O
  long ru_majflt;   // ... with I/O (file-backed pages)
  long ru_nvcsw, ru_nivcsw;
};

// fcntl
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

// lseek
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ioctl requests
#define FIONREAD   0x541B
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
struct winsize {
  unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

// mount flags
#define MS_RDONLY   1
#define MS_NOSUID   2
#define MS_NODEV    4
#define MS_NOEXEC   8
#define MS_REMOUNT  32
#define MS_BIND     4096

// getdents(2): what a directory entry looks like to a program
struct dirent_user {
  uint64 d_ino;
  int64 d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  // the name follows, zero terminated
};

// statfs(2)
struct statfs {
  uint64 f_type;
  uint64 f_bsize;
  uint64 f_blocks;
  uint64 f_bfree;
  uint64 f_bavail;
  uint64 f_files;
  uint64 f_ffree;
  uint64 f_namelen;
};

// the auxiliary vector exec leaves above the environment
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

#endif
