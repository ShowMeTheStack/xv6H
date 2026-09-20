#ifndef _XV6_LIBC_INTERNAL_H
#define _XV6_LIBC_INTERNAL_H
//
// what the libc's own files share and nothing outside it may use.
//
#include <stddef.h>
#include <stdio.h>

void __stdio_exit(void);          // flush and close every stream at exit
void __stdio_init(void);          // the three standard streams
int __stdio_flush_locked(FILE *f);
int __fmodeflags(const char *mode, int *oflags);
void __libc_lock(FILE *f);
void __libc_unlock(FILE *f);

// floating point conversion, from fltfmt.c (see fltfmt.h): digits out
// of a value, and the value back out of digits.
int __dtoa(long double v, int mode, int ndigits, int *decpt, int *sign, char *buf, int bufsize);
int __hextoa(long double v, int ndigits, int upper, int *exp2, int *sign, char *buf, int bufsize);

// thread-local storage: the size of a thread's block, and laying one
// out at a given address. the thread pointer is what comes back.
size_t __tls_size(void);
void *__tls_init(void *mem);
extern void *__tls_image;
extern size_t __tls_filesz, __tls_memsz, __tls_align;

// the thread library's primitives, used by malloc and stdio
struct mutex;
void __libc_mutex_lock(struct mutex *m);
void __libc_mutex_unlock(struct mutex *m);
#endif
