//
// the parts of <unistd.h>, <sys/stat.h> and their neighbours that are
// not system calls of their own: either a call built out of others, or
// an answer this system already knows.
//
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdarg.h>
#include "libc.h"

// the on-disk directory entry, spelled out here rather than taken from
// the kernel's fs.h, whose own struct dirent is a different thing from
// the one <dirent.h> defines.
#define DISK_NAMELEN 14
struct disk_dirent {
  unsigned short inum;
  char name[DISK_NAMELEN];
};

// ---- files ----

int
stat(const char *path, struct stat *sb)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    return -1;
  int r = fstat(fd, sb);
  int e = errno;
  close(fd);
  errno = e;
  return r;
}

// there are no symbolic links yet, so this is stat.
int
lstat(const char *path, struct stat *sb)
{
  return stat(path, sb);
}

int
fstatat(int dirfd, const char *path, struct stat *sb, int flags)
{
  (void)flags;
  if (dirfd != AT_FDCWD && path[0] != '/') {
    errno = ENOSYS;
    return -1;
  }
  return stat(path, sb);
}

// the inode type of a stat, as the mode POSIX programs expect.
mode_t
__type2mode(short type)
{
  switch (type) {
  case T_DIR:
    return S_IFDIR | 0755;
  case T_DEVICE:
    return S_IFCHR | 0666;
  default:
    return S_IFREG | 0644;
  }
}

int
access(const char *path, int mode)
{
  struct stat sb;

  (void)mode; // permissions arrive with users and groups
  return stat(path, &sb) < 0 ? -1 : 0;
}

// xv6's unlink removes an empty directory as well as a file.
int
rmdir(const char *path)
{
  struct stat sb;

  if (stat(path, &sb) < 0)
    return -1;
  if (sb.type != T_DIR) {
    errno = ENOTDIR;
    return -1;
  }
  return unlink(path);
}

int
truncate(const char *path, off_t len)
{
  int fd = open(path, O_WRONLY);

  if (fd < 0)
    return -1;
  int r = ftruncate(fd, len);
  int e = errno;
  close(fd);
  errno = e;
  return r;
}

// fsync is a real syscall now: the file system writes this file's data
// and then commits the metadata that names it, in that order.
int
fdatasync(int fd)
{
  return fsync(fd);
}

int
symlink(const char *from, const char *to)
{
  (void)from;
  (void)to;
  errno = ENOSYS;
  return -1;
}

ssize_t
readlink(const char *path, char *buf, size_t n)
{
  (void)path;
  (void)buf;
  (void)n;
  errno = EINVAL; // nothing here is a symbolic link
  return -1;
}

int
chmod(const char *path, mode_t mode)
{
  (void)path;
  (void)mode;
  return 0; // permissions are not enforced yet: succeed quietly
}

int
fchmod(int fd, mode_t mode)
{
  (void)fd;
  (void)mode;
  return 0;
}

int
chown(const char *path, uid_t u, gid_t g)
{
  (void)path;
  (void)u;
  (void)g;
  return 0;
}

int
fchown(int fd, uid_t u, gid_t g)
{
  (void)fd;
  (void)u;
  (void)g;
  return 0;
}

int
lchown(const char *path, uid_t u, gid_t g)
{
  return chown(path, u, g);
}

int
creat(const char *path, mode_t mode)
{
  (void)mode;
  return open(path, O_CREAT | O_WRONLY | O_TRUNC);
}

int
mkdirat(int dirfd, const char *path, mode_t mode)
{
  (void)mode;
  if (dirfd != AT_FDCWD && path[0] != '/') {
    errno = ENOSYS;
    return -1;
  }
  return mkdir(path, 0777);
}

int
openat(int dirfd, const char *path, int flags, ...)
{
  if (dirfd != AT_FDCWD && path[0] != '/') {
    errno = ENOSYS;
    return -1;
  }
  return open(path, flags);
}

// walk up from the working directory, finding each name in its parent
// by inode number, until the root. without a system call for it, this
// is how the path is recovered.
char *
getcwd(char *buf, size_t size)
{
  char path[PATH_MAX];
  char out[PATH_MAX];
  size_t outlen = 0;
  struct stat here, up, ent;

  if (buf && size == 0) {
    errno = EINVAL;
    return 0;
  }
  if (stat(".", &here) < 0)
    return 0;
  strcpy(path, ".");

  for (int depth = 0; depth < 64; depth++) {
    size_t plen = strlen(path);
    if (plen + 4 > sizeof(path)) {
      errno = ENAMETOOLONG;
      return 0;
    }
    strcpy(path + plen, "/..");
    if (stat(path, &up) < 0)
      return 0;
    if (up.ino == here.ino) // the root is its own parent
      break;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
      return 0;
    struct disk_dirent de;
    int found = 0;
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0 || de.inum != here.ino)
        continue;
      if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
        continue;
      // make sure it is really this directory and not a hard link
      char child[PATH_MAX];
      size_t n = strlen(path);
      if (n + 1 + DISK_NAMELEN + 1 > sizeof(child))
        break;
      memcpy(child, path, n);
      child[n] = '/';
      memcpy(child + n + 1, de.name, DISK_NAMELEN);
      child[n + 1 + DISK_NAMELEN] = 0;
      if (stat(child, &ent) < 0 || ent.ino != here.ino)
        continue;
      size_t namelen = strnlen(de.name, DISK_NAMELEN);
      if (outlen + namelen + 1 > sizeof(out)) {
        close(fd);
        errno = ENAMETOOLONG;
        return 0;
      }
      // the components come out deepest first, so build the path backwards
      memmove(out + namelen + 1, out, outlen);
      out[0] = '/';
      memcpy(out + 1, de.name, namelen);
      outlen += namelen + 1;
      found = 1;
      break;
    }
    close(fd);
    if (!found) {
      errno = ENOENT;
      return 0;
    }
    here = up;
  }

  if (outlen == 0) {
    out[0] = '/';
    outlen = 1;
  }
  out[outlen] = 0;
  if (buf == 0) {
    buf = malloc(outlen + 1);
    if (buf == 0)
      return 0;
  } else if (outlen + 1 > size) {
    errno = ERANGE;
    return 0;
  }
  memcpy(buf, out, outlen + 1);
  return buf;
}

char *
getwd(char *buf)
{
  return getcwd(buf, PATH_MAX);
}

int
fchdir(int fd)
{
  (void)fd;
  errno = ENOSYS;
  return -1;
}

// ---- terminals ----

int
isatty(int fd)
{
  struct termios t;

  if (ioctl(fd, TCGETS, &t) < 0) {
    errno = ENOTTY;
    return 0;
  }
  return 1;
}

char *
ttyname(int fd)
{
  static char name[] = "/console";

  return isatty(fd) ? name : 0;
}

int
ttyname_r(int fd, char *buf, size_t n)
{
  char *s = ttyname(fd);

  if (s == 0)
    return ENOTTY;
  if (strlen(s) + 1 > n)
    return ERANGE;
  strcpy(buf, s);
  return 0;
}

pid_t
tcgetpgrp(int fd)
{
  int pgid;

  if (ioctl(fd, TIOCGPGRP, &pgid) < 0)
    return -1;
  return pgid;
}

int
tcsetpgrp(int fd, pid_t pgid)
{
  return ioctl(fd, TIOCSPGRP, &pgid);
}

pid_t
tcgetsid(int fd)
{
  (void)fd;
  return getsid(0);
}

int
tcgetattr(int fd, struct termios *t)
{
  return ioctl(fd, TCGETS, t);
}

int
tcsetattr(int fd, int action, const struct termios *t)
{
  unsigned long req = action == TCSADRAIN ? TCSETSW : (action == TCSAFLUSH ? TCSETSF : TCSETS);

  return ioctl(fd, req, (void *)t);
}

int
tcflush(int fd, int q)
{
  return ioctl(fd, TCFLSH, (void *)(long)q);
}

int
tcdrain(int fd)
{
  return ioctl(fd, TCSETSW, 0) < 0 ? -1 : 0;
}

int
tcflow(int fd, int action)
{
  return ioctl(fd, TCXONC, (void *)(long)action);
}

int
tcsendbreak(int fd, int dur)
{
  (void)fd;
  (void)dur;
  return 0;
}

speed_t
cfgetispeed(const struct termios *t)
{
  return t->c_ispeed;
}

speed_t
cfgetospeed(const struct termios *t)
{
  return t->c_ospeed;
}

int
cfsetispeed(struct termios *t, speed_t s)
{
  t->c_ispeed = s;
  return 0;
}

int
cfsetospeed(struct termios *t, speed_t s)
{
  t->c_ospeed = s;
  return 0;
}

int
cfsetspeed(struct termios *t, speed_t s)
{
  t->c_ispeed = t->c_ospeed = s;
  return 0;
}

void
cfmakeraw(struct termios *t)
{
  t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~CSIZE;
  t->c_cflag |= CS8;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

// ---- waiting ----

unsigned int
sleep(unsigned int seconds)
{
  struct timespec req = {seconds, 0}, rem;

  if (nanosleep(&req, &rem) < 0 && errno == EINTR)
    return rem.tv_sec;
  return 0;
}

int
usleep(useconds_t usec)
{
  struct timespec req = {usec / 1000000, (long)(usec % 1000000) * 1000};

  return nanosleep(&req, 0);
}

// ---- running programs ----

int
execv(const char *path, char *const argv[])
{
  extern char **environ;

  return execve(path, argv, environ);
}

static int
exec_with_path(const char *file, char *const argv[], char *const envp[])
{
  if (strchr(file, '/'))
    return execve(file, argv, envp);

  const char *path = getenv("PATH");
  if (path == 0)
    path = "/:/bin";

  char buf[PATH_MAX];
  int saved = ENOENT;
  while (*path) {
    const char *end = strchr(path, ':');
    size_t len = end ? (size_t)(end - path) : strlen(path);
    if (len + 1 + strlen(file) + 1 > sizeof(buf)) {
      path = end ? end + 1 : path + len;
      continue;
    }
    memcpy(buf, path, len);
    if (len && buf[len - 1] != '/')
      buf[len++] = '/';
    strcpy(buf + len, file);
    execve(buf, argv, envp);
    if (errno != ENOENT)
      saved = errno;
    if (end == 0)
      break;
    path = end + 1;
  }
  errno = saved;
  return -1;
}

int
execvp(const char *file, char *const argv[])
{
  extern char **environ;

  return exec_with_path(file, argv, environ);
}

int
execvpe(const char *file, char *const argv[], char *const envp[])
{
  return exec_with_path(file, argv, envp);
}

// the varargs forms gather the arguments into an array first.
#define MAXEXECARG 64

static int
gather(va_list ap, const char *arg, char **argv, int max)
{
  int n = 0;

  argv[n++] = (char *)arg;
  while (n < max && (argv[n] = va_arg(ap, char *)) != 0)
    n++;
  argv[n] = 0;
  return n;
}

int
execl(const char *path, const char *arg, ...)
{
  char *argv[MAXEXECARG + 1];
  va_list ap;

  va_start(ap, arg);
  gather(ap, arg, argv, MAXEXECARG);
  va_end(ap);
  return execv(path, argv);
}

int
execlp(const char *file, const char *arg, ...)
{
  char *argv[MAXEXECARG + 1];
  va_list ap;

  va_start(ap, arg);
  gather(ap, arg, argv, MAXEXECARG);
  va_end(ap);
  return execvp(file, argv);
}

int
execle(const char *path, const char *arg, ...)
{
  char *argv[MAXEXECARG + 1];
  va_list ap;

  va_start(ap, arg);
  int n = gather(ap, arg, argv, MAXEXECARG);
  (void)n;
  char **envp = va_arg(ap, char **);
  va_end(ap);
  return execve(path, argv, envp);
}

pid_t
vfork(void)
{
  return fork(); // a real vfork needs the parent to block; fork is safe
}

// ---- identity, which users and groups will fill in ----

uid_t
getuid(void)
{
  return 0;
}

uid_t
geteuid(void)
{
  return 0;
}

gid_t
getgid(void)
{
  return 0;
}

gid_t
getegid(void)
{
  return 0;
}

int
setuid(uid_t u)
{
  return u == 0 ? 0 : (errno = EPERM, -1);
}

int
seteuid(uid_t u)
{
  return setuid(u);
}

int
setgid(gid_t g)
{
  return g == 0 ? 0 : (errno = EPERM, -1);
}

int
setegid(gid_t g)
{
  return setgid(g);
}

int
setreuid(uid_t r, uid_t e)
{
  return (r == 0 || r == (uid_t)-1) && (e == 0 || e == (uid_t)-1) ? 0 : (errno = EPERM, -1);
}

int
setregid(gid_t r, gid_t e)
{
  return (r == 0 || r == (gid_t)-1) && (e == 0 || e == (gid_t)-1) ? 0 : (errno = EPERM, -1);
}

int
getgroups(int n, gid_t *list)
{
  if (n > 0 && list)
    list[0] = 0;
  return n > 0 ? 1 : 1;
}

int
setgroups(size_t n, const gid_t *list)
{
  (void)n;
  (void)list;
  return 0;
}

char *
getlogin(void)
{
  char *user = getenv("USER");

  return user ? user : (char *)"root";
}

int
setpgrp(void)
{
  return setpgid(0, 0);
}

pid_t
getpgrp(void)
{
  return getpgid(0);
}

// ---- limits and resources ----

int
getpagesize(void)
{
  return (int)sysconf(_SC_PAGESIZE);
}

int
getdtablesize(void)
{
  return OPEN_MAX;
}

long
fpathconf(int fd, int name)
{
  (void)fd;
  switch (name) {
  case 0: return 32;   // _PC_LINK_MAX
  case 1: return 256;  // _PC_MAX_CANON
  case 4: return NAME_MAX;
  case 5: return PATH_MAX;
  case 6: return PIPE_BUF;
  default: return -1;
  }
}

long
pathconf(const char *path, int name)
{
  (void)path;
  return fpathconf(-1, name);
}

int
nice(int inc)
{
  (void)inc;
  return 0;
}

int
getrlimit(int res, struct rlimit *rl)
{
  rl->rlim_cur = rl->rlim_max = RLIM_INFINITY;
  switch (res) {
  case RLIMIT_NOFILE:
    rl->rlim_cur = rl->rlim_max = OPEN_MAX;
    break;
  case RLIMIT_STACK:
    rl->rlim_cur = rl->rlim_max = 8UL << 20;
    break;
  default:
    break;
  }
  return 0;
}

int
setrlimit(int res, const struct rlimit *rl)
{
  (void)res;
  (void)rl;
  return 0;
}

int
getpriority(int which, id_t who)
{
  (void)which;
  (void)who;
  return 0;
}

int
setpriority(int which, id_t who, int prio)
{
  (void)which;
  (void)who;
  (void)prio;
  return 0;
}

int
gettimeofday(struct timeval *tv, void *tz)
{
  struct timespec ts;

  (void)tz;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    return -1;
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;
  return 0;
}

int
clock_getres(clockid_t clk, struct timespec *ts)
{
  (void)clk;
  ts->tv_sec = 0;
  ts->tv_nsec = 1000000000L / (long)sysconf(_SC_CLK_TCK);
  return 0;
}

int
msync(void *addr, size_t len, int flags)
{
  (void)addr;
  (void)len;
  (void)flags;
  sync();
  return 0;
}

int
madvise(void *addr, size_t len, int advice)
{
  (void)addr;
  (void)len;
  (void)advice;
  return 0;
}

int
mlock(const void *addr, size_t len)
{
  (void)addr;
  (void)len;
  return 0;
}

int
munlock(const void *addr, size_t len)
{
  (void)addr;
  (void)len;
  return 0;
}

// ---- names ----

char *
basename(char *path)
{
  static char dot[] = ".";

  if (path == 0 || *path == 0)
    return dot;
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] == '/')
    path[--n] = 0;
  char *p = strrchr(path, '/');
  if (p == 0)
    return path;
  if (p == path && path[1] == 0)
    return path; // "/"
  return p + 1;
}

char *
dirname(char *path)
{
  static char dot[] = ".";
  static char root[] = "/";

  if (path == 0 || *path == 0)
    return dot;
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] == '/')
    path[--n] = 0;
  char *p = strrchr(path, '/');
  if (p == 0)
    return dot;
  if (p == path)
    return root;
  *p = 0;
  return path;
}
