// stat and readlink: what the file system knows about a name.
//
//   stat [-L] [-t] [-c FORMAT] [-f] FILE...
//   readlink [-f] [-n] FILE...
//
// the format letters are the ones people have in their fingers:
//
//   %n name        %s size        %b blocks      %a mode in octal
//   %A mode as rwx %f raw mode    %F kind of file
//   %u uid         %U user name   %g gid         %G group name
//   %i inode       %h links       %d device      %t major  %T minor
//   %x atime       %y mtime       %z ctime       %X %Y %Z as seconds
//   %N name, and what it points at when it is a link
//
// -f asks about the file system the file is on rather than the file.
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <sys/statfs.h>

static int follow;      // -L
static int terse;       // -t
static int about_fs;    // -f
static const char *format;
static int status;

static const char *
kind_of(const struct stat *st)
{
  switch (st->type) {
  case T_DIR: return "directory";
  case T_SYMLINK: return "symbolic link";
  case T_DEVICE:
    return (st->rdev >> 8) == STAT_DEV_BLOCK ? "block special file"
                                             : "character special file";
  }
  return st->size == 0 ? "regular empty file" : "regular file";
}

static void
rwx(const struct stat *st, char *out)
{
  static const char *set = "rwxrwxrwx";
  uint m = st->mode;

  out[0] = st->type == T_DIR ? 'd'
         : st->type == T_SYMLINK ? 'l'
         : st->type == T_DEVICE
             ? ((st->rdev >> 8) == STAT_DEV_BLOCK ? 'b' : 'c')
             : '-';
  for (int i = 0; i < 9; i++)
    out[i + 1] = (m & (1 << (8 - i))) ? set[i] : '-';
  if (m & S_ISUID)
    out[3] = (m & 0100) ? 's' : 'S';
  if (m & S_ISGID)
    out[6] = (m & 0010) ? 's' : 'S';
  if (m & S_ISVTX)
    out[9] = (m & 0001) ? 't' : 'T';
  out[10] = 0;
}

// seconds since the epoch, as a date someone can read
static void
print_time(uint64 t)
{
  char buf[64];
  time_t tt = (time_t)t;
  struct tm tm;

  if (t == 0) {
    printf("-");
    return;
  }
  gmtime_r(&tt, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S +0000", &tm);
  printf("%s", buf);
}

static void
print_format(const char *fmt, const char *path, const struct stat *st)
{
  char modes[12];
  struct passwd *pw;
  struct group *gr;

  for (const char *p = fmt; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
      case 'n': putchar('\n'); break;
      case 't': putchar('\t'); break;
      case '\\': putchar('\\'); break;
      default: putchar(*p);
      }
      continue;
    }
    if (*p != '%') {
      putchar(*p);
      continue;
    }
    p++;
    if (*p == 0)
      break;
    switch (*p) {
    case 'n': printf("%s", path); break;
    case 'N':
      printf("'%s'", path);
      if (st->type == T_SYMLINK) {
        char target[PATH_MAX];
        int n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
          target[n] = 0;
          printf(" -> '%s'", target);
        }
      }
      break;
    case 's': printf("%llu", (unsigned long long)st->size); break;
    case 'b': printf("%llu", (unsigned long long)st->blocks); break;
    case 'B': printf("512"); break;
    case 'a': printf("%o", st->mode & 07777); break;
    case 'A': rwx(st, modes); printf("%s", modes); break;
    case 'f': printf("%x", ST_MODE(*st)); break;
    case 'F': printf("%s", kind_of(st)); break;
    case 'u': printf("%u", st->uid); break;
    case 'U':
      pw = getpwuid(st->uid);
      printf("%s", pw ? pw->pw_name : "UNKNOWN");
      break;
    case 'g': printf("%u", st->gid); break;
    case 'G':
      gr = getgrgid(st->gid);
      printf("%s", gr ? gr->gr_name : "UNKNOWN");
      break;
    case 'i': printf("%u", st->ino); break;
    case 'h': printf("%d", st->nlink); break;
    case 'd': printf("%d", st->dev); break;
    case 't': printf("%x", (st->rdev >> 8) & 0xff); break;
    case 'T': printf("%x", st->rdev & 0xff); break;
    case 'x': print_time(st->atime); break;
    case 'y': print_time(st->mtime); break;
    case 'z': print_time(st->ctime); break;
    case 'X': printf("%llu", (unsigned long long)st->atime); break;
    case 'Y': printf("%llu", (unsigned long long)st->mtime); break;
    case 'Z': printf("%llu", (unsigned long long)st->ctime); break;
    case '%': putchar('%'); break;
    default: putchar('%'); putchar(*p);
    }
  }
}

static void
print_long(const char *path, const struct stat *st)
{
  char modes[12];
  struct passwd *pw = getpwuid(st->uid);
  struct group *gr = getgrgid(st->gid);

  rwx(st, modes);
  printf("  File: '%s'", path);
  if (st->type == T_SYMLINK) {
    char target[PATH_MAX];
    int n = readlink(path, target, sizeof(target) - 1);
    if (n > 0) {
      target[n] = 0;
      printf(" -> '%s'", target);
    }
  }
  printf("\n");
  printf("  Size: %-11llu Blocks: %-8llu IO Block: 4096 %s\n",
         (unsigned long long)st->size, (unsigned long long)st->blocks,
         kind_of(st));
  if (st->type == T_DEVICE)
    printf("Device: %xh/%dd\tInode: %-11u Links: %-5d Device type: %x,%x\n",
           st->dev, st->dev, st->ino, st->nlink,
           (st->rdev >> 8) & 0xff, st->rdev & 0xff);
  else
    printf("Device: %xh/%dd\tInode: %-11u Links: %d\n",
           st->dev, st->dev, st->ino, st->nlink);
  printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
         st->mode & 07777, modes, st->uid, pw ? pw->pw_name : "UNKNOWN",
         st->gid, gr ? gr->gr_name : "UNKNOWN");
  printf("Access: ");
  print_time(st->atime);
  printf("\nModify: ");
  print_time(st->mtime);
  printf("\nChange: ");
  print_time(st->ctime);
  printf("\n");
}

static void
print_terse(const char *path, const struct stat *st)
{
  printf("%s %llu %llu %x %u %u %d %u %d %x %x %llu %llu %llu 512\n",
         path, (unsigned long long)st->size, (unsigned long long)st->blocks,
         ST_MODE(*st), st->uid, st->gid, st->dev, st->ino, st->nlink,
         (st->rdev >> 8) & 0xff, st->rdev & 0xff,
         (unsigned long long)st->atime, (unsigned long long)st->mtime,
         (unsigned long long)st->ctime);
}

static void
do_statfs(const char *path)
{
  struct statfs sf;

  if (statfs(path, &sf) < 0) {
    fprintf(stderr, "stat: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  if (format) {
    // the -f format letters are a smaller set of their own
    for (const char *p = format; *p; p++) {
      if (*p == '\\' && p[1]) {
        p++;
        putchar(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
        continue;
      }
      if (*p != '%') {
        putchar(*p);
        continue;
      }
      switch (*++p) {
      case 'n': printf("%s", path); break;
      case 'b': printf("%llu", (unsigned long long)sf.f_blocks); break;
      case 'f': printf("%llu", (unsigned long long)sf.f_bfree); break;
      case 'a': printf("%llu", (unsigned long long)sf.f_bavail); break;
      case 'c': printf("%llu", (unsigned long long)sf.f_files); break;
      case 'd': printf("%llu", (unsigned long long)sf.f_ffree); break;
      case 's': printf("%ld", (long)sf.f_bsize); break;
      case 'S': printf("%ld", (long)sf.f_bsize); break;
      case 't': printf("%lx", (long)sf.f_type); break;
      case '%': putchar('%'); break;
      default: putchar('%'); putchar(*p);
      }
    }
    printf("\n");
    return;
  }
  if (terse) {
    printf("%s %lx %ld %llu %llu %llu %llu %llu %ld\n", path, (long)sf.f_type,
           (long)sf.f_bsize, (unsigned long long)sf.f_blocks,
           (unsigned long long)sf.f_bfree, (unsigned long long)sf.f_bavail,
           (unsigned long long)sf.f_files, (unsigned long long)sf.f_ffree,
           (long)sf.f_namelen);
    return;
  }
  printf("  File: \"%s\"\n", path);
  printf("    ID: 0        Namelen: %-7ld Type: %lx\n", (long)sf.f_namelen,
         (long)sf.f_type);
  printf("Block size: %-10ld Fundamental block size: %ld\n", (long)sf.f_bsize,
         (long)sf.f_bsize);
  printf("Blocks: Total: %-10llu Free: %-10llu Available: %llu\n",
         (unsigned long long)sf.f_blocks, (unsigned long long)sf.f_bfree,
         (unsigned long long)sf.f_bavail);
  printf("Inodes: Total: %-10llu Free: %llu\n",
         (unsigned long long)sf.f_files, (unsigned long long)sf.f_ffree);
}

static void
do_stat(const char *path)
{
  struct stat st;

  if (about_fs) {
    do_statfs(path);
    return;
  }
  int r = follow ? stat(path, &st) : lstat(path, &st);
  if (r < 0) {
    fprintf(stderr, "stat: cannot stat '%s': %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  if (format) {
    print_format(format, path, &st);
    printf("\n");
  } else if (terse) {
    print_terse(path, &st);
  } else {
    print_long(path, &st);
  }
}

// ---- readlink ----

static int
do_readlink(const char *path, int canonical, int no_newline)
{
  char buf[PATH_MAX];

  if (canonical) {
    if (realpath(path, buf) == 0) {
      status = 1;
      return -1;
    }
  } else {
    int n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) {
      status = 1;
      return -1;
    }
    buf[n] = 0;
  }
  printf("%s%s", buf, no_newline ? "" : "\n");
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  if (strcmp(me, "readlink") == 0) {
    int canonical = 0, no_newline = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
      for (const char *p = argv[i] + 1; *p; p++) {
        if (*p == 'f' || *p == 'e' || *p == 'm')
          canonical = 1;
        else if (*p == 'n')
          no_newline = 1;
        else if (*p == 'q' || *p == 's' || *p == 'v')
          ;
        else {
          fprintf(stderr, "usage: readlink [-f] [-n] FILE...\n");
          exit(1);
        }
      }
    }
    if (i >= argc) {
      fprintf(stderr, "usage: readlink [-f] [-n] FILE...\n");
      exit(1);
    }
    for (; i < argc; i++)
      do_readlink(argv[i], canonical, no_newline);
    exit(status);
  }

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strncmp(argv[i], "-c", 2) == 0 || strncmp(argv[i], "--format", 8) == 0) {
      const char *v = strchr(argv[i], '=');
      if (v)
        format = v + 1;
      else if (argv[i][2])
        format = argv[i] + 2;
      else if (i + 1 < argc)
        format = argv[++i];
      continue;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'L': follow = 1; break;
      case 't': terse = 1; break;
      case 'f': about_fs = 1; break;
      default:
        fprintf(stderr, "usage: stat [-L] [-t] [-f] [-c FORMAT] FILE...\n");
        exit(1);
      }
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: stat [-L] [-t] [-f] [-c FORMAT] FILE...\n");
    exit(1);
  }
  for (; i < argc; i++)
    do_stat(argv[i]);
  exit(status);
}
