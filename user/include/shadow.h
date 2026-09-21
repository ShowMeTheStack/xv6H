#ifndef _SHADOW_H
#define _SHADOW_H
#include <sys/types.h>
#include <stdio.h>

// one line of /etc/shadow: name:hash:lastchange:min:max:warn:inactive:expire:flag
struct spwd {
  char *sp_namp;
  char *sp_pwdp;
  long sp_lstchg;
  long sp_min, sp_max, sp_warn, sp_inact, sp_expire;
  unsigned long sp_flag;
};

struct spwd *getspnam(const char *name);
void setspent(void);
struct spwd *getspent(void);
void endspent(void);

// the password hash this system stores: pbkdf2 over sha-256, in the
// form $pbkdf2-sha256$ROUNDS$SALT$HASH, with the salt and the hash in
// base64. crypt() takes a setting of that shape and returns the whole
// string, so that checking a password is strcmp against what is
// stored.
char *crypt(const char *password, const char *setting);
// a fresh setting with `rounds` iterations and a random salt
char *crypt_gensalt(int rounds);

#endif
