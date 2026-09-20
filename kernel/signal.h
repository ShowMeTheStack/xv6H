#ifndef XV6_SIGNAL_H
#define XV6_SIGNAL_H
#ifndef __ASSEMBLER__
#include "types.h"
#endif
//
// POSIX signals: numbers, sets, dispositions, and the structures the
// kernel puts on the user stack when it delivers one.
//
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   29
#define SIGPWR    30
#define SIGSYS    31
#define SIGRTMIN  32
#define SIGRTMAX  63
#define NSIG      64   // signals 1..63; bit (sig-1) of a sigset_t

typedef unsigned long sigset_t;

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

// sigprocmask
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// sa_flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

// si_code
#define SI_USER     0
#define SI_QUEUE   -1
#define SI_TIMER   -2
#define SI_KERNEL  128
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6
#define SEGV_MAPERR   1
#define SEGV_ACCERR   2
#define BUS_ADRERR    2
#define ILL_ILLOPC    1
#define FPE_INTDIV    1

union sigval {
  int sival_int;
  void *sival_ptr;
};

typedef struct {
  int si_signo;
  int si_errno;
  int si_code;
  int si_pid;
  int si_uid;
  int si_status;
  void *si_addr;
  union sigval si_value;
  long _pad[8];
} siginfo_t;

struct sigaction {
  void *sa_handler; // void (*)(int), or void (*)(int, siginfo_t *, void *) with SA_SIGINFO
  unsigned long sa_flags;
  void *sa_restorer; // unused: the kernel returns through its own trampoline
  sigset_t sa_mask;
};

// sigaltstack
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 4096
#define SIGSTKSZ    16384
typedef struct {
  void *ss_sp;
  int ss_flags;
  unsigned long ss_size;
} stack_t;

// the interrupted machine state, in the frame the kernel builds
typedef struct {
  unsigned long pc;
  unsigned long regs[31]; // x1..x31
  unsigned long fregs[32];
  unsigned long fcsr;
  unsigned long vcsrs[4];
  unsigned long vlenb; // bytes per vector register if the state follows the frame, else 0
} mcontext_t;

typedef struct ucontext {
  unsigned long uc_flags;
  struct ucontext *uc_link;
  stack_t uc_stack;
  sigset_t uc_sigmask;
  mcontext_t uc_mcontext;
} ucontext_t;

// what the kernel pushes on the user stack: the handler sees
// a0 = signo, a1 = &info, a2 = &uc, ra = the kernel's sigreturn trampoline.
struct sigframe {
  siginfo_t info;
  ucontext_t uc;
  // 32 * vlenb bytes of vector state follow when uc.uc_mcontext.vlenb != 0
};

// wait4 options and the status macros
#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   2
#define WCONTINUED 8
#define WEXITED    4
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WCOREDUMP(s)    0
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)

// setitimer
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

// sigset helpers (usable from both sides)
static inline int sigismember_(sigset_t s, int sig) { return (s >> (sig - 1)) & 1; }
static inline sigset_t sigmask_(int sig) { return 1UL << (sig - 1); }
#define SIG_UNCATCHABLE (sigmask_(SIGKILL) | sigmask_(SIGSTOP))

#endif
