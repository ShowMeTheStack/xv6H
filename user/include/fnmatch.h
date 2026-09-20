#ifndef _FNMATCH_H
#define _FNMATCH_H
#include <sys/cdefs.h>

#define FNM_NOMATCH     1     // the string does not match the pattern

#define FNM_NOESCAPE    0x01  // a backslash is an ordinary character
#define FNM_PATHNAME    0x02  // a slash must be matched by a slash
#define FNM_PERIOD      0x04  // a leading period must be matched explicitly
#define FNM_CASEFOLD    0x08  // compare without regard to case
#define FNM_LEADING_DIR 0x10  // the pattern may match a leading directory

// the older spellings, which some programs use
#define FNM_FILE_NAME   FNM_PATHNAME
#define FNM_IGNORECASE  FNM_CASEFOLD

int fnmatch(const char *pattern, const char *string, int flags);
#endif
