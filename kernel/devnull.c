//
// the character devices that have no hardware behind them: /dev/null,
// /dev/zero, /dev/full and /dev/random. programs expect them to exist,
// and each is a few lines.
//
#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "errno.h"

// /dev/null: reads see end of file, writes disappear
static int
nullread(int user_dst, uint64 dst, int n)
{
  (void)user_dst;
  (void)dst;
  (void)n;
  return 0;
}

static int
nullwrite(int user_src, uint64 src, int n)
{
  (void)user_src;
  (void)src;
  return n;
}

// /dev/zero: an endless supply of zero bytes
static int
zeroread(int user_dst, uint64 dst, int n)
{
  static const char zeroes[64];

  for (int off = 0; off < n;) {
    int chunk = n - off;
    if (chunk > (int)sizeof(zeroes))
      chunk = sizeof(zeroes);
    if (either_copyout(user_dst, dst + off, (void *)zeroes, chunk) < 0)
      return off > 0 ? off : -EFAULT;
    off += chunk;
  }
  return n;
}

// /dev/full: reads zeroes, and every write fails as if the disk were full
static int
fullwrite(int user_src, uint64 src, int n)
{
  (void)user_src;
  (void)src;
  (void)n;
  return -ENOSPC;
}

// /dev/random and /dev/urandom: the kernel's generator
static int
randomread(int user_dst, uint64 dst, int n)
{
  char buf[64];

  for (int off = 0; off < n;) {
    int chunk = n - off;
    if (chunk > (int)sizeof(buf))
      chunk = sizeof(buf);
    random_bytes(buf, chunk);
    if (either_copyout(user_dst, dst + off, buf, chunk) < 0)
      return off > 0 ? off : -EFAULT;
    off += chunk;
  }
  return n;
}

static int
always_ready(int write)
{
  (void)write;
  return 1;
}

void
devnullinit(void)
{
  devsw[DEV_NULL].read = nullread;
  devsw[DEV_NULL].write = nullwrite;
  devsw[DEV_NULL].ready = always_ready;

  devsw[DEV_ZERO].read = zeroread;
  devsw[DEV_ZERO].write = nullwrite;
  devsw[DEV_ZERO].ready = always_ready;

  devsw[DEV_FULL].read = zeroread;
  devsw[DEV_FULL].write = fullwrite;
  devsw[DEV_FULL].ready = always_ready;

  devsw[DEV_RANDOM].read = randomread;
  devsw[DEV_RANDOM].write = nullwrite;
  devsw[DEV_RANDOM].ready = always_ready;
}
