#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <sys/types.h>
#include "kernel/sysconf.h" // the _SC_* names

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int dup(int fd);
int dup2(int old, int new);
int dup3(int old, int new, int flags);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int ftruncate(int fd, off_t len);
int truncate(const char *path, off_t len);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t n);

pid_t fork(void);
pid_t vfork(void);
int execv(const char *path, char *const argv[]);
int execve(const char *path, char *const argv[], char *const envp[]);
int execvp(const char *file, char *const argv[]);
int execvpe(const char *file, char *const argv[], char *const envp[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *path, const char *arg, ...);
void _exit(int status) __noreturn;
pid_t getpid(void);
pid_t getppid(void);
pid_t gettid(void);
pid_t getpgrp(void);
pid_t getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
int setpgrp(void);
pid_t setsid(void);
pid_t getsid(pid_t pid);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int seteuid(uid_t uid);
int setgid(gid_t gid);
int setegid(gid_t gid);
int setreuid(uid_t r, uid_t e);
int setregid(gid_t r, gid_t e);
int getgroups(int n, gid_t *list);
int setgroups(size_t n, const gid_t *list);
char *getlogin(void);

int chdir(const char *path);
int fchdir(int fd);
char *getcwd(char *buf, size_t n);
char *getwd(char *buf);
int unlink(const char *path);
int rmdir(const char *path);
int link(const char *from, const char *to);
int symlink(const char *from, const char *to);
ssize_t readlink(const char *path, char *buf, size_t n);
int access(const char *path, int mode);
int chown(const char *path, uid_t u, gid_t g);
int fchown(int fd, uid_t u, gid_t g);
int lchown(const char *path, uid_t u, gid_t g);

unsigned int alarm(unsigned int seconds);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
int pause(void);
long sysconf(int name);
long fpathconf(int fd, int name);
long pathconf(const char *path, int name);
int gethostname(char *name, size_t n);
int sethostname(const char *name, size_t n);
int getpagesize(void);
void *sbrk(long n);
int brk(void *addr);
int nice(int inc);
int getdtablesize(void);

extern char **environ;
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *spec);
#endif
