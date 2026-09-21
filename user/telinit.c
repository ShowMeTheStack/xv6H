// telinit: tell init to change runlevel or to re-read its table.
//
//   telinit N     go to runlevel N
//   telinit q     re-read /etc/inittab
#include "user/user.h"
#include <signal.h>

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "usage: telinit RUNLEVEL|q\n");
    exit(1);
  }
  if (geteuid() != 0) {
    fprintf(stderr, "telinit: only root may do that\n");
    exit(1);
  }
  if (argv[1][0] == 'q' || argv[1][0] == 'Q') {
    if (kill(1, SIGHUP) < 0) {
      fprintf(stderr, "telinit: %s\n", strerror(errno));
      exit(1);
    }
    exit(0);
  }
  if (argv[1][0] < '0' || argv[1][0] > '6' || argv[1][1]) {
    fprintf(stderr, "telinit: %s: not a runlevel\n", argv[1]);
    exit(1);
  }
  // 0 and 6 are halt and reboot, which shutdown does properly
  if (argv[1][0] == '0' || argv[1][0] == '6') {
    char *a[] = {"/sbin/shutdown", argv[1][0] == '6' ? "-r" : "-h", "now", 0};
    execv(a[0], a);
    fprintf(stderr, "telinit: %s\n", strerror(errno));
    exit(1);
  }
  // the runlevel is written where init looks for it, and the signal
  // tells it to look
  FILE *f = fopen("/var/run/runlevel", "w");
  if (f == 0) {
    fprintf(stderr, "telinit: /var/run/runlevel: %s\n", strerror(errno));
    exit(1);
  }
  fprintf(f, "%c\n", argv[1][0]);
  fclose(f);
  if (kill(1, SIGHUP) < 0) {
    fprintf(stderr, "telinit: %s\n", strerror(errno));
    exit(1);
  }
  exit(0);
}
