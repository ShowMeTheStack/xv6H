#ifndef _WCHAR_H
#define _WCHAR_H
#include <sys/cdefs.h>
#include <stddef.h> // wchar_t: a 32-bit Unicode code point on this target
#include <sys/types.h>

// the only locale is C.UTF-8, so every multibyte function here is a
// UTF-8 function and nothing looks at the locale.

#ifndef _WINT_T_DEFINED
#define _WINT_T_DEFINED
typedef unsigned int wint_t;
#endif

#define WEOF ((wint_t)-1)
// <stdint.h> states the same two limits, and either header may come first
#ifndef WCHAR_MIN
#define WCHAR_MIN (-0x7fffffff - 1)
#define WCHAR_MAX 0x7fffffff
#endif

// the conversion state. a multibyte character may be split across two
// calls, so the bytes of an unfinished sequence live here until the
// rest of them arrive; `want` is zero exactly in the initial state.
typedef struct {
  unsigned char buf[4];
  unsigned char have; // bytes of the sequence already in buf
  unsigned char want; // bytes the whole sequence needs, 0 when idle
} mbstate_t;

size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *st);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *st);
size_t mbrlen(const char *s, size_t n, mbstate_t *st);
size_t mbsrtowcs(wchar_t *d, const char **src, size_t n, mbstate_t *st);
size_t wcsrtombs(char *d, const wchar_t **src, size_t n, mbstate_t *st);
int mbsinit(const mbstate_t *st);
wint_t btowc(int c);
int wctob(wint_t c);

size_t wcslen(const wchar_t *s);
size_t wcsnlen(const wchar_t *s, size_t n);
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
wchar_t *wcpcpy(wchar_t *d, const wchar_t *s);
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcscat(wchar_t *d, const wchar_t *s);
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscasecmp(const wchar_t *a, const wchar_t *b);
int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n);
size_t wcsspn(const wchar_t *s, const wchar_t *set);
size_t wcscspn(const wchar_t *s, const wchar_t *set);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set);
wchar_t *wcstok(wchar_t *s, const wchar_t *sep, wchar_t **save);
wchar_t *wcsdup(const wchar_t *s);

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);
#endif
