// TCP (tcp.c)

#define NTCP 32
#define TCP_MSS 1400
// Four pages a ring, and that number is the migration wire's speed limit: with
// at most 8 KB in flight per ~10 ms round trip through the emulated path, a
// machine crosses at 0.83 MB/s (116 MB in 136 s between hosts that ping in 0 ms).
// It is NOT raised here, and the reason is measured: at sixteen pages the same
// transfer ran at 0.05 MB/s -- sixteen times slower -- because forty-odd
// segments in flight overrun the receiver's frame queue and its four-segment
// out-of-order buffer, and every burst became retransmission timeouts. Bigger
// rings need a deeper receive queue and real out-of-order recovery (SACK) to go
// with them; that is a change to make on purpose, with a TCP throughput test,
// not a constant to bump.
#define TRING_PAGES 4
#define TCP_SNDBUF (2 * 4096)
#define TCP_RCVBUF (TRING_PAGES * 4096)
#define TCP_BACKLOG 4
// Segments kept while there is a hole in front of them. Throwing away
// what arrives after a lost segment turns one loss into a window of
// losses: the sender has to send the whole window again, one segment per
// round trip, because each retransmission only fills the first gap. Four
// is a window's worth for this stack's eight-kilobyte send ring.
#define TCP_OOO 4

enum tcpstate {
  TCPS_CLOSED = 0, TCPS_LISTEN, TCPS_SYN_SENT, TCPS_SYN_RCVD, TCPS_ESTABLISHED,
  TCPS_FIN_WAIT_1, TCPS_FIN_WAIT_2, TCPS_CLOSE_WAIT, TCPS_CLOSING, TCPS_LAST_ACK, TCPS_TIME_WAIT
};

// a byte ring over a few non-contiguous pages.
struct tring {
  char *pg[TRING_PAGES];
  uint size;
  uint r, w; // read/write positions (mod size); w - r = bytes held
};

struct tcpcb {
  int state;
  int used;
  struct sock *sock;
  // the network this connection is on. A retransmission comes from the
  // timer, where there is no process to ask, so the connection has to
  // know for itself.
  struct netns *ns;
  uint32 lip, rip;
  uint16 lport, rport;
  // send side
  uint32 iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
  uint32 cwnd, ssthresh;
  uint16 mss;
  int dupacks;
  int fin_queued;    // FIN to send after the data
  int fin_sent;      // FIN has been (re)transmitted at least once
  // tcp_output is inside this connection already. A segment sent to an
  // address of this machine's own is delivered by the send itself, and
  // what that delivery does -- an acknowledgement, a window update --
  // comes back here before the send has returned.
  int sending;
  struct tring snd;  // unacknowledged and unsent data, starting at snd_una
  // receive side
  uint32 irs, rcv_nxt;
  struct tring rcv;  // in-order data for the application
  // what arrived out of order, waiting for the piece in front of it.
  // The mbufs are this connection's until they are delivered or it goes.
  struct {
    struct mbuf *m;
    char *data;
    uint32 seq, len;
  } ooo[TCP_OOO];
  int nooo;
  int rcv_fin;       // peer sent FIN (EOF after rcv drains)
  int rcv_rst;       // connection reset
  // timers (ticks)
  uint rto, rtx_timer, retries, timewait_timer;
  // a sender whose peer has closed its window has nothing to retransmit
  // and nothing it is allowed to send, so no timer of the two above is
  // running: this one is, and it probes.
  uint persist;
  // a connect that says how long it will wait: nothing else would wake
  // the sleeper when the answer never comes, so the tick hand does
  uint conn_deadline;
  uint srtt, rttvar;
  uint32 rtt_seq;    // sequence number being timed (0: none)
  uint rtt_start;
  // listening socket: completed connections waiting for accept
  struct tcpcb *backlog[TCP_BACKLOG];
  int nbacklog, maxbacklog;
  struct tcpcb *parent;
};

int tcp_connect(struct sock *s);
int tcp_listen(struct sock *s, int backlog);
struct sock *tcp_accept(struct sock *s);
int tcp_read(struct sock *s, int user, uint64 addr, int n);
int tcp_ready(struct tcpcb *t, int write);
int tcp_write(struct sock *s, int user, uint64 addr, int n);
void tcp_close(struct sock *s);
void tcp_shutdown(struct sock *s);
