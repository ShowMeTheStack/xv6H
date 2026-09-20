#ifndef _WORDEXP_H
#define _WORDEXP_H
#include <sys/cdefs.h>
#include <stddef.h>

typedef struct {
  size_t we_wordc;   // how many words came out
  char **we_wordv;   // the words, followed by a null pointer
  size_t we_offs;    // slots to reserve in front of them, with WRDE_DOOFFS
} wordexp_t;

#define WRDE_APPEND  0x01  // add to the words already in the wordexp_t
#define WRDE_DOOFFS  0x02  // leave we_offs null pointers in front of the words
#define WRDE_NOCMD   0x04  // refuse command substitution rather than running it
#define WRDE_REUSE   0x08  // the wordexp_t is from an earlier call: free it first
#define WRDE_SHOWERR 0x10  // do not silence the words' own error output
#define WRDE_UNDEF   0x20  // an unset variable is an error

#define WRDE_BADCHAR 1     // an unquoted character that only a shell may use
#define WRDE_BADVAL  2     // an unset variable, under WRDE_UNDEF
#define WRDE_CMDSUB  3     // command substitution, under WRDE_NOCMD
#define WRDE_NOSPACE 4     // out of memory
#define WRDE_SYNTAX  5     // an unbalanced quote or a trailing backslash

int wordexp(const char *words, wordexp_t *we, int flags);
void wordfree(wordexp_t *we);
#endif
