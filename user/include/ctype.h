#ifndef _CTYPE_H
#define _CTYPE_H
#include <sys/cdefs.h>
// the C locale, as a table lookup on the 257 entries from EOF to 255.
#define _CT_UPPER 0x01
#define _CT_LOWER 0x02
#define _CT_DIGIT 0x04
#define _CT_SPACE 0x08
#define _CT_PUNCT 0x10
#define _CT_CNTRL 0x20
#define _CT_XDIGIT 0x40
#define _CT_BLANK 0x80
extern const unsigned char __ctype_table[257];
#define __ct(c) (__ctype_table[(c) + 1])
static inline int isalnum(int c) { return __ct(c) & (_CT_UPPER | _CT_LOWER | _CT_DIGIT); }
static inline int isalpha(int c) { return __ct(c) & (_CT_UPPER | _CT_LOWER); }
static inline int isblank(int c) { return __ct(c) & _CT_BLANK; }
static inline int iscntrl(int c) { return __ct(c) & _CT_CNTRL; }
static inline int isdigit(int c) { return __ct(c) & _CT_DIGIT; }
static inline int isgraph(int c) { return (c) > 0x20 && (c) < 0x7f; }
static inline int islower(int c) { return __ct(c) & _CT_LOWER; }
static inline int isprint(int c) { return (c) >= 0x20 && (c) < 0x7f; }
static inline int ispunct(int c) { return __ct(c) & _CT_PUNCT; }
static inline int isspace(int c) { return __ct(c) & _CT_SPACE; }
static inline int isupper(int c) { return __ct(c) & _CT_UPPER; }
static inline int isxdigit(int c) { return __ct(c) & _CT_XDIGIT; }
static inline int isascii(int c) { return (unsigned)(c) < 128; }
static inline int toascii(int c) { return (c) & 0x7f; }
int tolower(int c);
int toupper(int c);
#endif
