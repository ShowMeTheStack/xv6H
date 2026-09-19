//
// sockets: UDP, ICMP echo ("ping" sockets) and TCP streams as file
// descriptors, plus the socket system calls.
//
//   socket(type)                       SOCK_DGRAM, SOCK_STREAM, SOCK_ICMP
//   bind(fd, port)                     local port (0: pick one)
//   connect(fd, ip, port)              TCP: three-way handshake; UDP: default peer
//   listen(fd, backlog) / accept(fd, &ip, &port)
//   sendto(fd, buf, n, ip, port)       UDP/ICMP (ICMP: port is the sequence number)
//   recvfrom(fd, buf, n, &ip, &port, timeout_ticks)
//   read/write/close                   TCP streams (and UDP with a default peer)
//   netinfo(&info)
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "file.h"
#include "net.h"
#include "sock.h"
#include "tcp.h"

struct sock socks[NSOCK];
static struct spinlock socklock;
static uint16 next_port = 49152;

// allocate a file descriptor for f in the current process.
static int
sock_fdalloc(struct file *f)
{
  struct proc *p = myproc();
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

void
sockinit(void)
{
  initlock(&socklock, "socks");
  for (int i = 0; i < NSOCK; i++)
    initlock(&socks[i].lock, "sock");
}

static struct sock *
sockalloc(int type)
{
  acquire(&socklock);
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type == SOCK_NONE) {
      s->type = type;
      s->lip = s->rip = 0;
      s->lport = s->rport = 0;
      s->rxq = 0;
      s->rxcount = 0;
      s->tcp = 0;
      s->closed = 0;
      s->id = i;
      release(&socklock);
      return s;
    }
  }
  release(&socklock);
  return 0;
}

static uint16
port_alloc(void)
{
  // ephemeral ports 49152..65535, skipping ones in use.
  for (int tries = 0; tries < 16384; tries++) {
    uint16 p = next_port++;
    if (next_port == 0)
      next_port = 49152;
    int used = 0;
    for (int i = 0; i < NSOCK; i++)
      if (socks[i].type != SOCK_NONE && socks[i].lport == p)
        used = 1;
    if (!used)
      return p;
  }
  return 0;
}

void
sockclose(struct sock *s)
{
  acquire(&s->lock);
  s->closed = 1;
  if (s->type == SOCK_STREAM && s->tcp) {
    release(&s->lock);
    tcp_close(s);
    acquire(&s->lock);
  }
  while (s->rxq) {
    struct mbuf *m = s->rxq;
    s->rxq = m->next;
    mbuffree(m);
  }
  s->rxcount = 0;
  s->type = SOCK_NONE;
  release(&s->lock);
}

// queue a received datagram on a socket; caller holds netlock.
static int
sock_enqueue(struct sock *s, struct mbuf *m)
{
  acquire(&s->lock);
  if (s->rxcount >= SOCK_RXQ) {
    release(&s->lock);
    return -1;
  }
  m->next = 0;
  if (s->rxq == 0) {
    s->rxq = m;
  } else {
    struct mbuf *t = s->rxq;
    while (t->next)
      t = t->next;
    t->next = m;
  }
  s->rxcount++;
  release(&s->lock);
  wakeup(s);
  return 0;
}

int
sock_udp_rx(struct mbuf *m, uint32 src, uint16 sport, uint16 dport)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type == SOCK_DGRAM && s->lport == dport && (s->rport == 0 || (s->rip == src && s->rport == sport))) {
      m->arg = src;
      m->arg16 = sport;
      return sock_enqueue(s, m);
    }
  }
  return -1;
}

int
sock_icmp_rx(struct mbuf *m, uint32 src, uint16 id)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type == SOCK_ICMP && s->lport == id) {
      m->arg = src;
      m->arg16 = ntohs(((struct icmp *)m->head)->seq);
      mbufpull(m, sizeof(struct icmp));
      return sock_enqueue(s, m);
    }
  }
  return -1;
}

// wait for the interface to be configured (at most timeout ticks).
static int
wait_up(int timeout)
{
  uint t0 = ticks;
  while (!netcfg.up) {
    if (timeout && ticks - t0 >= timeout)
      return -1;
    if (killed(myproc()))
      return -1;
    sleep_prepare(&netcfg);
    if (!netcfg.up)
      sleep();
  }
  return 0;
}

// ---- system calls ----

static struct sock *
fdsock(int fd, struct file **fp)
{
  struct file *f;
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0 || f->type != FD_SOCK)
    return 0;
  if (fp)
    *fp = f;
  return f->sock;
}

uint64
sys_socket(void)
{
  int type;
  argint(0, &type);
  if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_ICMP)
    return -1;
  if (!virtio_net_present())
    return -1;
  struct sock *s = sockalloc(type);
  if (s == 0)
    return -1;
  struct file *f = filealloc();
  int fd;
  if (f == 0 || (fd = sock_fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    sockclose(s);
    return -1;
  }
  f->type = FD_SOCK;
  f->sock = s;
  f->readable = 1;
  f->writable = 1;
  if (type == SOCK_ICMP)
    s->lport = 0x5800 | s->id; // echo identifier
  return fd;
}

uint64
sys_bind(void)
{
  int fd, port;
  argint(0, &fd);
  argint(1, &port);
  struct sock *s = fdsock(fd, 0);
  if (s == 0 || port < 0 || port > 65535)
    return -1;
  acquire(&socklock);
  if (port == 0)
    port = port_alloc();
  for (int i = 0; i < NSOCK; i++)
    if (&socks[i] != s && socks[i].type == s->type && socks[i].lport == port) {
      release(&socklock);
      return -1;
    }
  s->lport = port;
  release(&socklock);
  return 0;
}

uint64
sys_connect(void)
{
  int fd, port;
  uint64 ip;
  argint(0, &fd);
  argaddr(1, &ip);
  argint(2, &port);
  struct sock *s = fdsock(fd, 0);
  if (s == 0 || port <= 0 || port > 65535)
    return -1;
  if (wait_up(300) < 0)
    return -1;
  if (s->lport == 0) {
    acquire(&socklock);
    s->lport = port_alloc();
    release(&socklock);
  }
  s->rip = (uint32)ip;
  s->rport = port;
  if (s->type == SOCK_STREAM)
    return tcp_connect(s);
  return 0;
}

uint64
sys_listen(void)
{
  int fd, backlog;
  argint(0, &fd);
  argint(1, &backlog);
  struct sock *s = fdsock(fd, 0);
  if (s == 0 || s->type != SOCK_STREAM || s->lport == 0)
    return -1;
  return tcp_listen(s, backlog);
}

uint64
sys_accept(void)
{
  int fd;
  uint64 ipp, portp;
  argint(0, &fd);
  argaddr(1, &ipp);
  argaddr(2, &portp);
  struct sock *s = fdsock(fd, 0);
  if (s == 0 || s->type != SOCK_STREAM)
    return -1;
  struct sock *ns = tcp_accept(s);
  if (ns == 0)
    return -1;
  struct file *f = filealloc();
  int nfd;
  if (f == 0 || (nfd = sock_fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    sockclose(ns);
    return -1;
  }
  f->type = FD_SOCK;
  f->sock = ns;
  f->readable = 1;
  f->writable = 1;
  if (ipp && either_copyout(1, ipp, &ns->rip, 4) < 0)
    return -1;
  int rp = ns->rport;
  if (portp && either_copyout(1, portp, &rp, 4) < 0)
    return -1;
  return nfd;
}

uint64
sys_sendto(void)
{
  int fd, n, port;
  uint64 buf, ip;
  argint(0, &fd);
  argaddr(1, &buf);
  argint(2, &n);
  argaddr(3, &ip);
  argint(4, &port);
  struct sock *s = fdsock(fd, 0);
  if (s == 0 || n < 0 || n > 1400)
    return -1;
  uint32 dst = (uint32)ip;
  if (s->type == SOCK_ICMP) {
    char data[64];
    if (n > sizeof(data))
      n = sizeof(data);
    if (either_copyin(data, 1, buf, n) < 0)
      return -1;
    if (wait_up(300) < 0)
      return -1;
    if (!net_policy_ok(dst, IPPROTO_ICMP, 0, 0)) {
      net_policy_dropped++;
      return -2;
    }
    return net_tx_icmp_echo(dst, s->lport, port, data, n) < 0 ? -1 : n;
  }
  if (s->type != SOCK_DGRAM)
    return -1;
  if (wait_up(300) < 0)
    return -1;
  if (s->lport == 0) {
    acquire(&socklock);
    s->lport = port_alloc();
    release(&socklock);
  }
  if (!net_policy_ok(dst, IPPROTO_UDP, port, 0)) {
    net_policy_dropped++;
    return -2;
  }
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return -1;
  char *p = mbufput(m, n);
  if (either_copyin(p, 1, buf, n) < 0) {
    mbuffree(m);
    return -1;
  }
  if (net_tx_udp(m, dst, s->lport, port) < 0)
    return -1;
  return n;
}

// recvfrom(fd, buf, n, &ip, &port, timeout): timeout in ticks, 0 = forever.
uint64
sys_recvfrom(void)
{
  int fd, n, timeout;
  uint64 buf, ipp, portp;
  argint(0, &fd);
  argaddr(1, &buf);
  argint(2, &n);
  argaddr(3, &ipp);
  argaddr(4, &portp);
  argint(5, &timeout);
  struct sock *s = fdsock(fd, 0);
  if (s == 0 || n < 0 || (s->type != SOCK_DGRAM && s->type != SOCK_ICMP))
    return -1;

  uint t0 = ticks;
  acquire(&s->lock);
  while (s->rxq == 0) {
    if (killed(myproc()) || (timeout && ticks - t0 >= (uint)timeout)) {
      release(&s->lock);
      return -1;
    }
    sleep_prepare(s);
    release(&s->lock);
    sleep();
    acquire(&s->lock);
  }
  struct mbuf *m = s->rxq;
  s->rxq = m->next;
  s->rxcount--;
  release(&s->lock);

  int len = m->len < n ? m->len : n;
  if (either_copyout(1, buf, m->head, len) < 0)
    len = -1;
  uint32 src = m->arg;
  int sport = m->arg16;
  mbuffree(m);
  if (len < 0)
    return -1;
  if (ipp && either_copyout(1, ipp, &src, 4) < 0)
    return -1;
  if (portp && either_copyout(1, portp, &sport, 4) < 0)
    return -1;
  return len;
}

uint64
sys_netinfo(void)
{
  uint64 addr;
  struct netinfo ni;
  argaddr(0, &addr);
  memset(&ni, 0, sizeof(ni));
  net_stats(&ni);
  if (either_copyout(1, addr, &ni, sizeof(ni)) < 0)
    return -1;
  return virtio_net_present() ? 0 : -1;
}

// file-level read/write on sockets (file.c calls these).
int
sockread(struct sock *s, int user, uint64 addr, int n)
{
  if (s->type == SOCK_STREAM)
    return tcp_read(s, user, addr, n);
  // datagram: next message, source discarded
  acquire(&s->lock);
  while (s->rxq == 0) {
    if (killed(myproc()) || s->closed) {
      release(&s->lock);
      return -1;
    }
    sleep_prepare(s);
    release(&s->lock);
    sleep();
    acquire(&s->lock);
  }
  struct mbuf *m = s->rxq;
  s->rxq = m->next;
  s->rxcount--;
  release(&s->lock);
  int len = m->len < n ? m->len : n;
  if (either_copyout(user, addr, m->head, len) < 0)
    len = -1;
  mbuffree(m);
  return len;
}

int
sockwrite(struct sock *s, int user, uint64 addr, int n)
{
  if (s->type == SOCK_STREAM)
    return tcp_write(s, user, addr, n);
  if (s->type != SOCK_DGRAM || s->rport == 0 || n > 1400)
    return -1;
  if (!net_policy_ok(s->rip, IPPROTO_UDP, s->rport, 0)) {
    net_policy_dropped++;
    return -2;
  }
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return -1;
  char *p = mbufput(m, n);
  if (either_copyin(p, user, addr, n) < 0) {
    mbuffree(m);
    return -1;
  }
  if (net_tx_udp(m, s->rip, s->lport, s->rport) < 0)
    return -1;
  return n;
}
