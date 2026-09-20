#ifndef _ERR_H
#define _ERR_H
#include <sys/cdefs.h>
#include <stdarg.h>
void err(int status, const char *fmt, ...) __noreturn __printflike(2, 3);
void errx(int status, const char *fmt, ...) __noreturn __printflike(2, 3);
void warn(const char *fmt, ...) __printflike(1, 2);
void warnx(const char *fmt, ...) __printflike(1, 2);
void verr(int status, const char *fmt, va_list ap) __noreturn;
void verrx(int status, const char *fmt, va_list ap) __noreturn;
void vwarn(const char *fmt, va_list ap);
void vwarnx(const char *fmt, va_list ap);
#endif
