//
// login: ask who someone is, check it, and become them.
//
// the password is read with the terminal's echo turned off, the hash
// is compared with what /etc/shadow holds, and a failure costs a few
// seconds and a line in the log -- which is what makes guessing
// expensive rather than free. on success every trace of being root is
// dropped before the shell is started: the groups, the group id and
// the user id, in that order, because the calls that set the first two
// need the privilege the third one gives up.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <termios.h>
#include <syslog.h>
#include <utmp.h>

#define RETRIES 3
#define FAIL_DELAY_MS 3000

// read a line with the terminal's echo off, so the password is not on
// the screen for whoever walks past
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
    int r = read(0, &c, 1);
    if (r <= 0) {
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

// is this the right password for this account?
static int
check(const char *name, const char *password)
{
  struct spwd *sp = getspnam(name);

  if (sp == 0)
    return 0;
  // "*" or "!" as the hash means the account has no password anyone
  // can give: it cannot be logged into at all
  if (sp->sp_pwdp[0] == '*' || sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == 0)
    return 0;
  char *got = crypt(password, sp->sp_pwdp);
  if (got == 0)
    return 0;
  // a comparison that takes the same time whatever the answer, so the
  // reply does not say how much of the hash was right
  int diff = 0;
  const char *a = got, *b = sp->sp_pwdp;
  for (int i = 0; a[i] || b[i]; i++)
    diff |= a[i] ^ b[i];
  return diff == 0;
}

// become this user and run their shell
static void
become(struct passwd *pw, int preserve_env)
{
  gid_t groups[32];
  int n = 32;

  if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
    n = 1;
  // the order matters: setuid last, because the two before it need the
  // privilege it gives up
  if (setgroups(n, groups) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
    fprintf(stderr, "login: cannot become %s: %s\n", pw->pw_name, strerror(errno));
    exit(1);
  }
  if (!preserve_env) {
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh", 1);
    setenv("PATH", pw->pw_uid == 0 ? "/sbin:/bin:/usr/bin" : "/bin:/usr/bin", 1);
  }
  if (chdir(pw->pw_dir) < 0)
    chdir("/");

  const char *shell = pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
  // a login shell is named with a leading dash, which is how it knows
  // to read /etc/profile
  char arg0[64];
  const char *base = shell;
  for (const char *p = shell; *p; p++)
    if (*p == '/')
      base = p + 1;
  snprintf(arg0, sizeof(arg0), "-%s", base);
  char *argv[] = {arg0, 0};
  execv(shell, argv);
  fprintf(stderr, "login: cannot run %s: %s\n", shell, strerror(errno));
  exit(1);
}

int
main(int argc, char *argv[])
{
  char name[64], password[128];
  const char *given = 0;

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-')
      given = argv[i];
  }
  openlog("login", 0, LOG_AUTH);

  for (int tries = 0; tries < RETRIES; tries++) {
    if (given && tries == 0) {
      snprintf(name, sizeof(name), "%s", given);
    } else {
      printf("login: ");
      fflush(stdout);
      if (fgets(name, sizeof(name), stdin) == 0)
        exit(1);
      int n = strlen(name);
      while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r'))
        name[--n] = 0;
      if (n == 0)
        continue;
    }
    printf("Password: ");
    fflush(stdout);
    if (read_password(password, sizeof(password)) < 0)
      exit(1);

    struct passwd *pw = getpwnam(name);
    if (pw && check(name, password)) {
      syslog(LOG_INFO, "session opened for user %s", name);
      // who is logged in, for who(1), and the record of it for last(1)
      const char *line = ttyname(0);
      utmp_login(name, line ? line : "console", getpid());
      memset(password, 0, sizeof(password));
      become(pw, 0);
    }
    memset(password, 0, sizeof(password));
    syslog(LOG_WARNING, "failed login for %s", name);
    // the delay is what makes guessing expensive, and the message
    // never says which of the two was wrong
    usleep(FAIL_DELAY_MS * 1000);
    printf("Login incorrect\n");
  }
  exit(1);
}
