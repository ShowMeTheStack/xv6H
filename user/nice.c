// nice and renice: how much of the processor a process is entitled to.
//
//   nice [-n INCREMENT] COMMAND [ARG...]
//   renice [-n] VALUE [-p] PID...
//   renice [-n] VALUE -u USER...
//   renice [-n] VALUE -g PGID...
//
// the value runs from -20 (most) to 19 (least); only root may ask for
// a smaller number than the process already has, which is the rule
// everywhere and the reason a program cannot promote itself.
#include "user/user.h"
#include <pwd.h>
#include <sys/resource.h>

static int status;

static int
run_nice(int argc, char *argv[])
{
  int increment = 10, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      increment = atoi(argv[++i]);
      continue;
    }
    if (argv[i][1] == 'n' && argv[i][2]) {
      increment = atoi(argv[i] + 2);
      continue;
    }
    // the old spelling: nice -5 command, and nice --5 for -5
    if (argv[i][1] == '-' && argv[i][2]) {
      increment = -atoi(argv[i] + 2);
      continue;
    }
    if ((argv[i][1] >= '0' && argv[i][1] <= '9')) {
      increment = atoi(argv[i] + 1);
      continue;
    }
    fprintf(stderr, "usage: nice [-n INCREMENT] COMMAND [ARG...]\n");
    return 125;
  }

  if (i >= argc) {
    // with no command it says what this process's value is
    errno = 0;
    int cur = getpriority(PRIO_PROCESS, 0);
    if (cur == -1 && errno) {
      fprintf(stderr, "nice: %s\n", strerror(errno));
      return 125;
    }
    printf("%d\n", cur);
    return 0;
  }

  errno = 0;
  int cur = getpriority(PRIO_PROCESS, 0);
  if (cur == -1 && errno)
    cur = 0;
  int want = cur + increment;
  if (want > 19)
    want = 19;
  if (want < -20)
    want = -20;
  if (setpriority(PRIO_PROCESS, 0, want) < 0)
    fprintf(stderr, "nice: cannot set the value: %s\n", strerror(errno));

  execvp(argv[i], argv + i);
  fprintf(stderr, "nice: %s: %s\n", argv[i], strerror(errno));
  return errno == ENOENT ? 127 : 126;
}

static void
set_one(int which, int who, int value, const char *what, const char *name)
{
  errno = 0;
  int old = getpriority(which, who);

  if (setpriority(which, who, value) < 0) {
    fprintf(stderr, "renice: %s %s: %s\n", what, name, strerror(errno));
    status = 1;
    return;
  }
  printf("%s %s: old priority %d, new priority %d\n", what, name, old, value);
}

static int
run_renice(int argc, char *argv[])
{
  int which = PRIO_PROCESS, i = 1;
  int have_value = 0, value = 0;

  for (; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      value = atoi(argv[++i]);
      have_value = 1;
      continue;
    }
    if (strcmp(argv[i], "-p") == 0) {
      which = PRIO_PROCESS;
      continue;
    }
    if (strcmp(argv[i], "-g") == 0) {
      which = PRIO_PGRP;
      continue;
    }
    if (strcmp(argv[i], "-u") == 0) {
      which = PRIO_USER;
      continue;
    }
    if (!have_value) {
      // the first plain word, or a leading -N, is the value
      value = atoi(argv[i]);
      have_value = 1;
      continue;
    }

    if (which == PRIO_USER) {
      struct passwd *pw = getpwnam(argv[i]);
      int uid = pw ? (int)pw->pw_uid : atoi(argv[i]);
      if (pw == 0 && (argv[i][0] < '0' || argv[i][0] > '9')) {
        fprintf(stderr, "renice: unknown user %s\n", argv[i]);
        status = 1;
        continue;
      }
      set_one(PRIO_USER, uid, value, "user", argv[i]);
    } else {
      set_one(which, atoi(argv[i]), value,
              which == PRIO_PGRP ? "pgid" : "pid", argv[i]);
    }
  }

  if (!have_value) {
    fprintf(stderr, "usage: renice [-n] VALUE [-p PID | -g PGID | -u USER]...\n");
    return 1;
  }
  return status;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  if (strcmp(me, "renice") == 0)
    exit(run_renice(argc, argv));
  exit(run_nice(argc, argv));
}
