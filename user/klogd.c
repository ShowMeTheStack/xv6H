//
// klogd: the kernel's messages, into the log.
//
//   klogd [-n]
//
// reads /dev/kmsg, which printk fills as it prints, and hands each
// line to syslog(3) with the kernel facility -- so it ends up in
// kern.log and in messages, beside everything else, with a timestamp.
//
#include "user/user.h"
#include <syslog.h>

int
main(int argc, char *argv[])
{
  int foreground = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0)
      foreground = 1;
    else {
      fprintf(stderr, "usage: klogd [-n]\n");
      exit(1);
    }
  }

  int fd = open("/dev/kmsg", O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "klogd: /dev/kmsg: %s\n", strerror(errno));
    exit(1);
  }
  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0)
      exit(1);
    if (pid > 0) {
      FILE *f = fopen("/var/run/klogd.pid", "w");
      if (f) {
        fprintf(f, "%d\n", (int)pid);
        fclose(f);
      }
      exit(0);
    }
    setsid();
  }
  openlog("kernel", 0, LOG_KERN);

  char line[512];
  for (;;) {
    int n = read(fd, line, sizeof(line) - 1);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      n--;
    line[n] = 0;
    if (n > 0)
      syslog(LOG_INFO, "%s", line);
  }
  exit(0);
}
