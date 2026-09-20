//
// the small corners of the library: error text, assertions, getopt,
// <err.h>, locales, and the signal helpers that are not system calls.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <getopt.h>
#include <err.h>
#include <limits.h>
#include "libc.h"

// the program's name, which <err.h> puts in front of its messages
extern char *program_invocation_name, *program_invocation_short_name;

// ---- error text ----

// one table, indexed by the error number, with the gaps left empty.
static const char *const errtext[] = {
    [0] = "Success",
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "Input/output error",
    [ENXIO] = "No such device or address",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [EXDEV] = "Invalid cross-device link",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [ENFILE] = "Too many open files in system",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Inappropriate ioctl for device",
    [ETXTBSY] = "Text file busy",
    [EFBIG] = "File too large",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EMLINK] = "Too many links",
    [EPIPE] = "Broken pipe",
    [EDOM] = "Numerical argument out of domain",
    [ERANGE] = "Numerical result out of range",
    [EDEADLK] = "Resource deadlock avoided",
    [ENAMETOOLONG] = "File name too long",
    [ENOLCK] = "No locks available",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [ELOOP] = "Too many levels of symbolic links",
    [ENOMSG] = "No message of desired type",
    [EIDRM] = "Identifier removed",
    [ENOSTR] = "Device not a stream",
    [ENODATA] = "No data available",
    [ETIME] = "Timer expired",
    [EBADMSG] = "Bad message",
    [EOVERFLOW] = "Value too large for defined data type",
    [EILSEQ] = "Invalid or incomplete multibyte or wide character",
    [EUSERS] = "Too many users",
    [ENOTSOCK] = "Socket operation on non-socket",
    [EDESTADDRREQ] = "Destination address required",
    [EMSGSIZE] = "Message too long",
    [EPROTOTYPE] = "Protocol wrong type for socket",
    [ENOPROTOOPT] = "Protocol not available",
    [EPROTONOSUPPORT] = "Protocol not supported",
    [EOPNOTSUPP] = "Operation not supported",
    [EAFNOSUPPORT] = "Address family not supported by protocol",
    [EADDRINUSE] = "Address already in use",
    [EADDRNOTAVAIL] = "Cannot assign requested address",
    [ENETDOWN] = "Network is down",
    [ENETUNREACH] = "Network is unreachable",
    [ECONNABORTED] = "Software caused connection abort",
    [ECONNRESET] = "Connection reset by peer",
    [ENOBUFS] = "No buffer space available",
    [EISCONN] = "Transport endpoint is already connected",
    [ENOTCONN] = "Transport endpoint is not connected",
    [ETIMEDOUT] = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTUNREACH] = "No route to host",
    [EALREADY] = "Operation already in progress",
    [EINPROGRESS] = "Operation now in progress",
    [ESTALE] = "Stale file handle",
    [EDQUOT] = "Disk quota exceeded",
    [ECANCELED] = "Operation canceled",
    [EOWNERDEAD] = "Owner died",
    [ENOTRECOVERABLE] = "State not recoverable",
};

char *
strerror(int err)
{
  static char buf[48];

  if (err >= 0 && (size_t)err < sizeof(errtext) / sizeof(errtext[0]) && errtext[err])
    return (char *)errtext[err];
  snprintf(buf, sizeof(buf), "Unknown error %d", err);
  return buf;
}

int
strerror_r(int err, char *buf, size_t n)
{
  const char *s = strerror(err);

  if (strlen(s) + 1 > n)
    return ERANGE;
  strcpy(buf, s);
  return 0;
}

static const char *const signames[] = {
    [SIGHUP] = "Hangup",
    [SIGINT] = "Interrupt",
    [SIGQUIT] = "Quit",
    [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap",
    [SIGABRT] = "Aborted",
    [SIGBUS] = "Bus error",
    [SIGFPE] = "Floating point exception",
    [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2",
    [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock",
    [SIGTERM] = "Terminated",
    [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued",
    [SIGSTOP] = "Stopped (signal)",
    [SIGTSTP] = "Stopped",
    [SIGTTIN] = "Stopped (tty input)",
    [SIGTTOU] = "Stopped (tty output)",
    [SIGURG] = "Urgent I/O condition",
    [SIGXCPU] = "CPU time limit exceeded",
    [SIGXFSZ] = "File size limit exceeded",
    [SIGVTALRM] = "Virtual timer expired",
    [SIGPROF] = "Profiling timer expired",
    [SIGWINCH] = "Window changed",
    [SIGSYS] = "Bad system call",
};

char *
strsignal(int sig)
{
  static char buf[32];

  if (sig > 0 && (size_t)sig < sizeof(signames) / sizeof(signames[0]) && signames[sig])
    return (char *)signames[sig];
  snprintf(buf, sizeof(buf), "Unknown signal %d", sig);
  return buf;
}

void
psignal(int sig, const char *prefix)
{
  if (prefix && *prefix)
    fprintf(stderr, "%s: ", prefix);
  fprintf(stderr, "%s\n", strsignal(sig));
}

// ---- assertions ----

void
__assert_fail(const char *expr, const char *file, int line, const char *fn)
{
  fflush(0);
  fprintf(stderr, "%s:%d: %s: assertion failed: %s\n", file, line, fn, expr);
  abort();
}

// ---- <err.h> ----

void
vwarn(const char *fmt, va_list ap)
{
  int e = errno;

  fprintf(stderr, "%s: ", program_invocation_short_name);
  if (fmt)
    vfprintf(stderr, fmt, ap);
  fprintf(stderr, "%s%s\n", fmt ? ": " : "", strerror(e));
}

void
vwarnx(const char *fmt, va_list ap)
{
  fprintf(stderr, "%s: ", program_invocation_short_name);
  if (fmt)
    vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

void
verr(int status, const char *fmt, va_list ap)
{
  vwarn(fmt, ap);
  exit(status);
}

void
verrx(int status, const char *fmt, va_list ap)
{
  vwarnx(fmt, ap);
  exit(status);
}

void
warn(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vwarn(fmt, ap);
  va_end(ap);
}

void
warnx(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vwarnx(fmt, ap);
  va_end(ap);
}

void
err(int status, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  verr(status, fmt, ap);
  va_end(ap);
}

void
errx(int status, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  verrx(status, fmt, ap);
  va_end(ap);
}

// ---- getopt ----

char *optarg;
int optind = 1, opterr = 1, optopt;
static int optpos; // where we are inside a bundle like -abc

static void
reset_if_new(void)
{
  if (optind == 0) {
    optind = 1;
    optpos = 0;
  }
}

int
getopt(int argc, char *const argv[], const char *spec)
{
  reset_if_new();
  if (optpos == 0) {
    if (optind >= argc || argv[optind] == 0 || argv[optind][0] != '-' || argv[optind][1] == 0)
      return -1;
    if (strcmp(argv[optind], "--") == 0) {
      optind++;
      return -1;
    }
    optpos = 1;
  }

  int c = argv[optind][optpos++];
  const char *p = strchr(spec, c);
  if (argv[optind][optpos] == 0) {
    optind++;
    optpos = 0;
  }
  if (c == ':' || p == 0) {
    optopt = c;
    if (opterr && spec[0] != ':')
      fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
    return '?';
  }
  if (p[1] == ':') {
    optarg = 0;
    if (optpos) {
      optarg = (char *)argv[optind] + optpos;
      optind++;
      optpos = 0;
    } else if (p[2] != ':') {
      if (optind >= argc) {
        optopt = c;
        if (opterr && spec[0] != ':')
          fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
        return spec[0] == ':' ? ':' : '?';
      }
      optarg = (char *)argv[optind++];
    }
  }
  return c;
}

int
getopt_long(int argc, char *const argv[], const char *spec, const struct option *opts, int *index)
{
  reset_if_new();
  if (optpos == 0 && optind < argc && argv[optind] && argv[optind][0] == '-' &&
      argv[optind][1] == '-' && argv[optind][2] != 0) {
    const char *arg = argv[optind] + 2;
    const char *eq = strchr(arg, '=');
    size_t len = eq ? (size_t)(eq - arg) : strlen(arg);

    for (int i = 0; opts && opts[i].name; i++) {
      if (strncmp(opts[i].name, arg, len) != 0 || strlen(opts[i].name) != len)
        continue;
      optind++;
      if (index)
        *index = i;
      optarg = 0;
      if (opts[i].has_arg == required_argument) {
        if (eq) {
          optarg = (char *)eq + 1;
        } else if (optind < argc) {
          optarg = (char *)argv[optind++];
        } else {
          if (opterr)
            fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], opts[i].name);
          return '?';
        }
      } else if (opts[i].has_arg == optional_argument && eq) {
        optarg = (char *)eq + 1;
      }
      if (opts[i].flag) {
        *opts[i].flag = opts[i].val;
        return 0;
      }
      return opts[i].val;
    }
    if (opterr)
      fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], arg);
    optind++;
    return '?';
  }
  return getopt(argc, argv, spec);
}

int
getopt_long_only(int argc, char *const argv[], const char *spec, const struct option *opts, int *index)
{
  return getopt_long(argc, argv, spec, opts, index);
}

// ---- locales ----
//
// there are two: "C" and "C.UTF-8", which differ only in how many
// bytes a character may take, and the library is written for both.

static char current_locale[16] = "C";

char *
setlocale(int category, const char *name)
{
  (void)category;
  if (name == 0)
    return current_locale;
  if (*name == 0 || strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0) {
    strcpy(current_locale, "C");
    return current_locale;
  }
  if (strcasecmp(name, "C.UTF-8") == 0 || strcasecmp(name, "en_US.UTF-8") == 0) {
    strcpy(current_locale, "C.UTF-8");
    return current_locale;
  }
  return 0;
}

struct lconv *
localeconv(void)
{
  static char empty[] = "";
  static char point[] = ".";
  static struct lconv lc;

  lc.decimal_point = point;
  lc.thousands_sep = empty;
  lc.grouping = empty;
  lc.int_curr_symbol = empty;
  lc.currency_symbol = empty;
  lc.mon_decimal_point = empty;
  lc.mon_thousands_sep = empty;
  lc.mon_grouping = empty;
  lc.positive_sign = empty;
  lc.negative_sign = empty;
  lc.int_frac_digits = CHAR_MAX;
  lc.frac_digits = CHAR_MAX;
  lc.p_cs_precedes = CHAR_MAX;
  lc.p_sep_by_space = CHAR_MAX;
  lc.n_cs_precedes = CHAR_MAX;
  lc.n_sep_by_space = CHAR_MAX;
  lc.p_sign_posn = CHAR_MAX;
  lc.n_sign_posn = CHAR_MAX;
  return &lc;
}
