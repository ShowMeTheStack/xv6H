// procstorm N     : N processes alive at once, then all reaped
// procstorm -s N  : N forks in sequence (pids wrap around a small pid space)
// prints the counts, the largest pid seen, and the free pages before and after.
#include "kernel/types.h"
#include "kernel/sysconf.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int seq = 0, n;
  if (argc == 3 && strcmp(argv[1], "-s") == 0) {
    seq = 1;
    n = atoi(argv[2]);
  } else if (argc == 2) {
    n = atoi(argv[1]);
  } else {
    fprintf(stderr, "usage: procstorm [-s] N\n");
    exit(1);
  }
  long free0 = sysconf(_SC_AVPHYS_PAGES), procs0 = sysconf(_SC_XV6_NPROCS);
  long maxpid = 0, pidmax = sysconf(_SC_XV6_PID_MAX);
  int made = 0, failed = 0;

  if (seq) {
    for (int i = 0; i < n; i++) {
      int pid = fork();
      if (pid < 0) {
        failed++;
        continue;
      }
      if (pid == 0)
        exit(0);
      if (pid > maxpid)
        maxpid = pid;
      if (pid >= pidmax) {
        printf("procstorm: pid %d beyond pid_max %ld\n", pid, pidmax);
        exit(1);
      }
      made++;
      wait(0);
    }
  } else {
    int fds[2];
    if (pipe(fds) < 0)
      exit(1);
    for (int i = 0; i < n; i++) {
      int pid = fork();
      if (pid < 0) {
        failed++;
        break;
      }
      if (pid == 0) {
        char c;
        close(fds[1]);
        read(fds[0], &c, 1);
        exit(0);
      }
      if (pid > maxpid)
        maxpid = pid;
      made++;
    }
    long alive = sysconf(_SC_XV6_NPROCS);
    printf("procstorm: %d alive (kernel counts %ld)\n", made, alive);
    close(fds[1]);
    for (int i = 0; i < made; i++)
      wait(0);
    close(fds[0]);
  }
  long free1 = sysconf(_SC_AVPHYS_PAGES), procs1 = sysconf(_SC_XV6_NPROCS);
  printf("procstorm: made %d failed %d maxpid %ld pid_max %ld procs %ld->%ld free %ld->%ld\n", made, failed,
         maxpid, pidmax, procs0, procs1, free0, free1);
  exit(0);
}
