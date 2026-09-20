#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H
#include <sys/cdefs.h>
#include <sys/types.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM   2
#define GRND_INSECURE 4
ssize_t getrandom(void *buf, size_t n, unsigned int flags);
int getentropy(void *buf, size_t n);
#endif
