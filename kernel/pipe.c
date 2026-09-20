#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "errno.h"
#include "signal.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;    // number of bytes read
  uint nwrite;   // number of bytes written
  int readopen;  // read fd is still open
  int writeopen; // write fd is still open
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if ((pi = (struct pipe *)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

bad:
  if (pi)
    kfree((char *)pi);
  if (*f0)
    fileclose(*f0);
  if (*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if (writable) {
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if (pi->readopen == 0 && pi->writeopen == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else
    release(&pi->lock);
}

// bytes waiting to be read, or whether a write would fit; -1 on hangup.
int
piperready(struct pipe *pi, int write)
{
  acquire(&pi->lock);
  int r;
  if (write)
    r = pi->readopen == 0 ? -1 : (pi->nwrite < pi->nread + PIPESIZE);
  else
    r = pi->nread != pi->nwrite ? (int)(pi->nwrite - pi->nread) : (pi->writeopen ? 0 : -1);
  release(&pi->lock);
  return r;
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();
  char chunk[128];

  // user memory is copied with the lock released: bringing a page in may
  // sleep (a file-backed page, a copy-on-write copy).
  while (i < n) {
    int k = n - i;
    if (k > sizeof(chunk))
      k = sizeof(chunk);
    if (copyin(pr->mm, chunk, addr + i, k) == -1)
      return i == 0 ? -1 : i;
    int done = 0;
    acquire(&pi->lock);
    while (done < k) {
      if (pi->readopen == 0) {
        release(&pi->lock);
        signal_fault(pr, SIGPIPE, SI_KERNEL, 0);
        return i + done > 0 ? i + done : -EPIPE;
      }
      if (killed(pr)) {
        release(&pi->lock);
        return -EPIPE;
      }
      if (signal_pending(pr)) {
        release(&pi->lock);
        return i + done > 0 ? i + done : -EINTR;
      }
      if (pi->nwrite == pi->nread + PIPESIZE) { //DOC: pipewrite-full
        wakeup(&pi->nread);
        sleep_prepare(&pi->nwrite);
        release(&pi->lock);
        sleep_intr();
        acquire(&pi->lock);
      } else {
        pi->data[pi->nwrite++ % PIPESIZE] = chunk[done++];
      }
    }
    wakeup(&pi->nread);
    release(&pi->lock);
    i += k;
  }
  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char chunk[128];

  acquire(&pi->lock);
  while (pi->nread == pi->nwrite && pi->writeopen) { //DOC: pipe-empty
    if (killed(pr)) {
      release(&pi->lock);
      return -EINTR;
    }
    if (signal_pending(pr)) {
      release(&pi->lock);
      return -EINTR;
    }
    sleep_prepare(&pi->nread); //DOC: piperead-sleep
    release(&pi->lock);
    sleep_intr();
    acquire(&pi->lock);
  }
  // take what is there (up to n), then copy it out without the lock.
  for (i = 0; i < n && i < sizeof(chunk); i++) { //DOC: piperead-copy
    if (pi->nread == pi->nwrite)
      break;
    chunk[i] = pi->data[pi->nread % PIPESIZE];
    pi->nread++;
  }
  wakeup(&pi->nwrite); //DOC: piperead-wakeup
  release(&pi->lock);
  if (i > 0 && copyout(pr->mm, addr, chunk, i) == -1)
    return -1;
  return i;
}
