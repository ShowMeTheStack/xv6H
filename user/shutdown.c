//
// shutdown, halt, reboot and poweroff.
//
//   shutdown [-h|-r|-P] [now|+MINUTES|hh:mm] [MESSAGE]
//   halt | reboot | poweroff [-f]
//
// everyone logged in is told, the services are stopped through the
// runlevel scripts, what is waiting to be written is written, and then
// the machine stops. -f skips the scripts, which is what to do when
// something is too broken to run them.
//
#include "user/user.h"
#include <signal.h>
#include <utmp.h>
#include <syslog.h>

static void
warn_everyone(const char *what, const char *message)
{
  int fd = open("/dev/console", O_WRONLY);

  if (fd >= 0) {
    char line[256];
    int n = snprintf(line, sizeof(line), "\nThe system is going down for %s NOW!%s%s\n", what,
                     message ? " " : "", message ? message : "");
    write(fd, line, n);
    close(fd);
  }
}

// how long to wait, from "now", "+N" or "hh:mm"
static int
delay_seconds(const char *when)
{
  if (when == 0 || strcmp(when, "now") == 0)
    return 0;
  if (when[0] == '+')
    return atoi(when + 1) * 60;
  const char *colon = strchr(when, ':');
  if (colon) {
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    int hh = atoi(when), mm = atoi(colon + 1);
    int target = hh * 3600 + mm * 60;
    int current = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    int d = target - current;
    if (d < 0)
      d += 24 * 3600;
    return d;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int how = XV6_REBOOT_POWEROFF, force = 0, i = 1;
  const char *when = 0, *message = 0;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  if (strcmp(me, "reboot") == 0)
    how = XV6_REBOOT_RESTART;
  else if (strcmp(me, "halt") == 0)
    how = XV6_REBOOT_HALT;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-h") == 0)
      how = XV6_REBOOT_POWEROFF;
    else if (strcmp(argv[i], "-P") == 0)
      how = XV6_REBOOT_POWEROFF;
    else if (strcmp(argv[i], "-r") == 0)
      how = XV6_REBOOT_RESTART;
    else if (strcmp(argv[i], "-H") == 0)
      how = XV6_REBOOT_HALT;
    else if (strcmp(argv[i], "-f") == 0)
      force = 1;
    else if (strcmp(argv[i], "-c") == 0) {
      fprintf(stderr, "%s: there is nothing scheduled to cancel\n", me);
      exit(1);
    } else {
      fprintf(stderr, "usage: %s [-h|-r|-H] [-f] [now|+MINUTES|hh:mm] [MESSAGE]\n", me);
      exit(1);
    }
  }
  if (strcmp(me, "shutdown") == 0) {
    if (i < argc)
      when = argv[i++];
    if (i < argc)
      message = argv[i];
  }

  if (geteuid() != 0) {
    fprintf(stderr, "%s: only root may do that\n", me);
    exit(1);
  }
  openlog(me, 0, LOG_AUTH);

  int wait_for = delay_seconds(when);
  if (wait_for > 0) {
    warn_everyone(how == XV6_REBOOT_RESTART ? "reboot" : "halt", message);
    printf("%s: in %d second(s)\n", me, wait_for);
    sleep(wait_for);
  }
  warn_everyone(how == XV6_REBOOT_RESTART ? "reboot" : "halt", message);
  syslog(LOG_NOTICE, "%s by root", how == XV6_REBOOT_RESTART ? "reboot" : "power off");

  // init stops respawning first, or it would start a new shell over
  // the messages of a machine that is going down
  kill(1, SIGUSR1);

  if (!force) {
    // the services, stopped the way they were started
    if (access("/etc/rc.d/rc", X_OK) == 0) {
      pid_t pid = fork();
      if (pid == 0) {
        char *a[] = {"/etc/rc.d/rc", how == XV6_REBOOT_RESTART ? "6" : "0", 0};
        execv(a[0], a);
        exit(0);
      }
      if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
      }
    }
    // everything else, politely and then not. init is spared: it is
    // the one that has to still be there afterwards.
    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);
  }
  sync();
  utmp_logout("console");

  if (reboot(how) < 0) {
    fprintf(stderr, "%s: %s\n", me, strerror(errno));
    exit(1);
  }
  exit(0);
}
