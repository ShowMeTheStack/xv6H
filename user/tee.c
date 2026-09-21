// tee: copy standard input to standard output and to each file.
//
//   tee [-a] [FILE...]    -a: append rather than truncate
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int append = 0, i = 1, status = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-a") == 0)
      append = 1;
    else if (strcmp(argv[i], "-i") == 0)
      signal(SIGINT, SIG_IGN);
    else {
      fprintf(stderr, "usage: tee [-a] [FILE...]\n");
      exit(1);
    }
  }
  int nf = argc - i;
  int fds[16];
  if (nf > 16)
    nf = 16;
  for (int k = 0; k < nf; k++) {
    fds[k] = open(argv[i + k], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666);
    if (fds[k] < 0) {
      fprintf(stderr, "tee: %s: %s\n", argv[i + k], strerror(errno));
      status = 1;
    }
  }
  char buf[4096];
  int n;
  while ((n = read(0, buf, sizeof(buf))) > 0) {
    if (write(1, buf, n) != n)
      status = 1;
    for (int k = 0; k < nf; k++)
      if (fds[k] >= 0 && write(fds[k], buf, n) != n)
        status = 1;
  }
  for (int k = 0; k < nf; k++)
    if (fds[k] >= 0)
      close(fds[k]);
  exit(status);
}
