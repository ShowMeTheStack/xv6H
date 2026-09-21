//
// /dev/log and /dev/kmsg: two rings of text.
//
// /dev/log is where a program's syslog(3) writes and where syslogd
// reads: a writer appends one message, a reader takes the oldest one
// that is still there and waits when there is none. it is a character
// device rather than a socket because AF_UNIX is not here yet, and the
// shape of the thing -- one message per write, one per read -- is the
// same either way.
//
// /dev/kmsg is the kernel's own messages, which printk copies into a
// ring as it prints them, so that klogd can put them in the log file
// and a program can read what happened before it started.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "defs.h"
#include "errno.h"

#define LOGRING 16384 // bytes of messages held before the oldest is dropped

struct logring {
  struct spinlock lock;
  char buf[LOGRING];
  uint64 w;       // bytes ever written; w - r is what is waiting
  uint64 r;       // bytes ever read
  uint64 dropped; // messages lost because nobody was reading
};

static struct logring userlog; // /dev/log
static struct logring kmsg;    // /dev/kmsg

static void
ring_put(struct logring *lr, const char *s, int n)
{
  acquire(&lr->lock);
  // a ring that nobody is draining drops the oldest, not the newest: a
  // log is worth more at the end than at the beginning
  while (lr->w - lr->r + (uint64)n > LOGRING) {
    // drop up to and including the next newline
    while (lr->r < lr->w && lr->buf[lr->r % LOGRING] != '\n')
      lr->r++;
    if (lr->r < lr->w)
      lr->r++;
    lr->dropped++;
    if (lr->w - lr->r == 0)
      break;
  }
  for (int i = 0; i < n && lr->w - lr->r < LOGRING; i++)
    lr->buf[lr->w++ % LOGRING] = s[i];
  release(&lr->lock);
  wakeup(lr);
}

// one line out of the ring, waiting for one if there is none
static int
ring_get(struct logring *lr, int user_dst, uint64 dst, int max)
{
  int n = 0;

  acquire(&lr->lock);
  while (lr->r == lr->w) {
    // a signal ends the wait: a daemon blocked here is stopped with
    // one, and a read that cannot be interrupted could not be
    if (killed(myproc()) || signal_pending(myproc())) {
      release(&lr->lock);
      return -EINTR;
    }
    sleep_prepare(lr);
    release(&lr->lock);
    sleep_intr();
    acquire(&lr->lock);
  }
  char line[512];
  while (lr->r < lr->w && n < max && n < (int)sizeof(line)) {
    char c = lr->buf[lr->r++ % LOGRING];
    line[n++] = c;
    if (c == '\n')
      break;
  }
  release(&lr->lock);
  if (either_copyout(user_dst, dst, line, n) < 0)
    return -EFAULT;
  return n;
}

// ---- the devices ----

static int
logdev_write(int user_src, uint64 src, int n)
{
  char buf[512];

  if (n <= 0)
    return 0;
  if (n > (int)sizeof(buf))
    n = sizeof(buf);
  if (either_copyin(buf, user_src, src, n) < 0)
    return -EFAULT;
  // every message is a line, whatever the writer sent
  if (buf[n - 1] != '\n' && n < (int)sizeof(buf))
    buf[n++] = '\n';
  ring_put(&userlog, buf, n);
  return n;
}

static int
logdev_read(int user_dst, uint64 dst, int n)
{
  return ring_get(&userlog, user_dst, dst, n);
}

static int
logdev_ready(int write)
{
  uint64 avail;

  if (write)
    return 1;
  acquire(&userlog.lock);
  avail = userlog.w - userlog.r;
  release(&userlog.lock);
  return (int)avail;
}

static int
kmsg_read(int user_dst, uint64 dst, int n)
{
  return ring_get(&kmsg, user_dst, dst, n);
}

static int
kmsg_write(int user_src, uint64 src, int n)
{
  // writing to /dev/kmsg puts a line in the kernel's log, which is
  // what a boot script does to say what it is doing
  char buf[512];

  if (n <= 0)
    return 0;
  if (n > (int)sizeof(buf) - 1)
    n = sizeof(buf) - 1;
  if (either_copyin(buf, user_src, src, n) < 0)
    return -EFAULT;
  if (buf[n - 1] != '\n')
    buf[n++] = '\n';
  ring_put(&kmsg, buf, n);
  return n;
}

static int
kmsg_ready(int write)
{
  uint64 avail;

  if (write)
    return 1;
  acquire(&kmsg.lock);
  avail = kmsg.w - kmsg.r;
  release(&kmsg.lock);
  return (int)avail;
}

// printk calls this for every line it prints, so that what went to the
// console is also readable afterwards.
void
kmsg_note(const char *s, int n)
{
  if (kmsg.lock.name == 0)
    return; // before loginit; the boot banner is on the console anyway
  ring_put(&kmsg, s, n);
}

void
loginit(void)
{
  initlock(&userlog.lock, "devlog");
  initlock(&kmsg.lock, "kmsg");
  devsw[DEV_LOG].read = logdev_read;
  devsw[DEV_LOG].write = logdev_write;
  devsw[DEV_LOG].ready = logdev_ready;
  devsw[DEV_KMSG].read = kmsg_read;
  devsw[DEV_KMSG].write = kmsg_write;
  devsw[DEV_KMSG].ready = kmsg_ready;
}
