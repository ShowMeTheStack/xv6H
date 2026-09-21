//
// /dev/pktcap: every packet that goes in or out, as a person with
// tcpdump would want it.
//
// what a reader gets is a stream of pcap records -- a sixteen-byte
// header of seconds, microseconds, captured length and real length,
// then the bytes of the frame. that is the format every tool reads,
// so tcpdump here only has to put the twenty-four byte file header in
// front of it and the result is a file Wireshark opens.
//
// nothing is captured while nobody is reading: the ring is allocated
// when the device is first opened and given back when the last reader
// closes it, and net.c checks one pointer before doing any work. a
// packet that arrives with the ring full is counted as dropped rather
// than made to wait, because a slow reader must not slow the network.
//
// the device is root's alone (mode 0600 in devfs): it sees every byte
// of every connection on the machine.
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
#include "net.h"
#include "errno.h"

#define CAP_SLOTS 128
#define CAP_SNAP  1600   // as much of a frame as is kept

struct caprec {
  uint64 secs;
  uint32 usecs;
  uint32 len;      // how long the frame really was
  uint32 caplen;   // how much of it is here
  uchar data[CAP_SNAP];
};

// one slot per page: a record is 1620 bytes, so a page holds one with
// room to spare, and the ring costs nothing at all while it is off
static struct spinlock caplock;
static struct caprec *slot[CAP_SLOTS];
static int ring;                 // zero when nobody is listening
static int readers;
static uint r, w;                // read and write positions, free-running
static uint64 captured, dropped;

// the reader is part way through a record: this many bytes of it have
// already been handed over
static uint partial;

void
pktcap_init(void)
{
  initlock(&caplock, "pktcap");
}

// is anyone listening? net.c asks this before gathering anything.
int
pktcap_active(void)
{
  return ring;
}

// one frame, as it appeared on the wire. `m->head` points at the
// ethernet header and `len` is the whole frame.
void
pktcap_frame(const void *frame, uint len)
{
  acquire(&caplock);
  if (!ring) {
    release(&caplock);
    return;
  }
  if (w - r >= CAP_SLOTS) {
    // the reader is behind; the packet is counted and let go rather
    // than made to wait for it
    dropped++;
    release(&caplock);
    return;
  }
  struct caprec *rec = slot[w % CAP_SLOTS];
  uint64 ns = clock_realtime_ns();
  rec->secs = ns / 1000000000UL;
  rec->usecs = (uint32)((ns % 1000000000UL) / 1000);
  rec->len = len;
  rec->caplen = len > CAP_SNAP ? CAP_SNAP : len;
  memmove(rec->data, frame, rec->caplen);
  w++;
  captured++;
  release(&caplock);
  wakeup(&ring);
}

// the sixteen-byte record header pcap puts in front of each frame
static void
put_header(uchar *out, const struct caprec *rec)
{
  uint32 v[4];

  v[0] = (uint32)rec->secs;
  v[1] = rec->usecs;
  v[2] = rec->caplen;
  v[3] = rec->len;
  for (int i = 0; i < 4; i++) {
    out[i * 4 + 0] = v[i] & 0xff;
    out[i * 4 + 1] = (v[i] >> 8) & 0xff;
    out[i * 4 + 2] = (v[i] >> 16) & 0xff;
    out[i * 4 + 3] = (v[i] >> 24) & 0xff;
  }
}

static int
pktcap_read(int user_dst, uint64 dst, int n)
{
  uchar hdr[16];

  if (n <= 0)
    return 0;
  acquire(&caplock);
  while (ring && r == w) {
    if (killed(myproc()) || signal_pending(myproc())) {
      release(&caplock);
      return -EINTR;
    }
    sleep_prepare(&ring);
    release(&caplock);
    sleep_intr();
    acquire(&caplock);
  }
  if (!ring) {
    release(&caplock);
    return 0;
  }

  struct caprec *rec = slot[r % CAP_SLOTS];
  put_header(hdr, rec);
  uint total = 16 + rec->caplen;
  uint left = total - partial;
  uint take = (uint)n < left ? (uint)n : left;
  release(&caplock);

  // the record is staged in a page of its own: copying to a user
  // address can fault, which must not happen under a spinlock, and a
  // frame is far too big to put on a kernel stack
  char *stage = kalloc();
  if (stage == 0)
    return -ENOMEM;
  acquire(&caplock);
  if (!ring || r == w) {
    // it went away while the page was being found; nothing is lost,
    // the next read will block again
    release(&caplock);
    kfree(stage);
    return 0;
  }
  rec = slot[r % CAP_SLOTS];
  put_header((uchar *)stage, rec);
  memmove(stage + 16, rec->data, rec->caplen);
  total = 16 + rec->caplen;
  left = total - partial;
  take = (uint)n < left ? (uint)n : left;
  uint at = partial;
  release(&caplock);

  int r2 = either_copyout(user_dst, dst, stage + at, take);
  kfree(stage);
  if (r2 < 0)
    return -EFAULT;

  acquire(&caplock);
  partial += take;
  if (partial >= total) {
    partial = 0;
    r++;
  }
  release(&caplock);
  return (int)take;
}

static int
pktcap_write(int user_src, uint64 src, int n)
{
  (void)user_src;
  (void)src;
  (void)n;
  return -EPERM; // there is nothing to say to it
}

static int
pktcap_ready(int write)
{
  int n;

  if (write)
    return 0;
  acquire(&caplock);
  n = ring && r != w ? 1 : 0;
  release(&caplock);
  return n;
}

// opening the device starts the capture and closing the last one stops
// it, so the cost is paid only while somebody is looking
int
pktcap_open(void)
{
  acquire(&caplock);
  if (readers > 0) {
    readers++;
    release(&caplock);
    return 0;
  }
  release(&caplock);

  // the pages are taken before the lock, because kalloc may sleep on
  // nothing here but the rule is the rule
  struct caprec *fresh[CAP_SLOTS];
  int got = 0;
  for (; got < CAP_SLOTS; got++) {
    fresh[got] = (struct caprec *)kalloc();
    if (fresh[got] == 0)
      break;
  }
  if (got < CAP_SLOTS) {
    for (int i = 0; i < got; i++)
      kfree(fresh[i]);
    return -ENOMEM;
  }

  acquire(&caplock);
  if (readers == 0) {
    for (int i = 0; i < CAP_SLOTS; i++)
      slot[i] = fresh[i];
    r = w = 0;
    partial = 0;
    captured = dropped = 0;
    ring = 1;
    got = 0;   // the pages now belong to the ring
  }
  readers++;
  release(&caplock);
  if (got) {
    // somebody opened it first while the pages were being gathered
    for (int i = 0; i < got; i++)
      kfree(fresh[i]);
  }
  return 0;
}

void
pktcap_close(void)
{
  struct caprec *old[CAP_SLOTS];
  int freeing = 0;

  acquire(&caplock);
  if (readers > 0 && --readers == 0) {
    for (int i = 0; i < CAP_SLOTS; i++) {
      old[i] = slot[i];
      slot[i] = 0;
    }
    ring = 0;
    freeing = 1;
  }
  release(&caplock);
  if (freeing) {
    wakeup(&ring);
    for (int i = 0; i < CAP_SLOTS; i++)
      if (old[i])
        kfree(old[i]);
  }
}

// how the capture is doing, for /proc and for tcpdump's parting line
void
pktcap_stats(uint64 *got, uint64 *lost)
{
  acquire(&caplock);
  *got = captured;
  *lost = dropped;
  release(&caplock);
}

void
pktcap_register(void)
{
  devsw[DEV_PKTCAP].read = pktcap_read;
  devsw[DEV_PKTCAP].write = pktcap_write;
  devsw[DEV_PKTCAP].ready = pktcap_ready;
  devsw[DEV_PKTCAP].open = pktcap_open;
  devsw[DEV_PKTCAP].close = pktcap_close;
}
