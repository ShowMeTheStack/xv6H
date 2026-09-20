#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/posix.h" // struct rusage, RUSAGE_*
#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9
#define RLIMIT_NPROC  6
#define RLIM_INFINITY (~0UL)
typedef unsigned long rlim_t;
struct rlimit {
  rlim_t rlim_cur, rlim_max;
};
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
int getrusage(int who, struct rusage *ru);
int getrlimit(int res, struct rlimit *rl);
int setrlimit(int res, const struct rlimit *rl);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);
#endif
