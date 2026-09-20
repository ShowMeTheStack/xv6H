#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/stat.h" // struct stat and the T_* types

// the file types, as POSIX spells them. xv6's inode type is a small
// number rather than a mode field; ownership and permissions arrive
// with the user milestone, and these map onto what exists today.
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01

// the inode type of a stat, as a mode
mode_t __type2mode(short type);
#define ST_MODE(sb) __type2mode((sb).type)

int stat(const char *path, struct stat *sb);
int lstat(const char *path, struct stat *sb);
int fstat(int fd, struct stat *sb);
int fstatat(int dirfd, const char *path, struct stat *sb, int flags);
// POSIX: the mode is taken and, for now, ignored -- this kernel makes
// a directory 0755 whatever is asked for. the argument is in the
// interface because every program that calls mkdir passes one.
int mkdir(const char *path, mode_t mode);
int mkdirat(int dirfd, const char *path, mode_t mode);
int mknod(const char *path, short major, short minor);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t mask);
#endif
