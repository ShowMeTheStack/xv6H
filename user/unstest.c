// unstest: a user namespace from inside one.
//
// What it proves is the container model: a container's root is an ordinary
// user of the machine. Inside its namespace it is root -- it may own its
// files, signal its processes, mount in its own mount namespace -- and
// outside it is whoever it was, with no more claim on the machine than
// before. The kernel keeps the machine's numbers and translates only at
// the edges, so a file it cannot name an owner for is owned by nobody.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/thread.h"
#include "user/user.h"
#include <errno.h>
#include <sys/wait.h>

static int failures;

static void
ok(int cond, const char *what)
{
  printf("unstest: %s: %s\n", cond ? "ok" : "FAILED", what);
  if (!cond)
    failures++;
}

int
main(int argc, char *argv[])
{
  // the child does the unsharing; the parent writes its map, because a
  // namespace that could widen its own mapping would be no boundary at
  // all. They talk over a pipe: the child says "made", the parent says
  // "mapped".
  int a[2], b[2];
  if (pipe(a) < 0 || pipe(b) < 0) {
    printf("unstest: FAILED: pipe\n");
    exit(1);
  }
  int pid = fork();
  if (pid < 0) {
    printf("unstest: FAILED: fork\n");
    exit(1);
  }
  if (pid == 0) {
    close(a[0]);
    close(b[1]);
    // an ordinary user may make one of these: that is the point of them
    if (unshare(CLONE_NEWUSER) < 0) {
      printf("unstest: FAILED: unshare: %s\n", strerror(errno));
      exit(1);
    }
    // before the map is written, this process is nobody in its own
    // namespace: its number there has no meaning yet
    ok(getuid() == 65534, "nobody until the map is written");
    char c = 'm';
    write(a[1], &c, 1);
    if (read(b[0], &c, 1) != 1) {
      printf("unstest: FAILED: no map\n");
      exit(1);
    }
    // and now 0 means something: the host user the map points at
    ok(setuid(0) == 0, "becoming root inside");
    ok(getuid() == 0, "and being it");
    // root inside may do what belongs to its own namespaces
    ok(unshare(CLONE_NEWUTS) == 0, "a name of its own");
    ok(sethostname("inside", 6) == 0, "and setting it");
    char hn[64];
    hn[0] = 0;
    gethostname(hn, sizeof(hn));
    ok(strcmp(hn, "inside") == 0, "and reading it back");
    // and not what belongs to the machine: rebooting it, for one, and
    // the machine's own mount namespace for another
    ok(mount("none", "/mnt", "ramfs", 0, "") < 0,
       "mounting in the machine's namespace is refused");
    ok(reboot(0) < 0, "rebooting the machine is refused");
    // a file it makes is its own inside...
    int fd = open("/tmp/unsmine", O_CREATE | O_RDWR, 0600);
    ok(fd >= 0, "making a file");
    if (fd >= 0)
      close(fd);
    struct stat st;
    ok(stat("/tmp/unsmine", &st) == 0 && st.uid == 0,
       "which it owns, inside");
    // ...and the machine's files are owned by nobody it can name
    ok(stat("/etc/passwd", &st) == 0 && st.uid == 65534,
       "and the machine's files are nobody's");
    exit(failures ? 1 : 0);
  }

  close(a[1]);
  close(b[0]);
  char c;
  if (read(a[0], &c, 1) != 1) {
    printf("unstest: FAILED: the child said nothing\n");
    exit(1);
  }
  // the map: inside 0..999 is host 4000..4999. 4000 is nobody in
  // particular on this machine, which is the point: root inside owns
  // what that user owns and nothing else.
  char path[64], line[64];
  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  int fd = open(path, O_WRONLY);
  int wrote = -1;
  if (fd >= 0) {
    snprintf(line, sizeof(line), "0 4000 1000");
    wrote = write(fd, line, strlen(line));
    close(fd);
  }
  ok(wrote > 0, "writing the child's uid map");
  snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
  fd = open(path, O_WRONLY);
  if (fd >= 0) {
    snprintf(line, sizeof(line), "0 4000 1000");
    write(fd, line, strlen(line));
    close(fd);
  }
  // it may be written once only
  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  fd = open(path, O_WRONLY);
  int again = 0;
  if (fd >= 0) {
    again = write(fd, "0 0 65536", 9);
    close(fd);
  }
  ok(again < 0, "and only once");
  // and it reads back
  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  fd = open(path, O_RDONLY);
  int n = fd >= 0 ? read(fd, line, sizeof(line) - 1) : -1;
  if (fd >= 0)
    close(fd);
  if (n > 0)
    line[n] = 0;
  ok(n > 0 && strstr(line, "4000") != 0, "and reads back as it was written");

  c = 'g';
  write(b[1], &c, 1);
  int st = 0;
  waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    failures++;
  printf(failures ? "unstest: %d failure(s)\n" : "unstest: all ok\n", failures);
  exit(failures ? 1 : 0);
}
