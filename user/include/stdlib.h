#ifndef _STDLIB_H
#define _STDLIB_H
#include <sys/cdefs.h>
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#define MB_CUR_MAX ((size_t)4)

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void *reallocarray(void *p, size_t n, size_t size);
void free(void *p);
int posix_memalign(void **out, size_t align, size_t n);
void *aligned_alloc(size_t align, size_t n);
void *memalign(size_t align, size_t n);
size_t malloc_usable_size(void *p);

void exit(int status) __noreturn;
void _Exit(int status) __noreturn;
void quick_exit(int status) __noreturn;
void abort(void) __noreturn;
int atexit(void (*fn)(void));
int at_quick_exit(void (*fn)(void));
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);

int rand(void);
void srand(unsigned seed);
int rand_r(unsigned *seed);
long random(void);
void srandom(unsigned seed);
char *initstate(unsigned seed, char *state, size_t n);
char *setstate(char *state);
double drand48(void);
long lrand48(void);
long mrand48(void);
void srand48(long seed);
unsigned short *seed48(unsigned short s[3]);
void lcong48(unsigned short p[7]);
double erand48(unsigned short s[3]);
long nrand48(unsigned short s[3]);
long jrand48(unsigned short s[3]);

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg);
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

int abs(int v);
long labs(long v);
long long llabs(long long v);
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *s);
int clearenv(void);
int system(const char *cmd);
char *realpath(const char *path, char *out);
char *mkdtemp(char *tmpl);
int mkstemp(char *tmpl);
char *mktemp(char *tmpl);
int getsubopt(char **opt, char *const *keys, char **value);

int mblen(const char *s, size_t n);
int mbtowc(int *wc, const char *s, size_t n);
int wctomb(char *s, int wc);
size_t mbstowcs(int *d, const char *s, size_t n);
size_t wcstombs(char *d, const int *s, size_t n);
#endif
