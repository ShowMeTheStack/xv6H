//
// AF_UNIX: a socket that never leaves the machine.
//
// two processes find each other through a name in the file system --
// or, with a leading zero byte, through a name in a namespace of its
// own that leaves nothing behind. Nothing here touches the network
// stack: a connected pair is two byte rings, and a datagram socket is
// a queue of messages, so what goes through is bytes copied from one
// address space to another and nothing more.
//
// the part worth reading is the ancillary data. SCM_RIGHTS lets a
// process hand a descriptor to another: the sender names a descriptor
// number, the kernel takes a reference on the open file behind it,
// and the receiver is given a fresh descriptor onto that same file.
// what travels is the open file -- its offset, its flags, the inode
// it holds -- and not the number, which means nothing outside the
// process it came from. SCM_CREDENTIALS travels the same way and
// carries who sent it, filled in by the kernel rather than by the
// sender, so it cannot be forged.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "stat.h"
#include "file.h"
#include "vfs.h"
#include "defs.h"
#include "errno.h"
#include "socket.h"
#include "unixsock.h"
#include "mm.h"

#define UN_BUFSZ   (16 * 1024)   // bytes a connected pair may hold each way
#define UN_MAXDG   32            // datagrams a socket may have waiting
#define UN_BACKLOG 16

// a message on a datagram socket, or a run of bytes with descriptors
// attached on a stream one
struct un_msg {
  struct un_msg *next;
  char *data;
  int len;
  int off;                       // how much of it has been read
  struct file *fds[UN_MAXFDS];    // descriptors travelling with it
  int nfds;
  struct ucred cred;             // who sent it
  char from[UN_PATHMAX];         // the sender's name, for a datagram
  int fromlen;
};

struct unsock {
  struct spinlock lock;
  int used;
  int type;                      // SOCK_STREAM or SOCK_DGRAM
  int listening;
  int shut_rd, shut_wr;
  int closed;                    // the descriptor on this end has gone
  int refs;

  // the name it is bound to: either a path in the file system, by the
  // inode it resolves to, or an abstract name
  char path[UN_PATHMAX];
  int pathlen;
  int abstract;
  uint bound_dev, bound_inum;
  int bound;

  struct unsock *peer;           // the other end of a connection
  int connected;
  int peer_gone;

  // a connected stream's incoming bytes, and the messages a datagram
  // socket has waiting
  struct un_msg *rxq, *rxtail;
  int rxbytes, rxcount;

  // a listening socket's queue of connections waiting to be accepted
  struct unsock *backlog[UN_BACKLOG];
  int nbacklog, maxbacklog;

  struct ucred cred;             // who made it
  int passcred;                  // SO_PASSCRED: tell the receiver who sent
};

#define NUNSOCK 64
static struct unsock unsocks[NUNSOCK];
static struct spinlock untable;

void
unixsock_init(void)
{
  initlock(&untable, "unsock");
  for (int i = 0; i < NUNSOCK; i++)
    initlock(&unsocks[i].lock, "unsock1");
}

// walk the table for /proc/net/unix. each socket's own lock is taken
// in turn rather than one lock over the whole walk, so a busy pair is
// not held up by a reader.
void
unixsock_walk(void (*fn)(const struct unsockinfo *si, void *arg), void *arg)
{
  for (int i = 0; i < NUNSOCK; i++) {
    struct unsock *s = &unsocks[i];
    struct unsockinfo si;

    acquire(&s->lock);
    if (!s->used) {
      release(&s->lock);
      continue;
    }
    si.index = i;
    si.refs = s->refs;
    si.type = s->type;
    si.listening = s->listening;
    si.connected = s->connected && s->peer != 0;
    si.abstract = s->abstract;
    si.pathlen = s->bound ? s->pathlen : 0;
    if (si.pathlen > 0)
      memmove(si.path, s->path, si.pathlen);
    release(&s->lock);
    fn(&si, arg);
  }
}

struct unsock *
unixsock_alloc(int type)
{
  struct proc *p = myproc();

  acquire(&untable);
  for (int i = 0; i < NUNSOCK; i++) {
    struct unsock *s = &unsocks[i];
    if (s->used)
      continue;
    memset((char *)s + sizeof(struct spinlock), 0,
           sizeof(*s) - sizeof(struct spinlock));
    s->used = 1;
    s->refs = 1;
    s->type = type;
    s->maxbacklog = 1;
    s->cred.pid = p->pid;
    s->cred.uid = p->cred.euid;
    s->cred.gid = p->cred.egid;
    release(&untable);
    return s;
  }
  release(&untable);
  return 0;
}

static void
msg_free(struct un_msg *m)
{
  for (int i = 0; i < m->nfds; i++)
    if (m->fds[i])
      fileclose(m->fds[i]);
  if (m->data)
    kfree(m->data);
  kfree((char *)m);
}

static void
un_drain(struct unsock *s)
{
  while (s->rxq) {
    struct un_msg *m = s->rxq;
    s->rxq = m->next;
    msg_free(m);
  }
  s->rxtail = 0;
  s->rxbytes = 0;
  s->rxcount = 0;
}

// drop one reference. the last one takes the socket apart.
//
// a reference and an open end are two different things, and keeping
// them apart is what makes end of file work. two joined sockets each
// hold a reference on the other, so closing one descriptor never
// brings its socket's count to zero; what it must do is break the
// link and tell the other end, which is unixsock_close below. this
// is only the counting half of that.
static void
un_unref(struct unsock *s)
{
  if (s == 0)
    return;
  acquire(&s->lock);
  if (--s->refs > 0) {
    release(&s->lock);
    return;
  }
  un_drain(s);
  s->used = 0;
  s->bound = 0;
  s->listening = 0;
  release(&s->lock);
}

// the descriptor on this end has gone. the link to the other end is
// broken -- so that a read there sees end of file rather than waiting
// for ever -- and the references the two held on each other are given
// up, along with this end's own.
void
unixsock_close(struct unsock *s)
{
  struct unsock *peer, *backlog[UN_BACKLOG];
  int nb = 0;

  if (s == 0)
    return;
  acquire(&s->lock);
  peer = s->peer;
  s->peer = 0;
  s->closed = 1;
  un_drain(s);
  for (int i = 0; i < s->nbacklog; i++)
    if (s->backlog[i])
      backlog[nb++] = s->backlog[i];
  s->nbacklog = 0;
  s->listening = 0;
  release(&s->lock);
  wakeup(s);

  // anything still waiting to be accepted never will be
  for (int i = 0; i < nb; i++)
    unixsock_close(backlog[i]);

  if (peer) {
    acquire(&peer->lock);
    peer->peer_gone = 1;
    int held = peer->peer == s;   // the reference the other end has on this one
    if (held)
      peer->peer = 0;
    release(&peer->lock);
    wakeup(peer);
    if (held)
      un_unref(s);
    un_unref(peer);               // and the one this end had on it
  }
  un_unref(s);
}

struct unsock *
unixsock_dup(struct unsock *s)
{
  if (s) {
    acquire(&s->lock);
    s->refs++;
    release(&s->lock);
  }
  return s;
}

// ---- names ----

// the socket bound to this name, with a reference held
static struct unsock *
un_find(const char *path, int len, int abstract, uint dev, uint inum)
{
  acquire(&untable);
  for (int i = 0; i < NUNSOCK; i++) {
    struct unsock *s = &unsocks[i];
    if (!s->used || !s->bound || s->abstract != abstract)
      continue;
    if (abstract) {
      if (s->pathlen == len && memcmp(s->path, path, len) == 0) {
        acquire(&s->lock);
        s->refs++;
        release(&s->lock);
        release(&untable);
        return s;
      }
    } else if (s->bound_dev == dev && s->bound_inum == inum) {
      acquire(&s->lock);
      s->refs++;
      release(&s->lock);
      release(&untable);
      return s;
    }
  }
  release(&untable);
  return 0;
}

// bind: make the name, and remember what it resolved to
int
unixsock_bind(struct unsock *s, const char *path, int len)
{
  if (s->bound)
    return -EINVAL;
  if (len <= 0 || len > UN_PATHMAX)
    return -EINVAL;

  if (path[0] == 0) {
    // an abstract name: nothing is made in the file system
    if (un_find(path, len, 1, 0, 0)) {
      return -EADDRINUSE;
    }
    acquire(&s->lock);
    memmove(s->path, path, len);
    s->pathlen = len;
    s->abstract = 1;
    s->bound = 1;
    release(&s->lock);
    return 0;
  }

  // a path: a node of its own kind, which is what lets ls show it for
  // what it is and what stops a plain file being mistaken for one
  char name[DIRSIZ];
  struct inode *dp = nameiparent((char *)path, name);
  if (dp == 0)
    return -ENOENT;
  if (vfs_readonly(dp)) {
    iput(dp);
    return -EROFS;
  }
  ilock(dp);
  if (dp->ops == 0 || dp->ops->create == 0) {
    iunlockput(dp);
    return -EPERM;
  }
  struct inode *ip = dirlookup(dp, name, 0);
  if (ip) {
    iput(ip);
    iunlockput(dp);
    return -EADDRINUSE;
  }
  int e = inode_permitted(dp, 2 | 1);
  if (e < 0) {
    iunlockput(dp);
    return e;
  }
  ip = dp->ops->create(dp, name, T_SOCK, 0, 0);
  if (ip == 0) {
    iunlockput(dp);
    return -EIO;
  }
  ip->mode = 0777 & ~myproc()->umask;
  ip->uid = myproc()->cred.euid;
  ip->gid = myproc()->cred.egid;
  iupdate(ip);
  uint dev = ip->dev, inum = ip->inum;
  iunlockput(ip);
  iunlockput(dp);

  acquire(&s->lock);
  memmove(s->path, path, len < UN_PATHMAX ? len : UN_PATHMAX);
  s->pathlen = len;
  s->abstract = 0;
  s->bound_dev = dev;
  s->bound_inum = inum;
  s->bound = 1;
  release(&s->lock);
  return 0;
}

// the socket a name leads to
static struct unsock *
un_lookup(const char *path, int len, int *err)
{
  *err = 0;
  if (len <= 0 || len > UN_PATHMAX) {
    *err = -EINVAL;
    return 0;
  }
  if (path[0] == 0) {
    struct unsock *s = un_find(path, len, 1, 0, 0);
    if (s == 0)
      *err = -ECONNREFUSED;
    return s;
  }
  struct inode *ip = namei((char *)path);
  if (ip == 0) {
    *err = -ENOENT;
    return 0;
  }
  ilock(ip);
  int type = ip->type;
  uint dev = ip->dev, inum = ip->inum;
  int e = inode_permitted(ip, 2 | 4);
  iunlockput(ip);
  if (type != T_SOCK) {
    *err = -ECONNREFUSED;
    return 0;
  }
  if (e < 0) {
    *err = e;
    return 0;
  }
  struct unsock *s = un_find(0, 0, 0, dev, inum);
  if (s == 0)
    *err = -ECONNREFUSED;
  return s;
}

int
unixsock_listen(struct unsock *s, int backlog)
{
  if (s->type != SOCK_STREAM)
    return -EOPNOTSUPP;
  if (!s->bound)
    return -EINVAL;
  acquire(&s->lock);
  s->listening = 1;
  s->maxbacklog = backlog < 1 ? 1 : (backlog > UN_BACKLOG ? UN_BACKLOG : backlog);
  release(&s->lock);
  return 0;
}

// connect: make the other half of the pair and hand it to the
// listener's queue, then wait to be accepted
int
unixsock_connect(struct unsock *s, const char *path, int len)
{
  int err = 0;

  if (s->type == SOCK_DGRAM) {
    // a datagram socket remembers where to send, and nothing more
    struct unsock *server = un_lookup(path, len, &err);
    if (server == 0)
      return err;
    acquire(&s->lock);
    if (s->peer)
      un_unref(s->peer);
    s->peer = server;
    s->connected = 1;
    memmove(s->path, path, len < UN_PATHMAX ? len : UN_PATHMAX);
    release(&s->lock);
    return 0;
  }

  if (s->connected)
    return -EISCONN;
  struct unsock *server = un_lookup(path, len, &err);
  if (server == 0)
    return err;

  acquire(&server->lock);
  if (!server->listening) {
    release(&server->lock);
    un_unref(server);
    return -ECONNREFUSED;
  }
  if (server->nbacklog >= server->maxbacklog) {
    release(&server->lock);
    un_unref(server);
    return -EAGAIN;
  }
  release(&server->lock);

  // the socket the listener will be given: this end's peer
  struct unsock *other = unixsock_alloc(SOCK_STREAM);
  if (other == 0) {
    un_unref(server);
    return -ENFILE;
  }

  acquire(&server->lock);
  if (!server->listening || server->nbacklog >= server->maxbacklog) {
    release(&server->lock);
    un_unref(other);
    un_unref(server);
    return -ECONNREFUSED;
  }
  // the two are joined before either is visible, so a reader can
  // never find one half of a pair
  acquire(&s->lock);
  s->peer = unixsock_dup(other);
  s->connected = 1;
  release(&s->lock);
  other->peer = unixsock_dup(s);
  other->connected = 1;
  other->cred = s->cred;   // the listener learns who connected

  server->backlog[server->nbacklog++] = other;
  release(&server->lock);
  wakeup(server);
  un_unref(server);
  return 0;
}

struct unsock *
unixsock_accept(struct unsock *s, int *err)
{
  struct proc *p = myproc();

  *err = 0;
  if (!s->listening) {
    *err = -EINVAL;
    return 0;
  }
  acquire(&s->lock);
  while (s->nbacklog == 0) {
    if (killed(p) || signal_pending(p)) {
      release(&s->lock);
      *err = -EINTR;
      return 0;
    }
    sleep_prepare(s);
    release(&s->lock);
    sleep_intr();
    acquire(&s->lock);
  }
  struct unsock *c = s->backlog[0];
  for (int i = 1; i < s->nbacklog; i++)
    s->backlog[i - 1] = s->backlog[i];
  s->nbacklog--;
  release(&s->lock);
  return c;
}

// two sockets joined to each other and to nothing else
int
unixsock_pair(int type, struct unsock **a, struct unsock **b)
{
  struct unsock *x = unixsock_alloc(type);
  struct unsock *y;

  if (x == 0)
    return -ENFILE;
  y = unixsock_alloc(type);
  if (y == 0) {
    un_unref(x);
    return -ENFILE;
  }
  x->peer = unixsock_dup(y);
  y->peer = unixsock_dup(x);
  x->connected = y->connected = 1;
  *a = x;
  *b = y;
  return 0;
}

// ---- moving bytes ----

static struct un_msg *
msg_alloc(int len)
{
  struct un_msg *m = (struct un_msg *)kalloc();

  if (m == 0)
    return 0;
  memset(m, 0, sizeof(*m));
  if (len > 0) {
    m->data = kalloc();
    if (m->data == 0) {
      kfree((char *)m);
      return 0;
    }
  }
  return m;
}

// put a message on a socket's queue. the socket's lock is held.
static void
un_enqueue(struct unsock *s, struct un_msg *m)
{
  m->next = 0;
  if (s->rxtail)
    s->rxtail->next = m;
  else
    s->rxq = m;
  s->rxtail = m;
  s->rxbytes += m->len;
  s->rxcount++;
}

// one send. `fds` and `nfds` are the descriptors travelling with it,
// already dup'd by the caller: from here they belong to the message.
int
unixsock_send(struct unsock *s, int user, uint64 addr, int n,
              const char *dest, int destlen, struct file **fds, int nfds)
{
  struct proc *p = myproc();
  struct unsock *to = 0;
  int err = 0;

  if (n < 0 || n > PGSIZE) {
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return n < 0 ? -EINVAL : -EMSGSIZE;
  }
  if (s->shut_wr) {
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return -EPIPE;
  }

  if (s->type == SOCK_DGRAM && dest && destlen > 0) {
    to = un_lookup(dest, destlen, &err);
    if (to == 0) {
      for (int i = 0; i < nfds; i++)
        fileclose(fds[i]);
      return err;
    }
  } else {
    acquire(&s->lock);
    to = unixsock_dup(s->peer);
    int gone = s->peer_gone;
    release(&s->lock);
    if (to == 0) {
      for (int i = 0; i < nfds; i++)
        fileclose(fds[i]);
      return gone ? -EPIPE : -ENOTCONN;
    }
  }

  struct un_msg *m = msg_alloc(n);
  if (m == 0) {
    un_unref(to);
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return -ENOMEM;
  }
  if (n > 0 && either_copyin(m->data, user, addr, n) < 0) {
    msg_free(m);
    un_unref(to);
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return -EFAULT;
  }
  m->len = n;
  for (int i = 0; i < nfds; i++)
    m->fds[i] = fds[i];
  m->nfds = nfds;
  // the identity is the kernel's word, not the sender's
  m->cred.pid = p->pid;
  m->cred.uid = p->cred.euid;
  m->cred.gid = p->cred.egid;
  if (s->bound) {
    memmove(m->from, s->path, s->pathlen);
    m->fromlen = s->pathlen;
  }

  acquire(&to->lock);
  // wait for room. a stream blocks on bytes, a datagram socket on the
  // number of messages waiting.
  for (;;) {
    int full = s->type == SOCK_STREAM ? (to->rxbytes + n > UN_BUFSZ)
                                      : (to->rxcount >= UN_MAXDG);
    if (!full)
      break;
    if (to->peer_gone || !to->used) {
      release(&to->lock);
      msg_free(m);
      un_unref(to);
      return -EPIPE;
    }
    if (killed(p) || signal_pending(p)) {
      release(&to->lock);
      msg_free(m);
      un_unref(to);
      return -EINTR;
    }
    sleep_prepare(to);
    release(&to->lock);
    sleep_intr();
    acquire(&to->lock);
  }
  un_enqueue(to, m);
  release(&to->lock);
  wakeup(to);
  un_unref(to);
  return n;
}

// one receive. `fds` is filled with the descriptors that came with
// the message, and the caller installs them.
int
unixsock_recv(struct unsock *s, int user, uint64 addr, int n, char *from,
              int *fromlen, struct file **fds, int *nfds, struct ucred *cred,
              int flags)
{
  struct proc *p = myproc();
  int got = 0;

  if (nfds)
    *nfds = 0;
  if (fromlen)
    *fromlen = 0;
  if (n < 0)
    return -EINVAL;

  acquire(&s->lock);
  while (s->rxq == 0) {
    if (s->shut_rd || s->peer_gone) {
      release(&s->lock);
      return 0;   // end of file: nobody will send any more
    }
    if (killed(p) || signal_pending(p)) {
      release(&s->lock);
      return -EINTR;
    }
    if (flags & MSG_DONTWAIT) {
      release(&s->lock);
      return -EAGAIN;
    }
    sleep_prepare(s);
    release(&s->lock);
    sleep_intr();
    acquire(&s->lock);
  }

  struct un_msg *m = s->rxq;
  int avail = m->len - m->off;
  int take = avail < n ? avail : n;

  // the descriptors and the identity belong to the message, so they
  // are handed over with its first byte and not again
  if (m->off == 0) {
    if (nfds && fds) {
      for (int i = 0; i < m->nfds; i++)
        fds[i] = m->fds[i];
      *nfds = m->nfds;
      m->nfds = 0;   // the caller owns them now
    }
    if (cred)
      *cred = m->cred;
    if (from && fromlen && m->fromlen > 0) {
      memmove(from, m->from, m->fromlen);
      *fromlen = m->fromlen;
    }
  }

  release(&s->lock);
  if (take > 0 && either_copyout(user, addr, m->data + m->off, take) < 0)
    return -EFAULT;
  acquire(&s->lock);

  got = take;
  if (flags & MSG_PEEK) {
    // nothing is consumed, so the message stays where it is
    release(&s->lock);
    return got;
  }
  if (s->type == SOCK_STREAM) {
    m->off += take;
    s->rxbytes -= take;
    if (m->off >= m->len) {
      s->rxq = m->next;
      if (s->rxq == 0)
        s->rxtail = 0;
      s->rxcount--;
      release(&s->lock);
      msg_free(m);
      wakeup(s);
      return got;
    }
  } else {
    // a datagram is taken whole: what did not fit is lost, which is
    // what a datagram means
    s->rxq = m->next;
    if (s->rxq == 0)
      s->rxtail = 0;
    s->rxbytes -= m->len;
    s->rxcount--;
    release(&s->lock);
    msg_free(m);
    wakeup(s);
    return got;
  }
  release(&s->lock);
  wakeup(s);
  return got;
}

int
unixsock_shutdown(struct unsock *s, int how)
{
  acquire(&s->lock);
  if (how == 0 || how == 2)
    s->shut_rd = 1;
  if (how == 1 || how == 2)
    s->shut_wr = 1;
  struct unsock *peer = s->peer;
  release(&s->lock);
  wakeup(s);
  if (peer && (how == 1 || how == 2)) {
    acquire(&peer->lock);
    peer->peer_gone = 1;
    release(&peer->lock);
    wakeup(peer);
  }
  return 0;
}

// how many bytes are there to read, or whether a write would go
int
unixsock_ready(struct unsock *s, int write)
{
  int n;

  acquire(&s->lock);
  if (write)
    n = s->peer || s->type == SOCK_DGRAM ? 1 : 0;
  else
    n = s->rxq ? (s->rxq->len - s->rxq->off) : (s->peer_gone ? 1 : 0);
  if (!write && s->nbacklog > 0)
    n = 1;   // a listening socket is readable when a connection waits
  release(&s->lock);
  return n;
}

// the name it is bound to, and the name of the other end
int
unixsock_name(struct unsock *s, int peer, char *out, int max)
{
  int n = 0;

  acquire(&s->lock);
  struct unsock *t = peer ? s->peer : s;
  if (t) {
    if (t != s)
      acquire(&t->lock);
    n = t->pathlen < max ? t->pathlen : max;
    if (n > 0)
      memmove(out, t->path, n);
    if (t != s)
      release(&t->lock);
  }
  release(&s->lock);
  return n;
}

int
unixsock_type(struct unsock *s)
{
  return s->type;
}

int
unixsock_getcred(struct unsock *s, struct ucred *out)
{
  acquire(&s->lock);
  struct unsock *peer = s->peer;
  if (peer) {
    *out = peer->cred;
    release(&s->lock);
    return 0;
  }
  release(&s->lock);
  return -ENOTCONN;
}

void
unixsock_passcred(struct unsock *s, int on)
{
  acquire(&s->lock);
  s->passcred = on;
  release(&s->lock);
}

int
unixsock_passcred_on(struct unsock *s)
{
  int on;

  acquire(&s->lock);
  on = s->passcred;
  release(&s->lock);
  return on;
}
