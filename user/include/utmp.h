#ifndef _UTMP_H
#define _UTMP_H
#include <sys/types.h>

// who is logged in, and who has been. this system's own format: a
// fixed-size record so that a half-written one cannot be read as a
// whole one, and no strings that have to be freed.
#define UTMP_FILE "/var/run/utmp"
#define WTMP_FILE "/var/log/wtmp"

#define UT_NAMESIZE 32
#define UT_LINESIZE 16
#define UT_HOSTSIZE 64

#define EMPTY         0
#define BOOT_TIME     2
#define USER_PROCESS  7
#define DEAD_PROCESS  8

struct utmp {
  short ut_type;
  pid_t ut_pid;
  char ut_line[UT_LINESIZE]; // the terminal
  char ut_user[UT_NAMESIZE];
  char ut_host[UT_HOSTSIZE];
  long ut_time;
};

// the calls that read and write it
void setutent(void);
struct utmp *getutent(void);
void endutent(void);
struct utmp *pututline(const struct utmp *u);
// and the line that records a login or a logout
int utmp_login(const char *user, const char *line, pid_t pid);
int utmp_logout(const char *line);

#endif
