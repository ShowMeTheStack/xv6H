// split: one file into several.
//
//   split [-l LINES] [-b SIZE] [-a N] [-d] [FILE [PREFIX]]
//
// the pieces are named PREFIX followed by a suffix that counts up --
// aa, ab, ac by default, or 00, 01, 02 with -d. PREFIX is "x" when
// none is given, which is what split has always called them.
#include "user/user.h"

static int suffix_len = 2;
static int numeric;
static const char *prefix = "x";
static long piece = 0;

static void
suffix_of(long n, char *out)
{
  // the suffix counts in base 26 (or ten), most significant first, so
  // that the names sort in the order the pieces were written
  int base = numeric ? 10 : 26;
  for (int i = suffix_len - 1; i >= 0; i--) {
    out[i] = (char)((numeric ? '0' : 'a') + (n % base));
    n /= base;
  }
  out[suffix_len] = 0;
}

static int
next_piece(void)
{
  char name[PATH_MAX], sfx[32];

  if (suffix_len > 30)
    suffix_len = 30;
  suffix_of(piece++, sfx);
  snprintf(name, sizeof(name), "%s%s", prefix, sfx);
  int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fprintf(stderr, "split: %s: %s\n", name, strerror(errno));
    exit(1);
  }
  return fd;
}

// a size may be plain, or carry a k/m/g suffix
static long long
parse_size(const char *p)
{
  long long v = 0;

  while (*p >= '0' && *p <= '9')
    v = v * 10 + (*p++ - '0');
  switch (*p) {
  case 'k': case 'K': v *= 1024; break;
  case 'm': case 'M': v *= 1024 * 1024; break;
  case 'g': case 'G': v *= 1024LL * 1024 * 1024; break;
  case 'b': v *= 512; break;
  }
  return v;
}

int
main(int argc, char *argv[])
{
  long long lines = 1000, bytes = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (a[1] == 'l' || a[1] == 'b' || a[1] == 'a') {
      const char *val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
      if (val == 0) {
        fprintf(stderr, "split: -%c wants a number\n", a[1]);
        exit(1);
      }
      if (a[1] == 'l') { lines = parse_size(val); bytes = 0; }
      else if (a[1] == 'b') { bytes = parse_size(val); lines = 0; }
      else suffix_len = atoi(val);
      continue;
    }
    if (a[1] == 'd') {
      numeric = 1;
      continue;
    }
    if (a[1] >= '0' && a[1] <= '9') {
      lines = parse_size(a + 1);
      bytes = 0;
      continue;
    }
    fprintf(stderr, "usage: split [-l N] [-b SIZE] [-a N] [-d] [FILE [PREFIX]]\n");
    exit(1);
  }

  FILE *f = stdin;
  if (i < argc && strcmp(argv[i], "-") != 0) {
    f = fopen(argv[i], "r");
    if (f == 0) {
      fprintf(stderr, "split: %s: %s\n", argv[i], strerror(errno));
      exit(1);
    }
  }
  if (i < argc)
    i++;
  if (i < argc)
    prefix = argv[i];

  int fd = -1;
  if (bytes > 0) {
    static char buf[8192];
    long long in_piece = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      size_t off = 0;
      while (off < n) {
        if (fd < 0) {
          fd = next_piece();
          in_piece = 0;
        }
        long long room = bytes - in_piece;
        size_t take = n - off;
        if ((long long)take > room)
          take = (size_t)room;
        if (write(fd, buf + off, take) != (int)take) {
          fprintf(stderr, "split: write: %s\n", strerror(errno));
          exit(1);
        }
        off += take;
        in_piece += take;
        if (in_piece >= bytes) {
          close(fd);
          fd = -1;
        }
      }
    }
  } else {
    static char line[65536];
    long long in_piece = 0;
    while (fgets(line, sizeof(line), f)) {
      if (fd < 0) {
        fd = next_piece();
        in_piece = 0;
      }
      int len = strlen(line);
      if (write(fd, line, len) != len) {
        fprintf(stderr, "split: write: %s\n", strerror(errno));
        exit(1);
      }
      if (line[len - 1] == '\n' && ++in_piece >= lines) {
        close(fd);
        fd = -1;
      }
    }
  }
  if (fd >= 0)
    close(fd);
  if (f != stdin)
    fclose(f);
  exit(0);
}
