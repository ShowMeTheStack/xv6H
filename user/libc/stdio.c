//
// <stdio.h>: streams.
//
// a stream is a descriptor, a buffer and a little state. reading fills
// the buffer from the descriptor and hands bytes out of it; writing
// gathers bytes and empties the buffer when it is full, at a newline
// for a line-buffered stream, or when the program asks. the buffer is
// used for one direction at a time, and switching directions flushes
// or discards what was in it, which is what the standard requires of
// an update stream between reads and writes.
//
// a stream may also live in memory rather than on a descriptor
// (fmemopen and open_memstream), in which case fd is -1 and the buffer
// is the file.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "user/thread.h"
#include "libc.h"

// every stream, so exit() can flush them and fflush(0) can reach them
static FILE *streams;
static mutex_t list_lock = MUTEX_INIT;

// the three the program starts with. their buffers are allocated on
// first use so that a program that never prints costs nothing.
static FILE stdin_file = {.fd = 0, .flags = _F_READ | _F_NOCLOSE, .buftype = _IOFBF};
static FILE stdout_file = {.fd = 1, .flags = _F_WRITE | _F_NOCLOSE, .buftype = _IOLBF};
static FILE stderr_file = {.fd = 2, .flags = _F_WRITE | _F_NOCLOSE, .buftype = _IONBF};

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

static void
link_stream(FILE *f)
{
  mutex_lock(&list_lock);
  f->next = streams;
  streams = f;
  mutex_unlock(&list_lock);
}

static void
unlink_stream(FILE *f)
{
  mutex_lock(&list_lock);
  FILE **pp = &streams;
  while (*pp && *pp != f)
    pp = &(*pp)->next;
  if (*pp)
    *pp = f->next;
  mutex_unlock(&list_lock);
}

// give the stream a buffer the first time it needs one.
static int
ensure_buf(FILE *f)
{
  if (f->buf || f->buftype == _IONBF)
    return 0;
  f->buf = malloc(BUFSIZ);
  if (f->buf == 0) {
    f->buftype = _IONBF; // carry on unbuffered rather than fail
    return 0;
  }
  f->bufsize = BUFSIZ;
  return 0;
}

// write everything in the buffer out, however many calls that takes.
static int
flush_write(FILE *f)
{
  size_t off = 0;

  if (!(f->flags & _F_WRITING) || f->pos == 0)
    return 0;
  if (f->flags & _F_MEM) {
    // a memory stream's buffer is the file: there is nothing to write
    f->offset += f->pos;
    f->pos = 0;
    return 0;
  }
  while (off < f->pos) {
    ssize_t n = write(f->fd, f->buf + off, f->pos - off);
    if (n <= 0) {
      // keep what is left so a later flush can try again
      if (off)
        memmove(f->buf, f->buf + off, f->pos - off);
      f->pos -= off;
      f->flags |= _F_ERR;
      return EOF;
    }
    off += n;
  }
  f->pos = 0;
  return 0;
}

// leave read mode: give back the bytes the program has not consumed,
// so the descriptor's offset matches what the program has seen.
static void
drop_read(FILE *f)
{
  if (f->flags & _F_READING) {
    if (f->end > f->pos && f->fd >= 0 && !(f->flags & _F_MEM))
      lseek(f->fd, -(off_t)(f->end - f->pos), SEEK_CUR);
    f->pos = f->end = 0;
    f->ungot = 0;
    f->flags &= ~_F_READING;
  }
}

int
__stdio_flush_locked(FILE *f)
{
  if (f->flags & _F_WRITING)
    return flush_write(f);
  return 0;
}

int
fflush(FILE *f)
{
  if (f == 0) {
    int r = 0;
    mutex_lock(&list_lock);
    for (FILE *s = streams; s; s = s->next)
      if (__stdio_flush_locked(s) == EOF)
        r = EOF;
    mutex_unlock(&list_lock);
    if (__stdio_flush_locked(stdout) == EOF || __stdio_flush_locked(stderr) == EOF)
      r = EOF;
    return r;
  }
  return __stdio_flush_locked(f);
}

// the mode string, as open() flags and stream flags.
int
__fmodeflags(const char *mode, int *oflags)
{
  int flags = 0, o = 0;

  switch (mode[0]) {
  case 'r':
    flags = _F_READ;
    o = O_RDONLY;
    break;
  case 'w':
    flags = _F_WRITE;
    o = O_WRONLY | O_CREAT | O_TRUNC;
    break;
  case 'a':
    flags = _F_WRITE | _F_APPEND;
    o = O_WRONLY | O_CREAT | O_APPEND;
    break;
  default:
    return -1;
  }
  for (const char *p = mode + 1; *p; p++) {
    if (*p == '+') {
      flags |= _F_READ | _F_WRITE;
      o = (o & ~O_ACCMODE) | O_RDWR;
    } else if (*p == 'e') {
      o |= O_CLOEXEC;
    }
  }
  if (oflags)
    *oflags = o;
  return flags;
}

static FILE *
newfile(int fd, int flags)
{
  FILE *f = calloc(1, sizeof(FILE));

  if (f == 0)
    return 0;
  f->fd = fd;
  f->flags = flags | _F_ALLOC;
  f->buftype = _IOFBF;
  link_stream(f);
  return f;
}

FILE *
fopen(const char *path, const char *mode)
{
  int o, flags = __fmodeflags(mode, &o);

  if (flags < 0) {
    errno = EINVAL;
    return 0;
  }
  int fd = open(path, o);
  if (fd < 0)
    return 0;
  FILE *f = newfile(fd, flags);
  if (f == 0) {
    close(fd);
    return 0;
  }
  return f;
}

FILE *
fdopen(int fd, const char *mode)
{
  int flags = __fmodeflags(mode, 0);

  if (flags < 0) {
    errno = EINVAL;
    return 0;
  }
  return newfile(fd, flags);
}

FILE *
freopen(const char *path, const char *mode, FILE *f)
{
  int o, flags = __fmodeflags(mode, &o);

  if (flags < 0) {
    errno = EINVAL;
    return 0;
  }
  fflush(f);
  if (path == 0) {
    errno = EINVAL; // changing the mode of an open descriptor is not supported
    return 0;
  }
  int fd = open(path, o);
  if (fd < 0)
    return 0;
  if (f->fd >= 0 && !(f->flags & _F_NOCLOSE))
    close(f->fd);
  if (f->fd >= 0 && (f->flags & _F_NOCLOSE)) {
    // the standard streams keep their descriptor numbers
    dup2(fd, f->fd);
    close(fd);
  } else {
    f->fd = fd;
  }
  f->flags = (f->flags & (_F_ALLOC | _F_NOCLOSE)) | flags;
  f->pos = f->end = 0;
  f->ungot = 0;
  return f;
}

FILE *
tmpfile(void)
{
  char name[] = "/tmp/tmpXXXXXX";
  int fd = mkstemp(name);

  if (fd < 0) {
    // without a /tmp, fall back to the working directory
    strcpy(name, "tmpXXXXXX");
    fd = mkstemp(name);
    if (fd < 0)
      return 0;
  }
  unlink(name); // it disappears when the last descriptor closes
  FILE *f = newfile(fd, _F_READ | _F_WRITE);
  if (f == 0) {
    close(fd);
    return 0;
  }
  return f;
}

FILE *
fmemopen(void *buf, size_t size, const char *mode)
{
  int flags = __fmodeflags(mode, 0);

  if (flags < 0) {
    errno = EINVAL;
    return 0;
  }
  FILE *f = newfile(-1, flags | _F_MEM);
  if (f == 0)
    return 0;
  if (buf == 0) {
    buf = malloc(size);
    if (buf == 0) {
      unlink_stream(f);
      free(f);
      return 0;
    }
    ((char *)buf)[0] = 0;
  } else if (flags & _F_APPEND) {
    f->pos = strnlen(buf, size);
  }
  f->buf = buf;
  f->bufsize = size;
  f->end = (flags & _F_READ) ? size : 0;
  f->flags |= _F_EXTBUF;
  return f;
}

FILE *
open_memstream(char **ptr, size_t *size)
{
  FILE *f = newfile(-1, _F_WRITE | _F_MEM);

  if (f == 0)
    return 0;
  f->buf = malloc(128);
  if (f->buf == 0) {
    unlink_stream(f);
    free(f);
    return 0;
  }
  f->bufsize = 128;
  f->buf[0] = 0;
  f->memptr = ptr;
  f->memsize = size;
  *ptr = f->buf;
  *size = 0;
  return f;
}

int
fclose(FILE *f)
{
  int r = 0;

  if (f == 0)
    return EOF;
  if (__stdio_flush_locked(f) == EOF)
    r = EOF;
  if (f->pid > 0) {
    int st;
    while (waitpid(f->pid, &st, 0) < 0 && errno == EINTR)
      ;
    r = st;
  }
  if (f->fd >= 0 && !(f->flags & _F_NOCLOSE) && close(f->fd) < 0)
    r = EOF;
  if (f->flags & _F_MEM) {
    if (f->memptr) {
      *f->memptr = f->buf;
      *f->memsize = f->pos + f->offset;
    }
  } else if (f->buf && !(f->flags & _F_EXTBUF)) {
    free(f->buf);
  }
  unlink_stream(f);
  if (f->flags & _F_ALLOC)
    free(f);
  else
    f->flags &= ~(_F_READING | _F_WRITING);
  return r;
}

void
__stdio_exit(void)
{
  fflush(0);
  mutex_lock(&list_lock);
  for (FILE *f = streams; f; f = f->next)
    if (f->flags & _F_MEM) {
      if (f->memptr) {
        *f->memptr = f->buf;
        *f->memsize = f->pos + f->offset;
      }
    }
  mutex_unlock(&list_lock);
}

int
setvbuf(FILE *f, char *buf, int mode, size_t size)
{
  if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)
    return -1;
  __stdio_flush_locked(f);
  if (f->buf && !(f->flags & _F_EXTBUF))
    free(f->buf);
  f->buf = 0;
  f->bufsize = 0;
  f->flags &= ~_F_EXTBUF;
  f->buftype = mode;
  if (buf && size && mode != _IONBF) {
    f->buf = buf;
    f->bufsize = size;
    f->flags |= _F_EXTBUF;
  }
  f->pos = f->end = 0;
  return 0;
}

void
setbuf(FILE *f, char *buf)
{
  setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void
setbuffer(FILE *f, char *buf, size_t size)
{
  setvbuf(f, buf, buf ? _IOFBF : _IONBF, size);
}

void
setlinebuf(FILE *f)
{
  setvbuf(f, 0, _IOLBF, 0);
}

// ---- reading ----

// fill the buffer from the descriptor; returns the bytes available.
static size_t
refill(FILE *f)
{
  if (!(f->flags & _F_READ)) {
    f->flags |= _F_ERR;
    errno = EBADF;
    return 0;
  }
  if (f->flags & _F_WRITING) {
    if (flush_write(f) == EOF)
      return 0;
    f->flags &= ~_F_WRITING;
  }
  if (f->flags & _F_MEM) {
    f->flags |= _F_EOF;
    return 0;
  }
  ensure_buf(f);
  f->flags |= _F_READING;
  f->pos = f->end = 0;
  if (f->buf == 0)
    return 0;
  ssize_t n = read(f->fd, f->buf, f->bufsize);
  if (n < 0) {
    f->flags |= _F_ERR;
    return 0;
  }
  if (n == 0) {
    f->flags |= _F_EOF;
    return 0;
  }
  f->end = n;
  return n;
}

int
fgetc(FILE *f)
{
  if (f->ungot)
    return f->back[--f->ungot];
  if (f->pos < f->end)
    return (unsigned char)f->buf[f->pos++];
  if (f->buftype == _IONBF && !(f->flags & _F_MEM)) {
    unsigned char c;
    if (f->flags & _F_WRITING) {
      if (flush_write(f) == EOF)
        return EOF;
      f->flags &= ~_F_WRITING;
    }
    ssize_t n = read(f->fd, &c, 1);
    if (n < 0)
      f->flags |= _F_ERR;
    else if (n == 0)
      f->flags |= _F_EOF;
    return n == 1 ? c : EOF;
  }
  if (refill(f) == 0)
    return EOF;
  return (unsigned char)f->buf[f->pos++];
}

int
getc(FILE *f)
{
  return fgetc(f);
}

int
getc_unlocked(FILE *f)
{
  return fgetc(f);
}

int
getchar(void)
{
  return fgetc(stdin);
}

int
getchar_unlocked(void)
{
  return fgetc(stdin);
}

int
ungetc(int c, FILE *f)
{
  if (c == EOF || f->ungot >= sizeof(f->back))
    return EOF;
  f->back[f->ungot++] = c;
  f->flags &= ~_F_EOF;
  return (unsigned char)c;
}

size_t
fread(void *p, size_t size, size_t n, FILE *f)
{
  char *d = p;
  size_t want = size * n, got = 0;

  if (size == 0 || n == 0)
    return 0;
  while (f->ungot && got < want)
    d[got++] = f->back[--f->ungot];
  while (got < want) {
    if (f->pos < f->end) {
      size_t have = f->end - f->pos;
      size_t take = want - got < have ? want - got : have;
      memcpy(d + got, f->buf + f->pos, take);
      f->pos += take;
      got += take;
      continue;
    }
    // a big read goes straight to the descriptor, bypassing the buffer
    if (want - got >= BUFSIZ && f->fd >= 0 && !(f->flags & _F_MEM)) {
      if (f->flags & _F_WRITING) {
        if (flush_write(f) == EOF)
          break;
        f->flags &= ~_F_WRITING;
      }
      f->flags |= _F_READING;
      ssize_t r = read(f->fd, d + got, want - got);
      if (r < 0) {
        f->flags |= _F_ERR;
        break;
      }
      if (r == 0) {
        f->flags |= _F_EOF;
        break;
      }
      got += r;
      continue;
    }
    if (refill(f) == 0)
      break;
  }
  return got / size;
}

char *
fgets(char *s, int n, FILE *f)
{
  int i = 0;

  if (n <= 0)
    return 0;
  while (i < n - 1) {
    int c = fgetc(f);
    if (c == EOF)
      break;
    s[i++] = c;
    if (c == '\n')
      break;
  }
  if (i == 0)
    return 0;
  s[i] = 0;
  return s;
}

ssize_t
getdelim(char **line, size_t *n, int delim, FILE *f)
{
  size_t i = 0;

  if (line == 0 || n == 0) {
    errno = EINVAL;
    return -1;
  }
  if (*line == 0 || *n < 32) {
    char *p = realloc(*line, 128);
    if (p == 0)
      return -1;
    *line = p;
    *n = 128;
  }
  for (;;) {
    int c = fgetc(f);
    if (c == EOF) {
      if (i == 0)
        return -1;
      break;
    }
    if (i + 2 > *n) {
      size_t want = *n * 2;
      char *p = realloc(*line, want);
      if (p == 0)
        return -1;
      *line = p;
      *n = want;
    }
    (*line)[i++] = c;
    if (c == delim)
      break;
  }
  (*line)[i] = 0;
  return i;
}

ssize_t
getline(char **line, size_t *n, FILE *f)
{
  return getdelim(line, n, '\n', f);
}

// xv6's own: read a line from the standard input into a fixed buffer.
char *
gets(char *buf, int max)
{
  int i = 0;

  while (i + 1 < max) {
    char c;
    int cc = read(0, &c, 1);
    if (cc < 1)
      break;
    buf[i++] = c;
    if (c == '\n' || c == '\r')
      break;
  }
  buf[i] = 0;
  return buf;
}

// ---- writing ----

// make room in a memory stream's buffer, or report that it is full.
static int
mem_grow(FILE *f, size_t want)
{
  if (f->memptr == 0)
    return -1; // fmemopen: the caller's buffer is all there is
  if (want <= f->bufsize)
    return 0;
  size_t size = f->bufsize * 2;
  while (size < want)
    size *= 2;
  char *p = realloc(f->buf, size);
  if (p == 0)
    return -1;
  f->buf = p;
  f->bufsize = size;
  *f->memptr = p;
  return 0;
}

static int
flush_if_needed(FILE *f, int c)
{
  if (f->pos == f->bufsize)
    return flush_write(f);
  if (f->buftype == _IOLBF && c == '\n')
    return flush_write(f);
  return 0;
}

int
fputc(int c, FILE *f)
{
  if (!(f->flags & _F_WRITE)) {
    f->flags |= _F_ERR;
    errno = EBADF;
    return EOF;
  }
  if (f->flags & _F_READING)
    drop_read(f);

  if (f->flags & _F_MEM) {
    if (f->pos + 1 >= f->bufsize && mem_grow(f, f->pos + 2) < 0) {
      f->flags |= _F_ERR;
      return EOF;
    }
    f->buf[f->pos++] = c;
    if (f->memptr) {
      f->buf[f->pos] = 0;
      *f->memsize = f->pos;
    }
    f->flags |= _F_WRITING;
    return (unsigned char)c;
  }

  if (f->buftype == _IONBF) {
    char ch = c;
    if (write(f->fd, &ch, 1) != 1) {
      f->flags |= _F_ERR;
      return EOF;
    }
    return (unsigned char)c;
  }
  ensure_buf(f);
  if (f->buf == 0) {
    char ch = c;
    return write(f->fd, &ch, 1) == 1 ? (unsigned char)c : EOF;
  }
  f->flags |= _F_WRITING;
  f->buf[f->pos++] = c;
  if (flush_if_needed(f, c) == EOF)
    return EOF;
  return (unsigned char)c;
}

int
putc(int c, FILE *f)
{
  return fputc(c, f);
}

int
putc_unlocked(int c, FILE *f)
{
  return fputc(c, f);
}

int
putchar(int c)
{
  return fputc(c, stdout);
}

int
putchar_unlocked(int c)
{
  return fputc(c, stdout);
}

size_t
fwrite(const void *p, size_t size, size_t n, FILE *f)
{
  const char *s = p;
  size_t want = size * n, done = 0;

  if (size == 0 || n == 0)
    return 0;
  if (!(f->flags & _F_WRITE)) {
    f->flags |= _F_ERR;
    errno = EBADF;
    return 0;
  }
  if (f->flags & _F_READING)
    drop_read(f);

  if (f->flags & _F_MEM) {
    if (f->pos + want + 1 > f->bufsize && mem_grow(f, f->pos + want + 1) < 0) {
      want = f->bufsize > f->pos + 1 ? f->bufsize - f->pos - 1 : 0;
      f->flags |= _F_ERR;
    }
    memcpy(f->buf + f->pos, s, want);
    f->pos += want;
    if (f->memptr) {
      f->buf[f->pos] = 0;
      *f->memsize = f->pos;
    }
    f->flags |= _F_WRITING;
    return want / size;
  }

  // a big write goes straight out, after what is buffered
  if (want >= BUFSIZ && f->buftype != _IOLBF) {
    if (flush_write(f) == EOF)
      return 0;
    while (done < want) {
      ssize_t w = write(f->fd, s + done, want - done);
      if (w <= 0) {
        f->flags |= _F_ERR;
        break;
      }
      done += w;
    }
    return done / size;
  }

  ensure_buf(f);
  if (f->buf == 0) {
    while (done < want) {
      ssize_t w = write(f->fd, s + done, want - done);
      if (w <= 0) {
        f->flags |= _F_ERR;
        break;
      }
      done += w;
    }
    return done / size;
  }
  f->flags |= _F_WRITING;
  while (done < want) {
    size_t room = f->bufsize - f->pos;
    size_t take = want - done < room ? want - done : room;
    memcpy(f->buf + f->pos, s + done, take);
    f->pos += take;
    done += take;
    if (f->pos == f->bufsize && flush_write(f) == EOF)
      break;
    if (f->buftype == _IOLBF && memchr(s + done - take, '\n', take) && flush_write(f) == EOF)
      break;
  }
  return done / size;
}

int
fputs(const char *s, FILE *f)
{
  size_t n = strlen(s);
  return fwrite(s, 1, n, f) == n ? (int)n : EOF;
}

int
puts(const char *s)
{
  if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF)
    return EOF;
  return 0;
}

// ---- position and state ----

int
fseeko(FILE *f, off_t off, int whence)
{
  if (f->flags & _F_MEM) {
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)f->pos : (off_t)f->end;
    if (base + off < 0)
      return -1;
    f->pos = base + off;
    f->flags &= ~_F_EOF;
    return 0;
  }
  if (__stdio_flush_locked(f) == EOF)
    return -1;
  if (whence == SEEK_CUR && (f->flags & _F_READING))
    off -= (off_t)(f->end - f->pos) + f->ungot;
  f->pos = f->end = 0;
  f->ungot = 0;
  f->flags &= ~(_F_EOF | _F_READING | _F_WRITING);
  return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

int
fseek(FILE *f, long off, int whence)
{
  return fseeko(f, off, whence);
}

off_t
ftello(FILE *f)
{
  if (f->flags & _F_MEM)
    return f->offset + f->pos;
  off_t pos = lseek(f->fd, 0, SEEK_CUR);
  if (pos < 0)
    return -1;
  if (f->flags & _F_READING)
    pos -= (off_t)(f->end - f->pos) + f->ungot;
  else if (f->flags & _F_WRITING)
    pos += f->pos;
  return pos;
}

long
ftell(FILE *f)
{
  return ftello(f);
}

void
rewind(FILE *f)
{
  fseeko(f, 0, SEEK_SET);
  f->flags &= ~_F_ERR;
}

int
fgetpos(FILE *f, fpos_t *pos)
{
  off_t p = ftello(f);

  if (p < 0)
    return -1;
  *pos = p;
  return 0;
}

int
fsetpos(FILE *f, const fpos_t *pos)
{
  return fseeko(f, *pos, SEEK_SET);
}

int
feof(FILE *f)
{
  return (f->flags & _F_EOF) != 0;
}

int
ferror(FILE *f)
{
  return (f->flags & _F_ERR) != 0;
}

void
clearerr(FILE *f)
{
  f->flags &= ~(_F_EOF | _F_ERR);
}

int
fileno(FILE *f)
{
  if (f->fd < 0) {
    errno = EBADF;
    return -1;
  }
  return f->fd;
}

// locking a stream is a promise about atomicity between threads; the
// streams here take the list lock only, which is enough while every
// stream operation is short.
void
flockfile(FILE *f)
{
  (void)f;
}

void
funlockfile(FILE *f)
{
  (void)f;
}

int
ftrylockfile(FILE *f)
{
  (void)f;
  return 0;
}

// ---- files by name ----

int
remove(const char *path)
{
  return unlink(path);
}

int
rename(const char *from, const char *to)
{
  // without a rename system call, a link and an unlink do the job for
  // a file; a directory cannot be linked, so it cannot be renamed yet.
  if (link(from, to) < 0)
    return -1;
  if (unlink(from) < 0) {
    unlink(to);
    return -1;
  }
  return 0;
}

void
perror(const char *s)
{
  const char *msg = strerror(errno);

  if (s && *s) {
    fputs(s, stderr);
    fputs(": ", stderr);
  }
  fputs(msg, stderr);
  fputc('\n', stderr);
  fflush(stderr);
}

// ---- a command as a stream ----

FILE *
popen(const char *cmd, const char *mode)
{
  int fds[2];
  int reading = (mode[0] == 'r');

  if (mode[0] != 'r' && mode[0] != 'w') {
    errno = EINVAL;
    return 0;
  }
  if (pipe(fds) < 0)
    return 0;

  int pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return 0;
  }
  if (pid == 0) {
    if (reading) {
      close(fds[0]);
      dup2(fds[1], 1);
      if (fds[1] != 1)
        close(fds[1]);
    } else {
      close(fds[1]);
      dup2(fds[0], 0);
      if (fds[0] != 0)
        close(fds[0]);
    }
    char *argv[] = {"sh", "-c", (char *)cmd, 0};
    execv("/sh", argv);
    _exit(127);
  }
  close(reading ? fds[1] : fds[0]);
  FILE *f = newfile(reading ? fds[0] : fds[1], reading ? _F_READ : _F_WRITE);
  if (f == 0) {
    close(reading ? fds[0] : fds[1]);
    return 0;
  }
  f->pid = pid;
  return f;
}

int
pclose(FILE *f)
{
  return fclose(f);
}
