#ifndef XV6_SCHEDGROUP_H
#define XV6_SCHEDGROUP_H
//
// the sched_group() system call: the scheduler's groups seen from
// user space. `cgrun' uses it, and the cgroup file system of section
// 8 will sit on it.
//
//   sched_group(op, id, a, b)
//
#define SGC_CREATE  0   // a = the name         -> the new group's id
#define SGC_JOIN    1   // id, or -1 for the root
#define SGC_WEIGHT  2   // id, a = weight (1024 is a nice-0 task's share)
#define SGC_MAX     3   // id, a = quota us per b = period us; a 0 removes it
#define SGC_FREEZE  4   // id, a = 0 or 1
#define SGC_DESTROY 5   // id
#define SGC_INFO    6   // id, a = room for eight numbers:
                        //   tasks, weight, quota us, period us,
                        //   throttles, frozen, runnable, usage us
#endif
