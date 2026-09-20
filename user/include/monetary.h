#ifndef _MONETARY_H
#define _MONETARY_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <sys/types.h> // ssize_t

// the per-call locale handle of the _l functions. no other header
// declares it yet, so it is declared here under a guard for the day
// one does.
#ifndef _LOCALE_T_DEFINED
#define _LOCALE_T_DEFINED
typedef struct __locale *locale_t;
#endif

ssize_t strfmon(char *s, size_t n, const char *fmt, ...);
ssize_t strfmon_l(char *s, size_t n, locale_t loc, const char *fmt, ...);
#endif
