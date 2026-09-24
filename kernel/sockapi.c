//
// the socket system calls, in the shapes POSIX gives them.
//
// everything a program calls -- socket, bind, connect, listen,
// accept4, sendto, recvfrom, sendmsg, recvmsg, socketpair,
// getsockname, getpeername, getsockopt, setsockopt, shutdown -- is
// here, and nothing else in the kernel knows what a `struct sockaddr'
// is. below this file are two unrelated things: sock.c, which is the
// network, and unixsock.c, which is two processes on this machine
// passing bytes and descriptors to each other. this file is what
// makes them look like one idea.
//
// the split matters because an address family is a question about
// naming and not about transport. AF_INET names a machine and a port
// and the answer goes out of the interface; AF_UNIX names a file and
// the answer never leaves. Writing the conversion once, here, is what
// keeps sock.c from having to know about paths and unixsock.c from
// having to know about ports.
//
// addresses on the network are held inside the kernel as a host-order
// uint32 and a plain int port, which is what net.c has always used; a
// sockaddr_in carries both in network order, so the two htonl/htons
// pairs in this file are the whole of the conversion.
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
#include "fcntl.h"
#include "posix.h"
#include "defs.h"
#include "errno.h"
#include "net.h"
#include "sock.h"
#include "socket.h"
#include "unixsock.h"
#include "mm.h"

// ---- the shapes a user program passes in ----

struct sockaddr_k {
  uint16 sa_family;
  char sa_data[126];
};

struct sockaddr_in_k {
  uint16 sin_family;
  uint16 sin_port;   // network order
  uint32 sin_addr;   // network order
  char sin_zero[8];
};

struct sockaddr_un_k {
  uint16 sun_family;
  char sun_path[UN_PATHMAX];
};

struct iovec_k {
  uint64 iov_base;
  uint64 iov_len;
};

// must match struct msghdr in user/include/sys/socket.h exactly
struct msghdr_k {
  uint64 msg_name;
  uint msg_namelen;
  uint _pad0;
  uint64 msg_iov;
  int msg_iovlen;
  int _pad1;
  uint64 msg_control;
  uint msg_controllen;
  int msg_flags;
};

struct cmsghdr_k {
  uint64 cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

// what an ICMP error says, in the shape Linux puts in a cmsg at
// IPPROTO_IP/IP_RECVERR
struct sock_extended_err_k {
  uint32 ee_errno;
  uint8 ee_origin;
  uint8 ee_type;
  uint8 ee_code;
  uint8 ee_pad;
  uint32 ee_info;
  uint32 ee_data;
};

#define SO_EE_ORIGIN_ICMP 2

#define CMSG_ALIGN_K(n) (((n) + 7) & ~7UL)
#define CMSG_SPACE_K(n) (CMSG_ALIGN_K(sizeof(struct cmsghdr_k)) + CMSG_ALIGN_K(n))
#define CMSG_LEN_K(n)   (CMSG_ALIGN_K(sizeof(struct cmsghdr_k)) + (n))

// these have to be laid out exactly as the library lays them out,
// because a program hands over a pointer to its own and the kernel
// reads it as one of these. the sizes are the whole of the agreement,
// so they are checked at build time rather than discovered later.
_Static_assert(sizeof(struct sockaddr_in_k) == 16, "sockaddr_in changed shape");
_Static_assert(sizeof(struct sockaddr_un_k) == 110, "sockaddr_un changed shape");
_Static_assert(sizeof(struct msghdr_k) == 48, "msghdr changed shape");
_Static_assert(sizeof(struct cmsghdr_k) == 16, "cmsghdr changed shape");
_Static_assert(sizeof(struct iovec_k) == 16, "iovec changed shape");

#define MAXIOV 16
// one message is gathered into a page and scattered back out of one,
// so a page is what sendmsg will take and what recvmsg will hand back
#define SOCKBUF PGSIZE

// ---- descriptors ----

// the socket behind a descriptor. `want` is FD_SOCK or FD_UNIX, or 0
// for either.
static struct file *
sockfile(int fd, int want, int *err)
{
  struct file *f = fdget(myproc(), fd);

  if (f == 0) {
    *err = -EBADF;
    return 0;
  }
  if (f->type != FD_SOCK && f->type != FD_UNIX) {
    *err = -ENOTSOCK;
    return 0;
  }
  if (want && f->type != want) {
    *err = -EOPNOTSUPP;
    return 0;
  }
  *err = 0;
  return f;
}

// a descriptor onto a socket that has just been made. on failure the
// socket is closed, because nothing else holds it yet.
static int
newfd(int type, void *s, int flags)
{
  struct file *f = filealloc();
  int fd;

  if (f == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    if (type == FD_SOCK)
      sockclose(s);
    else
      unixsock_close(s);
    return -EMFILE;
  }
  f->type = type;
  if (type == FD_SOCK)
    f->sock = s;
  else
    f->un = s;
  f->readable = 1;
  f->writable = 1;
  f->flags = (flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
  if (flags & SOCK_CLOEXEC)
    fdflags_set(myproc(), fd, FD_CLOEXEC);
  return fd;
}

// give up a descriptor that was just made, when what came after it
// failed. fdput hands back what was there, which is the file, and it
// is this call's to close.
static void
fdclose(int fd)
{
  struct file *f = fdput(myproc(), fd, 0, 0);

  if (f)
    fileclose(f);
}

static int
nonblocking(struct file *f, int flags)
{
  return (flags & MSG_DONTWAIT) || (f->flags & O_NONBLOCK);
}

// ---- addresses ----

// read a sockaddr in, and say which family it is. `out` is a buffer
// big enough for any of them.
static int
addr_in(uint64 uaddr, uint alen, struct sockaddr_k *out)
{
  if (uaddr == 0 || alen < sizeof(uint16) || alen > sizeof(*out))
    return -EINVAL;
  memset(out, 0, sizeof(*out));
  if (copyin(myproc()->mm, (char *)out, uaddr, alen) < 0)
    return -EFAULT;
  return 0;
}

// a sockaddr_in as the kernel wants it: host-order address and port
static int
in_get(const struct sockaddr_k *sa, uint alen, uint32 *ip, int *port)
{
  const struct sockaddr_in_k *si = (const struct sockaddr_in_k *)sa;

  if (sa->sa_family != AF_INET)
    return -EAFNOSUPPORT;
  if (alen < sizeof(*si))
    return -EINVAL;
  *ip = ntohl(si->sin_addr);
  *port = ntohs(si->sin_port);
  return 0;
}

// a sockaddr_un as unixsock.c wants it: a NUL-terminated path, or an
// abstract name whose first byte is zero and whose length is exact.
// `path` must have room for UN_PATHMAX+1 bytes.
static int
un_get(const struct sockaddr_k *sa, uint alen, char *path, int *len)
{
  const struct sockaddr_un_k *su = (const struct sockaddr_un_k *)sa;
  uint n;

  if (sa->sa_family != AF_UNIX)
    return -EAFNOSUPPORT;
  if (alen <= sizeof(uint16))
    return -EINVAL;
  n = alen - sizeof(uint16);
  if (n > UN_PATHMAX)
    n = UN_PATHMAX;
  memset(path, 0, UN_PATHMAX + 1);
  memmove(path, su->sun_path, n);
  if (path[0] == 0) {
    // abstract: every byte given is part of the name
    *len = n;
  } else {
    *len = strlen(path);
  }
  if (*len <= 0)
    return -EINVAL;
  return 0;
}

// hand an address back. `ulen` points at how much room there is and
// is left saying how much there would have been, which is how a
// caller learns its buffer was too small without losing the answer.
static int
addr_out(uint64 uaddr, uint64 ulen, const void *sa, uint salen)
{
  struct proc *p = myproc();
  uint room;

  if (uaddr == 0 || ulen == 0)
    return 0;
  if (copyin(p->mm, (char *)&room, ulen, sizeof(room)) < 0)
    return -EFAULT;
  uint n = room < salen ? room : salen;
  if (n > 0 && copyout(p->mm, uaddr, (char *)sa, n) < 0)
    return -EFAULT;
  if (copyout(p->mm, ulen, (char *)&salen, sizeof(salen)) < 0)
    return -EFAULT;
  return 0;
}

static uint
in_put(struct sockaddr_in_k *si, uint32 ip, int port)
{
  memset(si, 0, sizeof(*si));
  si->sin_family = AF_INET;
  si->sin_port = htons((uint16)port);
  si->sin_addr = htonl(ip);
  return sizeof(*si);
}

static uint
un_put(struct sockaddr_un_k *su, const char *path, int len)
{
  memset(su, 0, sizeof(*su));
  su->sun_family = AF_UNIX;
  if (len > UN_PATHMAX)
    len = UN_PATHMAX;
  if (len > 0)
    memmove(su->sun_path, path, len);
  // a bound name is given back with its terminator, an abstract one
  // with exactly the bytes it is made of, and an unbound socket as
  // the family alone
  if (len == 0)
    return sizeof(uint16);
  if (su->sun_path[0] == 0)
    return sizeof(uint16) + len;
  return sizeof(uint16) + len + 1;
}

// ---- socket, socketpair ----

// which kind of socket on the network a (type, protocol) pair asks
// for. ICMP is a socket of its own here: Linux spells it
// SOCK_DGRAM/IPPROTO_ICMP (an unprivileged ping socket) or
// SOCK_RAW/IPPROTO_ICMP, and both mean the same thing.
static int
ip_kind(int type, int protocol)
{
  if (protocol == IPPROTO_ICMP) {
    if (type == SOCK_DGRAM || type == SOCK_RAW)
      return SOCK_ICMP;
    return -EPROTONOSUPPORT;
  }
  if (type == SOCK_STREAM) {
    if (protocol != 0 && protocol != IPPROTO_TCP)
      return -EPROTONOSUPPORT;
    return SOCK_STREAM;
  }
  if (type == SOCK_DGRAM) {
    if (protocol != 0 && protocol != IPPROTO_UDP)
      return -EPROTONOSUPPORT;
    return SOCK_DGRAM;
  }
  if (type == SOCK_RAW)
    return -EPROTONOSUPPORT;
  return -ESOCKTNOSUPPORT;
}

uint64
sys_socket(void)
{
  int domain, type, protocol;

  argint(0, &domain);
  argint(1, &type);
  argint(2, &protocol);

  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

  if (domain == AF_UNIX) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
      return -ESOCKTNOSUPPORT;
    if (protocol != 0)
      return -EPROTONOSUPPORT;
    // SEQPACKET keeps message boundaries the way a datagram does and
    // is connected the way a stream is; nothing here needs the
    // difference, so it is a datagram socket
    struct unsock *s = unixsock_alloc(type == SOCK_STREAM ? SOCK_STREAM
                                                          : SOCK_DGRAM);
    if (s == 0)
      return -ENFILE;
    return newfd(FD_UNIX, s, flags);
  }
  if (domain != AF_INET)
    return -EAFNOSUPPORT;

  int kind = ip_kind(type, protocol);
  if (kind < 0)
    return kind;
  if (!virtio_net_present())
    return -EAFNOSUPPORT;   // there is no network on this machine
  struct sock *s = ipsock_new(kind);
  if (s == 0)
    return -ENFILE;         // and here there is, but the table is full
  return newfd(FD_SOCK, s, flags);
}

uint64
sys_socketpair(void)
{
  int domain, type, protocol;
  uint64 usv;
  struct unsock *a, *b;
  int fds[2], r;

  argint(0, &domain);
  argint(1, &type);
  argint(2, &protocol);
  argaddr(3, &usv);

  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

  // only AF_UNIX: a pair on the network would be two connections, not
  // one, and Linux does not offer it either
  if (domain != AF_UNIX)
    return -EOPNOTSUPP;
  if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
    return -ESOCKTNOSUPPORT;
  if (protocol != 0)
    return -EPROTONOSUPPORT;

  if ((r = unixsock_pair(type == SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, &a,
                         &b)) < 0)
    return r;
  if ((fds[0] = newfd(FD_UNIX, a, flags)) < 0) {
    unixsock_close(b);
    return fds[0];
  }
  if ((fds[1] = newfd(FD_UNIX, b, flags)) < 0) {
    fdclose(fds[0]);
    return fds[1];
  }
  if (copyout(myproc()->mm, usv, (char *)fds, sizeof(fds)) < 0) {
    fdclose(fds[0]);
    fdclose(fds[1]);
    return -EFAULT;
  }
  return 0;
}

// ---- bind, connect, listen, accept ----

uint64
sys_bind(void)
{
  int fd, err;
  uint64 uaddr;
  int alen;
  struct sockaddr_k sa;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &uaddr);
  argint(2, &alen);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if (alen < 0 || (err = addr_in(uaddr, alen, &sa)) < 0)
    return err;

  if (f->type == FD_UNIX) {
    char path[UN_PATHMAX + 1];
    int len;
    if ((err = un_get(&sa, alen, path, &len)) < 0)
      return err;
    return unixsock_bind(f->un, path, len);
  }
  uint32 ip;
  int port;
  if ((err = in_get(&sa, alen, &ip, &port)) < 0)
    return err;
  // the address in the sockaddr is read and then not used: there is
  // one interface here, so binding to it and binding to every
  // interface are the same thing, and a socket is named by its port
  (void)ip;
  return ipsock_bind(f->sock, port);
}

uint64
sys_connect(void)
{
  int fd, err;
  uint64 uaddr;
  int alen;
  struct sockaddr_k sa;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &uaddr);
  argint(2, &alen);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if (alen < 0 || (err = addr_in(uaddr, alen, &sa)) < 0)
    return err;

  if (f->type == FD_UNIX) {
    char path[UN_PATHMAX + 1];
    int len;
    if ((err = un_get(&sa, alen, path, &len)) < 0)
      return err;
    return unixsock_connect(f->un, path, len);
  }
  uint32 ip;
  int port;
  if ((err = in_get(&sa, alen, &ip, &port)) < 0)
    return err;
  return ipsock_connect(f->sock, ip, port);
}

uint64
sys_listen(void)
{
  int fd, backlog, err;
  struct file *f;

  argint(0, &fd);
  argint(1, &backlog);
  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if (f->type == FD_UNIX)
    return unixsock_listen(f->un, backlog);
  return ipsock_listen(f->sock, backlog);
}

// accept4(fd, addr, &alen, flags); accept() is this with flags 0
uint64
sys_accept(void)
{
  int fd, flags, err;
  uint64 uaddr, ulen;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &uaddr);
  argaddr(2, &ulen);
  argint(3, &flags);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;

  if (f->type == FD_UNIX) {
    struct unsock *ns = unixsock_accept(f->un, &err);
    if (ns == 0)
      return err;
    char path[UN_PATHMAX];
    int len = unixsock_name(ns, 1, path, sizeof(path));
    struct sockaddr_un_k su;
    uint salen = un_put(&su, path, len);
    int nfd = newfd(FD_UNIX, ns, flags);
    if (nfd < 0)
      return nfd;
    if ((err = addr_out(uaddr, ulen, &su, salen)) < 0) {
      fdclose(nfd);
      return err;
    }
    return nfd;
  }

  struct sock *ns = ipsock_accept(f->sock, &err);
  if (ns == 0)
    return err;
  struct sockaddr_in_k si;
  uint salen = in_put(&si, ns->rip, ns->rport);
  int nfd = newfd(FD_SOCK, ns, flags);
  if (nfd < 0)
    return nfd;
  if ((err = addr_out(uaddr, ulen, &si, salen)) < 0) {
    fdclose(nfd);
    return err;
  }
  return nfd;
}

// ---- sending and receiving ----

// one send, with the destination already read in. `sa` is null for a
// socket that is connected, which is what send() and write() are.
// `user` says whether `buf` is an address in the calling process or
// one in the kernel: sendmsg gathers its iovecs into a page of its
// own first, and that page is not the caller's.
static int
do_send(struct file *f, int user, uint64 buf, int n, int flags,
        const struct sockaddr_k *sa, uint alen, struct file **fds, int nfds)
{
  int err;

  if (f->type == FD_UNIX) {
    if (sa) {
      char path[UN_PATHMAX + 1];
      int len;
      if ((err = un_get(sa, alen, path, &len)) < 0) {
        for (int i = 0; i < nfds; i++)
          fileclose(fds[i]);
        return err;
      }
      return unixsock_send(f->un, user, buf, n, path, len, fds, nfds);
    }
    return unixsock_send(f->un, user, buf, n, 0, 0, fds, nfds);
  }

  for (int i = 0; i < nfds; i++)
    fileclose(fds[i]);   // a descriptor does not travel over the network
  if (nfds > 0)
    return -EOPNOTSUPP;

  struct sock *s = f->sock;
  uint32 ip;
  int port;
  if (sa) {
    if ((err = in_get(sa, alen, &ip, &port)) < 0)
      return err;
  } else {
    if (s->rport == 0)
      return -EDESTADDRREQ;
    ip = s->rip;
    port = s->rport;
  }
  if (s->type == SOCK_STREAM) {
    if (sa)
      return -EISCONN;
    return sockwrite(s, user, buf, n);
  }
  (void)flags;
  return ipsock_sendto(s, user, buf, n, ip, port);
}

// one receive. `sa` is filled with where it came from, `salen` with
// how long that is, and `icmp` with the ICMP type when what arrived
// was an error rather than an answer.
static int
do_recv(struct file *f, int user, uint64 buf, int n, int flags,
        struct sockaddr_k *sa, uint *salen, int *icmp, struct file **fds,
        int *nfds, struct ucred *cred)
{
  int err;

  *salen = 0;
  *icmp = 0;
  if (nfds)
    *nfds = 0;

  if (f->type == FD_UNIX) {
    char path[UN_PATHMAX];
    int len = 0;
    int r = unixsock_recv(f->un, user, buf, n,
                          sa ? path : 0, sa ? &len : 0, fds, nfds, cred,
                          nonblocking(f, flags) ? (flags | MSG_DONTWAIT)
                                                : flags);
    if (r >= 0 && sa)
      *salen = un_put((struct sockaddr_un_k *)sa, path, len);
    return r;
  }

  struct sock *s = f->sock;
  if (s->type == SOCK_STREAM) {
    if (nonblocking(f, flags) && sockready(s, 0) == 0)
      return -EAGAIN;
    int r = sockread(s, user, buf, n);
    if (r >= 0 && sa)
      *salen = in_put((struct sockaddr_in_k *)sa, s->rip, s->rport);
    return r < 0 ? -ECONNRESET : r;
  }

  uint32 ip = 0;
  int port = 0;
  int timeout;
  acquire(&s->lock);
  timeout = s->rcvtimeo;
  release(&s->lock);
  err = ipsock_recvfrom(s, user, buf, n, &ip, &port, timeout,
                        nonblocking(f, flags));
  if (err < 0)
    return err;
  *icmp = ICMP_TYPE(port);
  if (sa)
    *salen = in_put((struct sockaddr_in_k *)sa, ip, ICMP_SEQ(port));
  return err;
}

uint64
sys_sendto(void)
{
  int fd, n, flags, alen, err;
  uint64 buf, uaddr;
  struct sockaddr_k sa;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &buf);
  argint(2, &n);
  argint(3, &flags);
  argaddr(4, &uaddr);
  argint(5, &alen);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if (n < 0)
    return -EINVAL;
  if (uaddr && alen > 0) {
    if ((err = addr_in(uaddr, alen, &sa)) < 0)
      return err;
    return do_send(f, 1, buf, n, flags, &sa, alen, 0, 0);
  }
  return do_send(f, 1, buf, n, flags, 0, 0, 0, 0);
}

uint64
sys_recvfrom(void)
{
  int fd, n, flags, err, icmp;
  uint64 buf, uaddr, ulen;
  struct sockaddr_k sa;
  uint salen;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &buf);
  argint(2, &n);
  argint(3, &flags);
  argaddr(4, &uaddr);
  argaddr(5, &ulen);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if (n < 0)
    return -EINVAL;
  int r = do_recv(f, 1, buf, n, flags, uaddr ? &sa : 0, &salen, &icmp, 0, 0,
                  0);
  if (r < 0)
    return r;
  if (uaddr && (err = addr_out(uaddr, ulen, &sa, salen)) < 0)
    return err;
  return r;
}

// ---- sendmsg and recvmsg ----

// the iovecs, gathered into one buffer. these sockets deal in whole
// messages, so a scattered message is put together before it is sent
// and taken apart after it is received; the alternative is a copy
// routine that can stop half way through an iovec, which buys nothing
// at these sizes.
//
// how many bytes the iovecs cover, at most one page. a send that asks
// for more than that is refused; a receive simply takes what fits,
// which is what a receive does anyway.
static int
iov_len(struct iovec_k *iov, int n)
{
  int total = 0;

  for (int i = 0; i < n; i++) {
    if (iov[i].iov_len > SOCKBUF)
      return SOCKBUF + 1;
    total += iov[i].iov_len;
    if (total > SOCKBUF)
      return SOCKBUF + 1;
  }
  return total;
}

static int
iov_read(struct iovec_k *iov, int n, char *dst)
{
  struct proc *p = myproc();
  int off = 0;

  for (int i = 0; i < n; i++) {
    if (iov[i].iov_len == 0)
      continue;
    if (copyin(p->mm, dst + off, iov[i].iov_base, iov[i].iov_len) < 0)
      return -EFAULT;
    off += iov[i].iov_len;
  }
  return off;
}

static int
iov_write(struct iovec_k *iov, int n, const char *src, int len)
{
  struct proc *p = myproc();
  int off = 0;

  for (int i = 0; i < n && off < len; i++) {
    int take = len - off;
    if (take > (int)iov[i].iov_len)
      take = iov[i].iov_len;
    if (take > 0 && copyout(p->mm, iov[i].iov_base, (char *)src + off, take) < 0)
      return -EFAULT;
    off += take;
  }
  return off;
}

static int
msg_get(uint64 umsg, struct msghdr_k *mh, struct iovec_k *iov)
{
  struct proc *p = myproc();

  if (copyin(p->mm, (char *)mh, umsg, sizeof(*mh)) < 0)
    return -EFAULT;
  if (mh->msg_iovlen < 0 || mh->msg_iovlen > MAXIOV)
    return -EMSGSIZE;
  if (mh->msg_iovlen > 0 &&
      copyin(p->mm, (char *)iov, mh->msg_iov,
             mh->msg_iovlen * sizeof(struct iovec_k)) < 0)
    return -EFAULT;
  return 0;
}

// the descriptors an SCM_RIGHTS control message names, dup'd so that
// they survive the sender closing them
static int
scm_collect(struct msghdr_k *mh, struct file **fds, int *nfds)
{
  struct proc *p = myproc();
  char ctl[256];
  uint len = mh->msg_controllen;

  *nfds = 0;
  if (mh->msg_control == 0 || len == 0)
    return 0;
  if (len > sizeof(ctl))
    return -EINVAL;
  if (copyin(p->mm, ctl, mh->msg_control, len) < 0)
    return -EFAULT;

  uint off = 0;
  while (off + sizeof(struct cmsghdr_k) <= len) {
    struct cmsghdr_k *c = (struct cmsghdr_k *)(ctl + off);
    if (c->cmsg_len < sizeof(*c) || off + c->cmsg_len > len)
      return -EINVAL;
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int n = (c->cmsg_len - CMSG_ALIGN_K(sizeof(*c))) / sizeof(int);
      int *v = (int *)(ctl + off + CMSG_ALIGN_K(sizeof(*c)));
      if (n < 0 || *nfds + n > UN_MAXFDS) {
        for (int i = 0; i < *nfds; i++)
          fileclose(fds[i]);
        *nfds = 0;
        return -EINVAL;
      }
      for (int i = 0; i < n; i++) {
        struct file *f = fdget(p, v[i]);
        if (f == 0) {
          for (int j = 0; j < *nfds; j++)
            fileclose(fds[j]);
          *nfds = 0;
          return -EBADF;
        }
        fds[(*nfds)++] = filedup(f);
      }
    }
    // SCM_CREDENTIALS on the way out is ignored: what the receiver is
    // told is what the kernel saw, not what the sender claimed
    off += CMSG_ALIGN_K(c->cmsg_len);
    if (c->cmsg_len == 0)
      break;
  }
  return 0;
}

uint64
sys_sendmsg(void)
{
  int fd, flags, err;
  uint64 umsg;
  struct msghdr_k mh;
  struct iovec_k iov[MAXIOV];
  struct file *fds[UN_MAXFDS];
  int nfds = 0;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &umsg);
  argint(2, &flags);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if ((err = msg_get(umsg, &mh, iov)) < 0)
    return err;

  int total = iov_len(iov, mh.msg_iovlen);
  if (total > SOCKBUF)
    return -EMSGSIZE;

  if ((err = scm_collect(&mh, fds, &nfds)) < 0)
    return err;
  if (nfds > 0 && f->type != FD_UNIX) {
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return -EOPNOTSUPP;
  }

  char *buf = kalloc();
  if (buf == 0) {
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return -ENOMEM;
  }
  if ((err = iov_read(iov, mh.msg_iovlen, buf)) < 0) {
    kfree(buf);
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);
    return err;
  }

  struct sockaddr_k sa;
  int r;
  if (mh.msg_name && mh.msg_namelen > 0) {
    if ((err = addr_in(mh.msg_name, mh.msg_namelen, &sa)) < 0) {
      kfree(buf);
      for (int i = 0; i < nfds; i++)
        fileclose(fds[i]);
      return err;
    }
    r = do_send(f, 0, (uint64)buf, total, flags, &sa, mh.msg_namelen, fds,
                nfds);
  } else {
    r = do_send(f, 0, (uint64)buf, total, flags, 0, 0, fds, nfds);
  }
  kfree(buf);
  return r;
}

// the control messages a receive hands back, built into `ctl` and
// copied out as one
struct ctlout {
  char buf[256];
  uint len;
  int truncated;
};

static void
ctl_put(struct ctlout *co, uint cap, int level, int type, const void *data,
        uint n)
{
  uint need = CMSG_SPACE_K(n);

  if (co->len + need > cap || co->len + need > sizeof(co->buf)) {
    co->truncated = 1;
    return;
  }
  struct cmsghdr_k *c = (struct cmsghdr_k *)(co->buf + co->len);
  c->cmsg_len = CMSG_LEN_K(n);
  c->cmsg_level = level;
  c->cmsg_type = type;
  memmove(co->buf + co->len + CMSG_ALIGN_K(sizeof(*c)), data, n);
  co->len += need;
}

uint64
sys_recvmsg(void)
{
  int fd, flags, err, icmp = 0;
  uint64 umsg;
  struct msghdr_k mh;
  struct iovec_k iov[MAXIOV];
  struct file *fds[UN_MAXFDS];
  int nfds = 0;
  struct ucred cred;
  struct sockaddr_k sa;
  uint salen = 0;
  struct file *f;
  struct proc *p = myproc();

  argint(0, &fd);
  argaddr(1, &umsg);
  argint(2, &flags);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if ((err = msg_get(umsg, &mh, iov)) < 0)
    return err;

  int room = iov_len(iov, mh.msg_iovlen);
  if (room > SOCKBUF)
    room = SOCKBUF;

  char *buf = kalloc();
  if (buf == 0)
    return -ENOMEM;
  memset(&cred, 0, sizeof(cred));
  int r = do_recv(f, 0, (uint64)buf, room, flags, &sa, &salen, &icmp, fds,
                  &nfds, &cred);
  if (r < 0) {
    kfree(buf);
    return r;
  }
  int wrote = iov_write(iov, mh.msg_iovlen, buf, r);
  kfree(buf);
  if (wrote < 0) {
    for (int i = 0; i < nfds; i++)
      fileclose(fds[i]);   // nothing was delivered, so neither are these
    return wrote;
  }

  int outflags = 0;
  if (wrote < r)
    outflags |= MSG_TRUNC;

  // the descriptors that came with it become descriptors here -- but
  // only as many as the caller left room to be told about. one it is
  // not told about is one it can never close, so the rest are closed
  // here and MSG_CTRUNC says so.
  struct ctlout co;
  uint cap = mh.msg_control ? mh.msg_controllen : 0;
  co.len = 0;
  co.truncated = 0;
  if (nfds > 0) {
    int fit = 0;
    if (cap >= CMSG_SPACE_K(sizeof(int)))
      fit = (cap - CMSG_ALIGN_K(sizeof(struct cmsghdr_k))) / sizeof(int);
    if (fit > nfds)
      fit = nfds;
    int nums[UN_MAXFDS];
    int got = 0;
    for (int i = 0; i < nfds; i++) {
      int n = i < fit ? fdalloc(fds[i]) : -1;
      if (n < 0) {
        fileclose(fds[i]);
        continue;
      }
      nums[got++] = n;
    }
    if (got < nfds)
      outflags |= MSG_CTRUNC;
    if (got > 0)
      ctl_put(&co, cap, SOL_SOCKET, SCM_RIGHTS, nums, got * sizeof(int));
  }
  if (f->type == FD_UNIX && unixsock_passcred_on(f->un) && cred.pid)
    ctl_put(&co, cap, SOL_SOCKET, SCM_CREDENTIALS, &cred, sizeof(cred));
  if (icmp) {
    struct sock_extended_err_k ee;
    memset(&ee, 0, sizeof(ee));
    ee.ee_errno = EHOSTUNREACH;
    ee.ee_origin = SO_EE_ORIGIN_ICMP;
    ee.ee_type = (uint8)icmp;
    ctl_put(&co, cap, IPPROTO_IP, IP_RECVERR, &ee, sizeof(ee));
  }
  if (co.truncated)
    outflags |= MSG_CTRUNC;
  if (co.len > 0 && mh.msg_control &&
      copyout(p->mm, mh.msg_control, co.buf, co.len) < 0)
    return -EFAULT;

  // the header is handed back with what was filled in
  mh.msg_controllen = co.len;
  mh.msg_namelen = salen;
  mh.msg_flags = outflags;
  if (salen > 0 && mh.msg_name &&
      copyout(p->mm, mh.msg_name, (char *)&sa, salen) < 0)
    return -EFAULT;
  if (copyout(p->mm, umsg, (char *)&mh, sizeof(mh)) < 0)
    return -EFAULT;
  return wrote;
}

// ---- names ----

static uint64
getname(int peer)
{
  int fd, err;
  uint64 uaddr, ulen;
  struct file *f;

  argint(0, &fd);
  argaddr(1, &uaddr);
  argaddr(2, &ulen);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;

  if (f->type == FD_UNIX) {
    char path[UN_PATHMAX];
    int len = unixsock_name(f->un, peer, path, sizeof(path));
    struct sockaddr_un_k su;
    uint salen = un_put(&su, path, len);
    return addr_out(uaddr, ulen, &su, salen);
  }
  struct sock *s = f->sock;
  struct sockaddr_in_k si;
  uint salen;
  if (peer) {
    if (s->rport == 0)
      return -ENOTCONN;
    salen = in_put(&si, s->rip, s->rport);
  } else {
    salen = in_put(&si, s->lip, s->lport);
  }
  return addr_out(uaddr, ulen, &si, salen);
}

uint64
sys_getsockname(void)
{
  return getname(0);
}

uint64
sys_getpeername(void)
{
  return getname(1);
}

uint64
sys_shutdown(void)
{
  int fd, how, err;
  struct file *f;

  argint(0, &fd);
  argint(1, &how);
  if (how < SHUT_RD || how > SHUT_RDWR)
    return -EINVAL;
  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;
  if (f->type == FD_UNIX)
    return unixsock_shutdown(f->un, how);
  return sockshutdown(f->sock, how);
}

// ---- options ----

// a timeval, for SO_RCVTIMEO
struct timeval_k {
  int64 tv_sec;
  int64 tv_usec;
};

uint64
sys_setsockopt(void)
{
  int fd, level, opt, len, err;
  uint64 uval;
  struct file *f;
  struct proc *p = myproc();

  argint(0, &fd);
  argint(1, &level);
  argint(2, &opt);
  argaddr(3, &uval);
  argint(4, &len);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;

  if (level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO)) {
    struct timeval_k tv;
    if (len < (int)sizeof(tv) || copyin(p->mm, (char *)&tv, uval, sizeof(tv)) < 0)
      return -EINVAL;
    // one tick is a tenth of a second here; a timeout that is not
    // zero and rounds to nothing waits one tick rather than for ever
    int64 ticks10 = tv.tv_sec * tick_hz + tv.tv_usec * tick_hz / 1000000;
    if (ticks10 < 0 || ticks10 > 0x7fffffff)
      return -EINVAL;
    if (ticks10 == 0 && (tv.tv_sec || tv.tv_usec))
      ticks10 = 1;
    if (f->type == FD_UNIX)
      return 0;   // a local socket has no timeout to set
    return ipsock_setopt(f->sock,
                         opt == SO_RCVTIMEO ? SO_RCVTIMEO_TICKS
                                            : SO_SNDTIMEO_TICKS,
                         (int)ticks10);
  }

  int value = 0;
  if (len >= (int)sizeof(int)) {
    if (copyin(p->mm, (char *)&value, uval, sizeof(int)) < 0)
      return -EFAULT;
  } else if (len > 0) {
    return -EINVAL;
  }

  if (f->type == FD_UNIX) {
    if (level == SOL_SOCKET && opt == SO_PASSCRED) {
      unixsock_passcred(f->un, value);
      return 0;
    }
    if (level == SOL_SOCKET && (opt == SO_SNDBUF || opt == SO_RCVBUF ||
                                opt == SO_REUSEADDR))
      return 0;   // accepted and ignored: the sizes here are fixed
    return -ENOPROTOOPT;
  }

  if (level == IPPROTO_IP && opt == IP_TTL)
    return ipsock_setopt(f->sock, SO_TTL, value);
  if (level == IPPROTO_IP && opt == IP_RECVERR)
    return ipsock_setopt(f->sock, SO_ICMP_ERRORS, value);
  if (level == SOL_SOCKET &&
      (opt == SO_REUSEADDR || opt == SO_SNDBUF || opt == SO_RCVBUF ||
       opt == SO_KEEPALIVE || opt == SO_BROADCAST))
    return 0;
  return -ENOPROTOOPT;
}

uint64
sys_getsockopt(void)
{
  int fd, level, opt, err;
  uint64 uval, ulen;
  struct file *f;
  int value = 0;

  argint(0, &fd);
  argint(1, &level);
  argint(2, &opt);
  argaddr(3, &uval);
  argaddr(4, &ulen);

  if ((f = sockfile(fd, 0, &err)) == 0)
    return err;

  if (level == SOL_SOCKET && opt == SO_PEERCRED) {
    struct ucred cred;
    if (f->type != FD_UNIX)
      return -ENOPROTOOPT;
    memset(&cred, 0, sizeof(cred));
    if ((err = unixsock_getcred(f->un, &cred)) < 0)
      return err;
    return addr_out(uval, ulen, &cred, sizeof(cred));
  }

  if (level == SOL_SOCKET && opt == SO_TYPE) {
    value = f->type == FD_UNIX ? unixsock_type(f->un) : f->sock->type;
    if (value == SOCK_ICMP)
      value = SOCK_DGRAM;
  } else if (level == SOL_SOCKET && opt == SO_ERROR) {
    value = 0;   // errors here are reported by the call that hit them
  } else if (f->type == FD_UNIX) {
    return -ENOPROTOOPT;
  } else if (level == IPPROTO_IP && opt == IP_TTL) {
    if ((err = ipsock_getopt(f->sock, SO_TTL, &value)) < 0)
      return err;
  } else if (level == IPPROTO_IP && opt == IP_RECVERR) {
    if ((err = ipsock_getopt(f->sock, SO_ICMP_ERRORS, &value)) < 0)
      return err;
  } else {
    return -ENOPROTOOPT;
  }
  return addr_out(uval, ulen, &value, sizeof(value));
}
