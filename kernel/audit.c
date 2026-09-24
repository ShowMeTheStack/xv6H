//
// audit: what the privileged things that happened were.
//
// A machine with users on it has a handful of operations that change
// who somebody is or what the machine is: becoming another user,
// taking a capability, mounting a file system, starting or destroying
// a virtual machine, changing what share of the processor a group of
// tasks gets. None of those is frequent, and all of them are worth
// being able to look at afterwards -- which is the difference between
// "the machine is behaving oddly" and "at 11:04 this program became
// root".
//
// The records are lines in a ring: the newest ones survive, the
// oldest are dropped, and nothing waits for a reader. /proc/audit is
// where they are read (by root alone) and /dev/audit is where a
// program adds one of its own.
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
#include "cred.h"
#include "errno.h"

#define AUDITRING 8192  // bytes of records held before the oldest go

static struct {
  struct spinlock lock;
  int ready;
  char buf[AUDITRING];
  uint64 w;      // bytes ever written
  uint64 r;      // the oldest byte still here
  uint64 seq;    // records ever made
  uint64 lost;   // records dropped to make room
} au;

void
auditinit(void)
{
  initlock(&au.lock, "audit");
  au.ready = 1;
}

// ---- writing a record ----
//
// the kernel has no snprintf, and an audit record is not free-form
// text anyway: it is a few named fields, and a program that reads it
// wants them always in the same order. Hence put_* rather than a
// format string.

static void
put_bytes(const char *s, int n)
{
  while (au.w - au.r + (uint64)n > AUDITRING) {
    // drop the oldest record whole, up to and including its newline: a
    // half record read later would be worse than no record.
    while (au.r < au.w && au.buf[au.r % AUDITRING] != '\n')
      au.r++;
    if (au.r < au.w)
      au.r++;
    au.lost++;
    if (au.w == au.r)
      break;
  }
  for (int i = 0; i < n && au.w - au.r < AUDITRING; i++)
    au.buf[au.w++ % AUDITRING] = s[i];
}

static void
put_str(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  put_bytes(s, n);
}

static void
put_num(uint64 v)
{
  char tmp[24];
  int n = 0;

  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  }
  while (n > 0) {
    char c = tmp[--n];
    put_bytes(&c, 1);
  }
}

static void
put_kv(const char *key, uint64 v)
{
  put_str(key);
  put_str("=");
  put_num(v);
}

// one record. `op' is what was done, `detail' what it was done to (a
// path, a name, a field), and a and b are its numbers: for setuid the
// old and new user, for vm_create the id and the memory, and so on.
// `ok' is whether it was allowed.
void
audit(const char *op, const char *detail, uint64 a, uint64 b, int ok)
{
  struct proc *p = myproc();

  if (!au.ready)
    return;
  // the clock has a lock of its own: read it before taking this one,
  // so that the only nesting here is the one that cannot be avoided.
  uint64 now = clock_realtime_ns() / 1000000000UL;
  acquire(&au.lock);
  put_kv("seq", ++au.seq);
  put_kv(" t", now);
  put_kv(" pid", p ? p->pid : 0);
  put_kv(" uid", p ? p->cred.uid : 0);
  put_kv(" euid", p ? p->cred.euid : 0);
  put_str(" comm=");
  put_str(p ? p->name : "kernel");
  put_str(" op=");
  put_str(op);
  if (detail && detail[0]) {
    put_str(" what=");
    put_str(detail);
  }
  put_kv(" a", a);
  put_kv(" b", b);
  put_str(ok ? " res=ok\n" : " res=denied\n");
  release(&au.lock);
}

// a record a program made, through /dev/audit. What it says is its
// own; who said it is not.
int
audit_write(int user, uint64 src, int n)
{
  char buf[192];
  struct proc *p = myproc();

  if (!cap_ok(CAP_AUDIT_WRITE))
    return -EPERM;
  if (n <= 0)
    return 0;
  if (n > (int)sizeof(buf) - 1)
    n = sizeof(buf) - 1;
  if (either_copyin(buf, user, src, n) < 0)
    return -EFAULT;
  buf[n] = 0;
  for (int i = 0; i < n; i++)
    if (buf[i] == '\n' || buf[i] == '\r')
      buf[i] = ' ';        // one record is one line, whatever was sent

  if (!au.ready)
    return n;
  uint64 now = clock_realtime_ns() / 1000000000UL;
  acquire(&au.lock);
  put_kv("seq", ++au.seq);
  put_kv(" t", now);
  put_kv(" pid", p ? p->pid : 0);
  put_kv(" uid", p ? p->cred.uid : 0);
  put_kv(" euid", p ? p->cred.euid : 0);
  put_str(" comm=");
  put_str(p ? p->name : "kernel");
  put_str(" op=user msg=");
  put_str(buf);
  put_str(" res=ok\n");
  release(&au.lock);
  return n;
}

// ---- reading them ----
//
// the whole ring, oldest first, into dst; if it does not all fit the
// newest records are the ones kept, and the copy starts at a record
// boundary so that the first line is a whole line.
int
audit_snapshot(char *dst, int max)
{
  int n = 0;

  if (max <= 1)
    return 0;
  acquire(&au.lock);
  uint64 r = au.r;
  if (au.w - r > (uint64)(max - 1)) {
    r = au.w - (uint64)(max - 1);
    while (r < au.w && au.buf[r % AUDITRING] != '\n')
      r++;
    if (r < au.w)
      r++;
  }
  while (r < au.w && n < max - 1)
    dst[n++] = au.buf[r++ % AUDITRING];
  release(&au.lock);
  dst[n] = 0;
  return n;
}

// what the log itself has to say: how many records there have been,
// and how many were dropped before anybody read them.
void
audit_stats(uint64 *seq, uint64 *lost)
{
  acquire(&au.lock);
  *seq = au.seq;
  *lost = au.lost;
  release(&au.lock);
}

static int
auditdev_write(int user, uint64 src, int n)
{
  return audit_write(user, src, n);
}

// reading /dev/audit gives nothing: the records are read through
// /proc/audit, which is where a file of text belongs. The device is
// the way in, not the way out, exactly as /dev/log is.
static int
auditdev_read(int user, uint64 dst, int n)
{
  (void)user;
  (void)dst;
  (void)n;
  return 0;
}

void
auditdevinit(void)
{
  devsw[DEV_AUDIT].write = auditdev_write;
  devsw[DEV_AUDIT].read = auditdev_read;
}
