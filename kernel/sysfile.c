//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "vfs.h"
#include "blk.h"
#include "fcntl.h"
#include "posix.h"
#include "signal.h"
#include "errno.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if ((f = fdget(myproc(), fd)) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// fdalloc() lives in proc.c: it grows the table on demand.

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  fdtake(myproc(), fd);
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  if ((ip = namei(old)) == 0) {
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    return -1;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  // the link count was raised and written above, so all that is left is
  // the name itself, which the file system adds. dp may have been
  // unlinked while the path was being resolved; a name in an orphaned
  // directory would lose ip's raised count with nothing to drop it.
  {
    int r;
    if (dp->ops == 0 || dp->ops->link == 0 || dp->nlink == 0 || dp->dev != ip->dev) {
      iunlockput(dp);
      goto bad;
    }
    r = dp->ops->link(dp, name, ip);
    iunlockput(dp);
    if (r < 0)
      goto bad;
    iput(ip);
    return 0;
  }

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  return -1;
}

uint64
sys_unlink(void)
{
  struct inode *dp;
  char name[DIRSIZ], path[MAXPATH];
  int r;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  if ((dp = nameiparent(path, name)) == 0)
    return -1;

  ilock(dp);
  // "." and ".." are not names a file system may be asked to remove:
  // one of them is the directory itself and the other is its parent.
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    r = -EPERM;
  else if (dp->ops == 0 || dp->ops->unlink == 0)
    r = -EPERM;
  else
    r = dp->ops->unlink(dp, name);
  iunlockput(dp);
  return r;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  // a new directory's ".." would push dp->nlink past its maximum
  if (type == T_DIR && dp->nlink >= NLINK_MAX) {
    iunlockput(dp);
    return 0;
  }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  // the file system makes the whole thing: the inode, the directory
  // entry, and the two entries a directory needs. it comes back locked.
  if (dp->ops == 0 || dp->ops->create == 0) {
    iunlockput(dp);
    return 0;
  }
  ip = dp->ops->create(dp, name, type, major, minor);
  iunlockput(dp);
  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;


  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }
  f->flags = omode & (O_APPEND | O_NONBLOCK);
  if ((omode & O_APPEND) && ip->type == T_FILE)
    f->off = ip->size;

  iunlock(ip);

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    return -1;
  }
  iunlockput(ip);
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    return -1;
  }
  iunlockput(ip);
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    return -1;
  }
  iunlock(ip);
  {
    struct inode *old = proc_set_cwd(p, ip);
    if (old)
      iput(old);
  }
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      fdtake(p, fd0);
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->mm, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->mm, fdarray + sizeof(fd0), (char *)&fd1,
              sizeof(fd1)) < 0) {
    fdtake(p, fd0);
    fdtake(p, fd1);
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// execve(path, argv, envp)
uint64
sys_execve(void)
{
  char path[MAXPATH], *argv[MAXARG], *envp[MAXENV];
  int i;
  uint64 uargv, uenvp, uarg;
  struct proc *p = myproc();

  argaddr(1, &uargv);
  argaddr(2, &uenvp);
  if (argstr(0, path, MAXPATH) < 0)
    return -ENOENT;
  memset(argv, 0, sizeof(argv));
  memset(envp, 0, sizeof(envp));
  int ret = -EFAULT;
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      ret = -E2BIG;
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0)
      goto bad;
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0) {
      ret = -ENOMEM;
      goto bad;
    }
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }
  for (i = 0; uenvp; i++) {
    if (i >= NELEM(envp) - 1) {
      ret = -E2BIG;
      goto bad;
    }
    if (fetchaddr(uenvp + sizeof(uint64) * i, (uint64 *)&uarg) < 0)
      goto bad;
    if (uarg == 0) {
      envp[i] = 0;
      break;
    }
    envp[i] = kalloc();
    if (envp[i] == 0) {
      ret = -ENOMEM;
      goto bad;
    }
    if (fetchstr(uarg, envp[i], PGSIZE) < 0)
      goto bad;
  }
  (void)p;
  ret = kexecve(path, argv, uenvp ? envp : 0);

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  for (i = 0; i < NELEM(envp) && envp[i] != 0; i++)
    kfree(envp[i]);
  return ret;
}

// ---- POSIX descriptor and I/O calls ----

// lseek(fd, offset, whence)
uint64
sys_lseek(void)
{
  int fd, whence;
  uint64 off;
  struct file *f;
  argint(0, &fd);
  argaddr(1, &off);
  argint(2, &whence);
  if ((f = fdget(myproc(), fd)) == 0)
    return -EBADF;
  if (f->type != FD_INODE)
    return -ESPIPE;
  int64 target;
  switch (whence) {
  case SEEK_SET:
    target = (int64)off;
    break;
  case SEEK_CUR:
    target = (int64)f->off + (int64)off;
    break;
  case SEEK_END: {
    ilock(f->ip);
    target = (int64)f->ip->size + (int64)off;
    iunlock(f->ip);
    break;
  }
  default:
    return -EINVAL;
  }
  if (target < 0)
    return -EINVAL;
  f->off = target;
  return target;
}

// pread/pwrite: like read and write, but at an explicit offset
uint64
sys_pread(void)
{
  int fd, n;
  uint64 addr, off, saved;
  struct file *f;
  argint(0, &fd);
  argaddr(1, &addr);
  argint(2, &n);
  argaddr(3, &off);
  if ((f = fdget(myproc(), fd)) == 0)
    return -EBADF;
  if (f->type != FD_INODE)
    return -ESPIPE;
  saved = f->off;
  f->off = off;
  int r = fileread(f, addr, n);
  f->off = saved;
  return r;
}

uint64
sys_pwrite(void)
{
  int fd, n;
  uint64 addr, off, saved;
  struct file *f;
  argint(0, &fd);
  argaddr(1, &addr);
  argint(2, &n);
  argaddr(3, &off);
  if ((f = fdget(myproc(), fd)) == 0)
    return -EBADF;
  if (f->type != FD_INODE)
    return -ESPIPE;
  saved = f->off;
  f->off = off;
  int r = filewrite(f, addr, n);
  f->off = saved;
  return r;
}

// readv/writev over an array of iovecs
static uint64
do_vector(int write)
{
  int fd, cnt;
  uint64 vaddr;
  struct iovec iov;
  struct file *f;
  argint(0, &fd);
  argaddr(1, &vaddr);
  argint(2, &cnt);
  struct proc *p = myproc();
  if ((f = fdget(p, fd)) == 0)
    return -EBADF;
  if (cnt < 0 || cnt > 64)
    return -EINVAL;
  int total = 0;
  for (int i = 0; i < cnt; i++) {
    if (copyin(p->mm, (char *)&iov, vaddr + i * sizeof(iov), sizeof(iov)) < 0)
      return -EFAULT;
    if (iov.iov_len == 0)
      continue;
    int r = write ? filewrite(f, (uint64)iov.iov_base, iov.iov_len)
                  : fileread(f, (uint64)iov.iov_base, iov.iov_len);
    if (r < 0)
      return total > 0 ? total : r;
    total += r;
    if (r < (int)iov.iov_len)
      break; // a short transfer ends the call
  }
  return total;
}

uint64
sys_readv(void)
{
  return do_vector(0);
}

uint64
sys_writev(void)
{
  return do_vector(1);
}

// dup2/dup3: make newfd a copy of oldfd
uint64
sys_dup3(void)
{
  int oldfd, newfd, flags;
  argint(0, &oldfd);
  argint(1, &newfd);
  argint(2, &flags);
  struct proc *p = myproc();
  struct file *f = fdget(p, oldfd);
  if (f == 0)
    return -EBADF;
  if (newfd < 0 || newfd >= NOFILE)
    return -EBADF;
  if (oldfd == newfd)
    return -EINVAL; // dup3 always refuses this, unlike dup2
  if (fdtable_grow_to(p, newfd + 1) < 0)
    return -EMFILE;
  struct file *old = fdput(p, newfd, filedup(f), (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
  if (old)
    fileclose(old);
  return newfd;
}

uint64
sys_dup2(void)
{
  int oldfd, newfd;
  argint(0, &oldfd);
  argint(1, &newfd);
  struct proc *p = myproc();
  if (fdget(p, oldfd) == 0)
    return -EBADF;
  if (oldfd == newfd)
    return newfd;
  struct file *f = fdget(p, oldfd);
  if (newfd < 0 || newfd >= NOFILE)
    return -EBADF;
  if (fdtable_grow_to(p, newfd + 1) < 0)
    return -EMFILE;
  struct file *old = fdput(p, newfd, filedup(f), 0);
  if (old)
    fileclose(old);
  return newfd;
}

// fcntl(fd, cmd, arg)
uint64
sys_fcntl(void)
{
  int fd, cmd, arg;
  argint(0, &fd);
  argint(1, &cmd);
  argint(2, &arg);
  struct proc *p = myproc();
  struct file *f = fdget(p, fd);
  if (f == 0)
    return -EBADF;
  switch (cmd) {
  case F_DUPFD:
  case F_DUPFD_CLOEXEC: {
    int nfd = fdalloc_from(filedup(f), arg);
    if (nfd < 0) {
      fileclose(f);
      return nfd;
    }
    if (cmd == F_DUPFD_CLOEXEC)
      fdflags_set(p, nfd, FD_CLOEXEC);
    return nfd;
  }
  case F_GETFD:
    return fdflags_get(p, fd);
  case F_SETFD:
    return fdflags_set(p, fd, arg);
  case F_GETFL:
    return f->flags;
  case F_SETFL:
    // only the status flags may change
    f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) | (arg & (O_NONBLOCK | O_APPEND));
    return 0;
  }
  return -EINVAL;
}

// pipe2(fds, flags)
uint64
sys_pipe2(void)
{
  uint64 fdarray;
  int flags;
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  argint(1, &flags);
  if (pipealloc(&rf, &wf) < 0)
    return -ENFILE;
  rf->flags = (flags & O_NONBLOCK) | O_RDONLY;
  wf->flags = (flags & O_NONBLOCK) | O_WRONLY;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      fdtake(p, fd0);
    fileclose(rf);
    fileclose(wf);
    return -EMFILE;
  }
  if (flags & O_CLOEXEC) {
    fdflags_set(p, fd0, FD_CLOEXEC);
    fdflags_set(p, fd1, FD_CLOEXEC);
  }
  if (copyout(p->mm, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->mm, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0) {
    fdtake(p, fd0);
    fdtake(p, fd1);
    fileclose(rf);
    fileclose(wf);
    return -EFAULT;
  }
  return 0;
}

// ftruncate(fd, length)
uint64
sys_ftruncate(void)
{
  int fd;
  uint64 len;
  argint(0, &fd);
  argaddr(1, &len);
  struct file *f = fdget(myproc(), fd);
  if (f == 0)
    return -EBADF;
  if (f->type != FD_INODE)
    return -EINVAL;
  if (!f->writable)
    return -EBADF;
  if (len != 0)
    return -EINVAL; // only truncation to zero, until the new file system
  ilock(f->ip);
  itrunc(f->ip);
  iunlock(f->ip);
  return 0;
}

// ioctl(fd, request, arg)
uint64
sys_ioctl(void)
{
  int fd;
  uint64 req, arg;
  argint(0, &fd);
  argaddr(1, &req);
  argaddr(2, &arg);
  struct proc *p = myproc();
  struct file *f = fdget(p, fd);
  if (f == 0)
    return -EBADF;
  switch (req) {
  case FIONREAD: {
    int n = fileready(f, 0);
    if (n < 0)
      n = 0;
    if (copyout(p->mm, arg, (char *)&n, sizeof(n)) < 0)
      return -EFAULT;
    return 0;
  }
  }
  if (f->type == FD_DEVICE && f->major >= 0 && f->major < NDEV && devsw[f->major].ioctl)
    return devsw[f->major].ioctl(req, arg);
  return -ENOTTY;
}

// ---- poll and select ----

// one pass over the descriptors: fills in revents and returns how many
// are ready.
static int
poll_scan(struct proc *p, struct pollfd *fds, int n)
{
  int ready = 0;
  for (int i = 0; i < n; i++) {
    fds[i].revents = 0;
    if (fds[i].fd < 0)
      continue;
    struct file *f = fdget(p, fds[i].fd);
    if (f == 0) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }
    if (fds[i].events & POLLIN) {
      int r = fileready(f, 0);
      if (r > 0)
        fds[i].revents |= POLLIN;
      else if (r < 0)
        fds[i].revents |= POLLHUP;
    }
    if (fds[i].events & POLLOUT) {
      int r = fileready(f, 1);
      if (r > 0)
        fds[i].revents |= POLLOUT;
      else if (r < 0)
        fds[i].revents |= POLLERR;
    }
    if (fds[i].revents)
      ready++;
  }
  return ready;
}

// poll(fds, nfds, timeout_ms): -1 waits forever, 0 returns at once.
uint64
sys_poll(void)
{
  uint64 addr;
  int n, timeout;
  argaddr(0, &addr);
  argint(1, &n);
  argint(2, &timeout);
  struct proc *p = myproc();
  if (n < 0 || n > 64)
    return -EINVAL;
  struct pollfd fds[64];
  if (n > 0 && copyin(p->mm, (char *)fds, addr, n * sizeof(struct pollfd)) < 0)
    return -EFAULT;

  uint64 deadline = 0;
  if (timeout > 0) {
    acquire(&tickslock);
    deadline = ticks + ((uint64)timeout * TICK_HZ + 999) / 1000;
    release(&tickslock);
  }
  for (;;) {
    int ready = poll_scan(p, fds, n);
    if (ready > 0 || timeout == 0) {
      if (n > 0 && copyout(p->mm, addr, (char *)fds, n * sizeof(struct pollfd)) < 0)
        return -EFAULT;
      return ready;
    }
    if (killed(p) || signal_pending(p))
      return -EINTR;
    acquire(&tickslock);
    if (timeout > 0 && ticks >= deadline) {
      release(&tickslock);
      if (n > 0 && copyout(p->mm, addr, (char *)fds, n * sizeof(struct pollfd)) < 0)
        return -EFAULT;
      return 0;
    }
    // no readiness notification yet: wake on the next tick and look again
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep_intr();
  }
}

// select(nfds, readfds, writefds, exceptfds, timeout)
uint64
sys_select(void)
{
  int nfds;
  uint64 raddr, waddr, eaddr, taddr;
  argint(0, &nfds);
  argaddr(1, &raddr);
  argaddr(2, &waddr);
  argaddr(3, &eaddr);
  argaddr(4, &taddr);
  struct proc *p = myproc();
  if (nfds < 0 || nfds > FD_SETSIZE)
    return -EINVAL;
  fd_set rset, wset, eset, rout, wout;
  memset(&rset, 0, sizeof(rset));
  memset(&wset, 0, sizeof(wset));
  memset(&eset, 0, sizeof(eset));
  if (raddr && copyin(p->mm, (char *)&rset, raddr, sizeof(rset)) < 0)
    return -EFAULT;
  if (waddr && copyin(p->mm, (char *)&wset, waddr, sizeof(wset)) < 0)
    return -EFAULT;
  if (eaddr && copyin(p->mm, (char *)&eset, eaddr, sizeof(eset)) < 0)
    return -EFAULT;
  int timeout = -1;
  if (taddr) {
    struct timeval tv;
    if (copyin(p->mm, (char *)&tv, taddr, sizeof(tv)) < 0)
      return -EFAULT;
    timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000;
  }
  uint64 deadline = 0;
  if (timeout > 0) {
    acquire(&tickslock);
    deadline = ticks + ((uint64)timeout * TICK_HZ + 999) / 1000;
    release(&tickslock);
  }
  for (;;) {
    int ready = 0;
    memset(&rout, 0, sizeof(rout));
    memset(&wout, 0, sizeof(wout));
    for (int fd = 0; fd < nfds; fd++) {
      struct file *f = 0;
      if (FD_ISSET(fd, &rset) || FD_ISSET(fd, &wset)) {
        f = fdget(p, fd);
        if (f == 0)
          return -EBADF;
      }
      if (FD_ISSET(fd, &rset) && fileready(f, 0) != 0) {
        FD_SET(fd, &rout);
        ready++;
      }
      if (FD_ISSET(fd, &wset) && fileready(f, 1) != 0) {
        FD_SET(fd, &wout);
        ready++;
      }
    }
    if (ready > 0 || timeout == 0) {
      if (raddr && copyout(p->mm, raddr, (char *)&rout, sizeof(rout)) < 0)
        return -EFAULT;
      if (waddr && copyout(p->mm, waddr, (char *)&wout, sizeof(wout)) < 0)
        return -EFAULT;
      if (eaddr) {
        memset(&eset, 0, sizeof(eset));
        if (copyout(p->mm, eaddr, (char *)&eset, sizeof(eset)) < 0)
          return -EFAULT;
      }
      return ready;
    }
    if (killed(p) || signal_pending(p))
      return -EINTR;
    acquire(&tickslock);
    if (timeout > 0 && ticks >= deadline) {
      release(&tickslock);
      memset(&rout, 0, sizeof(rout));
      if (raddr && copyout(p->mm, raddr, (char *)&rout, sizeof(rout)) < 0)
        return -EFAULT;
      if (waddr && copyout(p->mm, waddr, (char *)&rout, sizeof(rout)) < 0)
        return -EFAULT;
      return 0;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep_intr();
  }
}

// ---- mounting ----

// mount(source, target, type, flags, options)
uint64
sys_mount(void)
{
  char source[MAXPATH], target[MAXPATH], type[32], opts[64];
  int flags;

  if (argstr(0, source, sizeof(source)) < 0 || argstr(1, target, sizeof(target)) < 0 ||
      argstr(2, type, sizeof(type)) < 0)
    return -EFAULT;
  argint(3, &flags);
  uint64 optaddr;
  argaddr(4, &optaddr);
  opts[0] = 0;
  if (optaddr && copyinstr(myproc()->mm, opts, optaddr, sizeof(opts)) < 0)
    opts[0] = 0;

  struct super *sb = vfs_mount(source, target, type, (flags & MS_RDONLY) != 0, opts);
  return sb ? 0 : -EINVAL;
}

uint64
sys_umount(void)
{
  char target[MAXPATH];

  if (argstr(0, target, sizeof(target)) < 0)
    return -EFAULT;
  return vfs_umount(target);
}

// getdents(fd, buf, len): directory entries in the POSIX shape, so
// that readdir does not have to know the on-disk format.
uint64
sys_getdents(void)
{
  struct file *f;
  uint64 addr;
  int fd, len;

  if (argfd(0, &fd, &f) < 0)
    return -EBADF;
  argaddr(1, &addr);
  argint(2, &len);
  if (f->type != FD_INODE)
    return -ENOTDIR;

  struct proc *p = myproc();
  int written = 0;
  ilock(f->ip);
  if (f->ip->type != T_DIR) {
    iunlock(f->ip);
    return -ENOTDIR;
  }
  for (;;) {
    char name[256];
    uint ino;
    uchar type;
    uint64 pos = f->off;
    int r = idirread(f->ip, &pos, name, sizeof(name), &ino, &type);
    if (r <= 0) {
      if (r < 0 && written == 0) {
        iunlock(f->ip);
        return r;
      }
      break;
    }
    int namelen = strlen(name);
    int reclen = (int)(sizeof(struct dirent_user) + namelen + 1 + 7) & ~7;
    if (written + reclen > len) {
      if (written == 0) {
        iunlock(f->ip);
        return -EINVAL;
      }
      break;
    }
    struct dirent_user de;
    memset(&de, 0, sizeof(de));
    de.d_ino = ino;
    de.d_off = pos;
    de.d_reclen = reclen;
    de.d_type = type;
    if (either_copyout(1, addr + written, &de, sizeof(de)) < 0 ||
        either_copyout(1, addr + written + sizeof(de), name, namelen + 1) < 0) {
      iunlock(f->ip);
      return -EFAULT;
    }
    (void)p;
    written += reclen;
    f->off = pos;
  }
  iunlock(f->ip);
  return written;
}

// statfs(path, buf)
uint64
sys_statfs(void)
{
  char path[MAXPATH];
  uint64 addr;

  if (argstr(0, path, sizeof(path)) < 0)
    return -EFAULT;
  argaddr(1, &addr);

  struct inode *ip = namei(path);
  if (ip == 0) {
    return -ENOENT;
  }
  struct super *sb = ip->sb;
  iput(ip);

  struct statfs st;
  memset(&st, 0, sizeof(st));
  st.f_bsize = BLKSIZE;
  st.f_namelen = NAME_MAX;
  if (sb && sb->ops && sb->ops->statfs) {
    uint64 blocks, bfree, files, ffree, bsize, namelen;
    sb->ops->statfs(sb, &blocks, &bfree, &files, &ffree, &bsize, &namelen);
    st.f_blocks = blocks;
    st.f_bfree = st.f_bavail = bfree;
    st.f_files = files;
    st.f_ffree = ffree;
    st.f_bsize = bsize;
    st.f_namelen = namelen;
  }
  if (either_copyout(1, addr, &st, sizeof(st)) < 0)
    return -EFAULT;
  return 0;
}

// fsync(fd): everything this file holds is on the disk when this
// returns. a file system that knows how to do that for one file does
// it; anything else falls back to writing everything, which is what
// this kernel did for every fsync until there was a journal to order.
uint64
sys_fsync(void)
{
  struct file *f;
  int r;

  // argfd does not take a reference, and this must not drop one: the
  // descriptor table is the process's own and it is this process that
  // is in the call.
  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  if (f->type != FD_INODE || f->ip == 0)
    return 0; // a pipe or a socket has nothing on a disk

  ilock(f->ip);
  if (f->ip->ops && f->ip->ops->fsync)
    r = f->ip->ops->fsync(f->ip, 0);
  else
    r = vfs_sync_all();
  iunlock(f->ip);
  return r;
}


// sync(): everything every mounted file system is holding goes to its
// disk. each file system commits what it has to commit; the block cache
// writes back what is left.
uint64
sys_sync(void)
{
  return vfs_sync_all();
}
