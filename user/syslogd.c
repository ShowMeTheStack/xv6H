//
// syslogd: read /dev/log and write what it says into /var/log.
//
//   syslogd [-n] [-f FILE]
//
// each message is "<PRI>timestamp tag[pid]: text", the shape the C
// library's syslog(3) sends. the priority says which file it belongs
// in -- auth to auth.log, kernel to kern.log, everything else to
// messages -- and every message also goes to messages, which is what
// makes that file the one to read first.
//
#include "user/user.h"
#include <syslog.h>

#define MESSAGES "/var/log/messages"
#define AUTHLOG  "/var/log/auth.log"
#define KERNLOG  "/var/log/kern.log"

static int
append(const char *path, const char *line, int n)
{
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0640);

  if (fd < 0)
    return -1;
  write(fd, line, n);
  close(fd);
  return 0;
}

// "<38>Sep 21 07:20:04 su[28]: su to user" -> facility 4 (auth)
static int
facility_of(const char *line)
{
  if (line[0] != '<')
    return LOG_USER >> 3;
  int pri = atoi(line + 1);
  return pri >> 3;
}

int
main(int argc, char *argv[])
{
  int foreground = 0;
  const char *main_log = MESSAGES;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0)
      foreground = 1;
    else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      main_log = argv[++i];
    else {
      fprintf(stderr, "usage: syslogd [-n] [-f FILE]\n");
      exit(1);
    }
  }

  int fd = open("/dev/log", O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "syslogd: /dev/log: %s\n", strerror(errno));
    exit(1);
  }
  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0) {
      fprintf(stderr, "syslogd: fork: %s\n", strerror(errno));
      exit(1);
    }
    if (pid > 0) {
      // the pid file is what start-stop-daemon and `service` look at
      FILE *f = fopen("/var/run/syslogd.pid", "w");
      if (f) {
        fprintf(f, "%d\n", (int)pid);
        fclose(f);
      }
      exit(0);
    }
    setsid();
  }

  mkdir("/var", 0755);
  mkdir("/var/log", 0755);
  append(main_log, "syslogd started\n", 16);

  char line[512];
  for (;;) {
    int n = read(fd, line, sizeof(line) - 1);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
    line[n] = 0;
    // the priority is for us, not for the file
    char *text = line;
    if (line[0] == '<') {
      char *close = strchr(line, '>');
      if (close)
        text = close + 1;
    }
    int len = strlen(text);
    append(main_log, text, len);
    int fac = facility_of(line);
    if (fac == (LOG_AUTH >> 3) || fac == (LOG_AUTHPRIV >> 3))
      append(AUTHLOG, text, len);
    else if (fac == (LOG_KERN >> 3))
      append(KERNLOG, text, len);
  }
  exit(0);
}
