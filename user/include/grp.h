#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>
#include <stdio.h>

// one line of /etc/group: name:password:gid:member,member,...
struct group {
  char *gr_name;
  char *gr_passwd;
  gid_t gr_gid;
  char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
void setgrent(void);
struct group *getgrent(void);
void endgrent(void);
struct group *fgetgrent(FILE *f);

// every group a user is in, from /etc/group and the user's own entry
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);
int initgroups(const char *user, gid_t group);

#endif
