//
// the network stack: packet buffers, ethernet, ARP, IPv4, ICMP, UDP,
// a DHCP client, and the egress policy.
//
// received frames arrive from the NIC's interrupt handler (net_rx) and
// are processed there under netlock, which is a spinlock: nothing in
// this file sleeps. sockets (sock.c) and TCP (tcp.c) build on it.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "net.h"
#include "cgw.h"
#include "errno.h"

// the root namespace: the physical NIC, the switch the guests are on,
// and the addresses this machine answers to. A container's namespace
// is another one of these (kernel/net.h).
struct netns rootns = { .ip_id = 1 };

static int lo_tx(struct netif *dev, struct mbuf *m);

// the namespaces, and the one a call belongs to. The root is not in the
// table: it is never freed, and a null pointer anywhere means it.
static struct netns netnstab[NNETNS];
static struct spinlock netnslock;

struct netns *
netns_cur(void)
{
  struct proc *p = myproc();

  if (p == 0 || p->ns == 0 || p->ns->net == 0)
    return &rootns;
  return p->ns->net;
}

int
netns_id(struct netns *ns)
{
  return ns == 0 || ns == &rootns ? 0 : ns->id;
}

void
netns_hold(struct netns *ns)
{
  if (ns == 0 || ns == &rootns)
    return;
  acquire(&netnslock);
  ns->ref++;
  release(&netnslock);
}

struct netns *
netns_new(void)
{
  struct netns *ns = 0;

  acquire(&netnslock);
  for (int i = 0; i < NNETNS; i++) {
    if (netnstab[i].ref == 0) {
      ns = &netnstab[i];
      memset(ns, 0, sizeof(*ns));
      ns->ref = 1;
      ns->id = i + 1;
      ns->ip_id = 1;
      break;
    }
  }
  release(&netnslock);
  if (ns == 0)
    return 0;
  // a network of one's own begins as a machine with no cable: the
  // loopback, its route, and nothing that reaches anybody else.
  struct netif *lo = netif_add(ns, "lo", NETIF_LOOPBACK, lo_tx, 0);
  if (lo == 0) {
    netns_put(ns);
    return 0;
  }
  route_add(ns, 0x7f000000, 0xff000000, 0, lo, 0);
  ns->cfg.up = 1;             // it can always talk to itself
  return ns;
}

void
netns_put(struct netns *ns)
{
  if (ns == 0 || ns == &rootns)
    return;
  acquire(&netnslock);
  int last = (--ns->ref <= 0);
  if (last)
    ns->ref = 0;
  release(&netnslock);
  if (!last)
    return;
  // the last process in it is gone. What it had goes with it: the port
  // on its gateway, its interfaces, its routes, and anything still
  // parked waiting for a hardware address.
  cgw_detach_ns(ns);
  acquire(&netlock);
  for (int i = 0; i < ARP_ENTRIES; i++)
    for (int j = 0; j < ARP_PENDING; j++)
      if (ns->arp[i].pending[j]) {
        mbuffree(ns->arp[i].pending[j]);
        ns->arp[i].pending[j] = 0;
      }
  while (ns->lo_head) {
    struct mbuf *m = ns->lo_head;
    ns->lo_head = m->next;
    mbuffree(m);
  }
  ns->lo_tail = 0;
  release(&netlock);
  route_flush(ns);
  while (ns->ifs)
    netif_del(ns->ifs);
  ns->lo = 0;
  memset(&ns->cfg, 0, sizeof(ns->cfg));
}

// ---- interfaces ----
//
// they come from a small fixed table rather than the allocator: an
// interface outlives the call that made it, there are never many, and a
// namespace that cannot make one has to fail rather than sleep. Eight
// per machine is the root's two plus a container's wires.
#define MAXNETIF 32        // the root's two, and two for every container
static struct netif netiftab[MAXNETIF];
static struct spinlock netiflock;

struct netif *
netif_add(struct netns *ns, const char *name, int flags,
          int (*tx)(struct netif *, struct mbuf *), void *priv)
{
  acquire(&netiflock);
  for (int i = 0; i < MAXNETIF; i++) {
    struct netif *d = &netiftab[i];
    if (d->ns != 0)
      continue;
    memset(d, 0, sizeof(*d));
    safestrcpy(d->name, name, NETIF_NAMELEN);
    d->flags = flags;
    d->tx = tx;
    d->priv = priv;
    d->ns = ns;
    d->up = 1;
    if (flags & NETIF_LOOPBACK) {
      d->ip = 0x7f000001;
      d->mask = 0xff000000;
      ns->lo = d;
    }
    // the list keeps the loopback first, which is the order `ip link'
    // and /proc/net/dev print
    if (ns->ifs == 0 || (flags & NETIF_LOOPBACK)) {
      d->next = ns->ifs;
      ns->ifs = d;
    } else {
      struct netif **t = &ns->ifs->next;
      while (*t)
        t = &(*t)->next;
      *t = d;
    }
    release(&netiflock);
    return d;
  }
  release(&netiflock);
  return 0;
}

// an interface goes away with the namespace that had it.
void
netif_del(struct netif *dev)
{
  if (dev == 0)
    return;
  acquire(&netiflock);
  struct netns *ns = dev->ns;
  if (ns) {
    struct netif **pp = &ns->ifs;
    while (*pp && *pp != dev)
      pp = &(*pp)->next;
    if (*pp)
      *pp = dev->next;
    if (ns->lo == dev)
      ns->lo = 0;
  }
  memset(dev, 0, sizeof(*dev));   // ns == 0 is what marks the slot free
  release(&netiflock);
}

struct netif *
netif_find(struct netns *ns, const char *name)
{
  for (struct netif *d = ns->ifs; d; d = d->next)
    if (strncmp(d->name, name, NETIF_NAMELEN) == 0)
      return d;
  return 0;
}

void
netif_set_addr(struct netif *dev, uint32 ip, uint32 mask)
{
  dev->ip = ip;
  dev->mask = mask;
}

// ---- routes ----
//
// also a fixed table: a namespace has a handful (its own subnets and a
// default), and a route is consulted on every packet, so walking a
// short array beats chasing an allocator.
#define MAXROUTE 32
static struct route routetab[MAXROUTE];
static struct spinlock routelock;

// every route a namespace has, which is what taking one apart needs.
void
route_flush(struct netns *ns)
{
  acquire(&routelock);
  struct route *r = ns->routes;
  while (r) {
    struct route *next = r->next;
    memset(r, 0, sizeof(*r));      // dev == 0 marks the slot free
    r = next;
  }
  ns->routes = 0;
  release(&routelock);
}

// remove a route. The gateway is part of what names it, so two routes
// to the same prefix through different gateways are two routes.
int
route_del(struct netns *ns, uint32 dst, uint32 mask, uint32 gw)
{
  int found = 0;

  acquire(&routelock);
  struct route **pp = &ns->routes;
  while (*pp) {
    struct route *r = *pp;
    if (r->dst == dst && r->mask == mask && r->gw == gw) {
      *pp = r->next;
      r->dev = 0;      // which is what marks the slot free
      found = 1;
      continue;
    }
    pp = &r->next;
  }
  release(&routelock);
  return found ? 0 : -1;
}

void
netif_walk(struct netns *ns, void (*fn)(const struct netif *, void *), void *arg)
{
  acquire(&netiflock);
  for (struct netif *d = ns->ifs; d; d = d->next)
    fn(d, arg);
  release(&netiflock);
}

int
route_add(struct netns *ns, uint32 dst, uint32 mask, uint32 gw,
          struct netif *dev, int metric)
{
  acquire(&routelock);
  for (int i = 0; i < MAXROUTE; i++) {
    struct route *r = &routetab[i];
    if (r->dev != 0)
      continue;
    r->dst = dst & mask;
    r->mask = mask;
    r->gw = gw;
    r->dev = dev;
    r->metric = metric;
    r->next = ns->routes;
    ns->routes = r;
    release(&routelock);
    return 0;
  }
  release(&routelock);
  return -1;
}

void
route_flush_dev(struct netns *ns, struct netif *dev)
{
  acquire(&routelock);
  struct route **t = &ns->routes;
  while (*t) {
    struct route *r = *t;
    if (r->dev == dev) {
      *t = r->next;
      memset(r, 0, sizeof(*r));
    } else {
      t = &r->next;
    }
  }
  release(&routelock);
}

// the longest prefix that matches, and where to send it: the
// destination itself when the route is on-link, the gateway when it is
// not. A tie on prefix length goes to the smaller metric.
struct netif *
route_lookup(struct netns *ns, uint32 dst, uint32 *nexthop)
{
  struct route *best = 0;
  int bestlen = -1;

  acquire(&routelock);
  for (struct route *r = ns->routes; r; r = r->next) {
    if ((dst & r->mask) != r->dst)
      continue;
    int len = 0;
    for (uint32 m = r->mask; m & 0x80000000; m <<= 1)
      len++;
    if (len > bestlen || (len == bestlen && best && r->metric < best->metric)) {
      best = r;
      bestlen = len;
    }
  }
  struct netif *dev = best ? best->dev : 0;
  if (best && nexthop)
    *nexthop = best->gw ? best->gw : dst;
  release(&routelock);
  return dev;
}

void
route_walk(struct netns *ns, void (*fn)(const struct route *r, void *arg),
           void *arg)
{
  for (struct route *r = ns->routes; r; r = r->next)
    fn(r, arg);
}
struct spinlock netlock;

// ---- packet buffers ----

struct mbuf *
mbufalloc(uint headroom)
{
  struct mbuf *m = kalloc();
  if (m == 0)
    return 0;
  if (headroom > MBUF_SIZE)
    headroom = MBUF_SIZE;
  m->next = 0;
  m->head = m->buf + headroom;
  m->len = 0;
  m->arg = 0;
  m->arg16 = 0;
  m->arg_type = 0;
  return m;
}

void
mbuffree(struct mbuf *m)
{
  kfree(m);
}

char *
mbufpush(struct mbuf *m, uint n)
{
  if (m->head - m->buf < n)
    panic("mbufpush");
  m->head -= n;
  m->len += n;
  return m->head;
}

char *
mbufpull(struct mbuf *m, uint n)
{
  if (m->len < n)
    return 0;
  char *p = m->head;
  m->head += n;
  m->len -= n;
  return p;
}

char *
mbufput(struct mbuf *m, uint n)
{
  if ((m->head - m->buf) + m->len + n > MBUF_SIZE)
    panic("mbufput");
  char *p = m->head + m->len;
  m->len += n;
  return p;
}

char *
mac2str(const uint8 *mac, char *buf)
{
  const char *hex = "0123456789abcdef";
  for (int i = 0; i < 6; i++) {
    buf[3 * i] = hex[mac[i] >> 4];
    buf[3 * i + 1] = hex[mac[i] & 15];
    buf[3 * i + 2] = (i == 5) ? 0 : ':';
  }
  return buf;
}

// ---- checksums ----

uint16
ip_checksum(void *data, int len, uint32 seed)
{
  uint32 sum = seed;
  uint16 *p = data;
  while (len > 1) {
    sum += *p++;
    len -= 2;
  }
  if (len)
    sum += *(uint8 *)p;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

// checksum over the IPv4 pseudo header plus the transport segment.
uint16
l4_checksum(uint32 src, uint32 dst, uint8 proto, void *data, int len)
{
  uint32 s = 0;
  uint32 nsrc = htonl(src), ndst = htonl(dst);
  s += (nsrc & 0xffff) + (nsrc >> 16);
  s += (ndst & 0xffff) + (ndst >> 16);
  s += htons(proto);
  s += htons(len);
  return ip_checksum(data, len, s);
}

// ---- egress policy ----
//
// the emulated network (qemu user-mode NAT) can reach the laptop (the
// gateway) and, through it, every network the laptop is on. this policy
// lets packets out only to public internet addresses, plus DHCP and the
// DNS forwarder; the laptop itself and any private network are off
// limits for the host and for every guest.
//
// other addresses on the emulated subnet are the host and the guests:
// that traffic stays inside the software switch (qemu answers ARP only
// for its own gateway/dns addresses, so nothing else on the subnet can
// be reached through the uplink), and it is allowed.

// is this machine's gateway a router inside the host rather than the
// door to the laptop? The hypervisor says so in the device tree
// (xv6h,outer), because a machine cannot tell from an address.
static int gw_local;

int
net_policy_ok(uint32 dst, uint8 proto, uint16 dport, int from_guest)
{
  // this machine talking to itself. 127.0.0.0/8 is not routable
  // anywhere by definition and the stack never puts such a packet on
  // the wire, so allowing it cannot reach the laptop or the network
  // beyond it -- and a machine that cannot talk to itself cannot run
  // a service and test it.
  if ((dst >> 24) == 127)
    return 1;

  // a container behind a gateway: its own subnet is its own network --
  // the gateway's address, and anything else behind the same gateway.
  // What it sends beyond that goes through the gateway, and the
  // translated packet meets this same policy on the way out of the
  // switch, so nothing here opens a door the machine does not have.
  struct netns *cur = netns_cur();
  if (cur != &rootns && cur->cfg.mask &&
      (dst & cur->cfg.mask) == (cur->cfg.ip & cur->cfg.mask) &&
      dst != (cur->cfg.ip | ~cur->cfg.mask))
    return 1;

  // DHCP: broadcast discover/request, and renewals to the server.
  if (proto == IPPROTO_UDP && dport == 67 && (dst == 0xffffffff || dst == netcfg.gw))
    return 1;
  // DNS to the forwarder that lives beside the gateway.
  if (proto == IPPROTO_UDP && dport == 53 && dst == netcfg.dns)
    return 1;
  uint8 a = dst >> 24, b = (dst >> 16) & 0xff;
  if ((dst == netcfg.gw && !gw_local) || dst == netcfg.dns)
    return 0; // the laptop, and qemu's forwarder
  if (netcfg.up && netcfg.mask && (dst & netcfg.mask) == (netcfg.ip & netcfg.mask) &&
      dst != (netcfg.ip | ~netcfg.mask))
    return 1; // a guest or the host: never leaves the switch
  // a machine behind a gateway: the switch is on the far side of it, and
  // the host and the other machines on it are reachable through the
  // gateway. The two addresses that are qemu's -- its router, which is
  // the laptop, and its name forwarder -- are not.
  if (plat.outer_mask && (dst & plat.outer_mask) == plat.outer_net) {
    if (dst == plat.outer_gw || dst == plat.outer_dns)
      return 0;
    if (dst == (plat.outer_net | ~plat.outer_mask))
      return 0;   // the broadcast address of a network that is not ours
    return 1;
  }
  if (a == 10 || a == 0)
    return 0; // 127 is handled above: it is ours and goes nowhere
  if (a == 172 && b >= 16 && b < 32)
    return 0;
  if (a == 192 && b == 168)
    return 0;
  if (a == 169 && b == 254)
    return 0;
  if (a == 100 && b >= 64 && b < 128)
    return 0; // carrier NAT
  if (a >= 224)
    return 0; // multicast, reserved, broadcast
  return 1;
}

// ---- ethernet ----

static const uint8 broadcast_mac[ETHADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int
netif_tx(struct netif *dev, struct mbuf *m, const uint8 *dhost, uint16 type)
{
  struct eth *e = (struct eth *)mbufpush(m, sizeof(*e));
  memmove(e->dhost, dhost, ETHADDR_LEN);
  memmove(e->shost, dev->mac, ETHADDR_LEN);
  e->type = htons(type);
  if (dev->tx == 0 || !dev->up) {
    dev->tx_dropped++;
    dev->ns->tx_dropped++;
    mbuffree(m);
    return -1;
  }
  dev->tx_packets++;
  return dev->tx(dev, m);
}

// the old spelling: out of whichever interface reaches the world.
int
net_tx_eth(struct mbuf *m, const uint8 *dhost, uint16 type)
{
  struct netif *dev = netif_find(&rootns, "eth0");

  if (dev == 0) {
    mbuffree(m);
    return -1;
  }
  return netif_tx(dev, m, dhost, type);
}

// ---- arp ----

// the cache is a field of the namespace, and a question leaves by the
// interface whose address is the answer's: a container asks its own
// gateway on its own wire, with its own hardware address on the front.
static void
arp_send(struct netif *dev, uint16 op, const uint8 *dmac, uint32 tip,
         const uint8 *tmac)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0 || dev == 0) {
    if (m)
      mbuffree(m);
    return;
  }
  struct arp *a = (struct arp *)mbufput(m, sizeof(*a));
  a->hrd = htons(ARP_HRD_ETHER);
  a->pro = htons(ETHTYPE_IP);
  a->hln = ETHADDR_LEN;
  a->pln = 4;
  a->op = htons(op);
  memmove(a->sha, dev->mac, ETHADDR_LEN);
  a->sip = htonl(dev->ip);
  memmove(a->tha, tmac, ETHADDR_LEN);
  a->tip = htonl(tip);
  netif_tx(dev, m, dmac, ETHTYPE_ARP);
}

static struct arpent *
arp_find(struct netns *ns, uint32 ip)
{
  for (int i = 0; i < ARP_ENTRIES; i++)
    if (ns->arp[i].ip == ip)
      return &ns->arp[i];
  return 0;
}

static struct arpent *
arp_entry(struct netns *ns, uint32 ip)
{
  struct arpent *e = arp_find(ns, ip);
  struct arpent *arptab = ns->arp;
  if (e)
    return e;
  // reuse: an empty slot, else an invalid one, else the oldest.
  struct arpent *victim = 0;
  for (int i = 0; i < ARP_ENTRIES; i++) {
    if (arptab[i].ip == 0)
      return memset(&arptab[i], 0, sizeof(arptab[i])), arptab[i].ip = ip, &arptab[i];
    if (!victim || (!arptab[i].valid && victim->valid) ||
        (arptab[i].valid == victim->valid && arptab[i].requested < victim->requested))
      victim = &arptab[i];
  }
  for (int j = 0; j < ARP_PENDING; j++)
    if (victim->pending[j]) {
      mbuffree(victim->pending[j]);
      victim->pending[j] = 0;
    }
  memset(victim, 0, sizeof(*victim));
  victim->ip = ip;
  return victim;
}

static void
arp_rx(struct netif *dev, struct mbuf *m)
{
  struct arp *a = (struct arp *)m->head;
  struct netns *ns = dev ? dev->ns : &rootns;
  uint32 mine = dev ? dev->ip : rootns.cfg.ip;

  if (m->len < sizeof(*a) || ntohs(a->hrd) != ARP_HRD_ETHER || ntohs(a->pro) != ETHTYPE_IP)
    goto out;
  uint32 sip = ntohl(a->sip), tip = ntohl(a->tip);

  // learn the sender (also from requests, as everyone does).
  struct arpent *e = arp_find(ns, sip);
  if (e || tip == mine) {
    if (!e)
      e = arp_entry(ns, sip);
    memmove(e->mac, a->sha, ETHADDR_LEN);
    e->valid = 1;
    for (int j = 0; j < ARP_PENDING; j++) {
      if (e->pending[j]) {
        struct mbuf *p = e->pending[j];
        e->pending[j] = 0;
        netif_tx(dev, p, e->mac, ETHTYPE_IP);
      }
    }
  }
  if (ntohs(a->op) == ARP_OP_REQUEST && dev && dev->up && mine && tip == mine)
    arp_send(dev, ARP_OP_REPLY, a->sha, sip, a->sha);
out:
  mbuffree(m);
}

// walk the arp cache for /proc/net/arp. the caller holds netlock,
// which is what everything else that touches the table holds.
void
arp_walk(void (*fn)(uint32 ip, const uint8 *mac, int valid, void *arg), void *arg)
{
  struct netns *ns = netns_cur();

  for (int i = 0; i < ARP_ENTRIES; i++)
    if (ns->arp[i].ip)
      fn(ns->arp[i].ip, ns->arp[i].mac, ns->arp[i].valid, arg);
}

// ---- ipv4 ----

// loopback: a packet to one of our own addresses goes back into the
// stack rather than to the wire (see below).
static int loopback_send(struct netns *ns, struct mbuf *m);
static int is_local(struct netns *ns, uint32 dst);

// the address this namespace speaks from, for a packet going to dst:
// the address of the interface the route chooses. A namespace has one
// address per interface and the route says which one is being used,
// which is the only answer that is right in a namespace with two.
static uint32
src_for(struct netns *ns, uint32 dst)
{
  uint32 nh = dst;

  if ((dst >> 24) == 127)
    return dst;                  // to 127.x from 127.x, always
  struct netif *dev = route_lookup(ns, dst, &nh);
  if (dev && dev->ip)
    return dev->ip;
  return ns->cfg.ip;
}

// the same question from outside this file: what address does this
// namespace speak from, to reach dst?
uint32
net_src_for(struct netns *ns, uint32 dst)
{
  return src_for(ns, dst);
}

// send m as an IP packet to dst. takes ownership. caller holds netlock.
// `ttl` of zero means the usual 64; a smaller one makes the packet
// expire on the way, which is how traceroute finds the routers.
static int
ip_send_ns(struct netns *ns, struct mbuf *m, uint32 dst, uint8 proto, int ttl)
{
  // the interface has to be up to reach anything but ourselves
  if (!ns->cfg.up && !(proto == IPPROTO_UDP) && (dst >> 24) != 127) {
    mbuffree(m);
    return -1;
  }
  struct ip *h = (struct ip *)mbufpush(m, sizeof(*h));
  h->vhl = 0x45;
  h->tos = 0;
  h->len = htons(m->len);
  h->id = htons(ns->ip_id++);
  h->off = 0;
  h->ttl = ttl > 0 && ttl < 256 ? (uint8)ttl : 64;
  h->pro = proto;
  h->sum = 0;
  // a packet to 127.x comes from 127.x, whatever this machine's own
  // address is -- and may be sent before there is one
  h->src = htonl(src_for(ns, dst));
  h->dst = htonl(dst);
  h->sum = ip_checksum(h, sizeof(*h), 0);

  if (is_local(ns, dst))
    return loopback_send(ns, m);

  uint32 nexthop = dst;
  struct netif *dev = route_lookup(ns, dst, &nexthop);
  if (dst == 0xffffffff) {
    // a broadcast goes out of the interface that has a route for it,
    // and out of the namespace's only one when nothing does
    if (dev == 0)
      dev = ns->ifs && ns->ifs->next ? ns->ifs->next : ns->ifs;
    if (dev == 0 || (dev->flags & NETIF_LOOPBACK)) {
      mbuffree(m);
      return -ENETUNREACH;
    }
    return netif_tx(dev, m, broadcast_mac, ETHTYPE_IP);
  }
  if (dev == 0) {
    // nothing in the table matches, so there is nowhere to send it.
    // (A broadcast, and anything to the machine itself, was answered
    // above and never gets here.)
    mbuffree(m);
    ns->tx_dropped++;
    return -ENETUNREACH;
  }
  if (dev->flags & NETIF_LOOPBACK)
    return loopback_send(ns, m);
  struct arpent *e = arp_entry(ns, nexthop);
  if (e->valid)
    return netif_tx(dev, m, e->mac, ETHTYPE_IP);

  // not resolved: park the packet and ask.
  int parked = 0;
  for (int j = 0; j < ARP_PENDING; j++) {
    if (e->pending[j] == 0) {
      e->pending[j] = m;
      parked = 1;
      break;
    }
  }
  if (!parked) {
    mbuffree(m);
    ns->tx_dropped++;
  }
  // one request in flight per tenth of a second, whatever a tick is
  if (ticks - e->requested >= (uint)(tick_hz / 10 ? tick_hz / 10 : 1) ||
      e->requested == 0) {
    e->requested = ticks ? ticks : 1;
    arp_send(dev, ARP_OP_REQUEST, broadcast_mac, nexthop, broadcast_mac);
  }
  return 0;
}

// the same, in the namespace the caller belongs to. Every path that has
// a process behind it ends up here; the ones that do not -- a TCP
// retransmission from the timer, a gateway forwarding a packet -- say
// which namespace they mean, because by then there is no process to ask.
static int
ip_send_ttl(struct mbuf *m, uint32 dst, uint8 proto, int ttl)
{
  return ip_send_ns(netns_cur(), m, dst, proto, ttl);
}

// the usual time to live
static int
ip_send(struct mbuf *m, uint32 dst, uint8 proto)
{
  return ip_send_ttl(m, dst, proto, 0);
}

int
net_tx_ip(struct mbuf *m, uint32 dst, uint8 proto)
{
  return ip_send(m, dst, proto);
}

int
net_tx_ip_ns(struct netns *ns, struct mbuf *m, uint32 dst, uint8 proto)
{
  return ip_send_ns(ns, m, dst, proto, 0);
}

int
net_tx_ip_ttl(struct mbuf *m, uint32 dst, uint8 proto, int ttl)
{
  return ip_send_ttl(m, dst, proto, ttl);
}

// ---- icmp ----

static void
icmp_rx(struct netns *ns, struct mbuf *m, struct ip *iph)
{
  struct icmp *ic = (struct icmp *)m->head;
  if (m->len < sizeof(*ic))
    goto drop;
  uint32 src = ntohl(iph->src);
  if (ic->type == ICMP_ECHO) {
    // echo reply: swap roles, keep the payload.
    ic->type = ICMP_ECHOREPLY;
    ic->sum = 0;
    ic->sum = ip_checksum(ic, m->len, 0);
    if (ns != &rootns ||
        net_policy_ok(src, IPPROTO_ICMP, 0, 0) ||
        (netcfg.mask && (src & netcfg.mask) == (netcfg.ip & netcfg.mask)))
      ip_send_ns(ns, m, src, IPPROTO_ICMP, 0);
    else
      mbuffree(m);
    return;
  }
  if (ic->type == ICMP_ECHOREPLY) {
    if (sock_icmp_rx(ns, m, src, ntohs(ic->id)) == 0)
      return; // consumed
    goto drop;
  }
  // an error carries the head of the packet that caused it: the IP
  // header and the first eight bytes after it, which is enough to
  // name the socket that sent it
  if (ic->type == ICMP_TIMXCEED || ic->type == ICMP_UNREACH ||
      ic->type == ICMP_PARAMPROB || ic->type == ICMP_SRCQUENCH) {
    if (m->len < sizeof(*ic) + sizeof(struct ip) + 8)
      goto drop;
    struct ip *inner = (struct ip *)((char *)ic + sizeof(*ic));
    int ihl = (inner->vhl & 0xf) * 4;
    if (ihl < (int)sizeof(struct ip) ||
        m->len < sizeof(*ic) + ihl + 8)
      goto drop;
    const unsigned char *l4 = (const unsigned char *)inner + ihl;
    if (inner->pro == IPPROTO_ICMP) {
      uint16 id = (l4[4] << 8) | l4[5];
      uint16 seq = (l4[6] << 8) | l4[7];
      sock_icmp_error(ns, src, ic->type, ic->code, id, seq);
    } else if (inner->pro == IPPROTO_UDP) {
      uint16 sport = (l4[0] << 8) | l4[1];
      sock_udp_error(ns, src, ic->type, ic->code, sport);
    }
    goto drop;
  }
drop:
  mbuffree(m);
}

int
net_tx_icmp_echo_ns(struct netns *ns, uint32 dst, uint16 id, uint16 seq,
                    char *data, int n, int ttl)
{
  if (n > 1400)
    n = 1400;
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return -1;
  struct icmp *ic = (struct icmp *)mbufput(m, sizeof(*ic) + n);
  ic->type = ICMP_ECHO;
  ic->code = 0;
  ic->sum = 0;
  ic->id = htons(id);
  ic->seq = htons(seq);
  memmove(ic + 1, data, n);
  ic->sum = ip_checksum(ic, sizeof(*ic) + n, 0);
  acquire(&netlock);
  int r = ip_send_ns(ns, m, dst, IPPROTO_ICMP, ttl);
  release(&netlock);
  return r;
}

// ---- udp ----

// takes ownership of m (payload already in place). caller holds netlock.
static int
udp_send(struct netns *ns, struct mbuf *m, uint32 dst, uint16 sport,
         uint16 dport, int ttl)
{
  struct udp *u = (struct udp *)mbufpush(m, sizeof(*u));
  u->sport = htons(sport);
  u->dport = htons(dport);
  u->ulen = htons(m->len);
  u->sum = 0;
  u->sum = l4_checksum(src_for(ns, dst), dst, IPPROTO_UDP, u, m->len);
  if (u->sum == 0)
    u->sum = 0xffff;
  return ip_send_ns(ns, m, dst, IPPROTO_UDP, ttl);
}

int
net_tx_udp_ns(struct netns *ns, struct mbuf *m, uint32 dst, uint16 sport,
              uint16 dport, int ttl)
{
  acquire(&netlock);
  int r = udp_send(ns, m, dst, sport, dport, ttl);
  release(&netlock);
  return r;
}

static void dhcp_rx(struct mbuf *m, uint32 src);

// an error about a packet that could not be delivered. the message
// carries the head of the offending packet -- its IP header and the
// first eight bytes after it -- which is how the sender tells which
// of its packets the complaint is about. caller holds netlock.
static void
icmp_send_error(struct netns *ns, uint32 dst, int type, int code,
                const struct ip *iph, const void *l4, int l4len)
{
  if (dst == 0 || dst == 0xffffffff || (dst >> 28) == 0xe)
    return; // never to a broadcast or to a group
  if (iph->pro == IPPROTO_ICMP)
    return; // and never about an ICMP message, which would go round
  int ihl = (iph->vhl & 0xf) * 4;
  if (ihl < (int)sizeof(struct ip) || ihl > 60)
    return;
  if (l4len > 8)
    l4len = 8;

  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return;
  int n = sizeof(struct icmp) + ihl + l4len;
  struct icmp *ic = (struct icmp *)mbufput(m, n);
  ic->type = (uint8)type;
  ic->code = (uint8)code;
  ic->sum = 0;
  ic->id = 0;   // unused in an error, and zero is what goes there
  ic->seq = 0;
  memmove((char *)ic + sizeof(*ic), iph, ihl);
  memmove((char *)ic + sizeof(*ic) + ihl, l4, l4len);
  ic->sum = ip_checksum(ic, n, 0);
  ip_send_ns(ns, m, dst, IPPROTO_ICMP, 0);
}

static void
udp_rx(struct netns *ns, struct mbuf *m, struct ip *iph)
{
  struct udp *u = (struct udp *)m->head;
  if (m->len < sizeof(*u) || ntohs(u->ulen) < sizeof(*u) || ntohs(u->ulen) > m->len) {
    mbuffree(m);
    return;
  }
  m->len = ntohs(u->ulen);
  uint16 sport = ntohs(u->sport), dport = ntohs(u->dport);
  uint32 src = ntohl(iph->src);
  mbufpull(m, sizeof(*u));
  if (dport == 68 && ns == &rootns) {
    dhcp_rx(m, src);
    return;
  }
  if (sock_udp_rx(ns, m, src, sport, dport) < 0) {
    ns->rx_dropped++;
    // nobody has that port. saying so is what every other stack does,
    // and it is what makes a UDP connection fail at once rather than
    // wait for a timeout. `u` still points at the header the pull
    // above stepped over.
    icmp_send_error(ns, src, ICMP_UNREACH, 3, iph, u, sizeof(*u));
    mbuffree(m);
  }
}

// ---- loopback ----
//
// a packet addressed to this machine, by its own address or by one in
// 127.0.0.0/8, never reaches the wire: it goes back into the stack. a
// machine that cannot talk to itself is missing something every other
// one has, and it is how a service is tested without opening a port
// to anyone else.
//
// the delivery is queued rather than recursive. a packet given to the
// stack may make it answer at once -- a SYN becomes a SYN-ACK -- and
// answering inside the delivery of the packet that caused it would
// nest one call frame per exchange. the outermost sender drains the
// queue instead, so the depth is two whatever the conversation.
// the queue is the namespace's: a container talking to itself does not
// queue behind the machine, and the machine does not deliver into it.
static void ip_rx(struct netns *ns, struct netif *dev, struct mbuf *m);

static int
loopback_send(struct netns *ns, struct mbuf *m)
{
  m->next = 0;
  if (ns->lo_tail)
    ns->lo_tail->next = m;
  else
    ns->lo_head = m;
  ns->lo_tail = m;
  if (ns->lo_delivering)
    return 0;

  ns->lo_delivering = 1;
  while (ns->lo_head) {
    struct mbuf *p = ns->lo_head;
    ns->lo_head = p->next;
    if (ns->lo_head == 0)
      ns->lo_tail = 0;
    p->next = 0;
    ip_rx(ns, ns->lo, p);
  }
  ns->lo_delivering = 0;
  return 0;
}

// is this address one of ours? 127 anything, and any address one of the
// namespace's own interfaces answers to.
static int
is_local(struct netns *ns, uint32 dst)
{
  if ((dst >> 24) == 127)
    return 1;
  for (struct netif *d = ns->ifs; d; d = d->next)
    if (d->up && d->ip && d->ip == dst)
      return 1;
  return 0;
}

static void
ip_rx(struct netns *ns, struct netif *dev, struct mbuf *m)
{
  struct ip *h = (struct ip *)m->head;
  if (m->len < sizeof(*h) || (h->vhl >> 4) != 4)
    goto drop;
  uint hl = (h->vhl & 0xf) * 4;
  uint len = ntohs(h->len);
  if (hl < sizeof(*h) || len < hl || len > m->len)
    goto drop;
  if (ntohs(h->off) & 0x3fff)
    goto drop; // fragments are not supported
  uint32 dst = ntohl(h->dst);
  // addressed to this namespace: one of its own addresses, a broadcast,
  // or 127. A namespace with no address yet takes what arrives, which is
  // what makes DHCP possible.
  if (dst != 0xffffffff && !is_local(ns, dst) &&
      ((dev && dev->ip) || ns->cfg.up))
    goto drop;
  m->len = len;
  mbufpull(m, hl);
  switch (h->pro) {
  case IPPROTO_ICMP:
    icmp_rx(ns, m, h);
    return;
  case IPPROTO_UDP:
    udp_rx(ns, m, h);
    return;
  case IPPROTO_TCP:
    tcp_rx(ns, m, ntohl(h->src), dst);
    return;
  }
drop:
  ns->rx_dropped++;
  mbuffree(m);
}

static void rx_drain(void);

// a frame arriving on an interface. takes ownership. dev may be null
// while the machine is still finding its network device.
void
netif_rx(struct netif *dev, struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  if (m->len < sizeof(*e)) {
    if (dev) {
      dev->rx_dropped++;
      dev->ns->rx_dropped++;
    }
    mbuffree(m);
    return;
  }
  if (dev) {
    dev->rx_packets++;
    // a frame addressed to somebody else is the switch's mistake, not
    // ours, but it is worth counting
    if ((e->dhost[0] & 1) == 0 && memcmp(e->dhost, dev->mac, ETHADDR_LEN) != 0)
      dev->rx_dropped++;
  }
  // captured before anything is taken off the front, so what a reader
  // sees is the frame as it arrived
  if (pktcap_active())
    pktcap_frame(m->head, m->len);
  uint16 type = ntohs(e->type);
  mbufpull(m, sizeof(*e));
  struct netns *ns = dev ? dev->ns : &rootns;
  acquire(&netlock);
  if (type == ETHTYPE_ARP)
    arp_rx(dev, m);
  else if (type == ETHTYPE_IP)
    ip_rx(ns, dev, m);
  else
    mbuffree(m);
  release(&netlock);
  rx_drain();
}

// Frames that arrived while this hart was already inside the stack, and
// the drain that takes them afterwards.
//
// The switch can answer a question the stack is in the middle of asking.
// A packet leaving through a gateway makes the stack ask for a hardware
// address; the gateway is a port on the same switch and answers at once,
// in the same call chain, and the answer arrives at a stack that is
// holding the lock it would need. (The panic was `acquire: net is
// already held by this cpu' from netif_rx under ip_rx under arp_send.)
// Such a frame waits here and is taken as soon as the lock is free,
// which is what the loopback queue does for the same reason.
static struct mbuf *rxq_head, *rxq_tail;   // netlock
static int rx_draining;

static void
rx_drain(void)
{
  for (;;) {
    acquire(&netlock);
    if (rx_draining || rxq_head == 0) {
      release(&netlock);
      return;
    }
    struct mbuf *m = rxq_head;
    rxq_head = m->next;
    if (rxq_head == 0)
      rxq_tail = 0;
    rx_draining = 1;
    release(&netlock);
    m->next = 0;
    struct netif *dev = m->rxdev ? m->rxdev : netif_find(&rootns, "eth0");
    m->rxdev = 0;
    netif_rx(dev, m);
    acquire(&netlock);
    rx_draining = 0;
    release(&netlock);
  }
}

// a frame from the NIC (or from the bridge). takes ownership.
void
net_rx(struct mbuf *m)
{
  net_rx_dev(netif_find(&rootns, "eth0"), m);
}

// the same for a frame arriving somewhere else: the container end of a
// gateway's wire, which is reached from inside the stack's own send --
// a container asks for a hardware address, the gateway answers at once,
// and the answer must not be delivered under the lock the question is
// holding.
void
net_rx_dev(struct netif *dev, struct mbuf *m)
{
  if (holding(&netlock)) {
    m->next = 0;
    m->rxdev = dev;
    if (rxq_tail)
      rxq_tail->next = m;
    else
      rxq_head = m;
    rxq_tail = m;
    return;
  }
  netif_rx(dev, m);
}

// ---- dhcp client ----

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

struct dhcp {
  uint8 op, htype, hlen, hops;
  uint32 xid;
  uint16 secs, flags;
  uint32 ciaddr, yiaddr, siaddr, giaddr;
  uint8 chaddr[16];
  uint8 sname[64];
  uint8 file[128];
  uint32 magic;
  uint8 options[312];
} __attribute__((packed));


static void
dhcp_send(int type)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return;
  struct dhcp *d = (struct dhcp *)mbufput(m, sizeof(*d));
  memset(d, 0, sizeof(*d));
  d->op = 1;
  d->htype = 1;
  d->hlen = 6;
  d->xid = htonl(rootns.dhcp.xid);
  d->flags = htons(0x8000); // please broadcast the reply
  memmove(d->chaddr, netcfg.mac, ETHADDR_LEN);
  d->magic = htonl(0x63825363);
  uint8 *o = d->options;
  *o++ = 53; *o++ = 1; *o++ = type;
  if (type == DHCP_REQUEST) {
    *o++ = 50; *o++ = 4;
    uint32 r = htonl(rootns.dhcp.offered); memmove(o, &r, 4); o += 4;
    *o++ = 54; *o++ = 4;
    uint32 s = htonl(rootns.dhcp.server); memmove(o, &s, 4); o += 4;
  }
  *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6; // subnet, router, dns
  *o++ = 255;
  m->len = (char *)o - (char *)d; // trim unused option space
  // 0.0.0.0 while unconfigured, which is where every lease begins. The
  // source address comes from the interface now, so it is the interface
  // that has to be unconfigured for the length of the send.
  struct netif *eth0 = netif_find(&rootns, "eth0");
  uint32 saved_ip = netcfg.ip, saved_dev = eth0 ? eth0->ip : 0;
  netcfg.ip = 0;
  if (eth0)
    eth0->ip = 0;
  udp_send(&rootns, m, 0xffffffff, 68, 67, 0);
  netcfg.ip = saved_ip;
  if (eth0)
    eth0->ip = saved_dev;
  rootns.dhcp.last_tx = ticks;
}

static void
dhcp_rx(struct mbuf *m, uint32 src)
{
  struct dhcp *d = (struct dhcp *)m->head;
  if (m->len < sizeof(*d) - sizeof(d->options) + 4 || ntohl(d->xid) != rootns.dhcp.xid ||
      ntohl(d->magic) != 0x63825363 || memcmp(d->chaddr, netcfg.mac, ETHADDR_LEN) != 0)
    goto out;
  int type = 0;
  uint32 mask = 0, gw = 0, dns = 0, server = 0;
  uint8 *o = d->options, *end = (uint8 *)m->head + m->len;
  while (o < end && *o != 255) {
    if (*o == 0) {
      o++;
      continue;
    }
    if (o + 2 > end)
      break;
    uint8 code = o[0], len = o[1];
    uint8 *v = o + 2;
    if (v + len > end)
      break;
    uint32 val = 0;
    if (len >= 4)
      val = ((uint32)v[0] << 24) | ((uint32)v[1] << 16) | ((uint32)v[2] << 8) | v[3];
    switch (code) {
    case 53: type = v[0]; break;
    case 1: mask = val; break;
    case 3: gw = val; break;
    case 6: dns = val; break;
    case 54: server = val; break;
    }
    o = v + len;
  }
  if (type == DHCP_OFFER && rootns.dhcp.state == 1) {
    rootns.dhcp.offered = ntohl(d->yiaddr);
    rootns.dhcp.server = server ? server : src;
    rootns.dhcp.state = 2;
    dhcp_send(DHCP_REQUEST);
  } else if (type == DHCP_ACK && rootns.dhcp.state == 2) {
    netcfg.ip = ntohl(d->yiaddr);
    netcfg.mask = mask ? mask : 0xffffff00;
    netcfg.gw = gw;
    netcfg.dns = dns ? dns : gw;
    netcfg.up = 1;
    netcfg.dhcp = 1;
    rootns.dhcp.state = 3;
    netns_configure(&rootns, "eth0", netcfg.ip, netcfg.mask, netcfg.gw);
    printk("net: dhcp: ip %d.%d.%d.%d gateway %d.%d.%d.%d dns %d.%d.%d.%d\n", netcfg.ip >> 24,
           (netcfg.ip >> 16) & 0xff, (netcfg.ip >> 8) & 0xff, netcfg.ip & 0xff, netcfg.gw >> 24,
           (netcfg.gw >> 16) & 0xff, (netcfg.gw >> 8) & 0xff, netcfg.gw & 0xff, netcfg.dns >> 24,
           (netcfg.dns >> 16) & 0xff, (netcfg.dns >> 8) & 0xff, netcfg.dns & 0xff);
    wakeup(&netcfg);
  } else if (type == DHCP_NAK) {
    rootns.dhcp.state = 1;
    rootns.dhcp.xid++;
    dhcp_send(DHCP_DISCOVER);
  }
out:
  mbuffree(m);
}

// called from the timer tick on hart 0 (interrupt context).
void
net_timer(void)
{
  ct_timer();
  cgw_timer();
  if (!virtio_net_present())
    return;
  acquire(&netlock);
  if ((rootns.dhcp.state == 1 || rootns.dhcp.state == 2) &&
      ticks - rootns.dhcp.last_tx >= (uint)(2 * tick_hz)) {
    // no answer in 2 seconds: try again (from the start after a while).
    if (++rootns.dhcp.attempts % 4 == 0)
      rootns.dhcp.state = 1;
    dhcp_send(rootns.dhcp.state == 1 ? DHCP_DISCOVER : DHCP_REQUEST);
  }
  tcp_timer();
  release(&netlock);
  sock_timer();
  rx_drain();
}

// static configuration (from the hypervisor's device tree or a request).
void
net_set_static(uint32 ip, uint32 mask, uint32 gw, uint32 dns)
{
  acquire(&netlock);
  netcfg.ip = ip;
  netcfg.mask = mask;
  netcfg.gw = gw;
  netcfg.dns = dns;
  netcfg.up = 1;
  netcfg.dhcp = 0;
  rootns.dhcp.state = 0;
  release(&netlock);
  netns_configure(&rootns, "eth0", ip, mask, gw);
  wakeup(&netcfg);
}

// an address on an interface, and the two routes that go with it: the
// subnet it is on, and (if there is a gateway) everything else. This is
// what `ifconfig eth0 addr' does, and what DHCP does when it is
// answered.
void
netns_configure(struct netns *ns, const char *ifname, uint32 ip, uint32 mask,
                uint32 gw)
{
  struct netif *dev = netif_find(ns, ifname);

  if (dev == 0)
    return;
  route_flush_dev(ns, dev);
  netif_set_addr(dev, ip, mask);
  if (mask)
    route_add(ns, ip & mask, mask, 0, dev, 0);
  if (gw)
    route_add(ns, 0, 0, gw, dev, 0);   // the default route
}

void
net_stats(struct netinfo *ni)
{
  struct netns *ns = netns_cur();

  acquire(&netlock);
  ni->ip = ns->cfg.ip;
  ni->mask = ns->cfg.mask;
  ni->gw = ns->cfg.gw;
  ni->dns = ns->cfg.dns;
  memmove(ni->mac, ns->cfg.mac, 6);
  ni->up = ns->cfg.up;
  ni->dhcp = ns->cfg.dhcp;
  ni->rx_packets = ns->rx_packets;
  ni->tx_packets = ns->tx_packets;
  ni->rx_dropped = ns->rx_dropped;
  ni->tx_dropped = ns->tx_dropped;
  ni->policy_dropped = ns->policy_dropped;
  release(&netlock);
}

// the root namespace's two interfaces. `lo' delivers to itself; `eth0'
// hands the frame to the switch, which decides whether it goes to the
// uplink, to a guest, or nowhere (kernel/bridge.c).
static int
eth0_tx(struct netif *dev, struct mbuf *m)
{
  if (pktcap_active())
    pktcap_frame(m->head, m->len);
  return bridge_tx_host(m);
}

static int
lo_tx(struct netif *dev, struct mbuf *m)
{
  return loopback_send(dev->ns, m);
}

void
netinit(void)
{
  pktcap_init();
  pktcap_register();
  initlock(&netlock, "net");
  initlock(&netiflock, "netif");
  initlock(&netnslock, "netns");
  gw_local = plat.outer_mask != 0;
  ctinit();
  cgwinit();
  initlock(&routelock, "route");
  netif_add(&rootns, "lo", NETIF_LOOPBACK, lo_tx, 0);
  route_add(&rootns, 0x7f000000, 0xff000000, 0, rootns.lo, 0);
  sockinit();
  if (virtio_net_init() < 0) {
    printk("net: no network device\n");
    return;
  }
  struct netif *eth0 = netif_add(&rootns, "eth0", 0, eth0_tx, 0);
  if (eth0 == 0)
    panic("netinit: no room for eth0");
  memmove(eth0->mac, netcfg.mac, ETHADDR_LEN);
  if (plat.net_ip) {
    net_set_static(plat.net_ip, plat.net_mask, plat.net_gw, plat.net_dns);
    printk("net: static ip %d.%d.%d.%d\n", netcfg.ip >> 24, (netcfg.ip >> 16) & 0xff,
           (netcfg.ip >> 8) & 0xff, netcfg.ip & 0xff);
    return;
  }
  acquire(&netlock);
  rootns.dhcp.xid = 0x78763648 ^ (netcfg.mac[5] << 8) ^ netcfg.mac[4];
  rootns.dhcp.state = 1;
  dhcp_send(DHCP_DISCOVER);
  release(&netlock);
}
