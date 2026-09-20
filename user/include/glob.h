#ifndef _GLOB_H
#define _GLOB_H
#include <sys/cdefs.h>
#include <stddef.h>

typedef struct {
  size_t gl_pathc;   // how many paths matched
  char **gl_pathv;   // the paths, followed by a null pointer
  size_t gl_offs;    // slots to reserve in front of them, with GLOB_DOOFFS
  int gl_flags;      // the flags the last call was given, plus GLOB_MAGCHAR
} glob_t;

#define GLOB_ERR      0x0001  // stop at the first directory that cannot be read
#define GLOB_MARK     0x0002  // append a slash to every directory found
#define GLOB_NOSORT   0x0004  // hand the paths back in the order they were found
#define GLOB_DOOFFS   0x0008  // leave gl_offs null pointers in front of the results
#define GLOB_NOCHECK  0x0010  // with no match, return the pattern itself
#define GLOB_APPEND   0x0020  // add to the results already in the glob_t
#define GLOB_NOESCAPE 0x0040  // a backslash is an ordinary character
#define GLOB_MAGCHAR  0x0100  // set by glob() when the pattern held a metacharacter
#define GLOB_ONLYDIR  0x0200  // a hint: the caller only wants directories

#define GLOB_NOSPACE  1       // out of memory
#define GLOB_ABORTED  2       // a read error, with GLOB_ERR or errfunc saying stop
#define GLOB_NOMATCH  3       // nothing matched and GLOB_NOCHECK was not given

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g);
void globfree(glob_t *g);
#endif
