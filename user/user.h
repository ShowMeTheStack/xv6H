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
// a descriptor for a machine: by id, or by name with id < 0. It may be
// inherited, passed over a unix socket, read and written (the guest's
// console), and closed; closing the last one stops a machine that has
// no name. See kernel/vmm.h.
int vm_open(int id, const char *name, int flags);
int vm_ctl(int, int, unsigned long);
int vm_run(int, int);
int vm_destroy(int);
int vm_cons(int, int, void *, int);
int vm_info(int, struct vminfo *);
int vm_stats(uint64 *, int);
struct hwinfo;
int hwinfo(struct hwinfo *);

// the network. the sockets themselves are POSIX and are declared in
// <sys/socket.h>, <netinet/in.h> and <netdb.h>; what is left here is
// this system's own look at the stack.
struct netinfo;
int netinfo(struct netinfo *);
struct rtreq;
int netroute(int op, struct rtreq *);
struct gwreq;
int netgw(int op, struct gwreq *);
int unshare(int flags);
int setns(int fd, int nstype);
int pivot_root(const char *new_root, const char *put_old);
int sysfilter(int profile, int num);
int vm_save(int id, const char *path);
int vm_restore(const char *path, const char *name);
int vm_snapshot(int id, const char *path);
int vm_clone(int id, const char *name, const char *disk);
int vm_migrate(int id, int fd);
int vm_accept(int fd, const char *name);
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
