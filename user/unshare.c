// unshare: run a command with namespaces of its own.
//
//   unshare [-u|--uts] [-p|--pid] [-f|--fork] [--hostname NAME]
//           [CMD [ARGS...]]
//
// A namespace is a view of the machine that can differ from process to
// process. A command run under `unshare -u' may call the machine
// whatever it likes without the rest of it hearing about it; one run
// under `unshare -p' is process number 1, with its own numbering under
// it.
//
// The command usually runs in this process, so what it is given -- the
// terminal, the working directory, the descriptors -- is what unshare
// was given. A new pid namespace is the exception: the caller cannot
// move into one (its number outside is known to whoever is waiting for
// it), so the command runs in a child, and unshare waits for it. That is
// what --fork means, and -p implies it.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/thread.h"
#include "user/user.h"
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

static void
usage(void)
{
  fprintf(stderr, "usage: unshare [-u|--uts] [-p|--pid] [-f|--fork]"
                  " [-m|--mount] [-U|--user]\n"
                  "               [--hostname NAME] [CMD [ARGS...]]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  int flags = 0, dofork = 0;
  const char *name = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--uts") == 0) {
      flags |= CLONE_NEWUTS;
    } else if (strcmp(argv[i], "-U") == 0 || strcmp(argv[i], "--user") == 0) {
      flags |= CLONE_NEWUSER;
    } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mount") == 0) {
      flags |= CLONE_NEWNS;
    } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pid") == 0) {
      flags |= CLONE_NEWPID;
      dofork = 1;              // the caller cannot go there itself
    } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fork") == 0) {
      dofork = 1;
    } else if (strcmp(argv[i], "--hostname") == 0 && i + 1 < argc) {
      name = argv[++i];
      flags |= CLONE_NEWUTS;     // a name of its own implies a namespace
    } else if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    } else {
      usage();
    }
  }
  if (flags == 0)
    usage();
  if (unshare(flags) < 0) {
    fprintf(stderr, "unshare: %s\n", strerror(errno));
    exit(1);
  }
  if (name && sethostname(name, strlen(name)) < 0) {
    fprintf(stderr, "unshare: hostname: %s\n", strerror(errno));
    exit(1);
  }
  if (dofork) {
    // the child is the first process in the new namespace, and so its
    // init: everything it starts is numbered from there, and when it
    // goes the namespace goes with it.
    int pid = fork();
    if (pid < 0) {
      fprintf(stderr, "unshare: fork: %s\n", strerror(errno));
      exit(1);
    }
    if (pid > 0) {
      int st = 0;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
      exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    }
  }
  if (i >= argc) {
    // no command: a shell, which is what unshare does with nothing to run
    char *sh[] = {"/bin/sh", 0};
    execvp(sh[0], sh);
    fprintf(stderr, "unshare: /bin/sh: %s\n", strerror(errno));
    exit(1);
  }
  execvp(argv[i], argv + i);      // the PATH, as every such tool does
  fprintf(stderr, "unshare: %s: %s\n", argv[i], strerror(errno));
  exit(1);
}
