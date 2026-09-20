#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H
// small shared definitions: nothing here is standard, everything else
// includes it.
#define __XV6_LIBC__ 1
#ifndef NULL
#define NULL ((void *)0)
#endif
#define __unused __attribute__((unused))
#define __noreturn __attribute__((noreturn))
#define __printflike(f, a) __attribute__((format(printf, f, a)))
#define __scanflike(f, a) __attribute__((format(scanf, f, a)))
#define __pure __attribute__((pure))
#endif
