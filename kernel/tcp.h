// TCP (tcp.c)

#define NTCP 32
#define TCP_MSS 1400
#define TCP_SNDBUF (2 * 4096)
#define TCP_RCVBUF (4 * 4096)
#define TCP_BACKLOG 4

enum tcpstate {
  TCPS_CLOSED = 0, TCPS_LISTEN, TCPS_SYN_SENT, TCPS_SYN_RCVD, TCPS_ESTABLISHED,
  TCPS_FIN_WAIT_1, TCPS_FIN_WAIT_2, TCPS_CLOSE_WAIT, TCPS_CLOSING, TCPS_LAST_ACK, TCPS_TIME_WAIT
};

// a byte ring over a few non-contiguous pages.
struct tring {
  char *pg[4];
  uint size;
  uint r, w; // read/write positions (mod size); w - r = bytes held
};

struct tcpcb {
  int state;
  int used;
  struct sock *sock;
  uint32 lip, rip;
  uint16 lport, rport;
  // send side
  uint32 iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
  uint32 cwnd, ssthresh;
  uint16 mss;
  int dupacks;
  int fin_queued;    // FIN to send after the data
  int fin_sent;      // FIN has been (re)transmitted at least once
  struct tring snd;  // unacknowledged and unsent data, starting at snd_una
  // receive side
  uint32 irs, rcv_nxt;
  struct tring rcv;  // in-order data for the application
  int rcv_fin;       // peer sent FIN (EOF after rcv drains)
  int rcv_rst;       // connection reset
  // timers (ticks)
  uint rto, rtx_timer, retries, timewait_timer;
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
