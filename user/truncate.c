// truncate and fallocate: a file's length, set directly.
//
//   truncate [-c] [-s SIZE] FILE...
//   fallocate [-l LENGTH] [-o OFFSET] FILE...
//
// SIZE may be a plain number, or +N / -N to move the length relative
// to what it is now, or %N to round it down to a multiple.
//
// this system has no way to reserve blocks without writing them, so
// fallocate grows the file to offset+length and leaves the new part a
// hole, which is what reading it will show.
#include "user/user.h"

static int no_create;   // -c

static long long
parse_size(const char *p, int *relative)
{
  long long v = 0;
  int sign = 1;

  *relative = 0;
  if (*p == '+') { *relative = 1; p++; }
  else if (*p == '-') { *relative = 1; sign = -1; p++; }
  else if (*p == '%') { *relative = 2; p++; }
  while (*p >= '0' && *p <= '9')
    v = v * 10 + (*p++ - '0');
  switch (*p) {
  case 'K': case 'k': v *= 1024; break;
  case 'M': case 'm': v *= 1024 * 1024; break;
  case 'G': case 'g': v *= 1024LL * 1024 * 1024; break;
  }
  return v * sign;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  long long size = 0, offset = 0;
  int have_size = 0, relative = 0, i = 1, status = 0;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  int is_fallocate = strcmp(me, "fallocate") == 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (a[1] == 's' || a[1] == 'l' || a[1] == 'o') {
      const char *val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
      if (val == 0) {
        fprintf(stderr, "%s: -%c wants a size\n", me, a[1]);
        exit(1);
      }
      if (a[1] == 'o')
        offset = parse_size(val, &relative);
      else {
        size = parse_size(val, &relative);
        have_size = 1;
      }
      continue;
    }
    if (a[1] == 'c') {
      no_create = 1;
      continue;
    }
    if (is_fallocate && (a[1] == 'n' || a[1] == 'p' || a[1] == 'z'))
      continue; // keep-size, punch-hole and zero-range have no meaning here
    fprintf(stderr, is_fallocate ? "usage: fallocate -l LEN [-o OFF] FILE...\n"
                                 : "usage: truncate [-c] -s SIZE FILE...\n");
    exit(1);
  }
  if (!have_size || i >= argc) {
    fprintf(stderr, is_fallocate ? "usage: fallocate -l LEN [-o OFF] FILE...\n"
                                 : "usage: truncate [-c] -s SIZE FILE...\n");
    exit(1);
  }

  for (; i < argc; i++) {
    struct stat st;
    int exists = stat(argv[i], &st) >= 0;
    if (!exists && no_create)
      continue; // -c: a file that is not there is left alone
    long long want;
    if (is_fallocate) {
      want = offset + size;
      if (exists && (long long)st.size > want)
        want = st.size; // fallocate never makes a file shorter
    } else if (relative == 1) {
      want = (exists ? (long long)st.size : 0) + size;
    } else if (relative == 2) {
      long long cur = exists ? (long long)st.size : 0;
      want = size > 0 ? (cur / size) * size : cur;
    } else {
      want = size;
    }
    if (want < 0)
      want = 0;

    int fd = open(argv[i], O_WRONLY | (no_create ? 0 : O_CREAT), 0644);
    if (fd < 0) {
      fprintf(stderr, "%s: %s: %s\n", me, argv[i], strerror(errno));
      status = 1;
      continue;
    }
    if (ftruncate(fd, (off_t)want) < 0) {
      fprintf(stderr, "%s: %s: %s\n", me, argv[i], strerror(errno));
      status = 1;
    }
    close(fd);
  }
  exit(status);
}
