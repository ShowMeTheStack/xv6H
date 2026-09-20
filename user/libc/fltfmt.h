#ifndef _LIBC_FLTFMT_H
#define _LIBC_FLTFMT_H
// the binary to decimal half of the floating point conversions, kept
// private to the library: printf and friends call these, nothing else
// should. see fltfmt.c for what the modes mean and what the buffers
// have to be able to hold.

int __dtoa(long double v, int mode, int ndigits, int *decpt, int *sign, char *buf, int bufsize);
int __hextoa(long double v, int ndigits, int upper, int *exp2, int *sign, char *buf, int bufsize);
#endif
