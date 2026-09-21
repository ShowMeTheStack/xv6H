#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"
#include "errno.h"
#include "mm.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if (addr + sizeof(uint64) < addr) // overflow
    return -1;
  if (copyin(p->mm, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if (copyinstr(p->mm, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (not including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_vm_create(void);
extern uint64 sys_vm_ctl(void);
extern uint64 sys_vm_stats(void);
extern uint64 sys_hwinfo(void);
extern uint64 sys_socket(void);
extern uint64 sys_bind(void);
extern uint64 sys_connect(void);
extern uint64 sys_listen(void);
extern uint64 sys_accept(void);
extern uint64 sys_sendto(void);
extern uint64 sys_recvfrom(void);
extern uint64 sys_netinfo(void);
extern uint64 sys_sockopt(void);
extern uint64 sys_setpriority(void);
extern uint64 sys_getpriority(void);
extern uint64 sys_ptrace(void);
extern uint64 sys_clock_gettime(void);
extern uint64 sys_sysconf(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_mprotect(void);
extern uint64 sys_sigaction(void);
extern uint64 sys_sigprocmask(void);
extern uint64 sys_sigpending(void);
extern uint64 sys_sigsuspend(void);
extern uint64 sys_sigaltstack(void);
extern uint64 sys_sigreturn(void);
extern uint64 sys_sigqueue(void);
extern uint64 sys_alarm(void);
extern uint64 sys_wait4(void);
extern uint64 sys_setpgid(void);
extern uint64 sys_getpgid(void);
extern uint64 sys_setsid(void);
extern uint64 sys_getsid(void);
extern uint64 sys_getppid(void);
extern uint64 sys_lseek(void);
extern uint64 sys_pread(void);
extern uint64 sys_pwrite(void);
extern uint64 sys_readv(void);
extern uint64 sys_writev(void);
extern uint64 sys_dup2(void);
extern uint64 sys_dup3(void);
extern uint64 sys_fcntl(void);
extern uint64 sys_pipe2(void);
extern uint64 sys_ftruncate(void);
extern uint64 sys_ioctl(void);
extern uint64 sys_poll(void);
extern uint64 sys_select(void);
extern uint64 sys_execve(void);
extern uint64 sys_nanosleep(void);
extern uint64 sys_times(void);
extern uint64 sys_getrusage(void);
extern uint64 sys_uname(void);
extern uint64 sys_sethostname(void);
extern uint64 sys_gethostname(void);
extern uint64 sys_umask(void);
extern uint64 sys_getrandom(void);
extern uint64 sys_vm_run(void);
extern uint64 sys_vm_destroy(void);
extern uint64 sys_vm_cons(void);
extern uint64 sys_vm_info(void);
extern uint64 sys_consraw(void);
extern uint64 sys_sync(void);
extern uint64 sys_clone(void);
extern uint64 sys_futex(void);
extern uint64 sys_gettid(void);
extern uint64 sys_tkill(void);
extern uint64 sys_tgkill(void);
extern uint64 sys_exit_group(void);
extern uint64 sys_set_tid_address(void);
extern uint64 sys_mount(void);
extern uint64 sys_umount(void);
extern uint64 sys_getdents(void);
extern uint64 sys_statfs(void);
extern uint64 sys_fsync(void);
extern uint64 sys_symlink(void);
extern uint64 sys_readlink(void);
extern uint64 sys_rename(void);
extern uint64 sys_fstatat(void);
extern uint64 sys_readlinkat(void);
extern uint64 sys_unlinkat(void);
extern uint64 sys_faccessat(void);
extern uint64 sys_openat(void);
extern uint64 sys_mkdirat(void);
extern uint64 sys_renameat(void);
extern uint64 sys_linkat(void);
extern uint64 sys_symlinkat(void);
extern uint64 sys_setxattr(void);
extern uint64 sys_getxattr(void);
extern uint64 sys_listxattr(void);
extern uint64 sys_removexattr(void);
extern uint64 sys_getuid(void);
extern uint64 sys_geteuid(void);
extern uint64 sys_getgid(void);
extern uint64 sys_getegid(void);
extern uint64 sys_setuid(void);
extern uint64 sys_setgid(void);
extern uint64 sys_setresuid(void);
extern uint64 sys_setresgid(void);
extern uint64 sys_getresuid(void);
extern uint64 sys_getresgid(void);
extern uint64 sys_getgroups(void);
extern uint64 sys_setgroups(void);
extern uint64 sys_capget(void);
extern uint64 sys_capset(void);
extern uint64 sys_chmod(void);
extern uint64 sys_fchmod(void);
extern uint64 sys_fchmodat(void);
extern uint64 sys_chown(void);
extern uint64 sys_fchown(void);
extern uint64 sys_fchownat(void);
extern uint64 sys_chroot(void);
extern uint64 sys_reboot(void);


// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64 (*syscalls[])(void) = {
  // clang-format off
  [SYS_fork]    = sys_fork,
  [SYS_exit]    = sys_exit,
  [SYS_wait]    = sys_wait,
  [SYS_pipe]    = sys_pipe,
  [SYS_read]    = sys_read,
  [SYS_kill]    = sys_kill,
  [SYS_exec]    = sys_exec,
  [SYS_fstat]   = sys_fstat,
  [SYS_chdir]   = sys_chdir,
  [SYS_dup]     = sys_dup,
  [SYS_getpid]  = sys_getpid,
  [SYS_sbrk]    = sys_sbrk,
  [SYS_pause]   = sys_pause,
  [SYS_uptime]  = sys_uptime,
  [SYS_open]    = sys_open,
  [SYS_write]   = sys_write,
  [SYS_mknod]   = sys_mknod,
  [SYS_unlink]  = sys_unlink,
  [SYS_link]    = sys_link,
  [SYS_mkdir]   = sys_mkdir,
  [SYS_close]   = sys_close,
  [SYS_vm_create]  = sys_vm_create,
  [SYS_vm_ctl]     = sys_vm_ctl,
  [SYS_vm_stats]   = sys_vm_stats,
  [SYS_hwinfo]     = sys_hwinfo,
  [SYS_socket]     = sys_socket,
  [SYS_bind]       = sys_bind,
  [SYS_connect]    = sys_connect,
  [SYS_listen]     = sys_listen,
  [SYS_accept]     = sys_accept,
  [SYS_sendto]     = sys_sendto,
  [SYS_recvfrom]   = sys_recvfrom,
  [SYS_netinfo]    = sys_netinfo,
  [SYS_sockopt]    = sys_sockopt,
  [SYS_setpriority] = sys_setpriority,
  [SYS_getpriority] = sys_getpriority,
  [SYS_ptrace]     = sys_ptrace,
  [SYS_clock_gettime] = sys_clock_gettime,
  [SYS_sysconf]    = sys_sysconf,
  [SYS_mmap]       = sys_mmap,
  [SYS_munmap]     = sys_munmap,
  [SYS_mprotect]   = sys_mprotect,
  [SYS_sigaction]  = sys_sigaction,
  [SYS_sigprocmask] = sys_sigprocmask,
  [SYS_sigpending] = sys_sigpending,
  [SYS_sigsuspend] = sys_sigsuspend,
  [SYS_sigaltstack] = sys_sigaltstack,
  [SYS_sigreturn]  = sys_sigreturn,
  [SYS_sigqueue]   = sys_sigqueue,
  [SYS_alarm]      = sys_alarm,
  [SYS_wait4]      = sys_wait4,
  [SYS_setpgid]    = sys_setpgid,
  [SYS_getpgid]    = sys_getpgid,
  [SYS_setsid]     = sys_setsid,
  [SYS_getsid]     = sys_getsid,
  [SYS_getppid]    = sys_getppid,
  [SYS_lseek]       = sys_lseek,
  [SYS_pread]       = sys_pread,
  [SYS_pwrite]      = sys_pwrite,
  [SYS_readv]       = sys_readv,
  [SYS_writev]      = sys_writev,
  [SYS_dup2]        = sys_dup2,
  [SYS_dup3]        = sys_dup3,
  [SYS_fcntl]       = sys_fcntl,
  [SYS_pipe2]       = sys_pipe2,
  [SYS_ftruncate]   = sys_ftruncate,
  [SYS_ioctl]       = sys_ioctl,
  [SYS_poll]        = sys_poll,
  [SYS_select]      = sys_select,
  [SYS_execve]      = sys_execve,
  [SYS_nanosleep]   = sys_nanosleep,
  [SYS_times]       = sys_times,
  [SYS_getrusage]   = sys_getrusage,
  [SYS_uname]       = sys_uname,
  [SYS_sethostname] = sys_sethostname,
  [SYS_gethostname] = sys_gethostname,
  [SYS_umask]       = sys_umask,
  [SYS_getrandom]   = sys_getrandom,
  [SYS_vm_run]     = sys_vm_run,
  [SYS_vm_destroy] = sys_vm_destroy,
  [SYS_vm_cons]    = sys_vm_cons,
  [SYS_vm_info]    = sys_vm_info,
  [SYS_consraw]    = sys_consraw,
  [SYS_sync]    = sys_sync,
  [SYS_clone]   = sys_clone,
  [SYS_futex]   = sys_futex,
  [SYS_gettid]  = sys_gettid,
  [SYS_tkill]   = sys_tkill,
  [SYS_tgkill]  = sys_tgkill,
  [SYS_exit_group] = sys_exit_group,
  [SYS_set_tid_address] = sys_set_tid_address,
  [SYS_mount]   = sys_mount,
  [SYS_umount]  = sys_umount,
  [SYS_getdents] = sys_getdents,
  [SYS_statfs]  = sys_statfs,
  [SYS_fsync]   = sys_fsync,
  [SYS_symlink] = sys_symlink,
  [SYS_readlink] = sys_readlink,
  [SYS_rename]  = sys_rename,
  [SYS_fstatat] = sys_fstatat,
  [SYS_readlinkat] = sys_readlinkat,
  [SYS_unlinkat] = sys_unlinkat,
  [SYS_faccessat] = sys_faccessat,
  [SYS_openat]  = sys_openat,
  [SYS_mkdirat] = sys_mkdirat,
  [SYS_renameat] = sys_renameat,
  [SYS_linkat]  = sys_linkat,
  [SYS_symlinkat] = sys_symlinkat,
  [SYS_setxattr] = sys_setxattr,
  [SYS_getxattr] = sys_getxattr,
  [SYS_listxattr] = sys_listxattr,
  [SYS_removexattr] = sys_removexattr,
  [SYS_getuid] = sys_getuid,
  [SYS_geteuid] = sys_geteuid,
  [SYS_getgid] = sys_getgid,
  [SYS_getegid] = sys_getegid,
  [SYS_setuid] = sys_setuid,
  [SYS_setgid] = sys_setgid,
  [SYS_setresuid] = sys_setresuid,
  [SYS_setresgid] = sys_setresgid,
  [SYS_getresuid] = sys_getresuid,
  [SYS_getresgid] = sys_getresgid,
  [SYS_getgroups] = sys_getgroups,
  [SYS_setgroups] = sys_setgroups,
  [SYS_capget] = sys_capget,
  [SYS_capset] = sys_capset,
  [SYS_chmod] = sys_chmod,
  [SYS_fchmod] = sys_fchmod,
  [SYS_fchmodat] = sys_fchmodat,
  [SYS_chown] = sys_chown,
  [SYS_fchown] = sys_fchown,
  [SYS_fchownat] = sys_fchownat,
  [SYS_chroot]  = sys_chroot,
  [SYS_reboot]  = sys_reboot,

  // clang-format on
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  p->syscall_a0 = p->trapframe->a0; // kept in case a signal restarts the call
  p->restartable = 0;
  p->norestart = 0;
  // a watched process stops on the way in, so a tracer can see the
  // number and the arguments before anything happens
  if (p->traced)
    ptrace_syscall_entry(p);
  if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
    if ((long)p->trapframe->a0 == -EINTR && !p->norestart)
      p->restartable = 1;
  } else {
    printk("%d %s: unknown sys call %d\n", p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
  // and on the way out, where the answer is
  if (p->traced)
    ptrace_syscall_exit(p);
}
