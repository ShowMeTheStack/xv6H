#ifndef _LIMITS_H
#define _LIMITS_H
#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX
#define MB_LEN_MAX 4
#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535
#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U
#define LONG_MIN   (-9223372036854775807L - 1)
#define LONG_MAX   9223372036854775807L
#define ULONG_MAX  18446744073709551615UL
#define LLONG_MIN  LONG_MIN
#define LLONG_MAX  LONG_MAX
#define ULLONG_MAX ULONG_MAX
#include "kernel/param.h" // MAXPATH, DIRSIZ, ARG_MAX, NOFILE and the rest
#ifndef PATH_MAX
#define PATH_MAX   MAXPATH
#endif
#ifndef NAME_MAX
#define NAME_MAX   255     // what ext4 allows; the old format stops at 14
#endif
#ifndef ARG_MAX
#define ARG_MAX    32768
#endif
#define OPEN_MAX   1024
#define NGROUPS_MAX 32
#define PIPE_BUF   512
#define SSIZE_MAX  LONG_MAX
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 32
#define TTY_NAME_MAX 32
#define SYMLOOP_MAX 8
#define LINE_MAX   4096
#define IOV_MAX    16
#endif
