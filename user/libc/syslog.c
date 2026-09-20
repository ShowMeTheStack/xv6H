//
// <syslog.h>: the system log, without a system logger.
//
// there is no syslogd on this machine yet. when one arrives it will
// listen on /dev/log, so that is where a message goes if the file can
// be opened at all; until then the open fails and the message goes to
// standard error instead, which is the behaviour a program running on
// a console wants anyway. nothing else about the interface changes
// when the daemon appears, and no program will need recompiling.
//
// the line is the one RFC 3164 describes, which is what the daemon
// will expect to parse:
//
//   <PRI>MMM DD HH:MM:SS ident[pid]: message
//
// where PRI is the facility and the priority added together. the
// process id is always there, so LOG_PID is the standing behaviour
// rather than an option.
//
#include <syslog.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

extern char *program_invocation_short_name;

static const char *logident;
static int logoption;
static int logfacility = LOG_USER;
static int logmask = 0xff; // everything, which is what POSIX starts with
static int logfd = -1;

static void
openfd(void)
{
  if (logfd < 0)
    logfd = open("/dev/log", O_WRONLY | O_CLOEXEC);
}

void
openlog(const char *ident, int option, int facility)
{
  logident = ident;
  logoption = option;
  if (facility)
    logfacility = facility & LOG_FACMASK;
  // LOG_NDELAY asks for the connection now rather than at the first
  // message, which is the only difference the two delay flags make.
  if (option & LOG_NDELAY)
    openfd();
}

void
closelog(void)
{
  if (logfd >= 0)
    close(logfd);
  logfd = -1;
  logident = 0;
  logoption = 0;
  logfacility = LOG_USER;
}

int
setlogmask(int mask)
{
  int old = logmask;

  if (mask) // a mask of zero only asks what the mask is
    logmask = mask;
  return old;
}

void
vsyslog(int pri, const char *fmt, va_list ap)
{
  static const char *const month[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  char buf[1024];
  int saved = errno; // syslog must not disturb what the caller was reporting

  if (!(LOG_MASK(LOG_PRI(pri)) & logmask))
    return;
  // a message that names no facility belongs to the one openlog set.
  if ((pri & LOG_FACMASK) == 0)
    pri |= logfacility;

  time_t now = time(0);
  struct tm tm;
  localtime_r(&now, &tm);
  const char *ident = logident ? logident : program_invocation_short_name;

  int n = snprintf(buf, sizeof(buf), "<%d>%s %2d %02d:%02d:%02d %s[%d]: ", pri,
                   month[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ident,
                   getpid());
  if (n < 0 || (size_t)n >= sizeof(buf))
    return;
  // %m is the one conversion syslog adds, and it is the caller's errno
  // rather than whatever this function has done to it since.
  errno = saved;
  int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
  if (m < 0)
    return;
  size_t len = (size_t)n + (size_t)m;
  if (len > sizeof(buf) - 2)
    len = sizeof(buf) - 2; // truncated, but still a whole line
  buf[len++] = '\n';

  openfd();
  if (logfd >= 0) {
    ssize_t w = write(logfd, buf, len);
    (void)w; // a log that cannot be written is not worth an error path
  }
  if (logfd < 0 || (logoption & (LOG_PERROR | LOG_CONS))) {
    ssize_t w = write(2, buf, len);
    (void)w;
  }
  errno = saved;
}

void
syslog(int pri, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsyslog(pri, fmt, ap);
  va_end(ap);
}
