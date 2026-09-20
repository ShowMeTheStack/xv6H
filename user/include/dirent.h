#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <limits.h>

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR  2
#define DT_DIR  4
#define DT_BLK  6
#define DT_REG  8
#define DT_LNK  10
#define DT_SOCK 12

struct dirent {
  ino_t d_ino;
  off_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[NAME_MAX + 1];
};

typedef struct __DIR DIR;

DIR *opendir(const char *path);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *d);
int readdir_r(DIR *d, struct dirent *entry, struct dirent **result);
void rewinddir(DIR *d);
void seekdir(DIR *d, long off);
long telldir(DIR *d);
int closedir(DIR *d);
int dirfd(DIR *d);
int scandir(const char *path, struct dirent ***list, int (*sel)(const struct dirent *),
            int (*cmp)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);
#endif
