// nsenter: run a command in another process's namespaces.
//
//   nsenter -t PID [-a] [-u] [-p] [-m] [-U] [-n] [CMD [ARGS...]]
//
// The namespace is named by a descriptor for /proc/PID/ns/<kind>, which
// is what setns(2) takes: the file names the process and the kind, and
// the kernel finds the namespace from those. Without a command this runs
// a shell, which is what nsenter is usually wanted for.
//
// The order the kinds are joined in is not a matter of taste. The user
// namespace goes last, because joining it is what takes away the
// machine-wide privilege the others need; and the pid namespace goes
// just before it, because after joining one, a host process number is no
// longer a number this process can name.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/thread.h"
#include "user/user.h"
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static void
usage(void)
{
  fprintf(stderr, "usage: nsenter -t PID [-a] [-u] [-p] [-m] [-U] [-n] "
                  "[CMD [ARGS...]]\n");
  exit(1);
}

// in the order they have to be joined
static const struct {
  const char *name;      // /proc/PID/ns/<name>
  const char *opt;
  const char *lopt;
  int flag;
} kinds[] = {
    {"net", "-n", "--net", CLONE_NEWNET},
    {"mnt", "-m", "--mount", CLONE_NEWNS},
    {"uts", "-u", "--uts", CLONE_NEWUTS},
    {"pid", "-p", "--pid", CLONE_NEWPID},
    {"user", "-U", "--user", CLONE_NEWUSER},
};
#define NKINDS (int)(sizeof(kinds) / sizeof(kinds[0]))

int
main(int argc, char *argv[])
{
  int pid = -1, want[NKINDS], any = 0, i = 1;

  for (int k = 0; k < NKINDS; k++)
    want[k] = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      pid = atoi(argv[++i]);
      continue;
    }
    if (strncmp(argv[i], "--target=", 9) == 0) {
      pid = atoi(argv[i] + 9);
      continue;
    }
    if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
      for (int k = 0; k < NKINDS; k++)
        want[k] = 1;
      any = 1;
      continue;
    }
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    int found = 0;
    for (int k = 0; k < NKINDS; k++)
      if (strcmp(argv[i], kinds[k].opt) == 0 ||
          strcmp(argv[i], kinds[k].lopt) == 0) {
        want[k] = 1;
        any = found = 1;
      }
    if (!found)
      usage();
  }
  if (pid <= 0)
    usage();
  if (!any)
    for (int k = 0; k < NKINDS; k++)
      want[k] = 1;              // asking for none means asking for all

  // all of them opened before any of them is joined: the path names the
  // process by a number this one can see it by, and joining its pid
  // namespace takes that number away.
  int fds[NKINDS];
  for (int k = 0; k < NKINDS; k++) {
    fds[k] = -1;
    if (!want[k])
      continue;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, kinds[k].name);
    if ((fds[k] = open(path, O_RDONLY)) < 0) {
      fprintf(stderr, "nsenter: %s: %s\n", path, strerror(errno));
      exit(1);
    }
  }
  for (int k = 0; k < NKINDS; k++) {
    if (fds[k] < 0)
      continue;
    if (setns(fds[k], kinds[k].flag) < 0) {
      fprintf(stderr, "nsenter: %s: %s\n", kinds[k].name, strerror(errno));
      exit(1);
    }
    close(fds[k]);
  }

  if (i >= argc) {
    char *sh[] = {"/bin/sh", 0};
    execvp(sh[0], sh);
    fprintf(stderr, "nsenter: /bin/sh: %s\n", strerror(errno));
    exit(1);
  }
  execvp(argv[i], argv + i);      // the PATH, as every such tool does
  fprintf(stderr, "nsenter: %s: %s\n", argv[i], strerror(errno));
  exit(1);
}
