//
// su: become another user, having proved you may.
//
//   su [-] [-c COMMAND] [USER]
//
// root is not asked for a password, because root could take the
// account anyway; anyone else is. "-" makes it a login shell, which
// reads /etc/profile and starts in the home directory.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <termios.h>
#include <syslog.h>

static int
read_password(char *buf, int max)
{
  struct termios old, raw;
  int haveterm = tcgetattr(0, &old) == 0;

  if (haveterm) {
    raw = old;
    raw.c_lflag &= ~ECHO;
    tcsetattr(0, TCSANOW, &raw);
  }
  int n = 0;
  for (;;) {
    char c;
    if (read(0, &c, 1) <= 0) {
      n = -1;
      break;
    }
    if (c == '\n' || c == '\r')
      break;
    if ((c == 0x7f || c == 8) && n > 0) {
      n--;
      continue;
    }
    if (n < max - 1)
      buf[n++] = c;
  }
  if (n >= 0)
    buf[n] = 0;
  if (haveterm)
    tcsetattr(0, TCSANOW, &old);
  printf("\n");
  return n;
}

static int
check(const char *name, const char *password)
{
  struct spwd *sp = getspnam(name);

  if (sp == 0 || sp->sp_pwdp[0] == '*' || sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == 0)
    return 0;
  char *got = crypt(password, sp->sp_pwdp);
  if (got == 0)
    return 0;
  int diff = 0;
  for (int i = 0; got[i] || sp->sp_pwdp[i]; i++)
    diff |= got[i] ^ sp->sp_pwdp[i];
  return diff == 0;
}

int
main(int argc, char *argv[])
{
  const char *target = "root", *command = 0;
  int login_shell = 0, i = 1;

  for (; i < argc; i++) {
    if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0 ||
        strcmp(argv[i], "--login") == 0)
      login_shell = 1;
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
      command = argv[++i];
    else if (argv[i][0] == '-') {
      fprintf(stderr, "usage: su [-] [-c COMMAND] [USER]\n");
      exit(1);
    } else
      target = argv[i];
  }

  struct passwd *pw = getpwnam(target);
  if (pw == 0) {
    fprintf(stderr, "su: user %s does not exist\n", target);
    exit(1);
  }
  openlog("su", 0, LOG_AUTH);

  if (geteuid() != 0) {
    char password[128];
    printf("Password: ");
    fflush(stdout);
    if (read_password(password, sizeof(password)) < 0)
      exit(1);
    int ok = check(target, password);
    memset(password, 0, sizeof(password));
    if (!ok) {
      syslog(LOG_WARNING, "FAILED su for %s", target);
      usleep(2000 * 1000);
      fprintf(stderr, "su: Authentication failure\n");
      exit(1);
    }
  }
  syslog(LOG_INFO, "su to %s", target);

  gid_t groups[32];
  int n = 32;
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
    n = 1;
  if (setgroups(n, groups) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
    fprintf(stderr, "su: cannot become %s: %s\n", target, strerror(errno));
    exit(1);
  }

  const char *shell = pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
  if (login_shell) {
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", shell, 1);
    if (chdir(pw->pw_dir) < 0)
      chdir("/");
  }
  if (command) {
    char *argvv[] = {(char *)shell, "-c", (char *)command, 0};
    execv(shell, argvv);
  } else {
    char arg0[64];
    const char *base = shell;
    for (const char *p = shell; *p; p++)
      if (*p == '/')
        base = p + 1;
    snprintf(arg0, sizeof(arg0), "%s%s", login_shell ? "-" : "", base);
    char *argvv[] = {arg0, 0};
    execv(shell, argvv);
  }
  fprintf(stderr, "su: cannot run %s: %s\n", shell, strerror(errno));
  exit(1);
}
