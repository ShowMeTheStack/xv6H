#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef unsigned int mode_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef long time_t;
typedef long suseconds_t;
typedef long clock_t;
typedef int clockid_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef unsigned int useconds_t;
typedef unsigned int id_t;
typedef long key_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef long register_t;
#endif
