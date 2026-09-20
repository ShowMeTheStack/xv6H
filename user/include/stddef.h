#ifndef _STDDEF_H
#define _STDDEF_H
#include <sys/cdefs.h>
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef long double max_align_t;
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
