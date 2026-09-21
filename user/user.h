#ifndef XV6_USER_H
#define XV6_USER_H
//
// what a program of this system gets beyond the C library: the calls
// that belong to xv6 itself, and the headers everything uses, so that
// a program need include only this one.
//
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <setjmp.h>
#include "kernel/types.h"
#include "user/thread.h" // clone, futex, and the thread library

#define SBRK_ERROR ((void *)-1)

// the hypervisor (kernel/vm.h has the structures)
struct vmconfig;
struct vminfo;
int vm_create(struct vmconfig *);
int vm_ctl(int, int);
int vm_run(int, int);
int vm_destroy(int);
int vm_cons(int, int, void *, int);
int vm_info(int, struct vminfo *);
int vm_stats(uint64 *, int);
struct hwinfo;
int hwinfo(struct hwinfo *);

// the network stack (kernel/net.h, kernel/socket.h)
struct netinfo;
int socket(int);
int bind(int, int);
int connect(int, uint32, int);
int listen(int, int);
int accept(int, uint32 *, int *);
int sendto(int, const void *, int, uint32, int);
int recvfrom(int, void *, int, uint32 *, int *, int);
int netinfo(struct netinfo *);
// SO_TTL and SO_ICMP_ERRORS live in kernel/socket.h
int sockopt(int fd, int option, int value);
// ptrace: the requests are in kernel/ptrace.h
long ptrace(int request, int pid, void *addr, void *data);

// user/net.c: names, addresses and whole transfers
int dns_resolve(const char *name, uint32 *ip);
int parse_ip(const char *s, uint32 *ip);
char *ip2str(uint32 ip, char *buf);
int tcp_open(const char *host, int port);
int read_all(int fd, char *buf, int max);

// mounting, and the calls that read a directory or a file system
int mount(const char *source, const char *target, const char *type, unsigned long flags,
          const void *options);
int umount(const char *target);
int getdents(int fd, void *buf, int len);
struct statfs;
int statfs(const char *path, struct statfs *buf);

// the raw exec system call: execv() and its relatives are built on it
int exec(const char *path, char **argv);

// the console, and the two system calls that are ours alone
int consraw(int);
char *sbrklazy(int);
int ticksleep(int ticks); // sleep n scheduler ticks, or until a signal
int mknod(const char *, short, short);
int mkdir(const char *, unsigned int);   // POSIX: the mode is ignored
int uptime(void);

#endif
