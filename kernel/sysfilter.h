#ifndef XV6_SYSFILTER_H
#define XV6_SYSFILTER_H
//
// what a process may ask the kernel for (kernel/sysfilter.c). A bitmap
// of denied calls, set once and inherited: a process may give up a call
// and may not take it back.
//

#define NSYSCALL 192          // room for every call number this kernel has

// the profiles sysfilter(2) knows by name
#define SYSFILTER_NONE      0
#define SYSFILTER_CONTAINER 1
// not a filter but the same kind of promise: exec may not gain anything
#define SYSFILTER_NO_NEW_PRIVS 2

struct sysfilter {
  int on;                     // 0: everything is allowed
  uint64 deny[NSYSCALL / 64];
};

void sysfilter_clear(struct sysfilter *f);
int sysfilter_denies(const struct sysfilter *f, int num);
int sysfilter_deny(struct sysfilter *f, int num);
int sysfilter_profile(struct sysfilter *f, int profile);

#endif
