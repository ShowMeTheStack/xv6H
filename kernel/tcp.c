//
// TCP: the RFC 793 state machine with sliding windows, retransmission
// with an RTT estimator and exponential backoff, fast retransmit on
// duplicate ACKs, slow start / congestion avoidance, listen/accept, and
// orderly close. out-of-order segments are acknowledged (duplicate ACK)
// but not stored; the peer retransmits.
//
// all TCP state is protected by netlock (a spinlock, held by net_rx in
// interrupt context and by the socket calls in process context).
// processes sleep on their tcpcb with netlock released.
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
#include "net.h"
#include "sock.h"
#include "tcp.h"

// every timer here is counted in timer ticks, and the tick rate is a
// setting (param.h, /sys/kernel/tick_hz). These turn the intervals
// the protocol is written in into whatever a tick is worth today.
#define MS_TICKS(ms)  (((ms) * tick_hz + 999) / 1000)
#define SEC_TICKS(s)  ((s) * tick_hz)


static struct tcpcb tcbs[NTCP];
static uint32 iss_next = 0x1000;

#define SEQ_LT(a, b) ((int)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((int)((a) - (b)) > 0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// ---- byte rings ----

static int
tring_init(struct tring *t, uint size)
{
  t->size = size;
  t->r = t->w = 0;
  for (int i = 0; i < TRING_PAGES; i++)
    t->pg[i] = 0;
  for (uint i = 0; i * PGSIZE < size; i++) {
    if ((t->pg[i] = kalloc()) == 0)
      return -1;
  }
  return 0;
}

static void
tring_free(struct tring *t)
{
  for (int i = 0; i < TRING_PAGES; i++) {
    if (t->pg[i])
      kfree(t->pg[i]);
    t->pg[i] = 0;
  }
}

static uint
tring_len(struct tring *t)
{
  return t->w - t->r;
}

static uint
tring_space(struct tring *t)
{
  return t->size - (t->w - t->r);
}

// copy n bytes at logical position pos (>= t->r) out of the ring.
static void
tring_peek(struct tring *t, uint pos, char *dst, uint n)
{
  while (n > 0) {
    uint off = pos % t->size;
    uint chunk = MIN(n, PGSIZE - (off % PGSIZE));
    memmove(dst, t->pg[off / PGSIZE] + (off % PGSIZE), chunk);
    dst += chunk;
    pos += chunk;
    n -= chunk;
  }
}

static void
tring_write(struct tring *t, const char *src, uint n)
{
  while (n > 0) {
    uint off = t->w % t->size;
    uint chunk = MIN(n, PGSIZE - (off % PGSIZE));
    memmove(t->pg[off / PGSIZE] + (off % PGSIZE), src, chunk);
    src += chunk;
    t->w += chunk;
    n -= chunk;
  }
}

// ---- control blocks ----

static struct tcpcb *
tcb_take(void)
{
  for (int i = 0; i < NTCP; i++) {
    if (!tcbs[i].used) {
      struct tcpcb *t = &tcbs[i];
      memset(t, 0, sizeof(*t));
      t->used = 1;
      t->state = TCPS_CLOSED;
      t->mss = TCP_MSS;
      t->rto = SEC_TICKS(1);
      t->cwnd = 4 * TCP_MSS;
      t->ssthresh = 65535;
      if (tring_init(&t->snd, TCP_SNDBUF) < 0 || tring_init(&t->rcv, TCP_RCVBUF) < 0) {
        tring_free(&t->snd);
        tring_free(&t->rcv);
        t->used = 0;
        return 0;
      }
      return t;
    }
  }
  return 0;
}

static void tcb_free(struct tcpcb *t);

// A control block, reclaiming one from TIME_WAIT if that is what it takes.
//
// There are NTCP of these and TIME_WAIT holds one for three seconds, so a
// program opening and closing connections as fast as it can ran out after
// about thirty of them and then went at the speed the oldest TIME_WAIT
// expired: netbench measured 8 connections a second on the loopback address,
// which is 32/3 and not a coincidence.
//
// TIME_WAIT is a courtesy to the network -- it absorbs a stale duplicate for
// one four-tuple -- and it must not be the reason this machine cannot open
// an unrelated connection. So when nothing is free the oldest one is taken:
// the one with the least of its timer left, which is the one whose duplicates
// are least likely to still be in flight.
static struct tcpcb *
tcb_alloc(void)
{
  struct tcpcb *t = tcb_take();

  if (t)
    return t;
  struct tcpcb *victim = 0;
  for (int i = 0; i < NTCP; i++) {
    struct tcpcb *c = &tcbs[i];
    if (c->used && c->state == TCPS_TIME_WAIT &&
        (victim == 0 || c->timewait_timer < victim->timewait_timer))
      victim = c;
  }
  if (victim == 0)
    return 0;
  tcb_free(victim);
  return tcb_take();
}

static void ooo_free(struct tcpcb *t);

static void
tcb_free(struct tcpcb *t)
{
  for (int i = 0; i < t->nbacklog; i++) {
    struct tcpcb *c = t->backlog[i];
    if (c) {
      c->parent = 0;
      c->state = TCPS_CLOSED;
      tring_free(&c->snd);
      tring_free(&c->rcv);
      ooo_free(c);
      c->used = 0;
    }
  }
  tring_free(&t->snd);
  tring_free(&t->rcv);
  ooo_free(t);
  if (t->sock)
    t->sock->tcp = 0;
  t->used = 0;
  t->state = TCPS_CLOSED;
}

static struct tcpcb *
tcb_find(struct netns *ns, uint32 lip, uint16 lport, uint32 rip, uint16 rport)
{
  struct tcpcb *listener = 0;
  for (int i = 0; i < NTCP; i++) {
    struct tcpcb *t = &tcbs[i];
    if (!t->used || t->ns != ns || t->lport != lport)
      continue;
    if (t->state == TCPS_LISTEN) {
      listener = t;
      continue;
    }
    if (t->rip == rip && t->rport == rport)
      return t;
  }
  return listener;
}

static void
tcb_wakeup(struct tcpcb *t)
{
  wakeup(t);
  if (t->sock)
    wakeup(t->sock);
}

// ---- output ----

// build and send one segment. data (if any) is copied from the send
// ring at logical position seq. caller holds netlock.
static void
tcp_send_segment(struct tcpcb *t, uint32 seq, uint32 ack, uint8 flags, uint32 datalen, int with_mss)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return;
  if (datalen) {
    char *p = mbufput(m, datalen);
    tring_peek(&t->snd, t->snd.r + (seq - t->snd_una), p, datalen);
  }
  int optlen = with_mss ? 4 : 0;
  struct tcp *h = (struct tcp *)mbufpush(m, sizeof(*h) + optlen);
  h->sport = htons(t->lport);
  h->dport = htons(t->rport);
  h->seq = htonl(seq);
  h->ack = htonl(ack);
  h->off = ((sizeof(*h) + optlen) / 4) << 4;
  h->flags = flags;
  uint32 wnd = tring_space(&t->rcv);
  h->win = htons(wnd > 65535 ? 65535 : wnd);
  h->sum = 0;
  h->urp = 0;
  if (with_mss) {
    uint8 *o = (uint8 *)(h + 1);
    o[0] = 2;
    o[1] = 4;
    o[2] = TCP_MSS >> 8;
    o[3] = TCP_MSS & 0xff;
  }
  h->sum = l4_checksum(t->lip, t->rip, IPPROTO_TCP, h, m->len);
  net_tx_ip_ns(t->ns, m, t->rip, IPPROTO_TCP);
}

static void
tcp_send_ack(struct tcpcb *t)
{
  tcp_send_segment(t, t->snd_nxt, t->rcv_nxt, TCP_ACK, 0, 0);
}

// send a reset in reply to a segment that has no connection.
static void
tcp_send_rst(struct netns *ns, uint32 lip, uint16 lport, uint32 rip,
             uint16 rport, uint32 seq, uint32 ack, int with_ack)
{
  struct mbuf *m = mbufalloc(MBUF_HEADROOM);
  if (m == 0)
    return;
  struct tcp *h = (struct tcp *)mbufpush(m, sizeof(*h));
  memset(h, 0, sizeof(*h));
  h->sport = htons(lport);
  h->dport = htons(rport);
  h->seq = htonl(seq);
  h->ack = htonl(ack);
  h->off = (sizeof(*h) / 4) << 4;
  h->flags = TCP_RST | (with_ack ? TCP_ACK : 0);
  h->sum = l4_checksum(lip, rip, IPPROTO_TCP, h, m->len);
  net_tx_ip_ns(ns, m, rip, IPPROTO_TCP);
}

static void
rtx_start(struct tcpcb *t)
{
  if (t->rtx_timer == 0)
    t->rtx_timer = t->rto;
}

// transmit whatever the windows allow from snd_nxt onward, plus a
// queued FIN. caller holds netlock.
static void
tcp_output(struct tcpcb *t)
{
  if (t->state != TCPS_ESTABLISHED && t->state != TCPS_CLOSE_WAIT && t->state != TCPS_FIN_WAIT_1 &&
      t->state != TCPS_CLOSING && t->state != TCPS_LAST_ACK)
    return;
  // A segment to one of this machine's own addresses is delivered by the
  // send: tcp_send_segment goes to the loopback, which hands the packet
  // to ip_rx before it returns, and the answer to it comes back into
  // this connection -- all of it inside the call below. Two things
  // follow. The sequence state is moved on *before* the segment goes,
  // because an acknowledgement that arrives during the send is measured
  // against snd_nxt and is thrown away as "acking something we never
  // sent" if it still holds yesterday's value. And a tcp_output that
  // the answer calls returns at once rather than sending the same bytes
  // a second time: nothing can be added to the ring meanwhile, because
  // the writer would need netlock, which this call holds.
  //
  // (Before this, every loopback segment was acknowledged into a stale
  // snd_nxt, the acknowledgement was dropped, and the conversation went
  // at the speed of the retransmission timer -- which is why a local
  // fetch took a second and a migration ran at 45 KB/s.)
  if (t->sending)
    return;
  t->sending = 1;
  uint32 unsent_end = t->snd_una + tring_len(&t->snd); // first byte not in the ring
  uint32 wnd = MIN(t->snd_wnd, t->cwnd);
  int sent = 0;
  while (SEQ_LT(t->snd_nxt, unsent_end)) {
    uint32 inflight = t->snd_nxt - t->snd_una;
    if (inflight >= wnd)
      break;
    uint32 len = MIN(unsent_end - t->snd_nxt, wnd - inflight);
    if (len > t->mss)
      len = t->mss;
    uint8 flags = TCP_ACK;
    if (t->snd_nxt + len == unsent_end)
      flags |= TCP_PSH;
    if (t->rtt_seq == 0) {
      t->rtt_seq = t->snd_nxt + len;
      t->rtt_start = ticks;
    }
    uint32 seq = t->snd_nxt;
    t->snd_nxt += len;
    tcp_send_segment(t, seq, t->rcv_nxt, flags, len, 0);
    sent = 1;
  }
  if (t->fin_queued && !t->fin_sent && t->snd_nxt == unsent_end) {
    uint32 seq = t->snd_nxt;
    t->snd_nxt += 1;
    t->fin_sent = 1;
    tcp_send_segment(t, seq, t->rcv_nxt, TCP_FIN | TCP_ACK, 0, 0);
    sent = 1;
  }
  if (sent)
    rtx_start(t);
  t->sending = 0;
}

// retransmit the oldest unacknowledged segment. caller holds netlock.
static void
tcp_retransmit(struct tcpcb *t)
{
  t->rtt_seq = 0; // Karn: no RTT sample across a retransmission
  if (t->state == TCPS_SYN_SENT) {
    tcp_send_segment(t, t->iss, 0, TCP_SYN, 0, 1);
    return;
  }
  if (t->state == TCPS_SYN_RCVD) {
    tcp_send_segment(t, t->iss, t->rcv_nxt, TCP_SYN | TCP_ACK, 0, 1);
    return;
  }
  uint32 unacked = t->snd_nxt - t->snd_una;
  uint32 datalen = tring_len(&t->snd);
  if (datalen > 0) {
    uint32 len = MIN(MIN(datalen, unacked), (uint32)t->mss);
    if (len > 0) {
      uint8 flags = TCP_ACK;
      if (t->fin_sent && len == datalen)
        flags |= TCP_FIN;
      tcp_send_segment(t, t->snd_una, t->rcv_nxt, flags, len, 0);
      return;
    }
  }
  if (t->fin_sent && unacked)
    tcp_send_segment(t, t->snd_una, t->rcv_nxt, TCP_FIN | TCP_ACK, 0, 0);
}

// ---- timers ----

void
tcp_timer(void)
{
  for (int i = 0; i < NTCP; i++) {
    struct tcpcb *t = &tcbs[i];
    if (!t->used)
      continue;
    if (t->state == TCPS_TIME_WAIT) {
      if (t->timewait_timer && --t->timewait_timer == 0)
        tcb_free(t);
      continue;
    }
    if (t->conn_deadline && (int)(ticks - t->conn_deadline) >= 0) {
      t->conn_deadline = 0;
      tcb_wakeup(t);        // the waiting connect gives up on its own
    }
    if (t->rtx_timer && --t->rtx_timer == 0) {
      // a connection that has not been answered is given up on sooner
      // than one that has: five SYNs is half a minute of asking, and
      // ten retransmissions is for a conversation that exists.
      if (++t->retries > (t->state == TCPS_SYN_SENT ? 5 : 10)) {
        // give up: reset the connection.
        t->rcv_rst = 1;
        t->state = TCPS_CLOSED;
        tcb_wakeup(t);
        if (t->sock == 0)
          tcb_free(t);
        continue;
      }
      t->ssthresh = MIN(t->cwnd, t->snd_nxt - t->snd_una) / 2;
      if (t->ssthresh < 2 * t->mss)
        t->ssthresh = 2 * t->mss;
      t->cwnd = t->mss;
      t->rto *= 2;
      // A minute between attempts is for a path that may come back in a
      // minute. Everything this kernel talks to is on the same machine or
      // one hop away, and a connection that has not been answered in
      // eight seconds is not going to be answered by waiting longer --
      // meanwhile a transfer that lost one segment is stopped for the
      // whole of it.
      if (t->rto > SEC_TICKS(8))
        t->rto = SEC_TICKS(8);
      tcp_retransmit(t);
      t->rtx_timer = t->rto;
    }
    // zero-window probe. With the peer's window shut there is nothing to
    // retransmit and nothing that may be sent, so neither timer above is
    // running: if the update that opens the window is lost, both ends
    // wait for ever. One byte the peer is not obliged to take makes it
    // answer with the window it has now.
    if ((t->state == TCPS_ESTABLISHED || t->state == TCPS_CLOSE_WAIT) &&
        t->snd_wnd == 0 && tring_len(&t->snd) > 0 && t->rtx_timer == 0) {
      if (t->persist == 0 || --t->persist == 0) {
        // one byte of the data that is waiting, sent as data and counted
        // as data: a probe that does not advance snd_nxt is a byte the
        // peer may take and acknowledge, leaving this end with an
        // acknowledgement for something it does not believe it sent --
        // which tcp_ack drops, and the connection stops for good. With it
        // counted, the retransmission timer takes over the asking.
        uint32 seq = t->snd_nxt;
        t->snd_nxt += 1;
        rtx_start(t);
        tcp_send_segment(t, seq, t->rcv_nxt, TCP_ACK, 1, 0);
        t->persist = SEC_TICKS(1);
      }
    } else {
      t->persist = 0;
    }
  }
}

// ---- input ----

static void
rtt_sample(struct tcpcb *t)
{
  uint r = ticks - t->rtt_start;
  if (t->srtt == 0) {
    t->srtt = r;
    t->rttvar = r / 2;
  } else {
    int d = (int)r - (int)t->srtt;
    if (d < 0)
      d = -d;
    t->rttvar = (3 * t->rttvar + d) / 4;
    t->srtt = (7 * t->srtt + r) / 8;
  }
  t->rto = t->srtt + 4 * t->rttvar;
  if (t->rto < MS_TICKS(300))
    t->rto = MS_TICKS(300); // a floor, so a fast link does not retransmit on noise
  if (t->rto > SEC_TICKS(60))
    t->rto = SEC_TICKS(60);
  t->rtt_seq = 0;
}

// process an acceptable ACK. returns 1 if it acknowledged new data.
static int
tcp_ack(struct tcpcb *t, uint32 ack)
{
  if (SEQ_LEQ(ack, t->snd_una)) {
    if (ack == t->snd_una && tring_len(&t->snd) > 0) {
      t->dupacks++;
      // Fast retransmit, with the threshold the window can actually
      // reach. Three duplicate acknowledgements need four segments in
      // flight, and this stack's send ring was eight kilobytes: a window
      // of two segments never got there, so every lost segment used to
      // cost a whole retransmission timeout -- and the timeout doubles.
      // A migration on a link that drops one frame of a back-to-back
      // pair went from 45 KB/s to a dead stop that way, one segment per
      // sixty seconds. With fewer than four segments outstanding the
      // threshold is one less than what is outstanding (RFC 5827's early
      // retransmit), which recovers in a round trip instead.
      uint32 out = (t->snd_nxt - t->snd_una + t->mss - 1) / t->mss;
      int thresh = 3;
      if (out > 1 && out < 4)
        thresh = (int)out - 1;
      if (t->dupacks == thresh) {
        t->ssthresh = MIN(t->cwnd, t->snd_nxt - t->snd_una) / 2;
        if (t->ssthresh < 2 * t->mss)
          t->ssthresh = 2 * t->mss;
        t->cwnd = t->ssthresh + 3 * t->mss;
        t->recover = t->snd_nxt;
        t->in_recovery = 1;
        tcp_retransmit(t);
      }
    }
    return 0;
  }
  if (SEQ_GT(ack, t->snd_nxt))
    return 0; // acks something we never sent
  uint32 acked = ack - t->snd_una;
  t->dupacks = 0;
  // data bytes leave the ring; SYN/FIN occupy sequence space but no bytes.
  uint32 data_acked = MIN(acked, tring_len(&t->snd));
  t->snd.r += data_acked;
  t->snd_una = ack;
  // NewReno (RFC 6582): an ack that moves snd_una but not past what was in
  // flight when the loss was found is a partial ack -- the next segment is
  // gone too. Retransmit it now, not after three more duplicates: a burst
  // that lost several frames (41 per side per hunt3 run) used to recover one
  // segment per round trip, and a 30-second wait on the far end ran out.
  if (t->in_recovery) {
    if (SEQ_LT(ack, t->recover))
      tcp_retransmit(t);
    else
      t->in_recovery = 0;
  }
  if (t->rtt_seq && SEQ_LEQ(t->rtt_seq, ack))
    rtt_sample(t);
  if (t->cwnd < t->ssthresh)
    t->cwnd += t->mss;
  else
    t->cwnd += (t->mss * t->mss) / t->cwnd + 1;
  t->retries = 0;
  t->rtx_timer = (t->snd_nxt == t->snd_una) ? 0 : t->rto;
  return 1;
}

// hand data to the application's ring, as much of it as there is room
// for, and say how much that was.
static uint32
tcp_deliver(struct tcpcb *t, char *data, uint32 len)
{
  uint32 room = tring_space(&t->rcv);
  if (len > room)
    len = room; // the window said so; excess is dropped and retransmitted
  tring_write(&t->rcv, data, len);
  t->rcv_nxt += len;
  return len;
}

// ---- what arrived out of order ----
//
// A segment that starts beyond rcv_nxt is not something to throw away:
// the piece in front of it will be retransmitted, and this one is then
// the next piece. What is kept here is the mbuf itself, which is why
// there are only four of them.

static void
ooo_free(struct tcpcb *t)
{
  for (int i = 0; i < t->nooo; i++)
    if (t->ooo[i].m)
      mbuffree(t->ooo[i].m);
  t->nooo = 0;
}

// keep this segment. returns 1 when it has taken the mbuf.
static int
ooo_keep(struct tcpcb *t, struct mbuf *m, char *data, uint32 seq, uint32 len)
{
  if (len == 0 || t->nooo >= TCP_OOO)
    return 0;
  for (int i = 0; i < t->nooo; i++)
    if (t->ooo[i].seq == seq)
      return 0;                      // already have this one
  t->ooo[t->nooo].m = m;
  t->ooo[t->nooo].data = data;
  t->ooo[t->nooo].seq = seq;
  t->ooo[t->nooo].len = len;
  t->nooo++;
  return 1;
}

// deliver whatever the hole in front of it was waiting for. returns the
// number of bytes that went in.
static uint32
ooo_drain(struct tcpcb *t)
{
  uint32 total = 0;

  for (int again = 1; again && t->nooo > 0;) {
    again = 0;
    for (int i = 0; i < t->nooo; i++) {
      uint32 seq = t->ooo[i].seq, len = t->ooo[i].len;
      int done = 0;
      if (SEQ_LEQ(seq + len, t->rcv_nxt)) {
        done = 1;                    // all of it is old
      } else if (SEQ_LEQ(seq, t->rcv_nxt)) {
        uint32 skip = t->rcv_nxt - seq;
        uint32 took = tcp_deliver(t, t->ooo[i].data + skip, len - skip);
        total += took;
        if (took == len - skip) {
          done = 1;
          again = 1;
        } else {
          return total;              // no room: it waits where it is
        }
      }
      if (done) {
        mbuffree(t->ooo[i].m);
        t->ooo[i] = t->ooo[--t->nooo];
        i--;
      }
    }
  }
  return total;
}

static void
listen_accept_syn(struct tcpcb *l, uint32 lip, uint32 rip, uint16 rport, uint32 seq, uint16 mss)
{
  if (l->nbacklog >= l->maxbacklog)
    return;
  struct tcpcb *t = tcb_alloc();
  if (t == 0)
    return;
  t->ns = l->ns;
  t->lip = lip;
  t->lport = l->lport;
  t->rip = rip;
  t->rport = rport;
  t->parent = l;
  t->irs = seq;
  t->rcv_nxt = seq + 1;
  t->iss = iss_next += 64000;
  t->snd_una = t->iss;
  t->snd_nxt = t->iss + 1;
  t->snd_wnd = TCP_MSS;
  if (mss)
    t->mss = MIN(mss, TCP_MSS);
  t->state = TCPS_SYN_RCVD;
  tcp_send_segment(t, t->iss, t->rcv_nxt, TCP_SYN | TCP_ACK, 0, 1);
  t->rtx_timer = t->rto;
}

void
tcp_rx(struct netns *ns, struct mbuf *m, uint32 src, uint32 dst)
{
  struct tcp *h = (struct tcp *)m->head;
  if (m->len < sizeof(*h))
    goto drop;
  uint hl = (h->off >> 4) * 4;
  if (hl < sizeof(*h) || hl > m->len)
    goto drop;
  if (l4_checksum(src, dst, IPPROTO_TCP, h, m->len) != 0)
    goto drop;

  uint16 sport = ntohs(h->sport), dport = ntohs(h->dport);
  uint32 seq = ntohl(h->seq), ack = ntohl(h->ack);
  uint8 flags = h->flags;
  uint16 win = ntohs(h->win);
  uint16 mss = 0;
  // options: only MSS matters.
  uint8 *o = (uint8 *)h + sizeof(*h), *oend = (uint8 *)h + hl;
  while (o < oend) {
    if (*o == 0)
      break;
    if (*o == 1) {
      o++;
      continue;
    }
    if (o + 1 >= oend || o[1] < 2 || o + o[1] > oend)
      break;
    if (*o == 2 && o[1] == 4)
      mss = (o[2] << 8) | o[3];
    o += o[1];
  }
  char *data = m->head + hl;
  uint32 datalen = m->len - hl;

  struct tcpcb *t = tcb_find(ns, dst, dport, src, sport);
  if (t == 0) {
    if (!(flags & TCP_RST))
      tcp_send_rst(ns, dst, dport, src, sport, (flags & TCP_ACK) ? ack : 0, seq + datalen + ((flags & TCP_SYN) ? 1 : 0), !(flags & TCP_ACK));
    goto drop;
  }

  if (t->state == TCPS_LISTEN) {
    if ((flags & TCP_SYN) && !(flags & (TCP_ACK | TCP_RST)))
      listen_accept_syn(t, dst, src, sport, seq, mss);
    goto drop;
  }

  if (t->state == TCPS_SYN_SENT) {
    if (flags & TCP_RST) {
      if ((flags & TCP_ACK) && ack == t->snd_nxt) {
        t->rcv_rst = 1;
        t->state = TCPS_CLOSED;
        tcb_wakeup(t);
      }
      goto drop;
    }
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == t->snd_nxt) {
      t->irs = seq;
      t->rcv_nxt = seq + 1;
      t->snd_una = ack;
      t->snd_wnd = win;
      t->snd_wl1 = seq;
      t->snd_wl2 = ack;
      if (mss)
        t->mss = MIN(mss, TCP_MSS);
      t->cwnd = 4 * t->mss;
      t->state = TCPS_ESTABLISHED;
      t->rtx_timer = 0;
      t->retries = 0;
      if (t->rtt_seq)
        rtt_sample(t);
      tcp_send_ack(t);
      tcb_wakeup(t);
    }
    goto drop;
  }

  // sequence check: we only take segments starting exactly at rcv_nxt
  // (or old data overlapping it); everything else gets an ACK.
  if (flags & TCP_RST) {
    if (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + tring_space(&t->rcv) + 1)) {
      t->rcv_rst = 1;
      t->state = TCPS_CLOSED;
      tcb_wakeup(t);
      if (t->sock == 0)
        tcb_free(t);
    }
    goto drop;
  }
  if (flags & TCP_SYN) {
    goto drop; // a SYN inside a connection: ignore
  }
  if (!(flags & TCP_ACK))
    goto drop;

  if (SEQ_LT(seq, t->rcv_nxt)) {
    // old data: trim the overlap
    uint32 skip = t->rcv_nxt - seq;
    if (skip > datalen) {
      tcp_send_ack(t);
      goto drop;
    }
    data += skip;
    datalen -= skip;
    seq = t->rcv_nxt;
  } else if (SEQ_GT(seq, t->rcv_nxt)) {
    // a hole: the duplicate acknowledgement asks for the missing piece,
    // and this segment waits for it rather than being thrown away.
    int kept = ooo_keep(t, m, data, seq, datalen);
    tcp_send_ack(t);
    if (kept)
      return;                 // the mbuf belongs to the queue now
    goto drop;
  }

  if (t->state == TCPS_SYN_RCVD) {
    if (SEQ_LT(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_nxt)) {
      t->state = TCPS_ESTABLISHED;
      t->snd_una = ack;
      t->snd_wnd = win;
      t->snd_wl1 = seq;
      t->snd_wl2 = ack;
      t->rtx_timer = 0;
      t->retries = 0;
      // hand the connection to the listening socket's accept queue
      struct tcpcb *l = t->parent;
      if (l && l->state == TCPS_LISTEN && l->nbacklog < l->maxbacklog) {
        l->backlog[l->nbacklog++] = t;
        tcb_wakeup(l);
      } else {
        t->state = TCPS_CLOSED;
        tcp_send_rst(t->ns, t->lip, t->lport, t->rip, t->rport, t->snd_nxt, 0, 0);
        tcb_free(t);
        goto drop;
      }
    } else {
      goto drop;
    }
  }

  int newack = tcp_ack(t, ack);
  // window update
  uint32 oldwnd = t->snd_wnd;
  if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
    t->snd_wnd = win;
    t->snd_wl1 = seq;
    t->snd_wl2 = ack;
  }

  // our FIN acknowledged?
  int fin_acked = t->fin_sent && t->snd_una == t->snd_nxt;
  switch (t->state) {
  case TCPS_FIN_WAIT_1:
    if (fin_acked)
      t->state = TCPS_FIN_WAIT_2;
    break;
  case TCPS_CLOSING:
    if (fin_acked) {
      t->state = TCPS_TIME_WAIT;
      t->timewait_timer = SEC_TICKS(3);
    }
    break;
  case TCPS_LAST_ACK:
    if (fin_acked) {
      t->state = TCPS_CLOSED;
      tcb_wakeup(t);
      if (t->sock == 0)
        tcb_free(t);
      goto drop;
    }
    break;
  }

  int must_ack = 0;
  if (datalen > 0 && (t->state == TCPS_ESTABLISHED || t->state == TCPS_FIN_WAIT_1 || t->state == TCPS_FIN_WAIT_2)) {
    tcp_deliver(t, data, datalen);
    // and whatever was waiting behind this piece
    if (t->nooo > 0)
      ooo_drain(t);
    must_ack = 1;
    tcb_wakeup(t);
  }
  if ((flags & TCP_FIN) && seq + datalen == t->rcv_nxt) {
    // in-order FIN (all data before it delivered)
    if (t->state == TCPS_ESTABLISHED || t->state == TCPS_FIN_WAIT_1 || t->state == TCPS_FIN_WAIT_2) {
      t->rcv_nxt += 1;
      t->rcv_fin = 1;
      must_ack = 1;
      if (t->state == TCPS_ESTABLISHED)
        t->state = TCPS_CLOSE_WAIT;
      else if (t->state == TCPS_FIN_WAIT_1)
        t->state = fin_acked ? TCPS_TIME_WAIT : TCPS_CLOSING;
      else
        t->state = TCPS_TIME_WAIT;
      if (t->state == TCPS_TIME_WAIT)
        t->timewait_timer = SEC_TICKS(3);
      tcb_wakeup(t);
    }
  }
  if (must_ack)
    tcp_send_ack(t);
  if (newack || t->snd_wnd > oldwnd) {
    // an acknowledgement is room in the send ring, and somebody may be
    // asleep waiting for exactly that. Without this wakeup a sender
    // stalls for ever the moment its ring fills: everything smaller than
    // the ring went through, which is why nothing noticed until a
    // machine was migrated down a socket (and a plain `nc' of 256 KB
    // stalls the same way, loopback included).
    //
    // A window that has just opened counts as well, and for the same
    // reason. A receiver whose buffer filled advertises nothing; the
    // segment that says it has room again acknowledges no new data, so
    // without the second test here nothing would send into the room it
    // announced -- and both ends would wait for the other. That is the
    // second half of the same defect: a migration stopped after five
    // pages of guest memory with data in the sender's ring, an open
    // window, and neither side about to speak again.
    tcb_wakeup(t);
    tcp_output(t);
  }
drop:
  mbuffree(m);
}

// ---- the socket side ----

int
tcp_connect(struct sock *s)
{
  acquire(&netlock);
  if (!net_policy_ok(s->rip, IPPROTO_TCP, s->rport, 0)) {
    net_policy_dropped++;
    release(&netlock);
    return -EACCES; // the egress policy refused it
  }
  struct tcpcb *t = tcb_alloc();
  if (t == 0) {
    release(&netlock);
    return -1;
  }
  t->sock = s;
  s->tcp = t;
  // talking to ourselves: the local address is the loopback one too,
  // because that is what the answer will be addressed to
  t->ns = s->ns;
  t->lip = (s->rip >> 24) == 127 ? s->rip : net_src_for(s->ns, s->rip);
  t->lport = s->lport;
  t->rip = s->rip;
  t->rport = s->rport;
  t->iss = iss_next += 64000;
  t->snd_una = t->iss;
  t->snd_nxt = t->iss + 1;
  t->snd_wnd = TCP_MSS;
  t->state = TCPS_SYN_SENT;
  t->rtt_seq = t->iss + 1;
  t->rtt_start = ticks;
  tcp_send_segment(t, t->iss, 0, TCP_SYN, 0, 1);
  t->rtx_timer = t->rto;
  uint t0 = ticks;
  int timedout = 0;
  t->conn_deadline = s->sndtimeo ? t0 + (uint)s->sndtimeo : 0;
  while (t->state == TCPS_SYN_SENT) {
    if (killed(myproc()))
      break;
    // SO_SNDTIMEO bounds the wait: a program that says how long it will
    // wait for a connection is told when that is up
    if (s->sndtimeo && (int)(ticks - t0) >= s->sndtimeo) {
      timedout = 1;
      break;
    }
    sleep_prepare(t);
    release(&netlock);
    sleep_intr();
    acquire(&netlock);
  }
  t->conn_deadline = 0;
  int ok = (t->state == TCPS_ESTABLISHED);
  int t_rcv_rst = t->rcv_rst;
  if (!ok) {
    s->tcp = 0;
    t->sock = 0;
    tcb_free(t);
  }
  int reset = t_rcv_rst;
  release(&netlock);
  if (ok)
    return 0;
  // why it failed is what the program has to know: a reset means
  // nothing is listening there, a timeout means nothing answered at
  // all, and those are different things to a program and to a person.
  if (reset)
    return -ECONNREFUSED;
  return timedout ? -ETIMEDOUT : -EINTR;
}

int
tcp_listen(struct sock *s, int backlog)
{
  acquire(&netlock);
  struct tcpcb *t = tcb_alloc();
  if (t == 0) {
    release(&netlock);
    return -1;
  }
  t->sock = s;
  s->tcp = t;
  t->ns = s->ns;
  t->lip = 0;              // it answers on whatever address it arrives at
  t->lport = s->lport;
  t->maxbacklog = (backlog < 1) ? 1 : MIN(backlog, TCP_BACKLOG);
  t->state = TCPS_LISTEN;
  release(&netlock);
  return 0;
}

struct sock *
tcp_accept(struct sock *s)
{
  acquire(&netlock);
  struct tcpcb *l = s->tcp;
  if (l == 0 || l->state != TCPS_LISTEN) {
    release(&netlock);
    return 0;
  }
  while (l->nbacklog == 0) {
    if (killed(myproc()) || s->closed) {
      release(&netlock);
      return 0;
    }
    sleep_prepare(l);
    release(&netlock);
    sleep_intr();
    acquire(&netlock);
    if (s->tcp != l) {
      release(&netlock);
      return 0;
    }
  }
  struct tcpcb *t = l->backlog[0];
  for (int i = 1; i < l->nbacklog; i++)
    l->backlog[i - 1] = l->backlog[i];
  l->nbacklog--;
  t->parent = 0;
  release(&netlock);

  // a socket for the new connection (sockalloc is in sock.c; do it inline)
  struct sock *ns = 0;
  for (int i = 0; i < NSOCK; i++) {
    acquire(&socks[i].lock);
    if (socks[i].type == SOCK_NONE) {
      ns = &socks[i];
      ns->type = SOCK_STREAM;
      ns->id = i;
      ns->rxq = 0;
      ns->rxcount = 0;
      ns->closed = 0;
      release(&socks[i].lock);
      break;
    }
    release(&socks[i].lock);
  }
  acquire(&netlock);
  if (ns == 0) {
    t->state = TCPS_CLOSED;
    tcp_send_rst(t->ns, t->lip, t->lport, t->rip, t->rport, t->snd_nxt, 0, 0);
    tcb_free(t);
    release(&netlock);
    return 0;
  }
  ns->lip = t->lip;
  ns->lport = t->lport;
  ns->rip = t->rip;
  ns->rport = t->rport;
  ns->tcp = t;
  t->sock = ns;
  release(&netlock);
  return ns;
}

int
tcp_read(struct sock *s, int user, uint64 addr, int n)
{
  char local[512]; // filled under netlock, copied out with it released
  acquire(&netlock);
  struct tcpcb *t = s->tcp;
  if (t == 0) {
    release(&netlock);
    return -1;
  }
  while (tring_len(&t->rcv) == 0) {
    if (t->rcv_fin || t->state == TCPS_CLOSED || t->state == TCPS_TIME_WAIT) {
      release(&netlock);
      return t->rcv_rst ? -1 : 0; // EOF
    }
    if (killed(myproc())) {
      release(&netlock);
      return -1;
    }
    sleep_prepare(t);
    release(&netlock);
    sleep_intr();
    acquire(&netlock);
    if (s->tcp != t) {
      release(&netlock);
      return -1;
    }
  }
  uint32 avail = tring_len(&t->rcv);
  uint32 before = tring_space(&t->rcv);
  int total = 0;
  while (total < n && avail > 0) {
    uint32 chunk = MIN(MIN(n - total, avail), sizeof(local));
    tring_peek(&t->rcv, t->rcv.r, local, chunk);
    t->rcv.r += chunk;
    avail -= chunk;
    release(&netlock);
    if (either_copyout(user, addr + total, local, chunk) < 0) {
      acquire(&netlock);
      break;
    }
    total += chunk;
    acquire(&netlock);
    if (s->tcp != t)
      break;
  }
  // room in the ring may be room for something the out-of-order queue is
  // still holding
  if (s->tcp == t && t->nooo > 0 && ooo_drain(t) > 0)
    tcp_send_ack(t);
  // window update if the receive buffer went from cramped to roomy
  if (s->tcp == t && before < TCP_RCVBUF / 2 && tring_space(&t->rcv) >= TCP_RCVBUF / 2)
    tcp_send_ack(t);
  release(&netlock);
  return total;
}

int
tcp_write(struct sock *s, int user, uint64 addr, int n)
{
  char local[512];
  int total = 0;
  while (total < n) {
    int chunk = MIN(n - total, (int)sizeof(local));
    if (either_copyin(local, user, addr + total, chunk) < 0)
      return total ? total : -1;
    acquire(&netlock);
    struct tcpcb *t = s->tcp;
    if (t == 0 || t->rcv_rst || t->fin_queued ||
        !(t->state == TCPS_ESTABLISHED || t->state == TCPS_CLOSE_WAIT)) {
      release(&netlock);
      return total ? total : -1;
    }
    while (tring_space(&t->snd) < (uint)chunk) {
      if (killed(myproc()) || t->rcv_rst || t->state == TCPS_CLOSED) {
        release(&netlock);
        return total ? total : -1;
      }
      sleep_prepare(t);
      release(&netlock);
      sleep_intr();
      acquire(&netlock);
      if (s->tcp != t) {
        release(&netlock);
        return total ? total : -1;
      }
    }
    tring_write(&t->snd, local, chunk);
    tcp_output(t);
    release(&netlock);
    total += chunk;
  }
  return total;
}

// half of a close: the FIN goes out, and the connection stays attached
// to the socket so that what the other end is still sending can be
// read. this is what shutdown(fd, SHUT_WR) means, and it is what a
// protocol that says "that is the whole request" over a stream needs.
void
tcp_shutdown(struct sock *s)
{
  acquire(&netlock);
  struct tcpcb *t = s->tcp;
  if (t == 0) {
    release(&netlock);
    return;
  }
  if (t->state == TCPS_ESTABLISHED) {
    t->state = TCPS_FIN_WAIT_1;
    t->fin_queued = 1;
    tcp_output(t);
  } else if (t->state == TCPS_CLOSE_WAIT) {
    t->state = TCPS_LAST_ACK;
    t->fin_queued = 1;
    tcp_output(t);
  }
  release(&netlock);
}

void
tcp_close(struct sock *s)
{
  acquire(&netlock);
  struct tcpcb *t = s->tcp;
  if (t == 0) {
    release(&netlock);
    return;
  }
  s->tcp = 0;
  t->sock = 0;
  switch (t->state) {
  case TCPS_ESTABLISHED:
    t->state = TCPS_FIN_WAIT_1;
    t->fin_queued = 1;
    tcp_output(t);
    break;
  case TCPS_CLOSE_WAIT:
    t->state = TCPS_LAST_ACK;
    t->fin_queued = 1;
    tcp_output(t);
    break;
  case TCPS_SYN_RCVD:
  case TCPS_SYN_SENT:
    tcp_send_rst(t->ns, t->lip, t->lport, t->rip, t->rport, t->snd_nxt, 0, 0);
    tcb_free(t);
    break;
  case TCPS_LISTEN:
  case TCPS_CLOSED:
    tcb_free(t);
    break;
  default:
    // already closing; the timers finish the job
    break;
  }
  release(&netlock);
}

// bytes the application can read, 1 if a write would not block, or -1
// when the connection is gone. netlock is taken here; the caller must
// not hold it.
int
tcp_ready(struct tcpcb *t, int write)
{
  if (t == 0)
    return -1;
  acquire(&netlock);
  int r;
  if (write) {
    if (t->state == TCPS_CLOSED || t->state == TCPS_TIME_WAIT)
      r = -1;
    else if (t->state != TCPS_ESTABLISHED && t->state != TCPS_CLOSE_WAIT)
      r = 0; // still connecting
    else
      r = tring_space(&t->snd) > 0;
  } else {
    r = tring_len(&t->rcv);
    if (r == 0 && (t->rcv_fin || t->state == TCPS_CLOSED || t->state == TCPS_TIME_WAIT))
      r = -1;
  }
  release(&netlock);
  return r;
}
