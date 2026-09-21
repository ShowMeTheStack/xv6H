//
// getty: open a terminal, say what this machine is, and run login.
//
//   getty [-n] [-l PROGRAM] [-i] TTY
//
// -n does not ask who is there and runs PROGRAM (a shell by default)
// straight away, which is what a console on a machine with no other
// users wants; without it, login asks.
//
#include "user/user.h"
#include <termios.h>

int
main(int argc, char *argv[])
{
  const char *program = 0;
  int nologin = 0, noissue = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-n") == 0)
      nologin = 1;
    else if (strcmp(argv[i], "-i") == 0)
      noissue = 1;
    else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
      program = argv[++i];
    else {
      fprintf(stderr, "usage: getty [-n] [-i] [-l PROGRAM] TTY\n");
      exit(1);
    }
  }
  const char *tty = i < argc ? argv[i] : "console";

  char path[64];
  if (tty[0] == '/')
    snprintf(path, sizeof(path), "%s", tty);
  else
    snprintf(path, sizeof(path), "/dev/%s", tty);

  int fd = open(path, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "getty: %s: %s\n", path, strerror(errno));
    exit(1);
  }
  setsid();
  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  if (fd > 2)
    close(fd);

  // the terminal starts in a known state, whatever the last program
  // left it in
  struct termios t;
  if (tcgetattr(0, &t) == 0) {
    t.c_lflag |= ECHO | ICANON | ISIG;
    t.c_iflag |= ICRNL;
    t.c_oflag |= OPOST | ONLCR;
    tcsetattr(0, TCSANOW, &t);
  }

  if (!noissue) {
    char host[64] = "";
    struct utsname u;
    gethostname(host, sizeof(host));
    if (uname(&u) == 0)
      printf("\n%s %s %s\n\n", u.sysname, u.release, u.machine);
    else
      printf("\n%s\n\n", host);
  }

  if (nologin) {
    const char *prog = program ? program : "/bin/sh";
    char arg0[64];
    const char *base = prog;
    for (const char *p = prog; *p; p++)
      if (*p == '/')
        base = p + 1;
    snprintf(arg0, sizeof(arg0), "-%s", base);
    char *argvv[] = {arg0, 0};
    execv(prog, argvv);
    fprintf(stderr, "getty: %s: %s\n", prog, strerror(errno));
    exit(1);
  }
  char *argvv[] = {"/bin/login", 0};
  execv(argvv[0], argvv);
  fprintf(stderr, "getty: /bin/login: %s\n", strerror(errno));
  exit(1);
}
