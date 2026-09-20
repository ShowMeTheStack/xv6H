#ifndef _REGEX_H
#define _REGEX_H
#include <sys/cdefs.h>
#include <stddef.h>

// the offset type POSIX gives match positions in; -1 means "no match
// for this subexpression".
typedef long regoff_t;

// the compiled pattern. re_nsub is the only field the standard
// promises; the rest is the engine's and nothing outside regex.c may
// touch it.
typedef struct {
  size_t re_nsub;    // number of parenthesised subexpressions
  void *__prog;      // struct prog *: the compiled program
  int __cflags;      // the flags regcomp was given
} regex_t;

typedef struct {
  regoff_t rm_so;    // byte offset of the start, or -1
  regoff_t rm_eo;    // byte offset one past the end, or -1
} regmatch_t;

// regcomp flags
#define REG_EXTENDED 0x01  // ERE rather than BRE syntax
#define REG_ICASE    0x02  // fold case while matching
#define REG_NOSUB    0x04  // report success only, no offsets
#define REG_NEWLINE  0x08  // a newline ends a line: . and [^] skip it, ^ and $ hug it

// regexec flags
#define REG_NOTBOL   0x10  // the string does not begin a line
#define REG_NOTEOL   0x20  // the string does not end a line

// error codes; REG_NOMATCH is the only one regexec returns
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

int regcomp(regex_t *re, const char *pattern, int cflags);
int regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t *pmatch, int eflags);
size_t regerror(int code, const regex_t *re, char *errbuf, size_t errbuf_size);
void regfree(regex_t *re);
#endif
