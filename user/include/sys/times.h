#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/posix.h" // struct tms
clock_t times(struct tms *t);
#endif
