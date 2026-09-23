#include "rbtree.h"
#include "cred.h"
#include "signal.h"
// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;      // The process running on this cpu, or null.
  struct context context; // swtch() here to enter scheduler().
  int noff;               // Depth of push_off() nesting.
  int intena;             // Were interrupts enabled before push_off()?
  int resched;            // something with less virtual time is waiting
  int tick;               // a whole timer tick passed at the last interrupt
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// prepare_return() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
  /* 288 */ uint64 fregs[32];   // f0..f31 (F/D)
  /* 544 */ uint64 fcsr;
  /* 552 */ uint64 vcsrs[4];    // vstart, vtype, vl, vcsr (V extension)
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, STOPPED, ZOMBIE };

// signal dispositions, shared by the threads of a process
struct sighand {
  struct spinlock lock;
  int ref;
  struct sigaction act[NSIG];
};

// a pending signal and what is known about it
struct sigq {
  struct sigq *next;
  siginfo_t info;
};
#define SIGQUEUE_MAX 64

struct waitq;
struct mm;
struct inode;

// the open files of a process: threads share one of these.
struct fdtable {
  struct spinlock lock;
  int ref;
  struct file **ofile;
  char *flags;   // FD_CLOEXEC per descriptor
  int n;         // entries
};

// a thread group: the threads of one process share it, and the group is
// what the parent's wait() sees.
struct tgroup {
  struct spinlock lock;
  int ref;        // threads holding it
  int tgid;       // the process id
  int nthreads;   // threads still running
  int exiting;    // exit_group(): every thread is on its way out
  int exit_status;
};

// the file system view: the working directory (the root follows with
// mount namespaces).
struct fsinfo {
  struct spinlock lock;
  int ref;
  struct inode *cwd;
  // the directory "/" means for this process. zero is the file
  // system's own root; chroot(2) makes it something else, and a path
  // walk that reaches it cannot go further up.
  struct inode *root;
};


// what a hart's tree holds. Two things go in it: a task, and a group
// of tasks standing in for all of them (sched.c). Both embed one of
// these as their first scheduler field, and rb.tag says which.
#define SE_TASK  0
#define SE_GROUP 1
struct sched_ent {
  struct rb_node rb;      // rb.key is the virtual time below
  uint64 vruntime;
  uint32 weight;          // from the nice value, or the group's
  uint32 inv_weight;      // 2^32 / weight, so dividing by it is a multiply
  uint32 load_weight;     // the weight it is counted for in the hart's
                          // load, which is not weight once nice has
                          // changed under it
  int on_rq;              // in the tree (as opposed to on the processor)
  int counted;            // counted in the load and nr_running
};
// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state; // Process state
  void *chan;           // If non-zero, sleeping on chan
  int killed;           // If non-zero, have been killed
  int xstate;           // Exit status to be returned to parent's wait
  int pid;              // Process ID

  // wait_lock must be held when using these:
  struct proc *parent;   // Parent process
  struct proc *children; // first child (doubly linked through sibling/sibling_prev)
  struct proc *sibling, *sibling_prev;
  struct proc *zombies;  // exited children not yet waited for (singly linked through zombie_next)
  struct proc *zombie_next;
  int notify;            // CLD_STOPPED or CLD_CONTINUED waiting for the parent's wait()

  struct tgroup *tg;     // the thread group; getpid() reports its tgid
  uint64 clear_tid;      // clone(CLONE_CHILD_CLEARTID): zero it and wake on exit
  uint64 tf_va;          // where this thread's trapframe is mapped
  uint64 set_tid;        // clone(CLONE_CHILD_SETTID): where to store the tid
  uint64 futex_until;    // deadline of a timed futex wait, in ticks
  int autoreap;          // a thread nobody waits for: freed as it leaves the CPU

  // job control
  int pgid, sid;         // process group and session
  int tty;               // controlling terminal (0: none)
  int stopped_sig;       // the signal that stopped it, 0 when running
  // set between marking itself STOPPED and actually switching away.
  // in that window the process is still running on its own hart, so
  // whoever wakes it must not also put it on the run queue.
  int stop_parked;

  // ptrace: a process is watched by its own parent and by nobody
  // else. `traced` is set by PTRACE_TRACEME, `trace_wants_stop` by
  // PTRACE_SYSCALL, and `trace_at_exit` remembers that the stop on
  // the way in has happened so that the one on the way out follows.
  int traced;
  int trace_wants_stop;
  int trace_at_exit;

  // signals (per thread once threads arrive; per process for now)
  struct sighand *sighand;
  sigset_t sigblocked, sigpending;
  struct sigq *sigqueue;
  int nsigq;
  stack_t sigaltstk;
  sigset_t sigsaved;     // sigsuspend's mask, restored by sigreturn
  int sigsuspended, sigrestore_valid;
  uint64 alarm_at;       // tick at which SIGALRM is due, 0 for none
  int exitsig;           // the signal that killed it, for the wait status
  uint64 syscall_a0;     // the first argument of the system call in progress
  int restartable;       // it returned EINTR and SA_RESTART may retry it
  int norestart;         // ... unless the call is one POSIX never restarts
  uint64 utime, stime;   // ticks spent in user and kernel code
  uint64 cutime, cstime; // ... by reaped children
  uint64 minflt, majflt; // page faults served without and with I/O
  int umask;
  struct cred cred;      // who this process is (cred.h)
  // the nice value, -20 (most of the processor) to 19 (least), as
  // setpriority(2) sets it and nice(1) asks for. it is inherited by
  // fork and kept across exec. The scheduler turns it into a weight
  // (sched.c): one step of nice is worth about a tenth of the
  // processor between two tasks that want all of it.
  int nice;

  // proc_lock (proc.c) must be held when using these:
  struct proc *all_next, *all_prev; // every live process
  struct proc *pid_next;            // the pid hash chain

  // the wait queue's lock, then p->lock:
  struct waitq *wq;                 // the queue p is on, or 0
  struct proc *wq_next, *wq_prev;

  // the run queue's lock (sched.c). A task's place in the queue is
  // its virtual time: the processor time it has had, divided by its
  // weight. See kernel/sched.c.
  struct sched_ent se;    // its place in a hart's tree
  uint64 exec_start;      // when it last went on the processor
  uint64 sum_exec;        // processor time it has had
  uint64 slice_end;       // when the hart should look again
  int cpu;                // the hart whose queue it is on, or last ran on
  uint64 cpus;            // the harts it may run on, a bit each (affinity)
  struct sched_group *sg; // the group it belongs to, or 0 for the root

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  struct mm *mm;               // the address space (mm.h); 0 for a kernel thread
  struct mm *detached_mm;      // freed by proc_put() once no lock is held
  struct fdtable *detached_fdt;
  struct fsinfo *detached_fs;
  void (*kfn)(void *);         // kernel thread: the function to run
  void *karg;
  struct trapframe *trapframe; // data page for trampoline.S
  char *vstate;                // vector registers v0..v31 (32 * vlenb bytes), or 0
  struct context context;      // swtch() here to run process
  struct fdtable *fdt;         // open files, shared by the threads of a process
  struct fsinfo *fs;           // working directory, shared likewise

  char name[16];               // Process name (debugging)
};
