#ifndef XV6_THREAD_H
#define XV6_THREAD_H
// clone() flags (the values Linux uses) and the futex operations.
#define CSIGNAL              0x000000ff
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000
#define CLONE_CHILD_SETTID   0x01000000
// the namespaces (kernel/nsproxy.h). CLONE_NEWNS is the odd one out
// because it is the oldest: it was the only namespace there was.
#define CLONE_NEWNS          0x00020000
#define CLONE_NEWCGROUP      0x02000000
#define CLONE_NEWUTS         0x04000000
#define CLONE_NEWIPC         0x08000000
#define CLONE_NEWUSER        0x10000000
#define CLONE_NEWPID         0x20000000
#define CLONE_NEWNET         0x40000000
// all of them, and the ones this revision does not have yet: asking for
// one of those is an error rather than a namespace that is not there.
#define CLONE_NEW_ANY                                                          \
  (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC |               \
   CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)
#define CLONE_NEW_UNSUPPORTED (CLONE_NEWCGROUP | CLONE_NEWIPC)

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CMD_MASK       (~(FUTEX_PRIVATE_FLAG))
#endif
