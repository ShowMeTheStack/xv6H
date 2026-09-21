#ifndef XV6_CRED_H
#define XV6_CRED_H
#include "types.h"
//
// who a process is.
//
// three user ids and three group ids, as POSIX asks: the real one is
// who logged in, the effective one is what every permission check uses,
// and the saved one is what a set-user-id program may go back to. a
// process may always set any of them to any of the other three; only a
// privileged one may set them to something else.
//
// capabilities split what "privileged" means, so that a program that
// needs to bind port 80 does not also get to read every file. the
// effective set is what is checked; the permitted set is what may be
// raised into it; the bounding set is the ceiling that exec cannot
// lift.
//
#define NGROUPS_MAX 32

#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_FSETID           4
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_SETPCAP          8
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_RAW          13
#define CAP_SYS_CHROOT       18
#define CAP_SYS_ADMIN        21
#define CAP_SYS_BOOT         22
#define CAP_SYS_NICE         23
#define CAP_SYS_RESOURCE     24
#define CAP_SYS_TIME         25
#define CAP_MKNOD            27
#define CAP_AUDIT_WRITE      29
#define CAP_NET_ADMIN        12
#define CAP_LINUX_IMMUTABLE   9
#define CAP_SYS_PTRACE       19
#define CAP_LAST_CAP         29

#define CAP_BIT(c) (1UL << (c))
#define CAP_FULL   0x3fffffffUL // every capability this kernel defines

struct cred {
  uint uid, euid, suid;   // real, effective, saved
  uint gid, egid, sgid;
  uint groups[NGROUPS_MAX];
  int ngroups;
  uint64 cap_effective;   // what is checked
  uint64 cap_permitted;   // what may be raised into the effective set
  uint64 cap_inheritable; // what survives exec of an ordinary program
  uint64 cap_bounding;    // the ceiling exec cannot lift
  int no_new_privs;       // exec may not gain anything
};

#endif
