// logger: put a line in the log.
//
//   logger [-p FACILITY.LEVEL] [-t TAG] [-s] MESSAGE...
//   logger ... (with no message, one line per line of standard input)
#include "user/user.h"
#include <syslog.h>

static const struct {
  const char *name;
  int value;
} facilities[] = {
    {"kern", LOG_KERN},     {"user", LOG_USER},     {"mail", LOG_MAIL},
    {"daemon", LOG_DAEMON}, {"auth", LOG_AUTH},     {"syslog", LOG_SYSLOG},
    {"cron", LOG_CRON},     {"local0", LOG_LOCAL0}, {"local1", LOG_LOCAL1},
    {0, 0},
};

static const struct {
  const char *name;
  int value;
} levels[] = {
    {"emerg", LOG_EMERG}, {"alert", LOG_ALERT},   {"crit", LOG_CRIT},
    {"err", LOG_ERR},     {"warning", LOG_WARNING}, {"notice", LOG_NOTICE},
    {"info", LOG_INFO},   {"debug", LOG_DEBUG},   {0, 0},
};

int
main(int argc, char *argv[])
{
  int facility = LOG_USER, level = LOG_NOTICE, option = 0, i = 1;
  const char *tag = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
      tag = argv[++i];
    else if (strcmp(argv[i], "-s") == 0)
      option |= LOG_PERROR;
    else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      char spec[64];
      snprintf(spec, sizeof(spec), "%s", argv[++i]);
      char *dot = strchr(spec, '.');
      if (dot) {
        *dot = 0;
        for (int k = 0; facilities[k].name; k++)
          if (strcmp(facilities[k].name, spec) == 0)
            facility = facilities[k].value;
        for (int k = 0; levels[k].name; k++)
          if (strcmp(levels[k].name, dot + 1) == 0)
            level = levels[k].value;
      } else {
        for (int k = 0; levels[k].name; k++)
          if (strcmp(levels[k].name, spec) == 0)
            level = levels[k].value;
      }
    } else {
      fprintf(stderr, "usage: logger [-p FACILITY.LEVEL] [-t TAG] [-s] [MESSAGE...]\n");
      exit(1);
    }
  }

  openlog(tag, option, facility);
  if (i < argc) {
    char msg[512];
    int n = 0;
    for (; i < argc; i++)
      n += snprintf(msg + n, sizeof(msg) - n, "%s%s", n ? " " : "", argv[i]);
    syslog(level, "%s", msg);
  } else {
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
      int n = strlen(line);
      while (n > 0 && line[n - 1] == '\n')
        line[--n] = 0;
      if (n > 0)
        syslog(level, "%s", line);
    }
  }
  closelog();
  exit(0);
}
