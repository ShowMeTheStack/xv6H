// dmesg: the kernel's messages.
//
// what the kernel printed is in /var/log/kern.log once klogd has put
// it there, and in /dev/kmsg before that -- but /dev/kmsg is a queue,
// and reading it would take the messages away from klogd, so this
// reads the file.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  const char *path = "/var/log/kern.log";
  int clear = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "-c") == 0)
      clear = 1;
    else {
      fprintf(stderr, "usage: dmesg [-C]\n");
      exit(1);
    }
  }
  FILE *f = fopen(path, "r");
  if (f == 0) {
    fprintf(stderr, "dmesg: %s: %s\n", path, strerror(errno));
    exit(1);
  }
  char line[512];
  while (fgets(line, sizeof(line), f))
    fputs(line, stdout);
  fclose(f);
  if (clear) {
    int fd = open(path, O_WRONLY | O_TRUNC, 0640);
    if (fd >= 0)
      close(fd);
  }
  exit(0);
}
