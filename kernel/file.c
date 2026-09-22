//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "blk.h"
#include "blkdev.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "slab.h"
#include "net.h"
#include "sock.h"
#include "unixsock.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock; // reference counts
} ftable;
static struct kmem_cache *file_cache;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
  file_cache = kmem_cache_create("file", sizeof(struct file), 8, 0);
}

// Allocate a file structure.
struct file *
filealloc(void)
{
  struct file *f = kmem_cache_alloc(file_cache);
  if (f == 0)
    return 0;
  memset(f, 0, sizeof(*f));
  f->ref = 1;
  return f;
}

// Increment ref count for file f.
struct file *
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if (f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if (f->ref < 1)
    panic("fileclose");
  if (--f->ref > 0) {
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);
  kmem_cache_free(file_cache, f); // the copy ff carries what is left to do

  if (ff.type == FD_DEVICE && ff.major >= 0 && ff.major < NDEV &&
      devsw[ff.major].close)
    devsw[ff.major].close();

  if (ff.type == FD_SOCK) {
    sockclose(ff.sock);
  } else if (ff.type == FD_UNIX) {
    unixsock_close(ff.un);
  } else if (ff.type == FD_PIPE) {
    pipeclose(ff.pipe, ff.writable);
  } else if (ff.type == FD_INODE || ff.type == FD_DEVICE || ff.type == FD_BLOCK) {
    iput(ff.ip);
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;

  if (f->type == FD_INODE || f->type == FD_DEVICE || f->type == FD_BLOCK) {
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if (copyout(p->mm, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  // a socket has no inode to describe. what fstat says about one is
  // that it is a socket and nothing more, which is what a program
  // asking `is this a socket?' wants; everything else still fails,
  // as it always has.
  memset(&st, 0, sizeof(st));
  st.nlink = 1;
  st.ino = ((uint64)f) >> 4;
  switch (f->type) {
  case FD_SOCK:
  case FD_UNIX:
    st.type = T_SOCK;
    break;
  default:
    return -1;
  }
  if (copyout(p->mm, addr, (char *)&st, sizeof(st)) < 0)
    return -1;
  return 0;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if (f->readable == 0 || n < 0)
    return -1;

  if (f->type == FD_SOCK) {
    r = sockread(f->sock, 1, addr, n);
  } else if (f->type == FD_UNIX) {
    r = unixsock_recv(f->un, 1, addr, n, 0, 0, 0, 0, 0, 0);
  } else if (f->type == FD_PIPE) {
    r = piperead(f->pipe, addr, n);
  } else if (f->type == FD_BLOCK) {
    r = blkdev_read(f->disk, 1, addr, f->off, n);
    if (r > 0)
      f->off += r;
  } else if (f->type == FD_DEVICE) {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if (f->type == FD_INODE) {
    ilock(f->ip);
    if ((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if (f->writable == 0 || n < 0)
    return -1;

  if (f->type == FD_SOCK) {
    ret = sockwrite(f->sock, 1, addr, n);
  } else if (f->type == FD_UNIX) {
    ret = unixsock_send(f->un, 1, addr, n, 0, 0, 0, 0);
  } else if (f->type == FD_PIPE) {
    ret = pipewrite(f->pipe, addr, n);
  } else if (f->type == FD_BLOCK) {
    ret = blkdev_write(f->disk, 1, addr, f->off, n);
    if (ret > 0)
      f->off += ret;
  } else if (f->type == FD_DEVICE) {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if (f->type == FD_INODE) {
    // a long write is broken into pieces so that one call does not
    // hold the inode for an unbounded time, and so that the file
    // system's transaction stays a size it can promise to fit.
    int max = FILEWRITE_MAX;
    int i = 0;
    while (i < n) {
      int n1 = n - i;
      if (n1 > max)
        n1 = max;

      ilock(f->ip);
      // a file opened for appending always writes at the end, however
      // far anyone else has moved it since the last write.
      if (f->flags & O_APPEND)
        f->off = f->ip->size;
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);

      if (r != n1) {
        // a short write or an error. what the file system said is
        // worth keeping: a caller that runs out of space should be
        // told so, and collapsing everything to -1 made errno say
        // "operation not permitted" for a full disk.
        if (r > 0)
          i += r;
        if (i == 0 && r < 0)
          return r;
        break;
      }
      i += r;
    }
    ret = (i > 0 ? i : (n == 0 ? 0 : -1));
  } else {
    panic("filewrite");
  }

  return ret;
}

// is this file ready for reading (write == 0) or writing (write == 1)?
// returns the number of bytes available for reading, 1 for a writable
// file, 0 when it would block, and -1 for a hangup.
int
fileready(struct file *f, int write)
{
  switch (f->type) {
  case FD_PIPE:
    return piperready(f->pipe, write);
  case FD_SOCK:
    return sockready(f->sock, write);
  case FD_UNIX:
    return unixsock_ready(f->un, write);
  case FD_DEVICE:
    if (f->major >= 0 && f->major < NDEV && devsw[f->major].ready)
      return devsw[f->major].ready(write);
    return 1;
  case FD_BLOCK:
  case FD_INODE:
    return write ? 1 : 1; // a regular file never blocks
  default:
    return -1;
  }
}
