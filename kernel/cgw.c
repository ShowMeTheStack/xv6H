//
// a router inside the host.
//
// A gateway object is a small router with two sides. On the inside it
// owns an address on a private subnet -- 172.17.0.1/16, say -- answers
// ARP for it, and is the default route of everything on that subnet. On
// the outside it is an ordinary port on the switch, with a MAC and an
// address of its own on the root subnet, and it looks from there like
// one more machine.
//
// What crosses it is translated. A packet going out has its source
// address and port replaced by the gateway's public address and a port
// the gateway picks, and the pair is remembered so that the answer can
// be turned back into the address inside. A packet arriving for a
// published port is translated the other way, to the machine and the
// port the port was published for. Nothing else that arrives is
// delivered, because nothing else has anywhere to go.
//
// The address inside is nobody else's business, which is the point: two
// machines behind two gateways may have the same address and never know
// it, and reach each other through their public ones.
//
// The gateway is not a machine: it has no stack of its own. It answers
// ARP for its two addresses and ICMP echo to them, and everything else
// it either translates or drops.
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
#include "intc.h"
#include "imsic.h"
#include "page.h"
#include "hyp.h"
#include "net.h"
#include "cgw.h"
#include "errno.h"

static struct cgw cgwtab[NCGW];
static struct spinlock cgwlock;
static const uint8 bcast_mac[ETHADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void
cgwinit(void)
{
  initlock(&cgwlock, "cgw");
  for (int i = 0; i < NCGW; i++)
    initlock(&cgwtab[i].lock, "cgwnat");
}

// ---- the table of gateways ----

struct cgw *
cgw_find(const char *name)
{
  acquire(&cgwlock);
  for (int i = 0; i < NCGW; i++) {
    if (cgwtab[i].used && strncmp(cgwtab[i].name, name, CGW_NAMELEN) == 0) {
      release(&cgwlock);
      return &cgwtab[i];
    }
  }
  release(&cgwlock);
  return 0;
}

// the gateway whose inside subnet holds this address, or whose own
// address it is.
struct cgw *
cgw_by_addr(uint32 ip)
{
  acquire(&cgwlock);
  for (int i = 0; i < NCGW; i++) {
    struct cgw *g = &cgwtab[i];
    if (g->used && g->mask && (ip & g->mask) == (g->addr & g->mask)) {
      release(&cgwlock);
      return g;
    }
  }
  release(&cgwlock);
  return 0;
}

// a frame addressed to one of a gateway's two ports. *pub says which.
struct cgw *
cgw_by_mac(const uint8 *mac, int *pub)
{
  acquire(&cgwlock);
  for (int i = 0; i < NCGW; i++) {
    struct cgw *g = &cgwtab[i];
    if (!g->used)
      continue;
    if (memcmp(g->mac, mac, ETHADDR_LEN) == 0) {
      if (pub)
        *pub = 0;
      release(&cgwlock);
      return g;
    }
    if (memcmp(g->pub_mac, mac, ETHADDR_LEN) == 0) {
      if (pub)
        *pub = 1;
      release(&cgwlock);
      return g;
    }
  }
  release(&cgwlock);
  return 0;
}

// the gateway in slot i, if the slot is in use. The switch walks the
// slots to hand a broadcast to every gateway's public port.
struct cgw *
cgw_at(int i)
{
  if (i < 0 || i >= NCGW)
    return 0;
  return cgwtab[i].used ? &cgwtab[i] : 0;
}

// a gateway that answers for this address on the root subnet.
struct cgw *
cgw_by_pub(uint32 ip)
{
  acquire(&cgwlock);
  for (int i = 0; i < NCGW; i++) {
    struct cgw *g = &cgwtab[i];
    if (g->used && g->pub_ip == ip) {
      release(&cgwlock);
      return g;
    }
  }
  release(&cgwlock);
  return 0;
}

// name, the address it owns inside, the subnet that address is on, and
// its address on the root subnet (0: one is chosen).
int
cgw_create(const char *name, uint32 addr, uint32 mask, uint32 pub_ip,
           struct cgw **out)
{
  if (name == 0 || name[0] == 0 || addr == 0 || mask == 0)
    return -EINVAL;
  if (cgw_find(name))
    return -EEXIST;
  // the inside may not be the root subnet: a gateway is for somewhere
  // else, and overlapping the switch's own subnet would make the
  // routing table ambiguous for every machine on it.
  if (netcfg.mask && (addr & netcfg.mask) == (netcfg.ip & netcfg.mask))
    return -EADDRINUSE;
  // two gateways may own the same private subnet: they are two segments
  // that cannot see each other, and a machine behind each may have the
  // same address as the other's. What may not be shared is an address on
  // the switch, where everybody can see everybody.
  if (pub_ip && cgw_by_pub(pub_ip))
    return -EADDRINUSE;

  acquire(&cgwlock);
  struct cgw *g = 0;
  int idx = 0;
  for (int i = 0; i < NCGW; i++) {
    if (!cgwtab[i].used) {
      g = &cgwtab[i];
      idx = i;
      break;
    }
  }
  if (g == 0) {
    release(&cgwlock);
    return -ENOSPC;
  }
  struct spinlock lk = g->lock;
  memset(g, 0, sizeof(*g));
  g->lock = lk;
  g->used = 1;
  safestrcpy(g->name, name, CGW_NAMELEN);
  g->addr = addr;
  g->mask = mask;
  // its two hardware addresses, stable per slot: 0x40 inside, 0x50 out.
  // Guests are 0x10 and up (kernel/vvnet.c), so nothing collides.
  uint8 base[ETHADDR_LEN] = {0x52, 0x54, 0x00, 0x78, 0x76, 0x40};
  memmove(g->mac, base, ETHADDR_LEN);
  g->mac[5] = 0x40 + idx;
  memmove(g->pub_mac, base, ETHADDR_LEN);
  g->pub_mac[5] = 0x50 + idx;
  // and its address on the root subnet. The switch's own subnet is a
  // /24 from qemu's DHCP, whose pool starts at .15; .100 and up is
  // outside it, so a gateway can take one without asking.
  g->pub_ip = pub_ip ? pub_ip
                     : ((netcfg.ip & 0xffffff00) | (100 + (uint32)idx));
  g->next_port = CGW_PORT_LOW;
  release(&cgwlock);
  if (out)
    *out = g;
  return idx;
}

int
cgw_destroy(struct cgw *g)
{
  if (g == 0 || !g->used)
    return -ESRCH;
  // a gateway with machines behind it is not taken away from them
  if (cgw_members(g) > 0)
    return -EBUSY;
  acquire(&g->lock);
  for (int i = 0; i < CGW_PENDING; i++)
    if (g->pending[i]) {
      mbuffree(g->pending[i]);
      g->pending[i] = 0;
    }
  release(&g->lock);
  acquire(&cgwlock);
  g->used = 0;
  release(&cgwlock);
  return 0;
}

// the caller is shown each gateway under the lock rather than a copy of
// it: a gateway is six kilobytes, most of it the table of conversations,
// and a reader wants a dozen numbers from the front of it. What reads
// them writes into a buffer and cannot sleep.
void
cgw_walk(void (*fn)(const struct cgw *, void *), void *arg)
{
  acquire(&cgwlock);
  for (int i = 0; i < NCGW; i++)
    if (cgwtab[i].used)
      fn(&cgwtab[i], arg);
  release(&cgwlock);
}

// ---- checksums ----
//
// A translated packet keeps its checksums correct by arithmetic rather
// than by being summed again: a checksum is a sum, so replacing a word
// in it is subtracting the old word and adding the new (RFC 1624).
//
// Everything here works on the bytes as they sit in the packet, and the
// arithmetic is done in the machine's own order with ntohs/htons at the
// ends. Mixing the two -- summing the words as they lie in memory but
// passing the replacements the other way round -- gives a checksum that
// is wrong in a way nothing reports: the packet simply never comes back.
static uint16
csum_repl(uint16 sum, const void *oldp, const void *newp, int len)
{
  const uint8 *o = oldp, *n = newp;
  uint32 s = (uint16)~ntohs(sum);

  for (int i = 0; i + 1 < len; i += 2) {
    s += (uint16)~(uint16)((o[i] << 8) | o[i + 1]);
    s += (uint16)((n[i] << 8) | n[i + 1]);
    while (s >> 16)
      s = (s & 0xffff) + (s >> 16);
  }
  return htons((uint16)~s);
}

// ---- the NAT table ----

static uint
gw_secs(uint n)
{
  uint hz = tick_hz ? tick_hz : 100;
  return n * hz;
}

static uint
nat_life(const struct gwnat *n)
{
  switch (n->proto) {
  case IPPROTO_TCP: return gw_secs(n->closing ? 60 : 3600);
  case IPPROTO_UDP: return gw_secs(30);
  default:          return gw_secs(10);
  }
}

static int
nat_expired(const struct gwnat *n, uint now)
{
  return (uint)(now - n->last) > nat_life(n);
}

// the conversation this packet belongs to, going out. Creates one, with
// a port of its own on the public side, if there is none. caller holds
// g->lock.
static struct gwnat *
nat_out(struct cgw *g, uint8 proto, uint32 iip, uint16 iport, uint32 rip,
        uint16 rport)
{
  uint now = ticks;
  struct gwnat *free = 0, *lru = 0;

  for (int i = 0; i < NGWNAT; i++) {
    struct gwnat *n = &g->nat[i];
    if (n->proto == 0 || nat_expired(n, now)) {
      if (free == 0)
        free = n;
      continue;
    }
    if (n->proto == proto && n->iip == iip && n->iport == iport &&
        n->rip == rip && n->rport == rport) {
      n->last = now;
      return n;
    }
    if (lru == 0 || (int)(n->last - lru->last) < 0)
      lru = n;
  }
  struct gwnat *n = free ? free : lru;
  if (n == 0)
    return 0;
  if (free == 0)
    g->natfull++;
  // a port nobody else here is using. The pool is walked rather than
  // hashed: a hundred and twenty-eight conversations is a busy gateway,
  // and the walk is what the rest of this does anyway.
  uint16 p = 0;
  for (int tries = 0; tries < CGW_PORT_HIGH - CGW_PORT_LOW; tries++) {
    uint16 cand = g->next_port++;
    if (g->next_port >= CGW_PORT_HIGH)
      g->next_port = CGW_PORT_LOW;
    int taken = 0;
    for (int i = 0; i < NGWNAT; i++) {
      struct gwnat *o = &g->nat[i];
      if (o != n && o->proto == proto && o->pport == cand &&
          !nat_expired(o, now)) {
        taken = 1;
        break;
      }
    }
    if (!taken) {
      p = cand;
      break;
    }
  }
  if (p == 0)
    return 0;
  memset(n, 0, sizeof(*n));
  n->proto = proto;
  n->iip = iip;
  n->iport = iport;
  n->rip = rip;
  n->rport = rport;
  n->pport = p;
  n->last = now;
  g->translated++;
  return n;
}

// the conversation a packet arriving from outside belongs to. caller
// holds g->lock.
static struct gwnat *
nat_in(struct cgw *g, uint8 proto, uint16 pport, uint32 rip, uint16 rport)
{
  uint now = ticks;

  for (int i = 0; i < NGWNAT; i++) {
    struct gwnat *n = &g->nat[i];
    if (n->proto != proto || n->pport != pport || nat_expired(n, now))
      continue;
    // both ends, both ways: a published port that two callers use is two
    // conversations, and each one's answer leaves with its own port.
    if (n->rip == rip && n->rport == rport) {
      n->last = now;
      return n;
    }
  }
  return 0;
}

// a published port: a conversation that is allowed to arrive, pointed at
// the machine it was published for. caller holds g->lock.
static struct gwnat *
nat_publish(struct cgw *g, uint8 proto, uint16 pport, uint32 iip,
            uint16 iport, uint32 rip, uint16 rport)
{
  uint now = ticks;
  struct gwnat *free = 0, *lru = 0;

  for (int i = 0; i < NGWNAT; i++) {
    struct gwnat *n = &g->nat[i];
    if (n->proto == 0 || nat_expired(n, now)) {
      if (free == 0)
        free = n;
      continue;
    }
    if (lru == 0 || (int)(n->last - lru->last) < 0)
      lru = n;
  }
  struct gwnat *n = free ? free : lru;
  if (n == 0)
    return 0;
  memset(n, 0, sizeof(*n));
  n->proto = proto;
  n->iip = iip;
  n->iport = iport;
  n->pport = pport;
  n->rip = rip;
  n->rport = rport;
  n->published = 1;
  n->last = now;
  g->translated++;
  return n;
}

void
cgw_timer(void)
{
  uint now = ticks;

  for (int i = 0; i < NCGW; i++) {
    struct cgw *g = &cgwtab[i];
    if (!g->used)
      continue;
    acquire(&g->lock);
    for (int j = 0; j < NGWNAT; j++)
      if (g->nat[j].proto && nat_expired(&g->nat[j], now))
        g->nat[j].proto = 0;
    // a frame waiting for a hardware address does not wait for ever
    for (int j = 0; j < CGW_PENDING; j++)
      if (g->pending[j] && (uint)(now - g->pending_at[j]) > gw_secs(3)) {
        mbuffree(g->pending[j]);
        g->pending[j] = 0;
        g->dropped++;
      }
    release(&g->lock);
  }
}

// ---- frames ----

// the ethernet header, rewritten for the side the frame is leaving by.
static void
reframe(struct mbuf *m, const uint8 *shost, const uint8 *dhost, uint16 type)
{
  struct eth *e = (struct eth *)m->head;

  memmove(e->shost, shost, ETHADDR_LEN);
  memmove(e->dhost, dhost, ETHADDR_LEN);
  e->type = htons(type);
}

// the same, for a packet the gateway has just built: there is no header
// in front of it yet, so one is pushed rather than overwritten. (This is
// the difference that made the gateway's ARP replies arrive as fourteen
// bytes of nonsense followed by a truncated packet, and a machine behind
// it unable to resolve its own gateway.)
static void
addframe(struct mbuf *m, const uint8 *shost, const uint8 *dhost, uint16 type)
{
  struct eth *e = (struct eth *)mbufpush(m, sizeof(struct eth));

  memmove(e->shost, shost, ETHADDR_LEN);
  memmove(e->dhost, dhost, ETHADDR_LEN);
  e->type = htons(type);
}

static struct ip *
ip_of(struct mbuf *m)
{
  if (m->len < sizeof(struct eth) + sizeof(struct ip))
    return 0;
  struct eth *e = (struct eth *)m->head;
  if (ntohs(e->type) != ETHTYPE_IP)
    return 0;
  struct ip *h = (struct ip *)(m->head + sizeof(*e));
  uint hl = (h->vhl & 0xf) * 4;
  if ((h->vhl >> 4) != 4 || hl < sizeof(*h) ||
      m->len < sizeof(*e) + hl + 4)
    return 0;
  return h;
}

// one hop: a router decrements what it forwards, and what reaches zero
// goes no further.
static int
hop(struct ip *h)
{
  if (h->ttl <= 1)
    return -1;
  uint8 was[2] = {h->ttl, h->pro};
  uint8 now[2] = {h->ttl - 1, h->pro};

  h->ttl--;
  h->sum = csum_repl(h->sum, was, now, 2);
  return 0;
}

// the ports of a packet, and where they live in it.
static uint16 *
l4_sport(struct ip *h)
{
  uint hl = (h->vhl & 0xf) * 4;
  return (uint16 *)((uint8 *)h + hl);
}

static uint16 *
l4_dport(struct ip *h)
{
  return l4_sport(h) + 1;
}

// is there a checksum above the IP header to adjust? TCP always has
// one. UDP may carry none, which is written as a zero, and adjusting
// that zero would turn "none" into a wrong one.
static int
has_l4_sum(struct ip *h, uint16 *sum)
{
  if (sum == 0)
    return 0;
  if (h->pro == IPPROTO_UDP)
    return *sum != 0;
  return 1;
}

// the checksum of whatever is above the IP header, if it has one that
// covers the addresses.
static uint16 *
l4_sum(struct ip *h, int *pseudo)
{
  uint hl = (h->vhl & 0xf) * 4;
  uint8 *l4 = (uint8 *)h + hl;

  *pseudo = 1;
  if (h->pro == IPPROTO_TCP)
    return (uint16 *)(l4 + 16);
  if (h->pro == IPPROTO_UDP)
    return (uint16 *)(l4 + 6);
  if (h->pro == IPPROTO_ICMP) {
    *pseudo = 0;                 // ICMP's checksum is of itself alone
    return (uint16 *)(l4 + 2);
  }
  return 0;
}

// send a frame out of the public side. Takes ownership. The next hop is
// the far end itself when it is on the root subnet, and the switch's own
// gateway otherwise; either way it takes a hardware address, which the
// gateway asks for in its own name.
static void
pub_send(struct cgw *g, struct mbuf *m, uint32 dst)
{
  uint32 nexthop = dst;

  if (netcfg.mask && (dst & netcfg.mask) != (netcfg.ip & netcfg.mask))
    nexthop = netcfg.gw;
  uint8 mac[ETHADDR_LEN];
  int have = 0;

  acquire(&g->lock);
  if (g->nh_valid && g->nh_ip == nexthop) {
    memmove(mac, g->nh_mac, ETHADDR_LEN);
    have = 1;
  }
  if (!have) {
    // park it and ask. The next frame of the conversation, or the
    // answer to this question, is what sends it.
    int parked = 0;
    for (int i = 0; i < CGW_PENDING; i++) {
      if (g->pending[i] == 0) {
        g->pending[i] = m;
        g->pending_at[i] = ticks;
        g->pending_ip[i] = nexthop;
        parked = 1;
        break;
      }
    }
    if (!parked) {
      mbuffree(m);
      g->dropped++;
    }
    int ask = g->nh_ip != nexthop || (uint)(ticks - g->nh_asked) >= gw_secs(1);
    g->nh_ip = nexthop;
    g->nh_valid = 0;
    if (ask)
      g->nh_asked = ticks;
    release(&g->lock);
    if (ask)
      cgw_arp_request(g, nexthop);
    return;
  }
  release(&g->lock);
  reframe(m, g->pub_mac, mac, ETHTYPE_IP);
  g->tx++;
  bridge_from_cgw(g, m, 1);
}

// an ARP request or reply from one of the gateway's two ports.
void
cgw_arp_request(struct cgw *g, uint32 target)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);

  if (m == 0)
    return;
  struct arp *a = (struct arp *)mbufput(m, sizeof(*a));
  a->hrd = htons(1);
  a->pro = htons(ETHTYPE_IP);
  a->hln = ETHADDR_LEN;
  a->pln = 4;
  a->op = htons(ARP_OP_REQUEST);
  memmove(a->sha, g->pub_mac, ETHADDR_LEN);
  a->sip = htonl(g->pub_ip);
  memset(a->tha, 0, ETHADDR_LEN);
  a->tip = htonl(target);
  addframe(m, g->pub_mac, bcast_mac, ETHTYPE_ARP);
  bridge_from_cgw(g, m, 1);
}

// the namespaces behind this gateway (below): what is on the inside is
// either a machine on the switch or an interface in a namespace, and the
// answers above have to be able to go to either.
static struct gwport *ns_port(struct cgw *g, uint32 ip);
static int ns_deliver(struct cgw *g, uint32 ip, struct mbuf *m, int reframe_it);

static void
arp_answer(struct cgw *g, int pub, const uint8 *tha, uint32 tip, uint32 sip)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);

  if (m == 0)
    return;
  const uint8 *mine = pub ? g->pub_mac : g->mac;
  struct arp *a = (struct arp *)mbufput(m, sizeof(*a));
  a->hrd = htons(1);
  a->pro = htons(ETHTYPE_IP);
  a->hln = ETHADDR_LEN;
  a->pln = 4;
  a->op = htons(ARP_OP_REPLY);
  memmove(a->sha, mine, ETHADDR_LEN);
  a->sip = htonl(sip);
  memmove(a->tha, tha, ETHADDR_LEN);
  a->tip = htonl(tip);
  addframe(m, mine, tha, ETHTYPE_ARP);
  if (!pub && ns_deliver(g, tip, m, 0) == 0)
    return;                // it was a container asking
  bridge_from_cgw(g, m, pub);
}

// an ICMP echo to one of the gateway's own addresses, answered by the
// gateway: it is a router, and a router answers for itself.
static int
echo_reply(struct cgw *g, int pub, struct mbuf *m, struct ip *h)
{
  struct icmp *ic = (struct icmp *)((uint8 *)h + (h->vhl & 0xf) * 4);

  if (h->pro != IPPROTO_ICMP || ic->type != ICMP_ECHO)
    return -1;
  struct eth *e = (struct eth *)m->head;
  uint8 peer[ETHADDR_LEN];
  memmove(peer, e->shost, ETHADDR_LEN);
  ic->type = ICMP_ECHOREPLY;
  uint8 was[2] = {ICMP_ECHO, ic->code};
  uint8 now[2] = {ICMP_ECHOREPLY, ic->code};
  ic->sum = csum_repl(ic->sum, was, now, 2);
  uint32 was_dst = h->dst;
  h->dst = h->src;
  h->src = was_dst;
  h->ttl = 64;
  h->sum = 0;
  h->sum = ip_checksum(h, (h->vhl & 0xf) * 4, 0);
  reframe(m, pub ? g->pub_mac : g->mac, peer, ETHTYPE_IP);
  g->tx++;
  if (!pub && ns_deliver(g, ntohl(h->dst), m, 0) == 0)
    return 0;
  bridge_from_cgw(g, m, pub);
  return 0;
}

// ---- the namespaces behind a gateway ----
//
// A machine behind a gateway is a port on the switch, found by its
// hardware address. A container is not on the switch at all: its packets
// come out of the kernel's own stack in a namespace of its own, and the
// wire between the two is this -- an interface inside the namespace
// whose transmit function is the gateway's inside door, and a table here
// so that a frame for its address can be put back through it.
static int
ns_tx(struct netif *dev, struct mbuf *m)
{
  struct gwport *p = dev->priv;
  struct cgw *g = p ? p->gw : 0;

  if (g == 0 || p->ns == 0) {
    mbuffree(m);
    return -1;
  }
  // a container may only speak as itself. The stack picks the source
  // from the interface, so this is about what a raw socket could do.
  struct ip *h = ip_of(m);
  if (h && ntohl(h->src) != p->ip) {
    mbuffree(m);
    g->dropped++;
    return -1;
  }
  cgw_rx_internal(g, 0, m);
  return 0;
}

// which namespace behind this gateway has that address, if any
static struct gwport *
ns_port(struct cgw *g, uint32 ip)
{
  struct gwport *p = 0;

  acquire(&g->lock);
  for (int i = 0; i < NGWPORT; i++)
    if (g->ports[i].ns && g->ports[i].ip == ip && g->ports[i].dev) {
      p = &g->ports[i];
      break;
    }
  release(&g->lock);
  return p;
}

// put a frame through that port. Takes ownership. 0 if it went, -1 if
// there is nothing of that address here.
static int
ns_deliver(struct cgw *g, uint32 ip, struct mbuf *m, int reframe_it)
{
  struct gwport *p = ns_port(g, ip);

  if (p == 0)
    return -1;
  if (reframe_it)
    reframe(m, g->mac, p->mac, ETHTYPE_IP);
  g->tx++;
  net_rx_dev(p->dev, m);
  return 0;
}

// attach a namespace: an interface in it with this address, a route to
// the gateway's subnet and a default route through the gateway.
int
cgw_attach_ns(struct cgw *g, struct netns *ns, uint32 ip, const char *name)
{
  struct gwport *p = 0;

  if (g == 0 || ns == 0 || ip == 0)
    return -EINVAL;
  if ((ip & g->mask) != (g->addr & g->mask))
    return -EINVAL;        // not on the subnet this gateway owns
  if (ip == g->addr)
    return -EADDRINUSE;
  acquire(&g->lock);
  for (int i = 0; i < NGWPORT; i++) {
    if (g->ports[i].ns == 0 && p == 0)
      p = &g->ports[i];
    else if (g->ports[i].ns && g->ports[i].ip == ip)
      p = 0, i = NGWPORT;  // taken
  }
  if (p == 0) {
    release(&g->lock);
    return -EADDRINUSE;
  }
  p->ns = ns;
  p->gw = g;
  p->ip = ip;
  // a hardware address of its own, from the gateway's and the last octet
  // of the address: locally administered, and different for every port
  memmove(p->mac, g->mac, ETHADDR_LEN);
  p->mac[0] = 0x02;
  p->mac[5] = (uint8)(ip & 0xff);
  release(&g->lock);

  struct netif *dev = netif_add(ns, name && name[0] ? name : "eth0", 0, ns_tx, p);
  if (dev == 0) {
    acquire(&g->lock);
    p->ns = 0;
    release(&g->lock);
    return -ENOSPC;
  }
  memmove(dev->mac, p->mac, ETHADDR_LEN);
  p->dev = dev;
  netns_configure(ns, dev->name, ip, g->mask, g->addr);
  return 0;
}

void
cgw_detach_ns(struct netns *ns)
{
  if (ns == 0)
    return;
  for (int i = 0; i < NCGW; i++) {
    struct cgw *g = cgw_at(i);
    if (g == 0 || !g->used)
      continue;
    acquire(&g->lock);
    for (int j = 0; j < NGWPORT; j++) {
      if (g->ports[j].ns != ns)
        continue;
      uint32 ip = g->ports[j].ip;
      g->ports[j].ns = 0;
      g->ports[j].dev = 0;      // the namespace frees the interface
      g->ports[j].ip = 0;
      // and nothing published for an address that is gone stays published
      for (int k = 0; k < NGWPUB; k++)
        if (g->pub[k].proto && g->pub[k].ip == ip)
          g->pub[k].proto = 0;
      for (int k = 0; k < NGWNAT; k++)
        if (g->nat[k].proto && g->nat[k].iip == ip)
          g->nat[k].proto = 0;
    }
    release(&g->lock);
  }
}

// a port on the outside that belongs to something inside
int
cgw_publish_ns(struct cgw *g, uint8 proto, uint16 pport, uint32 ip,
               uint16 cport)
{
  if (g == 0 || proto == 0 || pport == 0 || ip == 0)
    return -EINVAL;
  acquire(&g->lock);
  struct gwpub *free = 0;
  for (int i = 0; i < NGWPUB; i++) {
    if (g->pub[i].proto == proto && g->pub[i].pport == pport) {
      release(&g->lock);
      return -EADDRINUSE;
    }
    if (g->pub[i].proto == 0 && free == 0)
      free = &g->pub[i];
  }
  if (free == 0) {
    release(&g->lock);
    return -ENOSPC;
  }
  free->proto = proto;
  free->pport = pport;
  free->ip = ip;
  free->cport = cport ? cport : pport;
  release(&g->lock);
  return 0;
}

int
cgw_unpublish_ns(struct cgw *g, uint8 proto, uint16 pport)
{
  if (g == 0)
    return -EINVAL;
  acquire(&g->lock);
  for (int i = 0; i < NGWPUB; i++)
    if (g->pub[i].proto == proto && g->pub[i].pport == pport) {
      g->pub[i].proto = 0;
      release(&g->lock);
      return 0;
    }
  release(&g->lock);
  return -ENOENT;
}

// caller holds g->lock: what a published port outside names inside
static struct gwpub *
pub_find(struct cgw *g, uint8 proto, uint16 pport)
{
  for (int i = 0; i < NGWPUB; i++)
    if (g->pub[i].proto == proto && g->pub[i].pport == pport)
      return &g->pub[i];
  return 0;
}

// ---- inside ----

// a frame from a machine behind the gateway, addressed to the gateway.
// Takes ownership.
void
cgw_rx_internal(struct cgw *g, struct vm *from, struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;

  g->rx++;
  if (m->len < sizeof(*e)) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  uint16 type = ntohs(e->type);
  if (type == ETHTYPE_ARP) {
    if (m->len < sizeof(*e) + sizeof(struct arp)) {
      mbuffree(m);
      return;
    }
    struct arp *a = (struct arp *)(m->head + sizeof(*e));
    uint32 tip = ntohl(a->tip);
    // the gateway answers for itself, and for the namespaces behind it.
    // Answering for them is what makes it a router rather than a switch:
    // two containers on one subnet reach each other *through* it, where
    // the policy and the counters are, and neither of them has a wire to
    // the other. (A machine behind it is on the switch, which broadcasts
    // the question to its own ports and needs no help here.)
    if (ntohs(a->op) == ARP_OP_REQUEST &&
        (tip == g->addr || ns_port(g, tip) != 0))
      arp_answer(g, 0, a->sha, ntohl(a->sip), tip);
    mbuffree(m);
    return;
  }
  struct ip *h = ip_of(m);
  if (h == 0) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  uint32 src = ntohl(h->src), dst = ntohl(h->dst);
  // a machine may only speak as itself: the address it was given is the
  // address it has, and a gateway is where that is enforced.
  if (from && from->cfg.ip && src != from->cfg.ip) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  if (dst == g->addr) {
    if (echo_reply(g, 0, m, h) == 0)
      return;
    mbuffree(m);           // nothing else listens on a router
    g->dropped++;
    return;
  }
  if (hop(h) < 0) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  // somewhere else on the inside: forward it there.
  if ((dst & g->mask) == (g->addr & g->mask)) {
    if (ns_deliver(g, dst, m, 1) == 0)
      return;
    struct vm *to = cgw_member_by_ip(g, dst);
    if (to == 0) {
      mbuffree(m);
      g->dropped++;
      return;
    }
    reframe(m, g->mac, to->vnet.mac, ETHTYPE_IP);
    g->tx++;
    bridge_to_vm(to, m);
    return;
  }

  // the world. Translate it to the public address and a port of the
  // gateway's own, and remember which is which.
  int pseudo = 0;
  uint16 *sum = l4_sum(h, &pseudo);
  uint16 sport = 0;
  if (h->pro == IPPROTO_TCP || h->pro == IPPROTO_UDP)
    sport = ntohs(*l4_sport(h));
  else if (h->pro == IPPROTO_ICMP)
    sport = ntohs(((struct icmp *)((uint8 *)h + (h->vhl & 0xf) * 4))->id);
  else {
    mbuffree(m);          // nothing else can be translated
    g->dropped++;
    return;
  }
  uint16 dport = (h->pro == IPPROTO_TCP || h->pro == IPPROTO_UDP)
                     ? ntohs(*l4_dport(h))
                     : 0;

  acquire(&g->lock);
  struct gwnat *n = nat_out(g, h->pro, src, sport, dst, dport);
  if (n == 0) {
    release(&g->lock);
    mbuffree(m);
    g->dropped++;
    return;
  }
  uint16 pport = n->pport;
  if (h->pro == IPPROTO_TCP) {
    struct tcp *t = (struct tcp *)((uint8 *)h + (h->vhl & 0xf) * 4);
    if (t->flags & (TCP_FIN | TCP_RST))
      n->closing = 1;
  }
  n->packets++;
  release(&g->lock);

  // the address first, then the port, and the checksums with them
  uint32 newsrc = htonl(g->pub_ip);
  h->sum = csum_repl(h->sum, &h->src, &newsrc, 4);
  if (pseudo && has_l4_sum(h, sum))
    *sum = csum_repl(*sum, &h->src, &newsrc, 4);
  h->src = newsrc;
  uint16 newport = htons(pport);
  if (h->pro == IPPROTO_ICMP) {
    struct icmp *ic = (struct icmp *)((uint8 *)h + (h->vhl & 0xf) * 4);
    ic->sum = csum_repl(ic->sum, &ic->id, &newport, 2);
    ic->id = newport;
  } else {
    if (has_l4_sum(h, sum))
      *sum = csum_repl(*sum, l4_sport(h), &newport, 2);
    *l4_sport(h) = newport;
  }
  (void)sport;
  g->snat++;
  pub_send(g, m, dst);
}

// ---- outside ----

// a frame from the root subnet addressed to the gateway's public port.
// Takes ownership.
void
cgw_rx_public(struct cgw *g, struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;

  g->rx++;
  if (m->len < sizeof(*e)) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  uint16 type = ntohs(e->type);
  if (type == ETHTYPE_ARP) {
    if (m->len < sizeof(*e) + sizeof(struct arp)) {
      mbuffree(m);
      return;
    }
    struct arp *a = (struct arp *)(m->head + sizeof(*e));
    uint16 op = ntohs(a->op);
    if (op == ARP_OP_REQUEST && ntohl(a->tip) == g->pub_ip) {
      arp_answer(g, 1, a->sha, ntohl(a->sip), g->pub_ip);
    } else if (op == ARP_OP_REPLY) {
      // the answer to its own question: remember it, and send what was
      // waiting for it
      struct mbuf *waiting[CGW_PENDING];
      uint32 ip = ntohl(a->sip);
      int nw = 0;
      acquire(&g->lock);
      if (g->nh_ip == ip) {
        memmove(g->nh_mac, a->sha, ETHADDR_LEN);
        g->nh_valid = 1;
        for (int i = 0; i < CGW_PENDING; i++) {
          if (g->pending[i] && g->pending_ip[i] == ip) {
            waiting[nw++] = g->pending[i];
            g->pending[i] = 0;
          }
        }
      }
      release(&g->lock);
      for (int i = 0; i < nw; i++) {
        reframe(waiting[i], g->pub_mac, a->sha, ETHTYPE_IP);
        g->tx++;
        bridge_from_cgw(g, waiting[i], 1);
      }
    }
    mbuffree(m);
    return;
  }
  struct ip *h = ip_of(m);
  if (h == 0) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  uint32 src = ntohl(h->src), dst = ntohl(h->dst);
  if (dst != g->pub_ip) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  uint16 sport = 0, dport = 0;
  if (h->pro == IPPROTO_TCP || h->pro == IPPROTO_UDP) {
    sport = ntohs(*l4_sport(h));
    dport = ntohs(*l4_dport(h));
  } else if (h->pro == IPPROTO_ICMP) {
    struct icmp *ic = (struct icmp *)((uint8 *)h + (h->vhl & 0xf) * 4);
    dport = ntohs(ic->id);       // the identifier is the port it has
    sport = 0;                   // and the far end has none
    // an echo to the public address is the gateway's own to answer;
    // a reply to something inside is translated below.
    if (ic->type == ICMP_ECHO) {
      if (echo_reply(g, 1, m, h) == 0)
        return;
      mbuffree(m);
      g->dropped++;
      return;
    }
  } else {
    mbuffree(m);
    g->dropped++;
    return;
  }

  acquire(&g->lock);
  struct gwnat *n = nat_in(g, h->pro, dport, src, sport);
  if (n == 0) {
    release(&g->lock);
    // nothing is expecting it. It may still be for a port somebody
    // behind the gateway has published.
    struct vm *to = 0;
    uint16 gport = 0;
    uint32 toip = 0;
    acquire(&g->lock);
    struct gwpub *pb = pub_find(g, h->pro, dport);
    if (pb) {
      toip = pb->ip;
      gport = pb->cport;
    }
    release(&g->lock);
    if (toip == 0) {
      if (cgw_published(g, h->pro, dport, &to, &gport) == 0) {
        mbuffree(m);
        g->dropped++;
        return;
      }
      toip = to->cfg.ip;
    }
    acquire(&g->lock);
    n = nat_publish(g, h->pro, dport, toip, gport, src, sport);
    if (n == 0) {
      release(&g->lock);
      mbuffree(m);
      g->dropped++;
      return;
    }
    g->dnat++;
  }
  uint32 iip = n->iip;
  uint16 iport = n->iport;
  if (h->pro == IPPROTO_TCP) {
    struct tcp *t = (struct tcp *)((uint8 *)h + (h->vhl & 0xf) * 4);
    if (t->flags & (TCP_FIN | TCP_RST))
      n->closing = 1;
  }
  n->replies++;
  release(&g->lock);

  struct vm *to = cgw_member_by_ip(g, iip);
  struct gwport *inside = to ? 0 : ns_port(g, iip);
  if (to == 0 && inside == 0) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  if (hop(h) < 0) {
    mbuffree(m);
    g->dropped++;
    return;
  }
  int pseudo = 0;
  uint16 *sum = l4_sum(h, &pseudo);
  uint32 newdst = htonl(iip);
  h->sum = csum_repl(h->sum, &h->dst, &newdst, 4);
  if (pseudo && has_l4_sum(h, sum))
    *sum = csum_repl(*sum, &h->dst, &newdst, 4);
  h->dst = newdst;
  uint16 newport = htons(iport);
  if (h->pro == IPPROTO_ICMP) {
    struct icmp *ic = (struct icmp *)((uint8 *)h + (h->vhl & 0xf) * 4);
    ic->sum = csum_repl(ic->sum, &ic->id, &newport, 2);
    ic->id = newport;
  } else {
    if (has_l4_sum(h, sum))
      *sum = csum_repl(*sum, l4_dport(h), &newport, 2);
    *l4_dport(h) = newport;
  }
  if (to == 0) {
    reframe(m, g->mac, inside->mac, ETHTYPE_IP);
    g->tx++;
    net_rx_dev(inside->dev, m);
    return;
  }
  reframe(m, g->mac, to->vnet.mac, ETHTYPE_IP);
  g->tx++;
  bridge_to_vm(to, m);
}
