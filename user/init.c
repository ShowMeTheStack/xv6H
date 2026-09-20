// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/file.h"
#include "kernel/fcntl.h"

char *argv[] = {"sh", 0};

// the environment every program starts with
char *envp[] = {"PATH=/", "HOME=/", "SHELL=/sh", "TERM=xv6", "USER=root", 0};

int
main(void)
{
  int pid, wpid;

  if (open("console", O_RDWR) < 0) {
    mknod("console", CONSOLE, 0);
    open("console", O_RDWR);
  }
  dup(0); // stdout
  dup(0); // stderr

  // the character devices that have no hardware behind them, in /dev
  mkdir("/dev", 0777);
  mknod("/dev/console", CONSOLE, 0);
  mknod("/dev/null", DEV_NULL, 0);
  mknod("/dev/zero", DEV_ZERO, 0);
  mknod("/dev/full", DEV_FULL, 0);
  mknod("/dev/random", DEV_RANDOM, 0);
  mknod("/dev/urandom", DEV_RANDOM, 0);
  mknod("/dev/tty", CONSOLE, 0);

  // the kernel's own state, as files
  mkdir("/proc", 0777);
  if (mount("proc", "/proc", "procfs", 0, 0) < 0)
    printf("init: cannot mount /proc\n");

  for (;;) {
    printf("init: starting sh\n");
    pid = fork();
    if (pid < 0) {
      printf("init: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      setsid(); // the shell leads its own session
      execve("sh", argv, envp);
      printf("init: exec sh failed\n");
      exit(1);
    }

    for (;;) {
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *)0);
      if (wpid < 0 && errno == EINTR)
        continue; // a signal interrupted the wait
      if (wpid == pid) {
        // the shell exited; restart it.
        break;
      } else if (wpid < 0) {
        printf("init: wait returned an error\n");
        exit(1);
      } else {
        // it was a parentless process; do nothing.
      }
    }
  }
}
