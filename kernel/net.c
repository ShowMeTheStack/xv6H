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

struct netcfg netcfg;
struct spinlock netlock;
uint64 net_rx_packets, net_tx_packets, net_rx_dropped, net_tx_dropped, net_policy_dropped;

static uint16 ip_id = 1;

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

  // DHCP: broadcast discover/request, and renewals to the server.
  if (proto == IPPROTO_UDP && dport == 67 && (dst == 0xffffffff || dst == netcfg.gw))
    return 1;
  // DNS to the forwarder that lives beside the gateway.
  if (proto == IPPROTO_UDP && dport == 53 && dst == netcfg.dns)
    return 1;
  uint8 a = dst >> 24, b = (dst >> 16) & 0xff;
  if (dst == netcfg.gw || dst == netcfg.dns)
    return 0; // the laptop, and qemu's forwarder
  if (netcfg.up && netcfg.mask && (dst & netcfg.mask) == (netcfg.ip & netcfg.mask) &&
      dst != (netcfg.ip | ~netcfg.mask))
    return 1; // a guest or the host: never leaves the switch
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

static int
net_tx_eth(struct mbuf *m, const uint8 *dhost, uint16 type)
{
  struct eth *e = (struct eth *)mbufpush(m, sizeof(*e));
  memmove(e->dhost, dhost, ETHADDR_LEN);
  memmove(e->shost, netcfg.mac, ETHADDR_LEN);
  e->type = htons(type);
  if (pktcap_active())
    pktcap_frame(m->head, m->len);
  return bridge_tx_host(m);
}

// ---- arp ----

#define ARP_ENTRIES 16
#define ARP_PENDING 4

static struct arpent {
  uint32 ip;
  uint8 mac[ETHADDR_LEN];
  int valid;
  uint requested;              // tick of the last request
  struct mbuf *pending[ARP_PENDING]; // packets waiting for the answer
} arptab[ARP_ENTRIES];

static void
arp_send(uint16 op, const uint8 *dmac, uint32 tip, const uint8 *tmac)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return;
  struct arp *a = (struct arp *)mbufput(m, sizeof(*a));
  a->hrd = htons(ARP_HRD_ETHER);
  a->pro = htons(ETHTYPE_IP);
  a->hln = ETHADDR_LEN;
  a->pln = 4;
  a->op = htons(op);
  memmove(a->sha, netcfg.mac, ETHADDR_LEN);
  a->sip = htonl(netcfg.ip);
  memmove(a->tha, tmac, ETHADDR_LEN);
  a->tip = htonl(tip);
  net_tx_eth(m, dmac, ETHTYPE_ARP);
}

static struct arpent *
arp_find(uint32 ip)
{
  for (int i = 0; i < ARP_ENTRIES; i++)
    if (arptab[i].ip == ip)
      return &arptab[i];
  return 0;
}

static struct arpent *
arp_entry(uint32 ip)
{
  struct arpent *e = arp_find(ip);
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
arp_rx(struct mbuf *m)
{
  struct arp *a = (struct arp *)m->head;
  if (m->len < sizeof(*a) || ntohs(a->hrd) != ARP_HRD_ETHER || ntohs(a->pro) != ETHTYPE_IP)
    goto out;
  uint32 sip = ntohl(a->sip), tip = ntohl(a->tip);

  // learn the sender (also from requests, as everyone does).
  struct arpent *e = arp_find(sip);
  if (e || tip == netcfg.ip) {
    if (!e)
      e = arp_entry(sip);
    memmove(e->mac, a->sha, ETHADDR_LEN);
    e->valid = 1;
    for (int j = 0; j < ARP_PENDING; j++) {
      if (e->pending[j]) {
        struct mbuf *p = e->pending[j];
        e->pending[j] = 0;
        net_tx_eth(p, e->mac, ETHTYPE_IP);
      }
    }
  }
  if (ntohs(a->op) == ARP_OP_REQUEST && netcfg.up && tip == netcfg.ip)
    arp_send(ARP_OP_REPLY, a->sha, sip, a->sha);
out:
  mbuffree(m);
}

// walk the arp cache for /proc/net/arp. the caller holds netlock,
// which is what everything else that touches the table holds.
void
arp_walk(void (*fn)(uint32 ip, const uint8 *mac, int valid, void *arg), void *arg)
{
  for (int i = 0; i < ARP_ENTRIES; i++)
    if (arptab[i].ip)
      fn(arptab[i].ip, arptab[i].mac, arptab[i].valid, arg);
}

// ---- ipv4 ----

// loopback: a packet to one of our own addresses goes back into the
// stack rather than to the wire (see below).
static int loopback_send(struct mbuf *m);
static int is_local(uint32 dst);

// send m as an IP packet to dst. takes ownership. caller holds netlock.
// `ttl` of zero means the usual 64; a smaller one makes the packet
// expire on the way, which is how traceroute finds the routers.
static int
ip_send_ttl(struct mbuf *m, uint32 dst, uint8 proto, int ttl)
{
  // the interface has to be up to reach anything but ourselves
  if (!netcfg.up && !(proto == IPPROTO_UDP) && (dst >> 24) != 127) {
    mbuffree(m);
    return -1;
  }
  struct ip *h = (struct ip *)mbufpush(m, sizeof(*h));
  h->vhl = 0x45;
  h->tos = 0;
  h->len = htons(m->len);
  h->id = htons(ip_id++);
  h->off = 0;
  h->ttl = ttl > 0 && ttl < 256 ? (uint8)ttl : 64;
  h->pro = proto;
  h->sum = 0;
  // a packet to 127.x comes from 127.x, whatever this machine's own
  // address is -- and may be sent before there is one
  h->src = htonl((dst >> 24) == 127 ? dst : netcfg.ip);
  h->dst = htonl(dst);
  h->sum = ip_checksum(h, sizeof(*h), 0);

  if (is_local(dst))
    return loopback_send(m);

  if (dst == 0xffffffff)
    return net_tx_eth(m, broadcast_mac, ETHTYPE_IP);

  uint32 nexthop = dst;
  if (netcfg.mask && (dst & netcfg.mask) != (netcfg.ip & netcfg.mask))
    nexthop = netcfg.gw;
  struct arpent *e = arp_entry(nexthop);
  if (e->valid)
    return net_tx_eth(m, e->mac, ETHTYPE_IP);

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
    net_tx_dropped++;
  }
  // one request in flight per tenth of a second, whatever a tick is
  if (ticks - e->requested >= (uint)(tick_hz / 10 ? tick_hz / 10 : 1) ||
      e->requested == 0) {
    e->requested = ticks ? ticks : 1;
    arp_send(ARP_OP_REQUEST, broadcast_mac, nexthop, broadcast_mac);
  }
  return 0;
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
net_tx_ip_ttl(struct mbuf *m, uint32 dst, uint8 proto, int ttl)
{
  return ip_send_ttl(m, dst, proto, ttl);
}

// ---- icmp ----

static void
icmp_rx(struct mbuf *m, struct ip *iph)
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
    if (net_policy_ok(src, IPPROTO_ICMP, 0, 0) || (netcfg.mask && (src & netcfg.mask) == (netcfg.ip & netcfg.mask)))
      ip_send(m, src, IPPROTO_ICMP);
    else
      mbuffree(m);
    return;
  }
  if (ic->type == ICMP_ECHOREPLY) {
    if (sock_icmp_rx(m, src, ntohs(ic->id)) == 0)
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
      sock_icmp_error(src, ic->type, ic->code, id, seq);
    } else if (inner->pro == IPPROTO_UDP) {
      uint16 sport = (l4[0] << 8) | l4[1];
      sock_udp_error(src, ic->type, ic->code, sport);
    }
    goto drop;
  }
drop:
  mbuffree(m);
}

int
net_tx_icmp_echo(uint32 dst, uint16 id, uint16 seq, char *data, int n, int ttl)
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
  int r = ip_send_ttl(m, dst, IPPROTO_ICMP, ttl);
  release(&netlock);
  return r;
}

// ---- udp ----

// takes ownership of m (payload already in place). caller holds netlock.
static int
udp_send(struct mbuf *m, uint32 dst, uint16 sport, uint16 dport, int ttl)
{
  struct udp *u = (struct udp *)mbufpush(m, sizeof(*u));
  u->sport = htons(sport);
  u->dport = htons(dport);
  u->ulen = htons(m->len);
  u->sum = 0;
  u->sum = l4_checksum(netcfg.ip, dst, IPPROTO_UDP, u, m->len);
  if (u->sum == 0)
    u->sum = 0xffff;
  return ip_send_ttl(m, dst, IPPROTO_UDP, ttl);
}

int
net_tx_udp(struct mbuf *m, uint32 dst, uint16 sport, uint16 dport, int ttl)
{
  acquire(&netlock);
  int r = udp_send(m, dst, sport, dport, ttl);
  release(&netlock);
  return r;
}

static void dhcp_rx(struct mbuf *m, uint32 src);

// an error about a packet that could not be delivered. the message
// carries the head of the offending packet -- its IP header and the
// first eight bytes after it -- which is how the sender tells which
// of its packets the complaint is about. caller holds netlock.
static void
icmp_send_error(uint32 dst, int type, int code, const struct ip *iph,
                const void *l4, int l4len)
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
  ip_send(m, dst, IPPROTO_ICMP);
}

static void
udp_rx(struct mbuf *m, struct ip *iph)
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
  if (dport == 68) {
    dhcp_rx(m, src);
    return;
  }
  if (sock_udp_rx(m, src, sport, dport) < 0) {
    net_rx_dropped++;
    // nobody has that port. saying so is what every other stack does,
    // and it is what makes a UDP connection fail at once rather than
    // wait for a timeout. `u` still points at the header the pull
    // above stepped over.
    icmp_send_error(src, ICMP_UNREACH, 3, iph, u, sizeof(*u));
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
static struct mbuf *lo_head, *lo_tail;
static int lo_delivering;

static void ip_rx(struct mbuf *m);

static int
loopback_send(struct mbuf *m)
{
  m->next = 0;
  if (lo_tail)
    lo_tail->next = m;
  else
    lo_head = m;
  lo_tail = m;
  if (lo_delivering)
    return 0;

  lo_delivering = 1;
  while (lo_head) {
    struct mbuf *p = lo_head;
    lo_head = p->next;
    if (lo_head == 0)
      lo_tail = 0;
    p->next = 0;
    ip_rx(p);
  }
  lo_delivering = 0;
  return 0;
}

// is this address one of ours?
static int
is_local(uint32 dst)
{
  return (dst >> 24) == 127 || (netcfg.up && dst == netcfg.ip);
}

static void
ip_rx(struct mbuf *m)
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
  if (netcfg.up && dst != netcfg.ip && dst != 0xffffffff && !is_local(dst))
    goto drop;
  m->len = len;
  mbufpull(m, hl);
  switch (h->pro) {
  case IPPROTO_ICMP:
    icmp_rx(m, h);
    return;
  case IPPROTO_UDP:
    udp_rx(m, h);
    return;
  case IPPROTO_TCP:
    tcp_rx(m, ntohl(h->src), dst);
    return;
  }
drop:
  net_rx_dropped++;
  mbuffree(m);
}

// a frame from the NIC (or from the bridge). takes ownership.
void
net_rx(struct mbuf *m)
{
  struct eth *e = (struct eth *)m->head;
  if (m->len < sizeof(*e)) {
    mbuffree(m);
    return;
  }
  // captured before anything is taken off the front, so what a reader
  // sees is the frame as it arrived
  if (pktcap_active())
    pktcap_frame(m->head, m->len);
  uint16 type = ntohs(e->type);
  mbufpull(m, sizeof(*e));
  acquire(&netlock);
  if (type == ETHTYPE_ARP)
    arp_rx(m);
  else if (type == ETHTYPE_IP)
    ip_rx(m);
  else
    mbuffree(m);
  release(&netlock);
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

static struct {
  int state;        // 0 idle, 1 discovering, 2 requesting, 3 bound
  uint32 xid;
  uint32 offered, server;
  uint last_tx;
  int attempts;
} dhcp;

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
  d->xid = htonl(dhcp.xid);
  d->flags = htons(0x8000); // please broadcast the reply
  memmove(d->chaddr, netcfg.mac, ETHADDR_LEN);
  d->magic = htonl(0x63825363);
  uint8 *o = d->options;
  *o++ = 53; *o++ = 1; *o++ = type;
  if (type == DHCP_REQUEST) {
    *o++ = 50; *o++ = 4;
    uint32 r = htonl(dhcp.offered); memmove(o, &r, 4); o += 4;
    *o++ = 54; *o++ = 4;
    uint32 s = htonl(dhcp.server); memmove(o, &s, 4); o += 4;
  }
  *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6; // subnet, router, dns
  *o++ = 255;
  m->len = (char *)o - (char *)d; // trim unused option space
  uint32 saved_ip = netcfg.ip;
  netcfg.ip = 0; // source 0.0.0.0 while unconfigured
  udp_send(m, 0xffffffff, 68, 67, 0);
  netcfg.ip = saved_ip;
  dhcp.last_tx = ticks;
}

static void
dhcp_rx(struct mbuf *m, uint32 src)
{
  struct dhcp *d = (struct dhcp *)m->head;
  if (m->len < sizeof(*d) - sizeof(d->options) + 4 || ntohl(d->xid) != dhcp.xid ||
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
  if (type == DHCP_OFFER && dhcp.state == 1) {
    dhcp.offered = ntohl(d->yiaddr);
    dhcp.server = server ? server : src;
    dhcp.state = 2;
    dhcp_send(DHCP_REQUEST);
  } else if (type == DHCP_ACK && dhcp.state == 2) {
    netcfg.ip = ntohl(d->yiaddr);
    netcfg.mask = mask ? mask : 0xffffff00;
    netcfg.gw = gw;
    netcfg.dns = dns ? dns : gw;
    netcfg.up = 1;
    netcfg.dhcp = 1;
    dhcp.state = 3;
    printk("net: dhcp: ip %d.%d.%d.%d gateway %d.%d.%d.%d dns %d.%d.%d.%d\n", netcfg.ip >> 24,
           (netcfg.ip >> 16) & 0xff, (netcfg.ip >> 8) & 0xff, netcfg.ip & 0xff, netcfg.gw >> 24,
           (netcfg.gw >> 16) & 0xff, (netcfg.gw >> 8) & 0xff, netcfg.gw & 0xff, netcfg.dns >> 24,
           (netcfg.dns >> 16) & 0xff, (netcfg.dns >> 8) & 0xff, netcfg.dns & 0xff);
    wakeup(&netcfg);
  } else if (type == DHCP_NAK) {
    dhcp.state = 1;
    dhcp.xid++;
    dhcp_send(DHCP_DISCOVER);
  }
out:
  mbuffree(m);
}

// called from the timer tick on hart 0 (interrupt context).
void
net_timer(void)
{
  if (!virtio_net_present())
    return;
  acquire(&netlock);
  if ((dhcp.state == 1 || dhcp.state == 2) &&
      ticks - dhcp.last_tx >= (uint)(2 * tick_hz)) {
    // no answer in 2 seconds: try again (from the start after a while).
    if (++dhcp.attempts % 4 == 0)
      dhcp.state = 1;
    dhcp_send(dhcp.state == 1 ? DHCP_DISCOVER : DHCP_REQUEST);
  }
  tcp_timer();
  release(&netlock);
  sock_timer();
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
  dhcp.state = 0;
  release(&netlock);
  wakeup(&netcfg);
}

void
net_stats(struct netinfo *ni)
{
  acquire(&netlock);
  ni->ip = netcfg.ip;
  ni->mask = netcfg.mask;
  ni->gw = netcfg.gw;
  ni->dns = netcfg.dns;
  memmove(ni->mac, netcfg.mac, 6);
  ni->up = netcfg.up;
  ni->dhcp = netcfg.dhcp;
  ni->rx_packets = net_rx_packets;
  ni->tx_packets = net_tx_packets;
  ni->rx_dropped = net_rx_dropped;
  ni->tx_dropped = net_tx_dropped;
  ni->policy_dropped = net_policy_dropped;
  release(&netlock);
}

void
netinit(void)
{
  pktcap_init();
  pktcap_register();
  initlock(&netlock, "net");
  sockinit();
  if (virtio_net_init() < 0) {
    printk("net: no network device\n");
    return;
  }
  if (plat.net_ip) {
    net_set_static(plat.net_ip, plat.net_mask, plat.net_gw, plat.net_dns);
    printk("net: static ip %d.%d.%d.%d\n", netcfg.ip >> 24, (netcfg.ip >> 16) & 0xff,
           (netcfg.ip >> 8) & 0xff, netcfg.ip & 0xff);
    return;
  }
  acquire(&netlock);
  dhcp.xid = 0x78763648 ^ (netcfg.mac[5] << 8) ^ netcfg.mac[4];
  dhcp.state = 1;
  dhcp_send(DHCP_DISCOVER);
  release(&netlock);
}
