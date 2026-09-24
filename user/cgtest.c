// cgtest: what a control group's limits feel like from inside one.
//
//   cgtest fork [MAX] fork up to MAX (200) children that are all alive
//                   at once; print how many there were
//   cgtest mem N      touch up to N pages; print how many were had
//   cgtest count FILE add a line to FILE for ever (for the freeze test)
//
// Run under cgrun, this is how a limit is shown to have teeth: the same
// program in the machine's own group takes what it asks for.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include <errno.h>
#include <sys/wait.h>

#define PGSIZE 4096

static void
forkuntil(int max)
{
  int made = 0;
  int stop[2];

  // every child blocks reading the pipe, so that all of them are alive
  // at once -- what is being counted is how many may exist together --
  // and closing the write end lets every one of them go.
  if (pipe(stop) < 0) {
    printf("cgtest: pipe: %s\n", strerror(errno));
    exit(1);
  }
  for (; made < max; made++) {
    int pid = fork();
    if (pid < 0)
      break;
    if (pid == 0) {
      char c;
      close(stop[1]);
      read(stop[0], &c, 1);   // until the parent lets go
      exit(0);
    }
  }
  printf("cgtest: forks %d of %d (%s)\n", made, max, strerror(errno));
  close(stop[1]);             // and they all go
  for (int i = 0; i < made; i++) {
    int st;
    wait(&st);
  }
}

static void
memuntil(int pages)
{
  int had = 0;

  for (int i = 0; i < pages; i++) {
    char *p = malloc(PGSIZE);
    if (p == 0)
      break;
    p[0] = 1;            // touch it: a page that is not touched is not held
    p[PGSIZE - 1] = 1;
    had++;
    // said as it goes, because a group at its limit stops this process
    // where it stands: the fault it cannot serve is the end of it.
    if (had % 100 == 0)
      printf("cgtest: at %d\n", had);
  }
  printf("cgtest: pages %d of %d\n", had, pages);
}

static void
counter(const char *path)
{
  int fd = open(path, O_CREATE | O_WRONLY, 0666);

  if (fd < 0) {
    printf("cgtest: %s: %s\n", path, strerror(errno));
    exit(1);
  }
  for (;;)
    write(fd, ".", 1);
}

int
main(int argc, char *argv[])
{
  if (argc >= 2 && strcmp(argv[1], "fork") == 0) {
    forkuntil(argc >= 3 ? atoi(argv[2]) : 200);
    exit(0);
  }
  if (argc >= 3 && strcmp(argv[1], "mem") == 0) {
    memuntil(atoi(argv[2]));
    exit(0);
  }
  if (argc >= 3 && strcmp(argv[1], "count") == 0) {
    counter(argv[2]);
    exit(0);
  }
  fprintf(stderr, "usage: cgtest fork | mem PAGES | count FILE\n");
  exit(1);
}
