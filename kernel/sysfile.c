//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "nsproxy.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "cred.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "vfs.h"
#include "blk.h"
#include "blkdev.h"
#include "fcntl.h"
#include "xattr.h"
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

// the directory an *at call resolves a relative path from; defined with
// the rest of that family at the end of this file.
static struct inode *at_lookup(int dirfd, char *path, int parent, int nofollow, char *name);

// Create the path new as a link to the same inode as old.
static int
do_link(int oldfd, char *old, int newfd, char *new, int flags)
{
  char name[DIRSIZ];
  struct inode *dp, *ip;

  // a hard link names an inode, so a symbolic link at the end of the
  // old path is followed only when AT_SYMLINK_FOLLOW says to.
  if ((ip = at_lookup(oldfd, old, 0, !(flags & AT_SYMLINK_FOLLOW), name)) == 0) {
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    return -1;
  }
  // an immutable or append-only file may not have another name made
  // for it: a second name is a second way to reach it
  if (ip->attr & (ATTR_IMMUTABLE | ATTR_APPEND)) {
    iunlockput(ip);
    return -EPERM;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = at_lookup(newfd, new, 1, 0, name)) == 0)
    goto bad;
  if (vfs_readonly(dp)) {
    iput(dp);
    goto bad;
  }
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

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char new[MAXPATH], old[MAXPATH];

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;
  return do_link(AT_FDCWD, old, AT_FDCWD, new, AT_SYMLINK_FOLLOW);
}

// linkat(oldfd, old, newfd, new, flags)
uint64
sys_linkat(void)
{
  char new[MAXPATH], old[MAXPATH];
  int oldfd, newfd, flags;

  argint(0, &oldfd);
  argint(2, &newfd);
  argint(4, &flags);
  if (argstr(1, old, MAXPATH) < 0 || argstr(3, new, MAXPATH) < 0)
    return -EFAULT;
  return do_link(oldfd, old, newfd, new, flags);
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

  if (vfs_readonly(dp)) {
    iput(dp);
    return -EROFS;
  }
  ilock(dp);
  // "." and ".." are not names a file system may be asked to remove:
  // one of them is the directory itself and the other is its parent.
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    r = -EPERM;
  else if (dp->ops == 0 || dp->ops->unlink == 0)
    r = -EPERM;
  else if ((r = inode_permitted(dp, 2 | 1)) == 0) {
    struct inode *victim = dirlookup(dp, name, 0);
    if (victim == 0)
      r = -ENOENT;
    else {
      ilock(victim);
      r = sticky_permitted(dp, victim);
      // neither an immutable nor an append-only file may be taken
      // away: a file that may only be added to may not be removed
      if (r == 0 && (victim->attr & (ATTR_IMMUTABLE | ATTR_APPEND)))
        r = -EPERM;
      iunlockput(victim);
      if (r == 0)
        r = dp->ops->unlink(dp, name);
    }
  }
  iunlockput(dp);
  return r;
}

static struct inode *
create_at(int dirfd, char *path, short type, short major, short minor, int perm)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = at_lookup(dirfd, path, 1, 0, name)) == 0)
    return 0;

  ilock(dp);

  // the name may already be there, in which case nothing is being
  // created and the directory is not being changed: opening an
  // existing file with O_CREATE must not need permission to write to
  // the directory it is in, or no ordinary program could open
  // /dev/null for writing.
  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if (vfs_readonly(dp)) {
    iunlockput(dp);
    return 0;
  }

  // a new name in a directory is a change to that directory
  if (inode_permitted(dp, 2 | 1) < 0) {
    iunlockput(dp);
    return 0;
  }

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  // a new directory's ".." would push dp->nlink past its maximum
  if (type == T_DIR && dp->nlink >= NLINK_MAX) {
    iunlockput(dp);
    return 0;
  }

  // the file system makes the whole thing: the inode, the directory
  // entry, and the two entries a directory needs. it comes back locked.
  if (dp->ops == 0 || dp->ops->create == 0) {
    iunlockput(dp);
    return 0;
  }
  ip = dp->ops->create(dp, name, type, major, minor);
  if (ip) {
    // it belongs to whoever made it, with the permissions the process
    // asked for less the ones its umask takes away. a new directory in
    // a set-group-id directory belongs to that directory's group and
    // is itself set-group-id, which is how a shared directory stays
    // shared.
    struct proc *p = myproc();
    ip->mode = (uint)perm & 07777 & ~(uint)p->umask;
    ip->uid = p->cred.euid;
    ip->gid = (dp->mode & 02000) ? dp->gid : p->cred.egid;
    if (type == T_DIR && (dp->mode & 02000))
      ip->mode |= 02000;
    iupdate(ip);
  }
  iunlockput(dp);
  return ip;
}

static struct inode *
create(char *path, short type, short major, short minor, int perm)
{
  return create_at(AT_FDCWD, path, type, major, minor, perm);
}

// open, whatever names the directory a relative path starts from.
static int
do_open(int dirfd, char *path, int omode, int perm)
{
  int fd;
  struct file *f;
  struct inode *ip;
  char name[DIRSIZ];

  if (omode & O_CREATE) {
    ip = create_at(dirfd, path, T_FILE, 0, 0, perm);
    if (ip == 0) {
      return -1;
    }
  } else {
    if ((ip = at_lookup(dirfd, path, 0, (omode & O_NOFOLLOW) != 0, name)) == 0) {
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && (omode & (O_WRONLY | O_RDWR | O_TRUNC | O_CREATE))) {
      iunlockput(ip);
      return -EISDIR;
    }
    // O_NOFOLLOW is about the last element only, and a link found
    // there is an error rather than something to open.
    if ((omode & O_NOFOLLOW) && ip->type == T_SYMLINK && !(omode & O_PATH)) {
      iunlockput(ip);
      return -ELOOP;
    }
    if ((omode & O_DIRECTORY) && ip->type != T_DIR) {
      iunlockput(ip);
      return -ENOTDIR;
    }
  }

  // what the mount was made with. nodev means a device node on it is
  // a file with no device behind it, which is what stops an image
  // somebody else wrote from carrying a way into the kernel.
  if (ip->sb && (ip->sb->flags & MS_NODEV) && ip->type == T_DEVICE) {
    iunlockput(ip);
    return -EACCES;
  }
  if (ip->sb && ip->sb->readonly && (omode & (O_WRONLY | O_RDWR | O_TRUNC))) {
    iunlockput(ip);
    return -EROFS;
  }

  // an immutable file cannot be opened for writing at all; an
  // append-only one can, but only to append -- so O_APPEND is
  // required and O_TRUNC refused. root is bound by this too: that is
  // the point of the flag, and only clearing it lifts it.
  if (omode & (O_WRONLY | O_RDWR | O_TRUNC)) {
    if (ip->attr & ATTR_IMMUTABLE) {
      iunlockput(ip);
      return -EPERM;
    }
    if ((ip->attr & ATTR_APPEND) &&
        ((omode & O_TRUNC) || !(omode & O_APPEND))) {
      iunlockput(ip);
      return -EPERM;
    }
  }

  // what the file itself allows. a file created by this call has just
  // been given the caller's own permissions, so the check is the same
  // one either way.
  {
    int want = 0;
    if (!(omode & O_WRONLY))
      want |= 4;
    if (omode & (O_WRONLY | O_RDWR))
      want |= 2;
    if ((omode & O_TRUNC) && ip->type == T_FILE)
      want |= 2;
    int e = want ? inode_permitted(ip, want) : 0;
    if (e < 0) {
      iunlockput(ip);
      return e;
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

  if (ip->type == T_DEVICE && ip->major == DEV_BLOCK) {
    // a disk opened as a file: reads and writes go through the block
    // cache at the file's own offset, so lseek works on it and a
    // mount of the same device sees what was written.
    f->disk = blk_by_index(ip->minor);
    if (f->disk == 0) {
      fdput(myproc(), fd, 0, 1);
      fileclose(f);
      iunlockput(ip);
      return -ENXIO;
    }
    f->type = FD_BLOCK;
    f->major = ip->major;
    f->off = 0;
  } else if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
    if (ip->major >= 0 && ip->major < NDEV && devsw[ip->major].open) {
      int r = devsw[ip->major].open();
      if (r < 0) {
        f->type = FD_NONE;
        fileclose(f);
        iunlockput(ip);
        return r;
      }
    }
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
sys_open(void)
{
  char path[MAXPATH];
  int omode, perm = 0666;

  argint(1, &omode);
  // the third argument is the mode a file created here is given; it is
  // only there when O_CREATE is, which is what POSIX says of it
  if (omode & O_CREATE)
    argint(2, &perm);
  if (argstr(0, path, MAXPATH) < 0)
    return -1;
  return do_open(AT_FDCWD, path, omode, perm);
}

// openat(dirfd, path, flags)
uint64
sys_openat(void)
{
  char path[MAXPATH];
  int dirfd, omode, perm = 0666;

  argint(0, &dirfd);
  argint(2, &omode);
  if (omode & O_CREATE)
    argint(3, &perm);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  return do_open(dirfd, path, omode, perm);
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  int perm;

  argint(1, &perm);
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0, perm)) == 0) {
    return -1;
  }
  iunlockput(ip);
  return 0;
}

// mkdirat(dirfd, path, mode)
uint64
sys_mkdirat(void)
{
  char path[MAXPATH];
  struct inode *ip;
  int dirfd, mode;

  argint(0, &dirfd);
  argint(2, &mode);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if ((ip = create_at(dirfd, path, T_DIR, 0, 0, mode)) == 0)
    return -EEXIST;
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
      (ip = create(path, T_DEVICE, major, minor, 0666)) == 0) {
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
  if (f->type != FD_INODE && f->type != FD_BLOCK)
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
    if (f->type == FD_BLOCK) {
      target = (int64)blkdev_size(f->disk) + (int64)off;
      break;
    }
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
  if (f->type != FD_INODE && f->type != FD_BLOCK)
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
  if (f->type != FD_INODE && f->type != FD_BLOCK)
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
  if ((int64)len < 0)
    return -EINVAL;
  ilock(f->ip);
  int r = isetsize(f->ip, len);
  iunlock(f->ip);
  return r;
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

  // the flags chattr(1) and lsattr(1) read and write
  case FS_IOC_GETFLAGS: {
    if (f->type != FD_INODE && f->type != FD_DEVICE)
      return -ENOTTY;
    ilock(f->ip);
    uint v = f->ip->attr;
    iunlock(f->ip);
    if (copyout(p->mm, arg, (char *)&v, sizeof(v)) < 0)
      return -EFAULT;
    return 0;
  }
  case FS_IOC_SETFLAGS: {
    uint v;

    if (f->type != FD_INODE && f->type != FD_DEVICE)
      return -ENOTTY;
    if (copyin(p->mm, (char *)&v, arg, sizeof(v)) < 0)
      return -EFAULT;
    ilock(f->ip);
    // only the owner may change them, and only someone with
    // CAP_LINUX_IMMUTABLE may set or clear the two that stop a file
    // being changed -- otherwise a file could make itself immutable
    // and then let itself go again
    if (f->ip->uid != p->cred.euid && !cap_ok(CAP_FOWNER)) {
      iunlock(f->ip);
      return -EPERM;
    }
    uint locked = ATTR_IMMUTABLE | ATTR_APPEND;
    if (((f->ip->attr ^ v) & locked) && !cap_ok(CAP_LINUX_IMMUTABLE)) {
      iunlock(f->ip);
      return -EPERM;
    }
    f->ip->attr = v;
    iupdate(f->ip);
    iunlock(f->ip);
    return 0;
  }
  }
  if (f->type == FD_BLOCK)
    return blkdev_ioctl(f->disk, req, arg);
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
    deadline = ticks + ((uint64)timeout * tick_hz + 999) / 1000;
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
    deadline = ticks + ((uint64)timeout * tick_hz + 999) / 1000;
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
  char source[MAXPATH], target[MAXPATH], type[32], opts[256];
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

  if (!cap_ok(CAP_SYS_ADMIN) ||
      (!userns_in_root() && mntns_current() == mntns_root())) {
    // privileged, and -- in a user namespace -- mounting into a mount
    // namespace of its own rather than into the machine's
    audit("mount", target, (uint64)flags, 0, 0);
    return -EPERM;
  }
  if (!userns_in_root()) {
    // and in a user namespace of its own -- a container -- only the file
    // systems that are made out of nothing, plus a second name for a
    // directory it can already reach. A block device is the machine's
    // to mount: root inside a container is not root over the disks, and
    // an ext4 image it could mount is an ext4 image it could corrupt the
    // kernel with. What it does mount carries nodev and nosuid, so a
    // device node or a set-user-id program it puts there is worth
    // nothing.
    static const char *const allowed[] = {"tmpfs", "ramfs", "procfs",
                                          "devfs", "none"};
    int ok = (flags & MS_BIND) != 0;
    for (int i = 0; !ok && i < (int)(sizeof(allowed) / sizeof(allowed[0])); i++)
      if (strncmp(type, allowed[i], sizeof(type)) == 0)
        ok = 1;
    if (!ok) {
      audit("mount", target, (uint64)flags, 0, 0);
      return -EPERM;
    }
    // nodev and nosuid on what it makes for itself: a device node or a
    // set-user-id program a container puts in its own tmpfs is worth
    // nothing. (devfs and procfs are one instance each, and the flags of
    // a shared super are the machine's mount's -- so they are not set
    // here, where they would be a promise this kernel cannot keep.)
    if (strncmp(type, "tmpfs", sizeof(type)) == 0 ||
        strncmp(type, "ramfs", sizeof(type)) == 0 || (flags & MS_BIND))
      flags |= MS_NODEV | MS_NOSUID;
  }
  struct super *sb = vfs_mount(source, target, type, flags, opts);
  audit("mount", target, (uint64)flags, 0, sb != 0);
  return sb ? 0 : -EINVAL;
}

// pivot_root(new_root, put_old)
uint64
sys_pivot_root(void)
{
  char newroot[MAXPATH], putold[MAXPATH];
  struct proc *p = myproc();

  if (argstr(0, newroot, sizeof(newroot)) < 0 ||
      argstr(1, putold, sizeof(putold)) < 0)
    return -EFAULT;
  if (!cap_ok(CAP_SYS_ADMIN))
    return -EPERM;
  struct mntns *mns = mntns_current();
  if (mns == mntns_root())
    return -EINVAL;      // the machine's own root is not pivoted away

  // new_root has to be a mount point: what is pivoted into is a file
  // system, not a directory of the one being left.
  struct inode *ip = namei(newroot);
  if (ip == 0)
    return -ENOENT;
  struct super *sb = 0;
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *c = vfs_slot(i);
    if (c && c->root && c->root->dev == ip->dev && c->root->inum == ip->inum) {
      sb = c;
      break;
    }
  }
  if (sb == 0) {
    iput(ip);
    return -EINVAL;
  }
  int slot = vfs_slot_of(sb);
  // what goes is everything outside the new root; what is mounted inside
  // it stays, which is what makes a volume bound in before the pivot
  // still be there after it.
  int rlen = strlen(newroot);
  while (rlen > 1 && newroot[rlen - 1] == '/')
    rlen--;
  unsigned int keep = 1u << slot;
  for (int i = 0; i < MAXMOUNT; i++) {
    struct super *c = vfs_slot(i);
    if (c == 0 || i == slot || !mntns_sees(mns, i))
      continue;
    if (strncmp(c->path, newroot, rlen) == 0 && c->path[rlen] == '/')
      keep |= 1u << i;
  }
  mntns_set_root(mns, sb);
  mntns_keep(mns, keep);
  // and this process is in it: its root and its working directory are
  // the new root, so a path cannot walk out of what it has pivoted into
  proc_set_root(p, ip);
  proc_set_cwd(p, idup(ip));
  audit("pivot_root", newroot, 0, 0, 1);
  return 0;
}

uint64
sys_umount(void)
{
  char target[MAXPATH];

  if (argstr(0, target, sizeof(target)) < 0)
    return -EFAULT;
  if (!cap_ok(CAP_SYS_ADMIN) ||
      (!userns_in_root() && mntns_current() == mntns_root())) {
    audit("umount", target, 0, 0, 0);
    return -EPERM;
  }
  int r = vfs_umount(target);
  audit("umount", target, 0, 0, r == 0);
  return r;
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

// symlink(target, path): a file whose contents are the path it names.
// the target is not looked at: a link may name something that is not
// there, and often does.
static int
do_symlink(char *target, int dirfd, char *path)
{
  char name[DIRSIZ];
  struct inode *dp;
  int r;

  if ((dp = at_lookup(dirfd, path, 1, 0, name)) == 0)
    return -ENOENT;
  if (vfs_readonly(dp)) {
    iput(dp);
    return -EROFS;
  }
  ilock(dp);
  if (dp->ops == 0 || dp->ops->symlink == 0)
    r = -EPERM;
  else
    r = dp->ops->symlink(dp, name, target);
  iunlockput(dp);
  return r;
}

uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];

  if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  return do_symlink(target, AT_FDCWD, path);
}

// symlinkat(target, dirfd, path)
uint64
sys_symlinkat(void)
{
  char target[MAXPATH], path[MAXPATH];
  int dirfd;

  argint(1, &dirfd);
  if (argstr(0, target, MAXPATH) < 0 || argstr(2, path, MAXPATH) < 0)
    return -EFAULT;
  return do_symlink(target, dirfd, path);
}

// readlink(path, buf, size): where a symbolic link points. the last
// element of the path is not followed, or this could never see one.
uint64
sys_readlink(void)
{
  char path[MAXPATH], buf[MAXPATH];
  uint64 addr;
  int size, n;
  struct inode *ip;

  if (argstr(0, path, MAXPATH) < 0)
    return -EFAULT;
  argaddr(1, &addr);
  argint(2, &size);
  if (size <= 0)
    return -EINVAL;
  if ((ip = namei_nofollow(path)) == 0)
    return -ENOENT;
  ilock(ip);
  if (ip->type != T_SYMLINK) {
    iunlockput(ip);
    return -EINVAL;
  }
  n = ip->ops && ip->ops->readlink ? ip->ops->readlink(ip, buf, sizeof(buf)) : -EINVAL;
  iunlockput(ip);
  if (n < 0)
    return n;
  if (n > size)
    n = size;
  if (either_copyout(1, addr, buf, n) < 0)
    return -EFAULT;
  return n;
}

// rename(from, to): one name for a file becomes another. this is the
// form that a file system can do in one transaction; doing it with a
// link and an unlink, which is what the library did without it, cannot
// rename a directory and is not atomic.
static int
do_rename(int fromfd, char *from, int tofd, char *to)
{
  char fname[DIRSIZ], tname[DIRSIZ];
  struct inode *fdp = 0, *tdp = 0, *ip = 0, *victim = 0;
  int r = 0;

  if (fromfd == tofd && strncmp(from, to, MAXPATH) == 0)
    return 0;

  if ((fdp = at_lookup(fromfd, from, 1, 0, fname)) == 0)
    return -ENOENT;
  if ((tdp = at_lookup(tofd, to, 1, 0, tname)) == 0) {
    iput(fdp);
    return -ENOENT;
  }
  if (vfs_readonly(fdp) || vfs_readonly(tdp)) {
    iput(fdp);
    iput(tdp);
    return -EROFS;
  }
  if (fdp->dev != tdp->dev) {
    iput(fdp);
    iput(tdp);
    return -EXDEV; // a rename does not move a file between file systems
  }
  if (namecmp(fname, ".") == 0 || namecmp(fname, "..") == 0 || namecmp(tname, ".") == 0 ||
      namecmp(tname, "..") == 0) {
    iput(fdp);
    iput(tdp);
    return -EINVAL;
  }

  // the two directories are locked in order of inode number, so that
  // two renames crossing each other cannot wait for one another.
  struct inode *first = fdp->inum <= tdp->inum ? fdp : tdp;
  struct inode *second = fdp->inum <= tdp->inum ? tdp : fdp;
  ilock(first);
  if (second != first)
    ilock(second);

  ip = dirlookup(fdp, fname, 0);
  if (ip == 0) {
    r = -ENOENT;
    goto out;
  }
  // a file that may not be changed may not be moved either, and the
  // same goes for one it would replace
  {
    ilock(ip);
    int locked = (ip->attr & (ATTR_IMMUTABLE | ATTR_APPEND)) != 0;
    iunlock(ip);
    if (locked) {
      r = -EPERM;
      goto out;
    }
  }
  victim = dirlookup(tdp, tname, 0);
  if (victim) {
    // the name is taken. it is replaced, unless what is there is a
    // directory with something in it.
    ilock(victim);
    int vdir = victim->type == T_DIR;
    int vlocked = (victim->attr & (ATTR_IMMUTABLE | ATTR_APPEND)) != 0;
    iunlock(victim);
    if (vlocked) {
      r = -EPERM;
      goto out;
    }
    if (vdir) {
      struct inode *ipv = ip;
      ilock(ipv);
      int sdir = ipv->type == T_DIR;
      iunlock(ipv);
      if (!sdir) {
        r = -EISDIR;
        goto out;
      }
    }
    if (tdp->ops == 0 || tdp->ops->unlink == 0) {
      r = -EPERM;
      goto out;
    }
    r = tdp->ops->unlink(tdp, tname);
    if (r < 0)
      goto out;
  }

  if (tdp->ops == 0 || tdp->ops->link == 0 || fdp->ops == 0 || fdp->ops->unlink == 0) {
    r = -EPERM;
    goto out;
  }
  // the new name first: if it cannot be made, the old one is still
  // there and nothing has been lost.
  ilock(ip);
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);
  r = tdp->ops->link(tdp, tname, ip);
  if (r < 0) {
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlock(ip);
    goto out;
  }
  r = fdp->ops->unlink(fdp, fname);

out:
  if (second != first)
    iunlock(second);
  iunlock(first);
  if (victim)
    iput(victim);
  if (ip)
    iput(ip);
  iput(fdp);
  iput(tdp);
  return r;
}

uint64
sys_rename(void)
{
  char from[MAXPATH], to[MAXPATH];

  if (argstr(0, from, MAXPATH) < 0 || argstr(1, to, MAXPATH) < 0)
    return -EFAULT;
  return do_rename(AT_FDCWD, from, AT_FDCWD, to);
}

// renameat(fromfd, from, tofd, to)
uint64
sys_renameat(void)
{
  char from[MAXPATH], to[MAXPATH];
  int fromfd, tofd;

  argint(0, &fromfd);
  argint(2, &tofd);
  if (argstr(1, from, MAXPATH) < 0 || argstr(3, to, MAXPATH) < 0)
    return -EFAULT;
  return do_rename(fromfd, from, tofd, to);
}

// ---- the *at family ----
//
// a relative path is resolved from the directory a descriptor names
// rather than from the process's own. AT_FDCWD means the current
// directory, which is what every call that is not an *at call uses.
//
// the descriptor is looked at only long enough to take a reference to
// its inode; namei_from releases that reference itself.
static struct inode *
at_lookup(int dirfd, char *path, int parent, int nofollow, char *name)
{
  struct inode *start = 0;

  if (path[0] != '/' && dirfd != AT_FDCWD) {
    struct file *f = fdget(myproc(), dirfd);
    if (f == 0 || f->type != FD_INODE || f->ip == 0)
      return 0;
    ilock(f->ip);
    int isdir = f->ip->type == T_DIR;
    iunlock(f->ip);
    if (!isdir)
      return 0;
    start = idup(f->ip);
  }
  return namei_from(start, path, parent, nofollow, name);
}

// fstatat(dirfd, path, statbuf, flags): what stat, lstat and fstat all
// are underneath. AT_SYMLINK_NOFOLLOW asks about the link itself,
// AT_EMPTY_PATH about the descriptor.
uint64
sys_fstatat(void)
{
  char path[MAXPATH], name[DIRSIZ];
  uint64 addr;
  int dirfd, flags;
  struct inode *ip;
  struct stat st;

  argint(0, &dirfd);
  argaddr(2, &addr);
  argint(3, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;

  if (path[0] == 0 && (flags & AT_EMPTY_PATH)) {
    struct file *f = fdget(myproc(), dirfd);
    if (f == 0)
      return -EBADF;
    return filestat(f, addr) < 0 ? -EFAULT : 0;
  }

  ip = at_lookup(dirfd, path, 0, (flags & AT_SYMLINK_NOFOLLOW) != 0, name);
  if (ip == 0)
    return -ENOENT;
  ilock(ip);
  stati(ip, &st);
  iunlockput(ip);
  if (either_copyout(1, addr, &st, sizeof(st)) < 0)
    return -EFAULT;
  return 0;
}

// readlinkat(dirfd, path, buf, size)
uint64
sys_readlinkat(void)
{
  char path[MAXPATH], name[DIRSIZ], buf[MAXPATH];
  uint64 addr;
  int dirfd, size, n;
  struct inode *ip;

  argint(0, &dirfd);
  argaddr(2, &addr);
  argint(3, &size);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if (size <= 0)
    return -EINVAL;
  if ((ip = at_lookup(dirfd, path, 0, 1, name)) == 0)
    return -ENOENT;
  ilock(ip);
  if (ip->type != T_SYMLINK) {
    iunlockput(ip);
    return -EINVAL;
  }
  n = ip->ops && ip->ops->readlink ? ip->ops->readlink(ip, buf, sizeof(buf)) : -EINVAL;
  iunlockput(ip);
  if (n < 0)
    return n;
  if (n > size)
    n = size;
  if (either_copyout(1, addr, buf, n) < 0)
    return -EFAULT;
  return n;
}

// unlinkat(dirfd, path, flags): AT_REMOVEDIR makes it rmdir.
uint64
sys_unlinkat(void)
{
  char path[MAXPATH], name[DIRSIZ];
  int dirfd, flags, r;
  struct inode *dp;

  argint(0, &dirfd);
  argint(2, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if ((dp = at_lookup(dirfd, path, 1, 0, name)) == 0)
    return -ENOENT;
  if (vfs_readonly(dp)) {
    iput(dp);
    return -EROFS;
  }
  ilock(dp);
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    r = -EPERM;
  else if (dp->ops == 0 || dp->ops->unlink == 0)
    r = -EPERM;
  else {
    // the flag says which of the two calls this is, and a mismatch is
    // an error rather than something to do anyway.
    struct inode *ip = dirlookup(dp, name, 0);
    if (ip == 0)
      r = -ENOENT;
    else {
      ilock(ip);
      int isdir = ip->type == T_DIR;
      // neither an immutable nor an append-only file may be taken
      // away: a file that may only be added to may not be removed
      int locked = (ip->attr & (ATTR_IMMUTABLE | ATTR_APPEND)) != 0;
      iunlockput(ip);
      if (locked)
        r = -EPERM;
      else if ((flags & AT_REMOVEDIR) && !isdir)
        r = -ENOTDIR;
      else if (!(flags & AT_REMOVEDIR) && isdir)
        r = -EISDIR;
      else
        r = dp->ops->unlink(dp, name);
    }
  }
  iunlockput(dp);
  return r;
}

// faccessat(dirfd, path, mode, flags): whether the name resolves. with
// no users yet there are no permissions to check, so this says what
// every unix says for root: the file is there, or it is not.
uint64
sys_faccessat(void)
{
  char path[MAXPATH], name[DIRSIZ];
  int dirfd, mode, flags;
  struct inode *ip;

  argint(0, &dirfd);
  argint(2, &mode);
  argint(3, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  (void)mode;
  ip = at_lookup(dirfd, path, 0, (flags & AT_SYMLINK_NOFOLLOW) != 0, name);
  if (ip == 0)
    return -ENOENT;
  iput(ip);
  return 0;
}

// ---- extended attributes ----
//
// twelve calls in POSIX, four here: the first argument says whether the
// file is named by a descriptor, by a path, or by a path whose last
// element is a symbolic link not to be followed. the library spells
// out the twelve.
static struct inode *
xattr_inode(int fd, char *path)
{
  struct inode *ip;

  if (fd >= 0) {
    struct file *f = fdget(myproc(), fd);
    if (f == 0 || f->type != FD_INODE || f->ip == 0)
      return 0;
    return idup(f->ip);
  }
  ip = fd == XATTR_AT_LPATH ? namei_nofollow(path) : namei(path);
  return ip;
}

// setxattr(fd_or_flag, path, name, value, size, flags). a null value
// removes the name, which is what removexattr is.
uint64
sys_setxattr(void)
{
  char path[MAXPATH], name[XATTR_NAME_MAX + 1];
  uint64 addr;
  int fd, size, flags, r;
  struct inode *ip;
  char *buf = 0;

  argint(0, &fd);
  argaddr(3, &addr);
  argint(4, &size);
  argint(5, &flags);
  if (argstr(1, path, MAXPATH) < 0 || argstr(2, name, sizeof(name)) < 0)
    return -EFAULT;
  if (size < 0 || size > PGSIZE)
    return -E2BIG;
  if (addr != 0) {
    if ((buf = kalloc()) == 0)
      return -ENOMEM;
    if (size > 0 && either_copyin(buf, 1, addr, size) < 0) {
      kfree(buf);
      return -EFAULT;
    }
  }
  if ((ip = xattr_inode(fd, path)) == 0) {
    if (buf)
      kfree(buf);
    return fd >= 0 ? -EBADF : -ENOENT;
  }
  ilock(ip);
  if (ip->sb && ip->sb->readonly)
    r = -EROFS;
  else if (ip->ops == 0 || ip->ops->setxattr == 0)
    r = -EOPNOTSUPP;
  else
    r = ip->ops->setxattr(ip, name, buf, size, flags);
  iunlockput(ip);
  if (buf)
    kfree(buf);
  return r;
}

uint64
sys_removexattr(void)
{
  char path[MAXPATH], name[XATTR_NAME_MAX + 1];
  int fd, r;
  struct inode *ip;

  argint(0, &fd);
  if (argstr(1, path, MAXPATH) < 0 || argstr(2, name, sizeof(name)) < 0)
    return -EFAULT;
  if ((ip = xattr_inode(fd, path)) == 0)
    return fd >= 0 ? -EBADF : -ENOENT;
  ilock(ip);
  if (ip->ops == 0 || ip->ops->setxattr == 0)
    r = -EOPNOTSUPP;
  else
    r = ip->ops->setxattr(ip, name, 0, 0, 0);
  iunlockput(ip);
  return r;
}

// getxattr(fd_or_flag, path, name, buf, size). size 0 asks how long
// the value is without copying it.
uint64
sys_getxattr(void)
{
  char path[MAXPATH], name[XATTR_NAME_MAX + 1];
  uint64 addr;
  int fd, size, r;
  struct inode *ip;
  char *buf = 0;

  argint(0, &fd);
  argaddr(3, &addr);
  argint(4, &size);
  if (argstr(1, path, MAXPATH) < 0 || argstr(2, name, sizeof(name)) < 0)
    return -EFAULT;
  if (size < 0)
    return -EINVAL;
  if (size > PGSIZE)
    size = PGSIZE;
  if (size > 0 && (buf = kalloc()) == 0)
    return -ENOMEM;
  if ((ip = xattr_inode(fd, path)) == 0) {
    if (buf)
      kfree(buf);
    return fd >= 0 ? -EBADF : -ENOENT;
  }
  ilock(ip);
  if (ip->ops == 0 || ip->ops->getxattr == 0)
    r = -EOPNOTSUPP;
  else
    r = ip->ops->getxattr(ip, name, buf, size);
  iunlockput(ip);
  if (r > 0 && size > 0 && either_copyout(1, addr, buf, r) < 0)
    r = -EFAULT;
  if (buf)
    kfree(buf);
  return r;
}

// listxattr(fd_or_flag, path, buf, size): every name, each ending in a
// zero byte. size 0 reports how much room they need.
uint64
sys_listxattr(void)
{
  char path[MAXPATH];
  uint64 addr;
  int fd, size, r;
  struct inode *ip;
  char *buf = 0;

  argint(0, &fd);
  argaddr(2, &addr);
  argint(3, &size);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if (size < 0)
    return -EINVAL;
  if (size > PGSIZE)
    size = PGSIZE;
  if (size > 0 && (buf = kalloc()) == 0)
    return -ENOMEM;
  if ((ip = xattr_inode(fd, path)) == 0) {
    if (buf)
      kfree(buf);
    return fd >= 0 ? -EBADF : -ENOENT;
  }
  ilock(ip);
  if (ip->ops == 0 || ip->ops->listxattr == 0)
    r = -EOPNOTSUPP;
  else
    r = ip->ops->listxattr(ip, buf, size);
  iunlockput(ip);
  if (r > 0 && size > 0 && either_copyout(1, addr, buf, r) < 0)
    r = -EFAULT;
  if (buf)
    kfree(buf);
  return r;
}

// ---- ownership and permission ----
//
// the checks every path goes through. the effective ids are what is
// asked about, because that is what POSIX says a check uses; the real
// ones are only who the process started as.

// may this process do `want` (a mask of 4 read, 2 write, 1 execute or
// search) to this inode?
int
inode_permitted(struct inode *ip, int want)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;
  uint mode = ip->mode;

  if (ip->sb && ip->sb->readonly && (want & 2))
    return -EROFS;

  // the owner's bits, the group's, or everyone else's -- the first of
  // the three that applies, not the most generous
  if (c->euid == ip->uid) {
    mode >>= 6;
  } else {
    int ingroup = c->egid == ip->gid;
    for (int i = 0; !ingroup && i < c->ngroups; i++)
      if (c->groups[i] == ip->gid)
        ingroup = 1;
    if (ingroup)
      mode >>= 3;
  }
  if ((mode & (uint)want) == (uint)want)
    return 0;

  // a capability can override the bits. reading and searching are one
  // capability and writing another, because a process that is allowed
  // to look at everything is not thereby allowed to change it.
  if (cap_ok(CAP_DAC_OVERRIDE)) {
    // even root may not execute a file no one may execute
    if (!(want & 1) || ip->type == T_DIR || (ip->mode & 0111))
      return 0;
  }
  if (!(want & 2) && cap_ok(CAP_DAC_READ_SEARCH))
    return 0;
  return -EACCES;
}

// the check a directory entry's removal or replacement needs: the
// sticky bit on a directory says only the owner of a file may take it
// out, which is what /tmp is for.
int
sticky_permitted(struct inode *dp, struct inode *ip)
{
  struct proc *p = myproc();

  if (!(dp->mode & 01000))
    return 0;
  if (p->cred.euid == ip->uid || p->cred.euid == dp->uid || cap_ok(CAP_FOWNER))
    return 0;
  return -EPERM;
}

// chmod: the owner or a process with CAP_FOWNER
static int
do_chmod(struct inode *ip, int mode)
{
  struct proc *p = myproc();

  if (p->cred.euid != ip->uid && !cap_ok(CAP_FOWNER))
    return -EPERM;
  if (ip->sb && ip->sb->readonly)
    return -EROFS;
  ip->mode = (uint)mode & 07777;
  // a group-executable file that is set-group-id and not the caller's
  // group loses the bit, as it does everywhere: it is the one way a
  // process could give itself a group it is not in.
  if (!cap_ok(CAP_FSETID) && ip->type != T_DIR && p->cred.egid != ip->gid) {
    int ingroup = 0;
    for (int i = 0; i < p->cred.ngroups; i++)
      if (p->cred.groups[i] == ip->gid)
        ingroup = 1;
    if (!ingroup)
      ip->mode &= ~02000u;
  }
  iupdate(ip);
  return 0;
}

// chown: only a privileged process may give a file away; the owner may
// change its group to one of its own.
static int
do_chown(struct inode *ip, int uid, int gid)
{
  struct proc *p = myproc();
  struct cred *c = &p->cred;

  if (ip->sb && ip->sb->readonly)
    return -EROFS;
  // the numbers came from a program, so they are its namespace's: a
  // name the namespace does not have is not an owner it may name
  if (uid >= 0) {
    int k = uid_from_ns(userns_current(), (unsigned int)uid);
    if (k < 0)
      return -EINVAL;
    uid = k;
  }
  if (gid >= 0) {
    int k = gid_from_ns(userns_current(), (unsigned int)gid);
    if (k < 0)
      return -EINVAL;
    gid = k;
  }
  if (uid >= 0 && (uint)uid != ip->uid && !cap_ok(CAP_CHOWN))
    return -EPERM;
  if (gid >= 0 && (uint)gid != ip->gid && !cap_ok(CAP_CHOWN)) {
    if (c->euid != ip->uid)
      return -EPERM;
    int ingroup = c->egid == (uint)gid;
    for (int i = 0; !ingroup && i < c->ngroups; i++)
      if (c->groups[i] == (uint)gid)
        ingroup = 1;
    if (!ingroup)
      return -EPERM;
  }
  if (uid >= 0)
    ip->uid = (uint)uid;
  if (gid >= 0)
    ip->gid = (uint)gid;
  // giving a file away takes its set-user-id and set-group-id bits
  // with it, or it would run as its new owner
  if (!cap_ok(CAP_FSETID) && ip->type != T_DIR)
    ip->mode &= ~06000u;
  iupdate(ip);
  return 0;
}

uint64
sys_chmod(void)
{
  char path[MAXPATH];
  struct inode *ip;
  int mode, r;

  argint(1, &mode);
  if (argstr(0, path, MAXPATH) < 0)
    return -EFAULT;
  if ((ip = namei(path)) == 0)
    return -ENOENT;
  ilock(ip);
  r = do_chmod(ip, mode);
  iunlockput(ip);
  return r;
}

uint64
sys_fchmod(void)
{
  struct file *f;
  int mode, r;

  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  argint(1, &mode);
  if (f->type != FD_INODE || f->ip == 0)
    return -EINVAL;
  ilock(f->ip);
  r = do_chmod(f->ip, mode);
  iunlock(f->ip);
  return r;
}

uint64
sys_fchmodat(void)
{
  char path[MAXPATH], name[DIRSIZ];
  struct inode *ip;
  int dirfd, mode, flags, r;

  argint(0, &dirfd);
  argint(2, &mode);
  argint(3, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if ((ip = at_lookup(dirfd, path, 0, (flags & AT_SYMLINK_NOFOLLOW) != 0, name)) == 0)
    return -ENOENT;
  ilock(ip);
  r = do_chmod(ip, mode);
  iunlockput(ip);
  return r;
}

uint64
sys_chown(void)
{
  char path[MAXPATH];
  struct inode *ip;
  int uid, gid, r;

  argint(1, &uid);
  argint(2, &gid);
  if (argstr(0, path, MAXPATH) < 0)
    return -EFAULT;
  if ((ip = namei(path)) == 0)
    return -ENOENT;
  ilock(ip);
  r = do_chown(ip, uid, gid);
  iunlockput(ip);
  return r;
}

uint64
sys_fchown(void)
{
  struct file *f;
  int uid, gid, r;

  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  argint(1, &uid);
  argint(2, &gid);
  if (f->type != FD_INODE || f->ip == 0)
    return -EINVAL;
  ilock(f->ip);
  r = do_chown(f->ip, uid, gid);
  iunlock(f->ip);
  return r;
}

uint64
sys_fchownat(void)
{
  char path[MAXPATH], name[DIRSIZ];
  struct inode *ip;
  int dirfd, uid, gid, flags, r;

  argint(0, &dirfd);
  argint(2, &uid);
  argint(3, &gid);
  argint(4, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if ((ip = at_lookup(dirfd, path, 0, (flags & AT_SYMLINK_NOFOLLOW) != 0, name)) == 0)
    return -ENOENT;
  ilock(ip);
  r = do_chown(ip, uid, gid);
  iunlockput(ip);
  return r;
}

// chroot(path): "/" means this directory for this process and every
// one it starts. only a process with CAP_SYS_CHROOT may do it, because
// a directory that holds a set-user-id program and a passwd file of
// its own would otherwise be a way to become anyone.
uint64
sys_chroot(void)
{
  char path[MAXPATH];
  struct inode *ip;

  if (argstr(0, path, MAXPATH) < 0)
    return -EFAULT;
  if (!cap_ok(CAP_SYS_CHROOT)) {
    audit("chroot", path, 0, 0, 0);
    return -EPERM;
  }
  if ((ip = namei(path)) == 0)
    return -ENOENT;
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    return -ENOTDIR;
  }
  if (inode_permitted(ip, 1) < 0) {
    iunlockput(ip);
    return -EACCES;
  }
  iunlock(ip);
  proc_set_root(myproc(), ip); // it takes the reference
  audit("chroot", path, 0, 0, 1);
  return 0;
}
