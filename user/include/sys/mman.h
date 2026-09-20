#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/mman.h" // PROT_*, MAP_*, MAP_FAILED
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t len, int flags);
int madvise(void *addr, size_t len, int advice);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
#endif
