#ifndef _STRING_H
#define _STRING_H
#include <sys/cdefs.h>
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
void *memmem(const void *h, size_t hn, const void *n, size_t nn);
void *mempcpy(void *d, const void *s, size_t n);

char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strchrnul(const char *s, int c);
size_t strcspn(const char *s, const char *set);
size_t strspn(const char *s, const char *set);
char *strpbrk(const char *s, const char *set);
char *strstr(const char *h, const char *n);
char *strcasestr(const char *h, const char *n);
char *strtok(char *s, const char *sep);
char *strtok_r(char *s, const char *sep, char **save);
char *strsep(char **sp, const char *sep);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strerror(int err);
int strerror_r(int err, char *buf, size_t n);
char *strsignal(int sig);
void *memccpy(void *d, const void *s, int c, size_t n);
#endif
