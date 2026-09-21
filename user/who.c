//
// who, w and last: who is logged in, and who has been.
//
// the record is /var/run/utmp, which login writes when a session
// starts and clears when it ends, and /var/log/wtmp, which keeps
// every one of them. the format is this system's own: a fixed-size
// record, so that a half-written one cannot corrupt the file.
//
#include "user/user.h"
#include <utmp.h>

static const char *me;

static void
show_time(time_t t, char *out, int max)
{
  struct tm tm;

  localtime_r(&t, &tm);
  strftime(out, max, "%Y-%m-%d %H:%M", &tm);
}

static void
do_who(void)
{
  struct utmp u;
  int fd = open(UTMP_FILE, O_RDONLY);

  if (fd < 0)
    return;
  while (read(fd, &u, sizeof(u)) == sizeof(u)) {
    if (u.ut_type != USER_PROCESS || u.ut_user[0] == 0)
      continue;
    char when[32];
    show_time((time_t)u.ut_time, when, sizeof(when));
    printf("%-10s %-8s %s\n", u.ut_user, u.ut_line, when);
  }
  close(fd);
}

// w: who, and what each one is running
static void
do_w(void)
{
  struct utmp u;
  int fd = open(UTMP_FILE, O_RDONLY);
  time_t now = time(0);
  struct tm tm;

  localtime_r(&now, &tm);
  char nowstr[32];
  strftime(nowstr, sizeof(nowstr), "%H:%M:%S", &tm);
  printf(" %s up, %d user(s)\n", nowstr, 0);
  printf("USER       TTY      LOGIN@           WHAT\n");
  if (fd < 0)
    return;
  while (read(fd, &u, sizeof(u)) == sizeof(u)) {
    if (u.ut_type != USER_PROCESS || u.ut_user[0] == 0)
      continue;
    char when[32], what[64] = "-";
    show_time((time_t)u.ut_time, when, sizeof(when));
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)u.ut_pid);
    int cf = open(path, O_RDONLY);
    if (cf >= 0) {
      int n = read(cf, what, sizeof(what) - 1);
      if (n > 0) {
        what[n] = 0;
        for (int i = 0; i < n; i++)
          if (what[i] == 0)
            what[i] = ' ';
      }
      close(cf);
    }
    printf("%-10s %-8s %-16s %s\n", u.ut_user, u.ut_line, when, what);
  }
  close(fd);
}

static void
do_last(int limit)
{
  struct utmp u;
  int fd = open(WTMP_FILE, O_RDONLY);
  int shown = 0;

  if (fd < 0)
    return;
  // the file is oldest first and `last` prints newest first, so it is
  // read into memory: these files are small and rotated by cron.
  static struct utmp all[256];
  int n = 0;
  while (n < 256 && read(fd, &all[n], sizeof(all[n])) == sizeof(all[n]))
    n++;
  close(fd);
  for (int i = n - 1; i >= 0 && (limit == 0 || shown < limit); i--) {
    u = all[i];
    char when[32];
    show_time((time_t)u.ut_time, when, sizeof(when));
    printf("%-10s %-8s %-18s %s\n", u.ut_user[0] ? u.ut_user : "(none)", u.ut_line, when,
           u.ut_type == USER_PROCESS ? "logged in" : "logged out");
    shown++;
  }
}

int
main(int argc, char *argv[])
{
  me = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  if (strcmp(me, "w") == 0) {
    do_w();
  } else if (strcmp(me, "last") == 0) {
    int limit = 0;
    for (int i = 1; i < argc; i++)
      if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9')
        limit = atoi(argv[i] + 1);
    do_last(limit);
  } else {
    do_who();
  }
  exit(0);
}
