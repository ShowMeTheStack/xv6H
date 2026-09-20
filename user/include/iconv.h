#ifndef _ICONV_H
#define _ICONV_H
#include <sys/cdefs.h>
#include <stddef.h>

// the descriptor is opaque; iconv_open returns (iconv_t)-1 on failure,
// the way open() returns -1, so a caller must check for that and not
// for a null pointer.
typedef void *iconv_t;

iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t cd, char **inbuf, size_t *inleft, char **outbuf, size_t *outleft);
int iconv_close(iconv_t cd);
#endif
