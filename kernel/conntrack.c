//
// connection tracking.
//
// A conversation is named by five numbers: the protocol, and the two
// addresses and two ports of the packet that started it. The packets
// coming back are the same five with the two ends exchanged, and
// knowing which of the two a packet is -- the way out or the way back
// -- is the whole point. It is what lets the machine say yes to a
// packet from outside because somebody inside asked for it, and no to
// one that nobody asked for, without either keeping the door open or
// closing it.
//
// The table is small and searched linearly. A machine with a few
// hundred conversations open is a busy machine, and the search costs
// less than the hash would on this scale; when the table is full the
// least recently used entry goes, so a flood of new flows cannot stop
// the ones that are working.
//
// Timeouts, which are the only thing that makes the table finite: a TCP
// conversation lives as long as it is used and for sixty seconds after
// it closes, UDP for thirty seconds of quiet, ICMP for ten. A TCP reset
// ends one at once, because that is what a reset means.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "net.h"

static struct spinlock ctlock;
static struct ct cttab[NCONNTRACK];
static uint64 ct_seq;        // for least-recently-used, and for a stable id
uint64 ct_created, ct_dropped, ct_early;

void
ctinit(void)
{
  initlock(&ctlock, "conntrack");
}

// seconds, in whatever a tick is on this machine.
static uint
secs(uint n)
{
  uint hz = tick_hz ? tick_hz : 100;
  return n * hz;
}

static uint
timeout_of(const struct ct *c)
{
  switch (c->proto) {
  case IPPROTO_TCP:
    switch (c->state) {
    case CT_SYN:     return secs(30);
    case CT_ESTAB:   return secs(3600);   // used, so it stays
    case CT_CLOSING: return secs(60);     // closed, plus the sixty
    default:         return secs(10);
    }
  case IPPROTO_UDP:  return secs(30);
  default:           return secs(10);     // ICMP and everything else
  }
}

// the five numbers of a packet, as it is written on the wire. Returns 0
// for anything this does not track (a fragment, a header that does not
// fit, a protocol without ports).
static int
tuple_of(const uint8 *p, int len, struct ctkey *k)
{
  const struct ip *h = (const struct ip *)p;

  if (len < (int)sizeof(*h) || (h->vhl >> 4) != 4)
    return 0;
  int hl = (h->vhl & 0xf) * 4;
  if (hl < (int)sizeof(*h) || len < hl)
    return 0;
  // a fragment after the first carries no ports: only the first is
  // tracked, and the rest belong to whatever it started.
  if (ntohs(h->off) & 0x1fff)
    return 0;
  k->proto = h->pro;
  k->src = ntohl(h->src);
  k->dst = ntohl(h->dst);
  k->sport = k->dport = 0;
  const uint8 *l4 = p + hl;
  int l4len = len - hl;
  if (h->pro == IPPROTO_TCP) {
    if (l4len < (int)sizeof(struct tcp))
      return 0;
    const struct tcp *t = (const struct tcp *)l4;
    k->sport = ntohs(t->sport);
    k->dport = ntohs(t->dport);
    k->flags = t->flags;
  } else if (h->pro == IPPROTO_UDP) {
    if (l4len < (int)sizeof(struct udp))
      return 0;
    const struct udp *u = (const struct udp *)l4;
    k->sport = ntohs(u->sport);
    k->dport = ntohs(u->dport);
    k->flags = 0;
  } else if (h->pro == IPPROTO_ICMP) {
    if (l4len < (int)sizeof(struct icmp))
      return 0;
    const struct icmp *ic = (const struct icmp *)l4;
    // an echo's identifier is the nearest thing it has to a port, and
    // the reply carries the same one, which is what matters here
    k->sport = k->dport = ntohs(ic->id);
    k->flags = ic->type;
  } else {
    return 0;
  }
  return 1;
}

static int
same_forward(const struct ct *c, const struct ctkey *k)
{
  return c->proto == k->proto && c->src == k->src && c->dst == k->dst &&
         c->sport == k->sport && c->dport == k->dport;
}

static int
same_reverse(const struct ct *c, const struct ctkey *k)
{
  return c->proto == k->proto && c->src == k->dst && c->dst == k->src &&
         c->sport == k->dport && c->dport == k->sport;
}

static int
expired(const struct ct *c, uint now)
{
  return (uint)(now - c->last) > timeout_of(c);
}

// the slot to use for a new conversation: a free one, an expired one, or
// the one nobody has touched for longest.
static struct ct *
ct_victim(uint now)
{
  struct ct *lru = 0;

  for (int i = 0; i < NCONNTRACK; i++) {
    struct ct *c = &cttab[i];
    if (c->proto == 0)
      return c;
    if (expired(c, now) || c->state == CT_CLOSED)
      return c;
    if (lru == 0 || c->seq < lru->seq)
      lru = c;
  }
  ct_dropped++;
  return lru;
}

// how a TCP conversation moves, told only by the flags, which is all a
// machine in the middle can see.
static void
tcp_advance(struct ct *c, const struct ctkey *k, int reverse)
{
  if (k->flags & TCP_RST) {
    c->state = CT_CLOSED;
    return;
  }
  if (k->flags & TCP_FIN) {
    c->state = CT_CLOSING;
    return;
  }
  if (c->state == CT_SYN && reverse && (k->flags & TCP_SYN) &&
      (k->flags & TCP_ACK))
    c->state = CT_ESTAB;          // the other end answered
  else if (c->state == CT_SYN && reverse && (k->flags & TCP_ACK))
    c->state = CT_ESTAB;
}

// a packet, from inside (dir = CT_OUT) or from outside (CT_IN), seen at
// `who''s door: 0 for the host and the uplink, a machine's id plus one
// for a machine's switch port. Returns 1 if the packet belongs to a
// conversation -- one it may start, if it is going out -- and 0 if it is
// a packet from outside that nobody asked for.
//
// Which way a conversation runs is decided by who started it, and that
// is why `who' matters: the reply to what a machine said is the
// machine's to receive, and the same packet arriving at a machine that
// said nothing is not.
int
ct_track(const uint8 *ip, int len, int dir, int who)
{
  struct ctkey k;
  uint now = ticks;
  int verdict = 0;

  if (!tuple_of(ip, len, &k))
    return dir == CT_OUT;     // not tracked: outbound yes, inbound no

  acquire(&ctlock);
  for (int i = 0; i < NCONNTRACK; i++) {
    struct ct *c = &cttab[i];
    if (c->proto == 0 || expired(c, now))
      continue;
    int fwd = same_forward(c, &k);
    int rev = !fwd && same_reverse(c, &k);
    if (!fwd && !rev)
      continue;
    // a packet coming in belongs here only if this is the door it
    // should come to: the answer to what the machine said, or the rest
    // of a connection that a published port let in. Nothing else
    // matches this tuple, so there is no point looking further.
    if (dir == CT_IN && !((rev && c->opener == who) || (fwd && c->published)))
      goto out;
    // the direction the conversation started in is `forward'; a packet
    // that matches it counts as originating, whichever way it arrived
    if (fwd)
      c->packets++;
    else
      c->replies++;
    c->last = now;
    c->seq = ++ct_seq;
    if (c->proto == IPPROTO_TCP)
      tcp_advance(c, &k, rev);
    else if (c->proto == IPPROTO_ICMP && rev)
      c->state = CT_ESTAB;
    verdict = 1;
    goto out;
  }

  // no conversation. One arriving from outside has not been asked for;
  // the caller decides whether something else (a published port) lets
  // it in, and calls ct_open() when it does.
  if (dir == CT_IN) {
    ct_early++;
    goto out;
  }
  struct ct *c = ct_victim(now);
  if (c == 0)
    goto out;
  memset(c, 0, sizeof(*c));
  c->proto = k.proto;
  c->src = k.src;
  c->dst = k.dst;
  c->sport = k.sport;
  c->dport = k.dport;
  c->state = k.proto == IPPROTO_TCP ? CT_SYN : CT_NEW;
  c->icmp_type = k.proto == IPPROTO_ICMP ? k.flags : 0;
  c->opener = (uint8)who;
  c->packets = 1;
  c->last = now;
  c->seq = ++ct_seq;
  ct_created++;
  verdict = 1;

out:
  release(&ctlock);
  return verdict;
}

// open a conversation for an inbound packet that something else has
// allowed -- a published port. The tuple is recorded as it arrived, so
// the machine's answers are the reply direction and need no second
// rule.
int
ct_open(const uint8 *ip, int len)
{
  struct ctkey k;
  uint now = ticks;

  if (!tuple_of(ip, len, &k))
    return 0;
  acquire(&ctlock);
  for (int i = 0; i < NCONNTRACK; i++) {
    struct ct *e = &cttab[i];
    if (e->proto == 0 || expired(e, now))
      continue;
    if (!same_forward(e, &k))
      continue;
    e->published = 1;
    e->last = now;
    e->seq = ++ct_seq;
    release(&ctlock);
    return 1;
  }
  struct ct *c = ct_victim(now);
  if (c == 0) {
    release(&ctlock);
    return 0;
  }
  memset(c, 0, sizeof(*c));
  c->proto = k.proto;
  c->src = k.src;
  c->dst = k.dst;
  c->sport = k.sport;
  c->dport = k.dport;
  c->state = k.proto == IPPROTO_TCP ? CT_SYN : CT_NEW;
  c->icmp_type = k.proto == IPPROTO_ICMP ? k.flags : 0;
  c->packets = 1;
  c->last = now;
  c->seq = ++ct_seq;
  c->published = 1;
  ct_created++;
  release(&ctlock);
  return 1;
}

// every tick: nothing has to be freed, but an entry that has timed out
// stops being reported and stops matching, and this is where that is
// made true rather than worked out again on every packet.
void
ct_timer(void)
{
  uint now = ticks;

  acquire(&ctlock);
  for (int i = 0; i < NCONNTRACK; i++) {
    struct ct *c = &cttab[i];
    if (c->proto && expired(c, now))
      c->proto = 0;
  }
  release(&ctlock);
}

int
ct_count(void)
{
  int n = 0;
  uint now = ticks;

  acquire(&ctlock);
  for (int i = 0; i < NCONNTRACK; i++)
    if (cttab[i].proto && !expired(&cttab[i], now))
      n++;
  release(&ctlock);
  return n;
}

// a copy of every live conversation, for /proc/net/conntrack. The copy
// is so that the reader is not holding the lock the network needs.
void
ct_walk(void (*fn)(const struct ct *, void *), void *arg)
{
  struct ct copy;
  uint now = ticks;

  for (int i = 0; i < NCONNTRACK; i++) {
    acquire(&ctlock);
    if (cttab[i].proto == 0 || expired(&cttab[i], now)) {
      release(&ctlock);
      continue;
    }
    copy = cttab[i];
    // what is left of its life, in seconds, which is what the file
    // reports
    uint hz = tick_hz ? tick_hz : 100;
    uint used = now - copy.last;
    uint life = timeout_of(&copy);
    copy.expires = (life > used ? life - used : 0) / hz;
    release(&ctlock);
    fn(&copy, arg);
  }
}
