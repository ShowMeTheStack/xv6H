//
// crond: run things at the times a table says.
//
//   crond [-n] [-l LEVEL]
//
// the tables are /etc/crontab and /etc/cron.d/* (which have a user
// field) and /var/spool/cron/crontabs/NAME (which do not, because the
// name of the file says whose they are). a line is
//
//   minute hour day-of-month month day-of-week [user] command
//
// where each field is *, a number, a-b, a list of those separated by
// commas, or any of them with /step. the shortcuts @reboot, @yearly,
// @annually, @monthly, @weekly, @daily, @midnight and @hourly stand
// for the obvious lines.
//
// output goes to the log rather than to mail, because there is no
// mail; a job that says something says it in /var/log/messages.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <syslog.h>
#include <signal.h>

#define MAXJOB 128
#define SPOOL "/var/spool/cron/crontabs"

struct job {
  unsigned char minute[60], hour[24], mday[32], month[13], wday[7];
  int at_reboot;
  char user[32];
  char command[256];
  long last_run; // the minute it last ran in, so it runs once a minute at most
};

static struct job jobs[MAXJOB];
static int njobs;
static volatile int reload_wanted;

static void
on_hup(int sig)
{
  (void)sig;
  reload_wanted = 1;
}

// one field: *, a number, a-b, a list, any with /step
static int
parse_field(const char *spec, unsigned char *set, int lo, int hi, int len)
{
  memset(set, 0, len);
  for (const char *p = spec; *p;) {
    int from = lo, to = hi, step = 1;
    if (*p == '*') {
      p++;
    } else {
      if (*p < '0' || *p > '9')
        return -1;
      from = 0;
      while (*p >= '0' && *p <= '9')
        from = from * 10 + (*p++ - '0');
      to = from;
      if (*p == '-') {
        p++;
        to = 0;
        while (*p >= '0' && *p <= '9')
          to = to * 10 + (*p++ - '0');
      }
    }
    if (*p == '/') {
      p++;
      step = 0;
      while (*p >= '0' && *p <= '9')
        step = step * 10 + (*p++ - '0');
      if (step <= 0)
        return -1;
    }
    if (from < lo || to > hi || from > to)
      return -1;
    for (int v = from; v <= to; v += step)
      if (v - lo < len)
        set[v - lo] = 1;
    if (*p == ',')
      p++;
    else if (*p)
      return -1;
  }
  return 0;
}

static void
fill(unsigned char *set, int len)
{
  memset(set, 1, len);
}

// the @ shortcuts
static int
parse_shortcut(struct job *j, const char *word)
{
  memset(j->minute, 0, sizeof(j->minute));
  memset(j->hour, 0, sizeof(j->hour));
  fill(j->mday, sizeof(j->mday));
  fill(j->month, sizeof(j->month));
  fill(j->wday, sizeof(j->wday));

  if (strcmp(word, "@reboot") == 0) {
    j->at_reboot = 1;
    return 0;
  }
  if (strcmp(word, "@hourly") == 0) {
    j->minute[0] = 1;
    fill(j->hour, sizeof(j->hour));
    return 0;
  }
  if (strcmp(word, "@daily") == 0 || strcmp(word, "@midnight") == 0) {
    j->minute[0] = 1;
    j->hour[0] = 1;
    return 0;
  }
  if (strcmp(word, "@weekly") == 0) {
    j->minute[0] = 1;
    j->hour[0] = 1;
    memset(j->wday, 0, sizeof(j->wday));
    j->wday[0] = 1;
    return 0;
  }
  if (strcmp(word, "@monthly") == 0) {
    j->minute[0] = 1;
    j->hour[0] = 1;
    memset(j->mday, 0, sizeof(j->mday));
    j->mday[1] = 1;
    return 0;
  }
  if (strcmp(word, "@yearly") == 0 || strcmp(word, "@annually") == 0) {
    j->minute[0] = 1;
    j->hour[0] = 1;
    memset(j->mday, 0, sizeof(j->mday));
    j->mday[1] = 1;
    memset(j->month, 0, sizeof(j->month));
    j->month[1] = 1;
    return 0;
  }
  return -1;
}

// a word, and where the rest of the line starts
static char *
next_word(char *p, char **word)
{
  while (*p == ' ' || *p == '\t')
    p++;
  *word = p;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  if (*p)
    *p++ = 0;
  return p;
}

static void
add_line(char *line, const char *as_user)
{
  char *p = line;

  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '#' || *p == 0 || *p == '\n')
    return;
  int n = strlen(p);
  while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r'))
    p[--n] = 0;
  if (*p == 0)
    return;
  // an assignment, which cron tables may have; nothing here uses them
  if (strchr(p, '=') && strchr(p, '=') < strchr(p, ' ') + 1 && strchr(p, ' ') == 0)
    return;
  if (njobs >= MAXJOB)
    return;

  struct job *j = &jobs[njobs];
  memset(j, 0, sizeof(*j));
  char *w;

  if (*p == '@') {
    p = next_word(p, &w);
    if (parse_shortcut(j, w) < 0)
      return;
  } else {
    char *f[5];
    for (int i = 0; i < 5; i++)
      p = next_word(p, &f[i]);
    if (parse_field(f[0], j->minute, 0, 59, 60) < 0 ||
        parse_field(f[1], j->hour, 0, 23, 24) < 0 ||
        parse_field(f[2], j->mday, 1, 31, 32) < 0 ||
        parse_field(f[3], j->month, 1, 12, 13) < 0 ||
        parse_field(f[4], j->wday, 0, 6, 7) < 0)
      return;
  }
  if (as_user) {
    snprintf(j->user, sizeof(j->user), "%s", as_user);
  } else {
    p = next_word(p, &w);
    snprintf(j->user, sizeof(j->user), "%s", w);
  }
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == 0)
    return;
  snprintf(j->command, sizeof(j->command), "%s", p);
  j->last_run = -1;
  njobs++;
}

static void
read_table(const char *path, const char *as_user)
{
  FILE *f = fopen(path, "r");
  char line[512];

  if (f == 0)
    return;
  while (fgets(line, sizeof(line), f))
    add_line(line, as_user);
  fclose(f);
}

static void
read_tables(void)
{
  njobs = 0;
  read_table("/etc/crontab", 0);

  DIR *d = opendir("/etc/cron.d");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      if (e->d_name[0] == '.')
        continue;
      char path[256];
      snprintf(path, sizeof(path), "/etc/cron.d/%s", e->d_name);
      read_table(path, 0);
    }
    closedir(d);
  }
  d = opendir(SPOOL);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      if (e->d_name[0] == '.')
        continue;
      char path[256];
      snprintf(path, sizeof(path), "%s/%s", SPOOL, e->d_name);
      read_table(path, e->d_name);
    }
    closedir(d);
  }
  syslog(LOG_INFO, "%d job(s) in the tables", njobs);
}

static void
run_job(const struct job *j)
{
  pid_t pid = fork();

  if (pid < 0)
    return;
  if (pid > 0)
    return; // reaped by the loop below

  // whoever the line says, which for a user's own table is the name of
  // the file it came from
  struct passwd *pw = getpwnam(j->user[0] ? j->user : "root");
  if (pw && pw->pw_uid != getuid()) {
    gid_t groups[32];
    int n = 32;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
      n = 1;
    if (setgroups(n, groups) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
      exit(1);
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
  }
  setsid();
  // what the job says goes to the log, because there is no mail
  int fd = open("/dev/log", O_WRONLY);
  if (fd >= 0) {
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
      close(fd);
  }
  char *argv[] = {"/bin/sh", "-c", (char *)j->command, 0};
  execv(argv[0], argv);
  exit(1);
}

int
main(int argc, char *argv[])
{
  int foreground = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-f") == 0)
      foreground = 1;
    else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
      i++;
    else {
      fprintf(stderr, "usage: crond [-n]\n");
      exit(1);
    }
  }
  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0)
      exit(1);
    if (pid > 0) {
      FILE *f = fopen("/var/run/crond.pid", "w");
      if (f) {
        fprintf(f, "%d\n", (int)pid);
        fclose(f);
      }
      exit(0);
    }
    setsid();
  }
  openlog("crond", 0, LOG_CRON);
  signal(SIGHUP, on_hup);
  signal(SIGCHLD, SIG_IGN); // a finished job needs no funeral

  read_tables();
  for (int i = 0; i < njobs; i++)
    if (jobs[i].at_reboot)
      run_job(&jobs[i]);

  long last_minute = -1;
  for (;;) {
    if (reload_wanted) {
      reload_wanted = 0;
      read_tables();
    }
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    long minute = (long)(now / 60);
    if (minute != last_minute) {
      last_minute = minute;
      for (int i = 0; i < njobs; i++) {
        struct job *j = &jobs[i];
        if (j->at_reboot || j->last_run == minute)
          continue;
        if (!j->minute[tm.tm_min] || !j->hour[tm.tm_hour] || !j->month[tm.tm_mon + 1])
          continue;
        // the day matches if either the day of the month or the day of
        // the week does, which is what cron has always done
        if (!j->mday[tm.tm_mday] && !j->wday[tm.tm_wday])
          continue;
        j->last_run = minute;
        syslog(LOG_INFO, "(%s) CMD (%s)", j->user[0] ? j->user : "root", j->command);
        run_job(j);
      }
    }
    // wake at the top of the next minute, not before
    now = time(0);
    int wait_for = 60 - (int)(now % 60);
    struct timespec ts = {wait_for, 0};
    nanosleep(&ts, 0);
  }
}
