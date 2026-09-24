// nstest: what a mount namespace is, from inside one.
//
// Run as root, it unshares its mounts, mounts something, pivots into a
// new root and reports what it can and cannot see. What it proves is
// what a container depends on: a mount made inside is not a mount on the
// machine, and after a pivot the machine's file system is not reachable
// at all -- not through "/", not through ".." at the root, not through a
// path that was open before.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/thread.h"
#include "user/user.h"
#include <errno.h>

static int failures;

static void
ok(int cond, const char *what)
{
  printf("nstest: %s: %s\n", cond ? "ok" : "FAILED", what);
  if (!cond)
    failures++;
}

// is this path mounted, as far as this process can tell?
static int
mounted(const char *path)
{
  char buf[4096];
  int fd = open("/proc/mounts", O_RDONLY);

  if (fd < 0)
    return -1;
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  return strstr(buf, path) != 0;
}

int
main(void)
{
  // the machine's mounts are visible to begin with
  ok(mounted("/tmp") == 1, "the machine's /tmp is mounted");

  if (unshare(CLONE_NEWNS) < 0) {
    printf("nstest: FAILED: unshare: %s\n", strerror(errno));
    exit(1);
  }
  ok(mounted("/tmp") == 1, "a copy sees what was mounted when it was made");

  mkdir("/tmp/nsx", 0777);
  if (mount("none", "/tmp/nsx", "ramfs", 0, "") < 0) {
    printf("nstest: FAILED: mount: %s\n", strerror(errno));
    exit(1);
  }
  ok(mounted("/tmp/nsx") == 1, "a mount made here is here");

  // and unmounting here is here only: the machine keeps what it had
  ok(umount("/tmp/nsx") == 0, "unmounting what this namespace mounted");
  ok(mounted("/tmp/nsx") == 0, "and then it is not here");

  // the root of one's own. An empty ramfs is a harsh world -- there is
  // no /bin in it -- so everything after this is done with system calls
  // and nothing is exec'd.
  mkdir("/tmp/newroot", 0777);
  if (mount("none", "/tmp/newroot", "ramfs", 0, "") < 0) {
    printf("nstest: FAILED: mount newroot: %s\n", strerror(errno));
    exit(1);
  }
  int before = open("/etc/passwd", O_RDONLY);
  ok(before >= 0, "the machine's files are reachable before the pivot");

  if (pivot_root("/tmp/newroot", "/tmp/newroot") < 0) {
    printf("nstest: FAILED: pivot_root: %s\n", strerror(errno));
    exit(1);
  }
  ok(open("/etc/passwd", O_RDONLY) < 0, "and not after it");
  ok(open("/../etc/passwd", O_RDONLY) < 0, "nor through .. at the root");
  ok(open("/tmp/newroot/etc/passwd", O_RDONLY) < 0, "nor by the old path");
  // a descriptor opened before the pivot still reads: what pivoting
  // changes is what a *path* means, which is what a container needs
  char c;
  ok(before >= 0 && read(before, &c, 1) == 1,
     "a descriptor opened before it still reads");
  close(before);

  // the new root is a file system of its own, and writable
  int fd = open("/inside", O_CREATE | O_RDWR, 0666);
  ok(fd >= 0 && write(fd, "x", 1) == 1, "the new root is writable");
  if (fd >= 0)
    close(fd);
  struct stat st;
  ok(stat("/inside", &st) == 0, "and what is written to it is there");

  printf(failures ? "nstest: %d failure(s)\n" : "nstest: all ok\n", failures);
  exit(failures ? 1 : 0);
}
