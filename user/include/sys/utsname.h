#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H
#include <sys/cdefs.h>
#include "kernel/posix.h" // struct utsname
int uname(struct utsname *u);
#endif
