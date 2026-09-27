//
// sockets on the network: UDP, ICMP echo ("ping" sockets) and TCP
// streams.
//
// what is here is the network half of a socket and nothing else. the
// calls are plain functions over `struct sock` -- ipsock_new, _bind,
// _connect, _listen, _accept, _sendto, _recvfrom, _setopt, _getopt --
// taking an address as a raw IPv4 address and a port. sockapi.c sits
// above this and over unixsock.c, turns sockaddrs into those, and is
// the only file that knows what a file descriptor or an address
// family is. keeping the split here means the network code never has
// to think about AF_UNIX, and the POSIX shapes are written once.
//
//   read/write/close on the descriptor still land in sockread,
//   sockwrite and sockclose below, as they always did.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "errno.h"
#include "fs.h"
#include "file.h"
#include "net.h"
#include "cgw.h"
#include "sock.h"
#include "tcp.h"

struct sock socks[NSOCK];
static struct spinlock socklock;
static uint16 next_port = 49152;

void
sockinit(void)
{
  initlock(&socklock, "socks");
  for (int i = 0; i < NSOCK; i++)
    initlock(&socks[i].lock, "sock");
}

// walk the socket table for /proc/net/tcp and /proc/net/udp. each
// socket's own lock is taken in turn rather than one lock over the
// whole walk, so a busy connection is not held up by a reader.
void
sock_walk(void (*fn)(const struct sockinfo *si, void *arg), void *arg)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    struct sockinfo si;

    acquire(&s->lock);
    if (s->type == SOCK_NONE) {
      release(&s->lock);
      continue;
    }
    memset(&si, 0, sizeof(si));
    si.index = i;
    si.type = s->type;
    si.lip = s->lip;
    si.rip = s->rip;
    si.lport = s->lport;
    si.rport = s->rport;
    si.rxcount = s->rxcount;
    si.closed = s->closed;
    si.state = -1;
    if (s->type == SOCK_STREAM && s->tcp) {
      si.state = s->tcp->state;
      si.rxbytes = s->tcp->rcv.w - s->tcp->rcv.r;
      si.txbytes = s->tcp->snd.w - s->tcp->snd.r;
    }
    release(&s->lock);
    fn(&si, arg);
  }
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
      s->ttl = 0;
      s->want_errors = 0;
      s->deadline = 0;
      s->rcvtimeo = 0;
      s->sndtimeo = 0;
      s->shut_rd = s->shut_wr = 0;
      s->tcp = 0;
      s->closed = 0;
      s->ns = netns_cur();
      s->id = i;
      release(&socklock);
      return s;
    }
  }
  release(&socklock);
  return 0;
}

static uint16
port_alloc(struct netns *ns)
{
  // ephemeral ports 49152..65535, skipping ones in use in this namespace.
  for (int tries = 0; tries < 16384; tries++) {
    uint16 p = next_port++;
    if (next_port == 0)
      next_port = 49152;
    int used = 0;
    for (int i = 0; i < NSOCK; i++)
      if (socks[i].type != SOCK_NONE && socks[i].ns == ns &&
          socks[i].lport == p)
        used = 1;
    if (!used)
      return p;
  }
  return 0;
}

// shutdown(2): stop one direction without giving up the descriptor.
// on a stream the write side sends the FIN, which is the point of it;
// a datagram socket has nothing to tell the other end, so what
// changes is only what this end will do from now on.
int
sockshutdown(struct sock *s, int how)
{
  if (s == 0 || s->type == SOCK_NONE)
    return -ENOTCONN;
  acquire(&s->lock);
  if (how == SHUT_RD || how == SHUT_RDWR)
    s->shut_rd = 1;
  if (how == SHUT_WR || how == SHUT_RDWR)
    s->shut_wr = 1;
  int stream = s->type == SOCK_STREAM && s->tcp != 0;
  release(&s->lock);
  if (stream && (how == SHUT_WR || how == SHUT_RDWR))
    tcp_shutdown(s);
  wakeup(s);
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
sock_udp_rx(struct netns *ns, struct mbuf *m, uint32 src, uint16 sport,
            uint16 dport)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type == SOCK_DGRAM && s->ns == ns && s->lport == dport && (s->rport == 0 || (s->rip == src && s->rport == sport))) {
      m->arg = src;
      m->arg16 = sport;
      return sock_enqueue(s, m);
    }
  }
  return -1;
}

int
sock_icmp_rx(struct netns *ns, struct mbuf *m, uint32 src, uint16 id)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type == SOCK_ICMP && s->ns == ns && s->lport == id) {
      m->arg = src;
      m->arg16 = ntohs(((struct icmp *)m->head)->seq);
      mbufpull(m, sizeof(struct icmp));
      return sock_enqueue(s, m);
    }
  }
  return -1;
}

// an ICMP error about a packet this machine sent. it is delivered as
// an empty datagram whose `port` carries the sequence number of the
// probe and, above it, the ICMP type -- an echo reply is type zero,
// so a program that does not care about errors never sees one and a
// program that does can tell them apart.
int
sock_icmp_error(struct netns *ns, uint32 src, int type, int code, uint16 id,
                uint16 seq)
{
  (void)code;
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type != SOCK_ICMP || s->ns != ns || s->lport != id ||
        !s->want_errors)
      continue;
    struct mbuf *m = mbufalloc(MBUF_HEADROOM);
    if (m == 0)
      return -1;
    m->arg = src;
    m->arg16 = seq;
    m->arg_type = type;
    return sock_enqueue(s, m);
  }
  return -1;
}

// the same for a UDP socket, named by the source port of the packet
// the error is about
int
sock_udp_error(struct netns *ns, uint32 src, int type, int code, uint16 sport)
{
  (void)code;
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->type != SOCK_DGRAM || s->ns != ns || s->lport != sport ||
        !s->want_errors)
      continue;
    struct mbuf *m = mbufalloc(MBUF_HEADROOM);
    if (m == 0)
      return -1;
    m->arg = src;
    m->arg16 = 0;
    m->arg_type = type;
    return sock_enqueue(s, m);
  }
  return -1;
}

// wake any socket whose receive has waited long enough. called once a
// tick; the loop is over thirty-two slots and only runs at all while
// something is waiting, which costs nothing worth measuring.
void
sock_timer(void)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socks[i];
    if (s->deadline == 0)
      continue;
    acquire(&s->lock);
    if (s->deadline != 0 && (int)(ticks - s->deadline) >= 0)
      wakeup(s);
    release(&s->lock);
  }
}

// wait for the interface to be configured (at most timeout ticks).
static int
wait_up(int timeout)
{
  uint t0 = ticks;
  struct netns *ns = netns_cur();

  while (!ns->cfg.up) {
    if (timeout && ticks - t0 >= timeout)
      return -1;
    if (killed(myproc()))
      return -1;
    sleep_prepare(&ns->cfg);
    if (!ns->cfg.up)
      sleep_intr();
    else
      sleep_cancel();   // came up in between: give the registration back, or
                        // this task is freed while still on the queue

  }
  return 0;
}

// ---- the calls, as sockapi.c uses them ----

// a socket on the network. the descriptor is made by the layer above,
// which is the one that knows about address families.
struct sock *
ipsock_new(int type)
{
  if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_ICMP)
    return 0;
  if (!virtio_net_present())
    return 0;
  struct sock *s = sockalloc(type);
  if (s == 0)
    return 0;
  if (type == SOCK_ICMP)
    s->lport = 0x5800 | s->id; // echo identifier
  return s;
}

int
ipsock_bind(struct sock *s, int port)
{
  if (s == 0 || port < 0 || port > 65535)
    return -EINVAL;
  acquire(&socklock);
  if (port == 0)
    port = port_alloc(s->ns);
  for (int i = 0; i < NSOCK; i++)
    if (&socks[i] != s && socks[i].type == s->type && socks[i].ns == s->ns &&
        socks[i].lport == port) {
      release(&socklock);
      return -EADDRINUSE;
    }
  s->lport = port;
  release(&socklock);
  return 0;
}

int
ipsock_connect(struct sock *s, uint32 ip, int port)
{
  if (s == 0 || port <= 0 || port > 65535)
    return -EINVAL;
  if (wait_up(300) < 0)
    return -ENETDOWN;
  if (s->lport == 0) {
    acquire(&socklock);
    s->lport = port_alloc(s->ns);
    release(&socklock);
  }
  s->rip = ip;
  s->rport = port;
  if (s->type == SOCK_STREAM)
    return tcp_connect(s);
  return 0;
}

int
ipsock_listen(struct sock *s, int backlog)
{
  if (s == 0 || s->type != SOCK_STREAM || s->lport == 0)
    return -EINVAL;
  return tcp_listen(s, backlog);
}

// the connection at the head of the queue. the descriptor for it is
// made by the caller, which is the layer that knows about families.
struct sock *
ipsock_accept(struct sock *s, int *err)
{
  if (s == 0 || s->type != SOCK_STREAM) {
    *err = -EINVAL;
    return 0;
  }
  struct sock *ns = tcp_accept(s);
  if (ns == 0) {
    *err = -EINVAL;
    return 0;
  }
  *err = 0;
  return ns;
}

int
ipsock_sendto(struct sock *s, int user, uint64 buf, int n, uint32 dst, int port)
{
  if (s == 0 || n < 0 || n > 1400)
    return -EINVAL;
  if (s->type == SOCK_ICMP) {
    char data[64];
    if (n > sizeof(data))
      n = sizeof(data);
    if (either_copyin(data, user, buf, n) < 0)
      return -EFAULT;
    if (wait_up(300) < 0)
      return -ENETDOWN;
    if (!net_policy_ok(dst, IPPROTO_ICMP, 0, 0)) {
      net_policy_dropped++;
      return -EACCES; // the egress policy refused it
    }
    int rc = net_tx_icmp_echo_ns(s->ns, dst, s->lport, port, data, n, s->ttl);
    if (rc < 0)
      return rc < -1 ? rc : -EIO;   // the reason, when there is one
    return n;
  }
  if (s->type != SOCK_DGRAM)
    return -EOPNOTSUPP;
  if (wait_up(300) < 0)
    return -ENETDOWN;
  if (s->lport == 0) {
    acquire(&socklock);
    s->lport = port_alloc(s->ns);
    release(&socklock);
  }
  if (!net_policy_ok(dst, IPPROTO_UDP, port, 0)) {
    net_policy_dropped++;
    return -EACCES; // the egress policy refused it
  }
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return -ENOMEM;
  char *p = mbufput(m, n);
  if (either_copyin(p, user, buf, n) < 0) {
    mbuffree(m);
    return -EFAULT;
  }
  int rc = net_tx_udp_ns(s->ns, m, dst, s->lport, port, s->ttl);
  if (rc < 0)
    return rc < -1 ? rc : -EIO;
  return n;
}

// one datagram. `timeout` is in ticks, 0 meaning wait as long as it
// takes; `nowait` returns at once when nothing has arrived.
int
ipsock_recvfrom(struct sock *s, int user, uint64 buf, int n, uint32 *rip,
                int *rport, int timeout, int nowait)
{
  if (s == 0 || n < 0 || (s->type != SOCK_DGRAM && s->type != SOCK_ICMP))
    return -EINVAL;

  if (s->shut_rd)
    return 0;

  uint t0 = ticks;
  acquire(&s->lock);
  // a timeout has to be able to fire on its own: nothing else will
  // wake this sleeper when the answer never comes, so the tick hand
  // is told to look at this socket (see sock_timer).
  s->deadline = timeout ? t0 + (uint)timeout : 0;
  while (s->rxq == 0) {
    if (nowait) {
      s->deadline = 0;
      release(&s->lock);
      return -EAGAIN;
    }
    if (killed(myproc()) || signal_pending(myproc())) {
      s->deadline = 0;
      release(&s->lock);
      return -EINTR;
    }
    if (timeout && ticks - t0 >= (uint)timeout) {
      s->deadline = 0;
      release(&s->lock);
      return -EAGAIN;
    }
    sleep_prepare(s);
    release(&s->lock);
    sleep_intr();
    acquire(&s->lock);
  }
  s->deadline = 0;
  struct mbuf *m = s->rxq;
  s->rxq = m->next;
  s->rxcount--;
  release(&s->lock);

  int len = m->len < n ? m->len : n;
  int ok = either_copyout(user, buf, m->head, len) >= 0;
  uint32 src = m->arg;
  // an ICMP error puts its type above the sequence number, so that one
  // call can hand back both what answered and what it said
  int sport = m->arg16 | ((int)m->arg_type << 16);
  mbuffree(m);
  if (!ok)
    return -EFAULT;
  if (rip)
    *rip = src;
  if (rport)
    *rport = sport;
  return len;
}

// the few knobs a socket on the network has. the names are the ones
// in socket.h; the POSIX layer above turns IP_TTL and the rest into
// these.
int
ipsock_setopt(struct sock *s, int opt, int value)
{
  if (s == 0)
    return -EBADF;
  acquire(&s->lock);
  int r = 0;
  switch (opt) {
  case SO_TTL:
    if (value < 0 || value > 255)
      r = -EINVAL;
    else
      s->ttl = value;
    break;
  case SO_ICMP_ERRORS:
    s->want_errors = value != 0;
    break;
  case SO_RCVTIMEO_TICKS:
    if (value < 0)
      r = -EINVAL;
    else
      s->rcvtimeo = value;
    break;
  case SO_SNDTIMEO_TICKS:
    if (value < 0)
      r = -EINVAL;
    else
      s->sndtimeo = value;
    break;
  default:
    r = -ENOPROTOOPT;
  }
  release(&s->lock);
  return r;
}

int
ipsock_getopt(struct sock *s, int opt, int *value)
{
  if (s == 0)
    return -EBADF;
  acquire(&s->lock);
  int r = 0;
  switch (opt) {
  case SO_TTL:
    *value = s->ttl ? s->ttl : 64;
    break;
  case SO_ICMP_ERRORS:
    *value = s->want_errors;
    break;
  case SO_RCVTIMEO_TICKS:
    *value = s->rcvtimeo;
    break;
  case SO_SNDTIMEO_TICKS:
    *value = s->sndtimeo;
    break;
  default:
    r = -ENOPROTOOPT;
  }
  release(&s->lock);
  return r;
}

// ip route add|del|get: the routing table, changed or asked. Changing
// it needs CAP_NET_ADMIN; asking does not, because a program is allowed
// to know how its own packets will go.
// vmm gw add|rm: a router inside the host. Making one is privileged --
// it takes an address on the switch and a subnet of its own -- and so is
// taking one away.
uint64
sys_netgw(void)
{
  struct gwreq r;
  uint64 addr;
  int op;

  argint(0, &op);
  argaddr(1, &addr);
  if (either_copyin(&r, 1, addr, sizeof(r)) < 0)
    return -EFAULT;
  r.name[CGW_NAMELEN - 1] = 0;
  if (!cap_ok(CAP_NET_ADMIN))
    return -EPERM;
  if (op == GW_ADD) {
    struct cgw *g = 0;
    int rc = cgw_create(r.name, r.addr, r.mask, r.pub_ip, &g);
    if (rc < 0)
      return rc;
    r.pub_ip = g->pub_ip;
    if (either_copyout(1, addr, &r, sizeof(r)) < 0)
      return -EFAULT;
    return rc;
  }
  if (op == GW_DEL) {
    struct cgw *g = cgw_find(r.name);
    if (g == 0)
      return -ESRCH;
    return cgw_destroy(g);
  }
  // the rest are a container runtime's, and are about the caller's own
  // network namespace. They are privileged (CAP_NET_ADMIN, above) and
  // the namespace has to be one of its own: attaching the machine's
  // network to a gateway would put the host behind it.
  if (op == GW_ATTACH || op == GW_PUB || op == GW_UNPUB) {
    struct netns *ns = netns_cur();
    struct cgw *g = r.name[0] ? cgw_find(r.name) : cgw_by_addr(r.addr);
    if (g == 0)
      return -ESRCH;
    if (op == GW_ATTACH) {
      if (ns == &rootns)
        return -EPERM;
      r.ifname[NETIF_NAMELEN - 1] = 0;
      int rc = cgw_attach_ns(g, ns, r.addr, r.ifname);
      if (rc < 0)
        return rc;
      // and what a program asks about its network: its address, the way
      // out, and the name server the machine uses, which it reaches
      // through the gateway like everything else
      ns->cfg.ip = r.addr;
      ns->cfg.mask = g->mask;
      ns->cfg.gw = g->addr;
      ns->cfg.dns = rootns.cfg.dns;
      ns->cfg.up = 1;
      struct netif *dev = netif_find(ns, r.ifname[0] ? r.ifname : "eth0");
      if (dev)
        memmove(ns->cfg.mac, dev->mac, ETHADDR_LEN);
      return 0;
    }
    if (r.proto != IPPROTO_TCP && r.proto != IPPROTO_UDP)
      return -EINVAL;
    if (op == GW_PUB)
      return cgw_publish_ns(g, r.proto, r.pport,
                            r.addr ? r.addr : ns->cfg.ip, r.cport);
    return cgw_unpublish_ns(g, r.proto, r.pport);
  }
  return -EINVAL;
}

uint64
sys_netroute(void)
{
  struct rtreq r;
  uint64 addr;
  int op;

  argint(0, &op);
  argaddr(1, &addr);
  if (either_copyin(&r, 1, addr, sizeof(r)) < 0)
    return -EFAULT;
  r.dev[NETIF_NAMELEN - 1] = 0;
  struct netns *ns = netns_cur();

  // an address on an interface: the address, the subnet it is on and the
  // way out of it, which is what DHCP sets when it is answered and what
  // nothing could set by hand until now. A host with no DHCP server on
  // its wire -- two of them joined to each other for a migration -- has
  // no other way to have an address at all.
  if (op == RT_ADDR) {
    if (!cap_ok(CAP_NET_ADMIN))
      return -EPERM;
    struct netif *dev = netif_find(ns, r.dev[0] ? r.dev : "eth0");
    if (dev == 0)
      return -ENODEV;
    if (dev->flags & NETIF_LOOPBACK)
      return -EINVAL;
    netns_configure(ns, dev->name, r.dst, r.mask, r.gw);
    ns->cfg.ip = r.dst;
    ns->cfg.mask = r.mask;
    ns->cfg.gw = r.gw;
    if (r.metric)                 // the resolver's, if one was given
      ns->cfg.dns = (uint32)r.metric;
    ns->cfg.up = 1;
    ns->cfg.dhcp = 0;
    memmove(ns->cfg.mac, dev->mac, ETHADDR_LEN);
    wakeup(&ns->cfg);             // anything waiting for the interface
    return 0;
  }

  if (op == RT_GET) {
    uint32 nexthop = r.dst;
    struct netif *dev = route_lookup(ns, r.dst, &nexthop);
    if (dev == 0)
      return -ENETUNREACH;
    r.gw = nexthop == r.dst ? 0 : nexthop;
    safestrcpy(r.dev, dev->name, NETIF_NAMELEN);
    if (either_copyout(1, addr, &r, sizeof(r)) < 0)
      return -EFAULT;
    return 0;
  }

  if (!cap_ok(CAP_NET_ADMIN))
    return -EPERM;
  struct netif *dev = netif_find(ns, r.dev);
  if (op == RT_ADD) {
    if (dev == 0)
      return -ENODEV;
    // a gateway has to be on the interface own subnet. A next hop that
    // is itself only reachable through a gateway is not a next hop:
    // there would be nothing to put in the ethernet header.
    if (r.gw && !(dev->mask && (r.gw & dev->mask) == (dev->ip & dev->mask)))
      return -ENETUNREACH;
    return route_add(ns, r.dst, r.mask, r.gw, dev, r.metric) < 0 ? -ENOSPC : 0;
  }
  if (op == RT_DEL)
    return route_del(ns, r.dst & r.mask, r.mask, r.gw) < 0 ? -ESRCH : 0;
  return -EINVAL;
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
  if (s->shut_rd)
    return 0;   // end of file: this end has said it wants no more
  if (s->type == SOCK_STREAM)
    return tcp_read(s, user, addr, n);
  // datagram: next message, source discarded
  acquire(&s->lock);
  while (s->rxq == 0) {
    if (killed(myproc()) || s->closed || s->shut_rd) {
      release(&s->lock);
      return -1;
    }
    sleep_prepare(s);
    release(&s->lock);
    sleep_intr();
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
  if (s->shut_wr)
    return -EPIPE;
  if (s->type == SOCK_STREAM)
    return tcp_write(s, user, addr, n);
  if (s->type != SOCK_DGRAM || s->rport == 0 || n > 1400)
    return -1;
  if (!net_policy_ok(s->rip, IPPROTO_UDP, s->rport, 0)) {
    net_policy_dropped++;
    return -EACCES; // the egress policy refused it
  }
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return -1;
  char *p = mbufput(m, n);
  if (either_copyin(p, user, addr, n) < 0) {
    mbuffree(m);
    return -1;
  }
  if (net_tx_udp_ns(s->ns, m, s->rip, s->lport, s->rport, s->ttl) < 0)
    return -1;
  return n;
}

// is this socket ready? bytes readable, 1 if writable, -1 on hangup.
int
sockready(struct sock *s, int write)
{
  if (s == 0)
    return -1;
  if (write ? s->shut_wr : s->shut_rd)
    return -1;
  if (s->type == SOCK_STREAM)
    return tcp_ready(s->tcp, write);
  acquire(&s->lock);
  int r;
  if (write) {
    r = 1;
  } else {
    r = s->rxq ? (int)s->rxq->len : 0;
    if (r == 0 && s->closed)
      r = -1;
  }
  release(&s->lock);
  return r;
}
