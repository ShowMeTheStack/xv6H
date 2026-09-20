#ifndef _STDIO_H
#define _STDIO_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 128
#define FOPEN_MAX 64
#define TMP_MAX 1000
#define L_tmpnam 20
#define P_tmpdir "/tmp"
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef long fpos_t;

// a stream: a descriptor, a buffer and what we know about it.
struct __FILE {
  int fd;
  unsigned short flags;
  signed char buftype;     // _IOFBF, _IOLBF, _IONBF
  unsigned char ungot;     // how many pushed-back bytes are waiting
  char *buf;               // the buffer, owned unless _F_EXTBUF
  size_t bufsize;
  size_t pos, end;         // read: valid bytes are buf[pos:end]; write: buf[0:pos]
  unsigned char back[8];   // ungetc storage
  off_t offset;            // memory streams and the file position of buf[0]
  char **memptr;           // open_memstream
  size_t *memsize;
  struct __FILE *next;     // every stream, for exit() and fflush(0)
  struct mutex *lock;      // 0 until the first thread asks for one
  int pid;                 // popen
};
typedef struct __FILE FILE;

// stream flags
#define _F_READ   0x0001
#define _F_WRITE  0x0002
#define _F_APPEND 0x0004
#define _F_EOF    0x0008
#define _F_ERR    0x0010
#define _F_EXTBUF 0x0020 // the caller's buffer (setvbuf)
#define _F_MEM    0x0040 // fmemopen or open_memstream
#define _F_ALLOC  0x0080 // the FILE itself came from malloc
#define _F_READING 0x0100
#define _F_WRITING 0x0200
#define _F_NOCLOSE 0x0400

extern FILE *stdin, *stdout, *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *fmemopen(void *buf, size_t size, const char *mode);
FILE *open_memstream(char **ptr, size_t *size);
FILE *tmpfile(void);
char *tmpnam(char *s);
int fclose(FILE *f);
int fflush(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
void setbuffer(FILE *f, char *buf, size_t size);
void setlinebuf(FILE *f);

size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
ssize_t getline(char **line, size_t *n, FILE *f);
ssize_t getdelim(char **line, size_t *n, int delim, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);
char *gets(char *s, int max); // xv6's bounded form, not the C one

int fseek(FILE *f, long off, int whence);
int fseeko(FILE *f, off_t off, int whence);
long ftell(FILE *f);
off_t ftello(FILE *f);
void rewind(FILE *f);
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);

int printf(const char *fmt, ...) __printflike(1, 2);
int fprintf(FILE *f, const char *fmt, ...) __printflike(2, 3);
int dprintf(int fd, const char *fmt, ...) __printflike(2, 3);
int sprintf(char *s, const char *fmt, ...) __printflike(2, 3);
int snprintf(char *s, size_t n, const char *fmt, ...) __printflike(3, 4);
int asprintf(char **out, const char *fmt, ...) __printflike(2, 3);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vasprintf(char **out, const char *fmt, va_list ap);

int scanf(const char *fmt, ...) __scanflike(1, 2);
int fscanf(FILE *f, const char *fmt, ...) __scanflike(2, 3);
int sscanf(const char *s, const char *fmt, ...) __scanflike(2, 3);
int vscanf(const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);
int vsscanf(const char *s, const char *fmt, va_list ap);

void perror(const char *s);
int remove(const char *path);
int rename(const char *from, const char *to);
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
void flockfile(FILE *f);
void funlockfile(FILE *f);
int ftrylockfile(FILE *f);
int getc_unlocked(FILE *f);
int putc_unlocked(int c, FILE *f);
int getchar_unlocked(void);
int putchar_unlocked(int c);
#endif
