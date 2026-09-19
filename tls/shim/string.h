// the string functions BearSSL needs, declared exactly as xv6's user
// library (user/user.h) declares them, so that both may be included.
#ifndef XV6_SHIM_STRING_H
#define XV6_SHIM_STRING_H
void *memcpy(void *, const void *, unsigned int);
void *memmove(void *, const void *, int);
void *memset(void *, int, unsigned int);
int memcmp(const void *, const void *, unsigned int);
unsigned int strlen(const char *);
#endif
