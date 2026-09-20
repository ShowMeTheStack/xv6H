#ifndef _WCTYPE_H
#define _WCTYPE_H
#include <sys/cdefs.h>
#include <stddef.h> // wchar_t

#ifndef _WINT_T_DEFINED
#define _WINT_T_DEFINED
typedef unsigned int wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

// the handle wctype() returns is an index into the small table of
// classes this library knows, and wctrans() one into the two mappings.
typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wctype_t wctype(const char *name);
int iswctype(wint_t c, wctype_t type);
wctrans_t wctrans(const char *name);
wint_t towctrans(wint_t c, wctrans_t trans);
#endif
