//
// passwd: change a password.
//
//   passwd [USER]      set it (root may set anyone's; you must give
//                      your own old one first)
//   passwd -l USER     lock the account
//   passwd -u USER     unlock it
//   passwd -d USER     remove the password entirely
//   passwd -S USER     say what state it is in
//
// /etc/shadow is rewritten through a temporary file and renamed, so a
// crash leaves the old file rather than half of the new one.
//
#include "user/user.h"
#include <pwd.h>
#include <shadow.h>
#include <termios.h>
#include <syslog.h>

#define SHADOW "/etc/shadow"
#define SHADOW_TMP "/etc/shadow.new"

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

// rewrite /etc/shadow with a new hash for one account
static int
store(const char *name, const char *hash)
{
  FILE *in = fopen(SHADOW, "r");
  FILE *out = fopen(SHADOW_TMP, "w");
  char line[512];
  int found = 0;

  if (in == 0 || out == 0) {
    fprintf(stderr, "passwd: cannot open %s: %s\n", SHADOW, strerror(errno));
    if (in)
      fclose(in);
    if (out)
      fclose(out);
    return -1;
  }
  while (fgets(line, sizeof(line), in)) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", line);
    char *colon = strchr(copy, ':');
    if (colon) {
      *colon = 0;
      if (strcmp(copy, name) == 0) {
        // keep every field but the hash
        char *rest = strchr(colon + 1, ':');
        fprintf(out, "%s:%s:%s", name, hash, rest ? rest + 1 : "19000:0:99999:7:::\n");
        found = 1;
        continue;
      }
    }
    fputs(line, out);
  }
  if (!found)
    fprintf(out, "%s:%s:19000:0:99999:7:::\n", name, hash);
  fclose(in);
  if (fclose(out) != 0) {
    fprintf(stderr, "passwd: cannot write %s: %s\n", SHADOW_TMP, strerror(errno));
    unlink(SHADOW_TMP);
    return -1;
  }
  chmod(SHADOW_TMP, 0600);
  if (rename(SHADOW_TMP, SHADOW) < 0) {
    fprintf(stderr, "passwd: cannot replace %s: %s\n", SHADOW, strerror(errno));
    unlink(SHADOW_TMP);
    return -1;
  }
  return 0;
}

static const char *
current_hash(const char *name)
{
  static char buf[256];
  struct spwd *sp = getspnam(name);

  if (sp == 0)
    return 0;
  snprintf(buf, sizeof(buf), "%s", sp->sp_pwdp);
  return buf;
}

int
main(int argc, char *argv[])
{
  int lock = 0, unlock = 0, del = 0, status = 0, i = 1;
  const char *name = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-l") == 0)
      lock = 1;
    else if (strcmp(argv[i], "-u") == 0)
      unlock = 1;
    else if (strcmp(argv[i], "-d") == 0)
      del = 1;
    else if (strcmp(argv[i], "-S") == 0)
      status = 1;
    else {
      fprintf(stderr, "usage: passwd [-l|-u|-d|-S] [USER]\n");
      exit(1);
    }
  }
  struct passwd *me = getpwuid(getuid());
  name = i < argc ? argv[i] : (me ? me->pw_name : 0);
  if (name == 0) {
    fprintf(stderr, "passwd: cannot tell who you are\n");
    exit(1);
  }
  if (getpwnam(name) == 0) {
    fprintf(stderr, "passwd: user %s does not exist\n", name);
    exit(1);
  }
  int self = me && strcmp(me->pw_name, name) == 0;
  if (!self && geteuid() != 0) {
    fprintf(stderr, "passwd: only root may change another user's password\n");
    exit(1);
  }
  openlog("passwd", 0, LOG_AUTH);

  const char *cur = current_hash(name);
  if (cur == 0) {
    fprintf(stderr, "passwd: cannot read %s: %s\n", SHADOW, strerror(errno));
    exit(1);
  }

  if (status) {
    printf("%s %s\n", name,
           cur[0] == '!' ? "L" : (cur[0] == 0 ? "NP" : (cur[0] == '*' ? "L" : "P")));
    exit(0);
  }
  if (lock || unlock || del) {
    if (geteuid() != 0) {
      fprintf(stderr, "passwd: only root may do that\n");
      exit(1);
    }
    char newhash[256];
    if (del)
      newhash[0] = 0;
    else if (lock)
      snprintf(newhash, sizeof(newhash), "!%s", cur[0] == '!' ? cur + 1 : cur);
    else
      snprintf(newhash, sizeof(newhash), "%s", cur[0] == '!' ? cur + 1 : cur);
    if (store(name, newhash) < 0)
      exit(1);
    syslog(LOG_INFO, "password for %s %s", name, del ? "removed" : lock ? "locked" : "unlocked");
    printf("passwd: password %s\n", del ? "deleted" : lock ? "locked" : "unlocked");
    exit(0);
  }

  // changing it: your own old password first, unless you are root
  char old[128], a[128], b[128];
  if (self && geteuid() != 0 && cur[0] && cur[0] != '*' && cur[0] != '!') {
    if (read_password("Current password: ", old, sizeof(old)) < 0)
      exit(1);
    char *got = crypt(old, cur);
    memset(old, 0, sizeof(old));
    if (got == 0 || strcmp(got, cur) != 0) {
      syslog(LOG_WARNING, "failed password change for %s", name);
      usleep(2000 * 1000);
      fprintf(stderr, "passwd: Authentication failure\n");
      exit(1);
    }
  }
  if (read_password("New password: ", a, sizeof(a)) < 0)
    exit(1);
  if (read_password("Retype new password: ", b, sizeof(b)) < 0)
    exit(1);
  if (strcmp(a, b) != 0) {
    fprintf(stderr, "passwd: passwords do not match\n");
    exit(1);
  }
  if (strlen(a) < 4) {
    fprintf(stderr, "passwd: a password of fewer than four characters is not worth having\n");
    exit(1);
  }
  char *setting = crypt_gensalt(100000);
  if (setting == 0) {
    fprintf(stderr, "passwd: no random salt available\n");
    exit(1);
  }
  char *hash = crypt(a, setting);
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  if (hash == 0 || store(name, hash) < 0)
    exit(1);
  syslog(LOG_INFO, "password changed for %s", name);
  printf("passwd: password updated successfully\n");
  exit(0);
}
