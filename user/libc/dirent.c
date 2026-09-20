//
// <dirent.h> over the file system's own directory format: a directory
// is a file of fixed-size entries, each an inode number and a name, so
// reading one is an ordinary read and the only work here is skipping
// the empty slots and keeping the offset for telldir and seekdir.
//
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "libc.h"

// the kernel call that reads a directory, whatever file system it is on
int getdents(int fd, void *buf, int len);

// the on-disk directory entry, spelled out here rather than taken from
// the kernel's fs.h, whose own struct dirent is a different thing from
// the one <dirent.h> defines.
#define DISK_NAMELEN 14
struct disk_dirent {
  unsigned short inum;
  char name[DISK_NAMELEN];
};

// the kernel hands entries over in the getdents shape, which is the
// same whatever file system the directory belongs to. a buffer of them
// is read at a time and handed out one by one.
#define DIRBUF 1024

struct __DIR {
  int fd;
  off_t off;              // where the next read starts, for telldir
  int len, pos;           // valid bytes in buf, and how far we have read
  struct dirent ent;      // what readdir() hands back
  char buf[DIRBUF];
};

DIR *
opendir(const char *path)
{
  struct stat sb;
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    return 0;
  if (fstat(fd, &sb) < 0 || sb.type != T_DIR) {
    close(fd);
    errno = ENOTDIR;
    return 0;
  }
  DIR *d = calloc(1, sizeof(DIR));
  if (d == 0) {
    close(fd);
    return 0;
  }
  d->fd = fd;
  return d;
}

DIR *
fdopendir(int fd)
{
  struct stat sb;

  if (fstat(fd, &sb) < 0)
    return 0;
  if (sb.type != T_DIR) {
    errno = ENOTDIR;
    return 0;
  }
  DIR *d = calloc(1, sizeof(DIR));
  if (d == 0)
    return 0;
  d->fd = fd;
  d->off = lseek(fd, 0, SEEK_CUR);
  return d;
}

int
closedir(DIR *d)
{
  int fd = d->fd;

  free(d);
  return close(fd);
}

int
dirfd(DIR *d)
{
  return d->fd;
}

// one entry, out of the buffer if there is anything left in it and
// from the kernel if there is not.
struct dirent *
readdir(DIR *d)
{
  for (;;) {
    if (d->pos >= d->len) {
      int n = getdents(d->fd, d->buf, sizeof(d->buf));
      if (n <= 0)
        return 0;
      d->len = n;
      d->pos = 0;
    }
    struct kernel_dirent {
      unsigned long d_ino;
      long d_off;
      unsigned short d_reclen;
      unsigned char d_type;
    } *kde = (void *)(d->buf + d->pos);
    if (kde->d_reclen == 0 || d->pos + kde->d_reclen > d->len) {
      d->pos = d->len;
      continue;
    }
    const char *name = d->buf + d->pos + sizeof(*kde);
    d->pos += kde->d_reclen;
    d->off = kde->d_off;
    d->ent.d_ino = kde->d_ino;
    d->ent.d_off = kde->d_off;
    d->ent.d_reclen = sizeof(struct dirent);
    d->ent.d_type = kde->d_type;
    strlcpy(d->ent.d_name, name, sizeof(d->ent.d_name));
    return &d->ent;
  }
}

int
readdir_r(DIR *d, struct dirent *entry, struct dirent **result)
{
  struct dirent *e = readdir(d);

  if (e == 0) {
    *result = 0;
    return 0;
  }
  *entry = *e;
  *result = entry;
  return 0;
}

void
rewinddir(DIR *d)
{
  lseek(d->fd, 0, SEEK_SET);
  d->off = 0;
  d->len = d->pos = 0;
}

long
telldir(DIR *d)
{
  return d->off;
}

void
seekdir(DIR *d, long off)
{
  lseek(d->fd, off, SEEK_SET);
  d->off = off;
  d->len = d->pos = 0;
}

int
alphasort(const struct dirent **a, const struct dirent **b)
{
  return strcmp((*a)->d_name, (*b)->d_name);
}

int
scandir(const char *path, struct dirent ***list, int (*sel)(const struct dirent *),
        int (*cmp)(const struct dirent **, const struct dirent **))
{
  DIR *d = opendir(path);

  if (d == 0)
    return -1;
  struct dirent **v = 0;
  int n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (sel && !sel(e))
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      struct dirent **nv = realloc(v, cap * sizeof(*v));
      if (nv == 0)
        goto fail;
      v = nv;
    }
    v[n] = malloc(sizeof(**v));
    if (v[n] == 0)
      goto fail;
    *v[n] = *e;
    n++;
  }
  closedir(d);
  if (cmp && n > 1)
    qsort(v, n, sizeof(*v), (int (*)(const void *, const void *))cmp);
  *list = v;
  return n;

fail:
  closedir(d);
  for (int i = 0; i < n; i++)
    free(v[i]);
  free(v);
  return -1;
}
