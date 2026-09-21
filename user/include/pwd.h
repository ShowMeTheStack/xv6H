#ifndef _PWD_H
#define _PWD_H
#include <sys/types.h>
#include <stdio.h>

// one line of /etc/passwd: name:password:uid:gid:gecos:home:shell
struct passwd {
  char *pw_name;
  char *pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  char *pw_gecos;
  char *pw_dir;
  char *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
// walking the file: setpwent rewinds, getpwent gives the next line,
// endpwent closes it.
void setpwent(void);
struct passwd *getpwent(void);
void endpwent(void);
struct passwd *fgetpwent(FILE *f);
int putpwent(const struct passwd *p, FILE *f);

#endif
