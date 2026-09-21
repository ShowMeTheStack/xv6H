//
// the user and group databases: /etc/passwd, /etc/group, /etc/shadow.
//
// each is a line per entry with colon-separated fields, which is the
// format every unix uses and every tool here reads and writes. a
// lookup reads the file from the start; there is no cache, because a
// program that changes a password and one that reads it must not
// disagree, and these files are a few hundred bytes.
//
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LINEMAX 512
#define MAXMEM  32

// the buffers a returned entry points into. one per kind, as POSIX
// allows: the next call overwrites them.
static char pw_line[LINEMAX];
static struct passwd pw_entry;
static FILE *pw_file;

static char gr_line[LINEMAX];
static struct group gr_entry;
static char *gr_members[MAXMEM + 1];
static FILE *gr_file;

static char sp_line[LINEMAX];
static struct spwd sp_entry;
static FILE *sp_file;

// split a line on colons, in place. returns how many fields there
// were; a missing field is an empty string rather than a null pointer.
static int
split(char *line, char **fields, int max)
{
  int n = 0;

  fields[n++] = line;
  for (char *p = line; *p && n < max; p++)
    if (*p == ':') {
      *p = 0;
      fields[n++] = p + 1;
    }
  // the newline at the end is not part of the last field
  char *last = fields[n - 1];
  int len = strlen(last);
  if (len > 0 && last[len - 1] == '\n')
    last[len - 1] = 0;
  return n;
}

// ---- /etc/passwd ----

static struct passwd *
pw_parse(char *line)
{
  char *f[8];

  if (line[0] == '#' || line[0] == '\n' || line[0] == 0)
    return 0;
  if (split(line, f, 7) < 7)
    return 0;
  pw_entry.pw_name = f[0];
  pw_entry.pw_passwd = f[1];
  pw_entry.pw_uid = (uid_t)strtoul(f[2], 0, 10);
  pw_entry.pw_gid = (gid_t)strtoul(f[3], 0, 10);
  pw_entry.pw_gecos = f[4];
  pw_entry.pw_dir = f[5];
  pw_entry.pw_shell = f[6];
  return &pw_entry;
}

struct passwd *
fgetpwent(FILE *f)
{
  while (fgets(pw_line, sizeof(pw_line), f)) {
    struct passwd *p = pw_parse(pw_line);
    if (p)
      return p;
  }
  return 0;
}

void
setpwent(void)
{
  if (pw_file)
    fclose(pw_file);
  pw_file = fopen("/etc/passwd", "r");
}

struct passwd *
getpwent(void)
{
  if (pw_file == 0)
    setpwent();
  if (pw_file == 0)
    return 0;
  return fgetpwent(pw_file);
}

void
endpwent(void)
{
  if (pw_file) {
    fclose(pw_file);
    pw_file = 0;
  }
}

struct passwd *
getpwnam(const char *name)
{
  struct passwd *p;

  setpwent();
  while ((p = getpwent()) != 0)
    if (strcmp(p->pw_name, name) == 0) {
      endpwent();
      return p;
    }
  endpwent();
  return 0;
}

struct passwd *
getpwuid(uid_t uid)
{
  struct passwd *p;

  setpwent();
  while ((p = getpwent()) != 0)
    if (p->pw_uid == uid) {
      endpwent();
      return p;
    }
  endpwent();
  return 0;
}

int
putpwent(const struct passwd *p, FILE *f)
{
  return fprintf(f, "%s:%s:%u:%u:%s:%s:%s\n", p->pw_name, p->pw_passwd, (unsigned)p->pw_uid,
                 (unsigned)p->pw_gid, p->pw_gecos, p->pw_dir, p->pw_shell) < 0
             ? -1
             : 0;
}

// ---- /etc/group ----

static struct group *
gr_parse(char *line)
{
  char *f[5];

  if (line[0] == '#' || line[0] == '\n' || line[0] == 0)
    return 0;
  if (split(line, f, 4) < 4)
    return 0;
  gr_entry.gr_name = f[0];
  gr_entry.gr_passwd = f[1];
  gr_entry.gr_gid = (gid_t)strtoul(f[2], 0, 10);

  int n = 0;
  char *p = f[3];
  while (*p && n < MAXMEM) {
    gr_members[n++] = p;
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      *p++ = 0;
  }
  gr_members[n] = 0;
  gr_entry.gr_mem = gr_members;
  return &gr_entry;
}

struct group *
fgetgrent(FILE *f)
{
  while (fgets(gr_line, sizeof(gr_line), f)) {
    struct group *g = gr_parse(gr_line);
    if (g)
      return g;
  }
  return 0;
}

void
setgrent(void)
{
  if (gr_file)
    fclose(gr_file);
  gr_file = fopen("/etc/group", "r");
}

struct group *
getgrent(void)
{
  if (gr_file == 0)
    setgrent();
  if (gr_file == 0)
    return 0;
  return fgetgrent(gr_file);
}

void
endgrent(void)
{
  if (gr_file) {
    fclose(gr_file);
    gr_file = 0;
  }
}

struct group *
getgrnam(const char *name)
{
  struct group *g;

  setgrent();
  while ((g = getgrent()) != 0)
    if (strcmp(g->gr_name, name) == 0) {
      endgrent();
      return g;
    }
  endgrent();
  return 0;
}

struct group *
getgrgid(gid_t gid)
{
  struct group *g;

  setgrent();
  while ((g = getgrent()) != 0)
    if (g->gr_gid == gid) {
      endgrent();
      return g;
    }
  endgrent();
  return 0;
}

// every group a user belongs to: the one in their passwd entry, and
// every group that names them as a member.
int
getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups)
{
  int room = *ngroups, n = 0;
  struct group *g;

  if (n < room)
    groups[n] = group;
  n++;
  setgrent();
  while ((g = getgrent()) != 0) {
    if (g->gr_gid == group)
      continue;
    for (char **m = g->gr_mem; *m; m++)
      if (strcmp(*m, user) == 0) {
        if (n < room)
          groups[n] = g->gr_gid;
        n++;
        break;
      }
  }
  endgrent();
  int fit = n <= room;
  *ngroups = n;
  return fit ? n : -1;
}

int
initgroups(const char *user, gid_t group)
{
  gid_t groups[32];
  int n = 32;

  if (getgrouplist(user, group, groups, &n) < 0)
    n = 32;
  return setgroups(n, groups);
}

// ---- /etc/shadow ----

static struct spwd *
sp_parse(char *line)
{
  char *f[9];

  if (line[0] == '#' || line[0] == '\n' || line[0] == 0)
    return 0;
  int n = split(line, f, 9);
  if (n < 2)
    return 0;
  memset(&sp_entry, 0, sizeof(sp_entry));
  sp_entry.sp_namp = f[0];
  sp_entry.sp_pwdp = f[1];
  sp_entry.sp_lstchg = n > 2 && f[2][0] ? strtol(f[2], 0, 10) : -1;
  sp_entry.sp_min = n > 3 && f[3][0] ? strtol(f[3], 0, 10) : -1;
  sp_entry.sp_max = n > 4 && f[4][0] ? strtol(f[4], 0, 10) : -1;
  sp_entry.sp_warn = n > 5 && f[5][0] ? strtol(f[5], 0, 10) : -1;
  sp_entry.sp_inact = n > 6 && f[6][0] ? strtol(f[6], 0, 10) : -1;
  sp_entry.sp_expire = n > 7 && f[7][0] ? strtol(f[7], 0, 10) : -1;
  return &sp_entry;
}

void
setspent(void)
{
  if (sp_file)
    fclose(sp_file);
  sp_file = fopen("/etc/shadow", "r");
}

struct spwd *
getspent(void)
{
  if (sp_file == 0)
    setspent();
  if (sp_file == 0)
    return 0;
  while (fgets(sp_line, sizeof(sp_line), sp_file)) {
    struct spwd *p = sp_parse(sp_line);
    if (p)
      return p;
  }
  return 0;
}

void
endspent(void)
{
  if (sp_file) {
    fclose(sp_file);
    sp_file = 0;
  }
}

struct spwd *
getspnam(const char *name)
{
  struct spwd *p;

  setspent();
  while ((p = getspent()) != 0)
    if (strcmp(p->sp_namp, name) == 0) {
      endspent();
      return p;
    }
  endspent();
  errno = EACCES;
  return 0;
}
