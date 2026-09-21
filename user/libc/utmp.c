//
// /var/run/utmp and /var/log/wtmp.
//
// utmp is who is logged in now: one record per terminal, rewritten in
// place as sessions come and go. wtmp is every login and logout, only
// ever appended to. both are fixed-size records, so a reader never
// sees half of one and a writer never has to lock the file.
//
#include <utmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static int utfd = -1;
static struct utmp current;

void
setutent(void)
{
  if (utfd >= 0)
    close(utfd);
  utfd = open(UTMP_FILE, O_RDONLY);
}

struct utmp *
getutent(void)
{
  if (utfd < 0)
    setutent();
  if (utfd < 0)
    return 0;
  if (read(utfd, &current, sizeof(current)) != sizeof(current))
    return 0;
  return &current;
}

void
endutent(void)
{
  if (utfd >= 0) {
    close(utfd);
    utfd = -1;
  }
}

// replace the record for this terminal, or add one if it is not there
struct utmp *
pututline(const struct utmp *u)
{
  struct utmp got;
  int fd = open(UTMP_FILE, O_RDWR | O_CREAT, 0644);
  off_t at = 0;

  if (fd < 0)
    return 0;
  for (;;) {
    off_t here = lseek(fd, 0, SEEK_CUR);
    if (read(fd, &got, sizeof(got)) != sizeof(got))
      break;
    if (strncmp(got.ut_line, u->ut_line, UT_LINESIZE) == 0) {
      at = here;
      goto write;
    }
    if (got.ut_type == EMPTY && at == 0)
      at = here;
  }
  at = lseek(fd, 0, SEEK_END);
write:
  lseek(fd, at, SEEK_SET);
  write(fd, u, sizeof(*u));
  close(fd);
  return (struct utmp *)u;
}

static void
wtmp_append(const struct utmp *u)
{
  int fd = open(WTMP_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);

  if (fd < 0)
    return;
  write(fd, u, sizeof(*u));
  close(fd);
}

int
utmp_login(const char *user, const char *line, pid_t pid)
{
  struct utmp u;

  memset(&u, 0, sizeof(u));
  u.ut_type = USER_PROCESS;
  u.ut_pid = pid;
  strncpy(u.ut_line, line, UT_LINESIZE - 1);
  strncpy(u.ut_user, user, UT_NAMESIZE - 1);
  u.ut_time = (long)time(0);
  pututline(&u);
  wtmp_append(&u);
  return 0;
}

int
utmp_logout(const char *line)
{
  struct utmp u;

  memset(&u, 0, sizeof(u));
  u.ut_type = DEAD_PROCESS;
  strncpy(u.ut_line, line, UT_LINESIZE - 1);
  u.ut_time = (long)time(0);
  pututline(&u);
  wtmp_append(&u);
  return 0;
}
