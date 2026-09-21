//
// sudo: run a command as another user, if /etc/sudoers says you may.
//
//   sudo [-u USER] COMMAND...   run it
//   sudo -i                     a login shell as that user
//   sudo -s                     a shell as that user
//   sudo -l                     what you are allowed
//
// /etc/sudoers is lines of
//
//   WHO  HOST = (RUNAS) COMMANDS
//
// where WHO is a user name or %group, HOST is a host name or ALL,
// RUNAS is a user name or ALL, and COMMANDS is ALL or a comma-separated
// list of absolute paths. NOPASSWD: before the commands means the
// password is not asked for. the first matching line wins, which is
// not what real sudo does -- it takes the last -- but the files here
// are short and the rule is easier to reason about.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <termios.h>
#include <syslog.h>

#define SUDOERS "/etc/sudoers"

struct rule {
  int nopasswd;
  char runas[64];
  char commands[256];
};

static int
in_group(const char *user, const char *group)
{
  struct group *g = getgrnam(group);

  if (g == 0)
    return 0;
  struct passwd *pw = getpwnam(user);
  if (pw && pw->pw_gid == g->gr_gid)
    return 1;
  for (char **m = g->gr_mem; *m; m++)
    if (strcmp(*m, user) == 0)
      return 1;
  return 0;
}

// whitespace-trimmed copy
static char *
trim(char *s)
{
  while (*s == ' ' || *s == '\t')
    s++;
  int n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
    s[--n] = 0;
  return s;
}

// the first rule in /etc/sudoers that applies to this user
static int
find_rule(const char *user, struct rule *out)
{
  FILE *f = fopen(SUDOERS, "r");
  char line[512];

  if (f == 0)
    return 0;
  while (fgets(line, sizeof(line), f)) {
    char *p = trim(line);
    if (*p == 0 || *p == '#')
      continue;
    // WHO
    char who[64];
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < (int)sizeof(who) - 1)
      who[n++] = *p++;
    who[n] = 0;
    int applies = who[0] == '%' ? in_group(user, who + 1) : strcmp(who, user) == 0;
    if (!applies)
      continue;
    // HOST = ...
    p = trim(p);
    char *eq = strchr(p, '=');
    if (eq == 0)
      continue;
    *eq = 0;
    char *host = trim(p);
    if (strcmp(host, "ALL") != 0) {
      char hn[64];
      if (gethostname(hn, sizeof(hn)) != 0 || strcmp(hn, host) != 0)
        continue;
    }
    p = trim(eq + 1);
    memset(out, 0, sizeof(*out));
    // (RUNAS)
    if (*p == '(') {
      char *close = strchr(p, ')');
      if (close == 0)
        continue;
      *close = 0;
      snprintf(out->runas, sizeof(out->runas), "%s", trim(p + 1));
      p = trim(close + 1);
    } else {
      snprintf(out->runas, sizeof(out->runas), "root");
    }
    if (strncmp(p, "NOPASSWD:", 9) == 0) {
      out->nopasswd = 1;
      p = trim(p + 9);
    }
    snprintf(out->commands, sizeof(out->commands), "%s", p);
    fclose(f);
    return 1;
  }
  fclose(f);
  return 0;
}

static int
command_allowed(const struct rule *r, const char *path)
{
  char copy[256];

  if (strcmp(r->commands, "ALL") == 0)
    return 1;
  snprintf(copy, sizeof(copy), "%s", r->commands);
  for (char *p = copy; *p;) {
    char *comma = strchr(p, ',');
    if (comma)
      *comma = 0;
    if (strcmp(trim(p), path) == 0)
      return 1;
    if (comma == 0)
      break;
    p = comma + 1;
  }
  return 0;
}

static int
read_password(const char *prompt, char *buf, int max)
{
  struct termios old, raw;
  int haveterm = tcgetattr(0, &old) == 0;

  printf("%s", prompt);
  fflush(stdout);
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
check_password(const char *name)
{
  struct spwd *sp = getspnam(name);
  char password[128];

  if (sp == 0)
    return 0;
  if (read_password("[sudo] password: ", password, sizeof(password)) < 0)
    return 0;
  char *got = crypt(password, sp->sp_pwdp);
  int ok = got && strcmp(got, sp->sp_pwdp) == 0;
  memset(password, 0, sizeof(password));
  return ok;
}

// the program a command names, looked up on PATH so that the rule can
// be written against the path it will actually run
static int
resolve(const char *cmd, char *out, int max)
{
  if (strchr(cmd, '/')) {
    snprintf(out, max, "%s", cmd);
    return access(out, X_OK) == 0 ? 0 : -1;
  }
  const char *path = getenv("PATH");
  if (path == 0)
    path = "/bin:/sbin:/usr/bin";
  char copy[256];
  snprintf(copy, sizeof(copy), "%s", path);
  for (char *p = copy; p && *p;) {
    char *colon = strchr(p, ':');
    if (colon)
      *colon = 0;
    snprintf(out, max, "%s/%s", p, cmd);
    if (access(out, X_OK) == 0)
      return 0;
    p = colon ? colon + 1 : 0;
  }
  return -1;
}

int
main(int argc, char *argv[])
{
  const char *target = "root";
  int login_shell = 0, shell = 0, list = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
      target = argv[++i];
    else if (strcmp(argv[i], "-i") == 0)
      login_shell = 1;
    else if (strcmp(argv[i], "-s") == 0)
      shell = 1;
    else if (strcmp(argv[i], "-l") == 0)
      list = 1;
    else {
      fprintf(stderr, "usage: sudo [-u USER] [-i|-s|-l] [COMMAND ...]\n");
      exit(1);
    }
  }

  struct passwd *me = getpwuid(getuid());
  if (me == 0) {
    fprintf(stderr, "sudo: cannot tell who you are\n");
    exit(1);
  }
  char myname[64];
  snprintf(myname, sizeof(myname), "%s", me->pw_name);

  openlog("sudo", 0, LOG_AUTH);

  struct rule r;
  if (!find_rule(myname, &r)) {
    syslog(LOG_WARNING, "%s is not in the sudoers file", myname);
    fprintf(stderr, "sudo: %s is not in the sudoers file. This incident will be reported.\n",
            myname);
    exit(1);
  }
  if (list) {
    printf("User %s may run the following commands:\n", myname);
    printf("    (%s) %s%s\n", r.runas, r.nopasswd ? "NOPASSWD: " : "", r.commands);
    exit(0);
  }
  if (strcmp(r.runas, "ALL") != 0 && strcmp(r.runas, target) != 0) {
    fprintf(stderr, "sudo: you may not run commands as %s\n", target);
    exit(1);
  }

  struct passwd *pw = getpwnam(target);
  if (pw == 0) {
    fprintf(stderr, "sudo: unknown user %s\n", target);
    exit(1);
  }

  char program[256];
  char *const *cmdv;
  char *shellv[3];
  if (login_shell || shell || i >= argc) {
    snprintf(program, sizeof(program), "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
    shellv[0] = program;
    shellv[1] = 0;
    cmdv = shellv;
  } else {
    if (resolve(argv[i], program, sizeof(program)) < 0) {
      fprintf(stderr, "sudo: %s: command not found\n", argv[i]);
      exit(1);
    }
    cmdv = &argv[i];
  }
  if (!command_allowed(&r, program)) {
    syslog(LOG_WARNING, "%s tried to run %s", myname, program);
    fprintf(stderr, "sudo: %s is not one of the commands you may run\n", program);
    exit(1);
  }
  // root is never asked; anyone else is, unless the rule says not to
  if (getuid() != 0 && !r.nopasswd && !check_password(myname)) {
    syslog(LOG_WARNING, "authentication failure for %s", myname);
    usleep(2000 * 1000);
    fprintf(stderr, "sudo: Authentication failure\n");
    exit(1);
  }
  syslog(LOG_INFO, "%s : TTY=console ; USER=%s ; COMMAND=%s", myname, target, program);

  gid_t groups[32];
  int n = 32;
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
    n = 1;
  if (setgroups(n, groups) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
    fprintf(stderr, "sudo: cannot become %s: %s\n", target, strerror(errno));
    exit(1);
  }
  if (login_shell) {
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    if (chdir(pw->pw_dir) < 0)
      chdir("/");
  }
  setenv("SUDO_USER", myname, 1);
  execv(program, (char *const *)cmdv);
  fprintf(stderr, "sudo: %s: %s\n", program, strerror(errno));
  exit(1);
}
