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

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CMD_MASK       (~(FUTEX_PRIVATE_FLAG))
#endif
