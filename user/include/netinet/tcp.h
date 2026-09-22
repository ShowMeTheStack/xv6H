#ifndef _NETINET_TCP_H
#define _NETINET_TCP_H
#include <netinet/in.h>

// the options that belong to a TCP connection. this stack sends a
// segment as soon as it has one, so TCP_NODELAY is already how it
// behaves and setting it changes nothing; it is here so that a
// program that asks for it is answered rather than refused.
#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_KEEPIDLE  4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT   6

// the states a connection goes through, as /proc/net/tcp numbers them
enum {
  TCP_ESTABLISHED = 1,
  TCP_SYN_SENT,
  TCP_SYN_RECV,
  TCP_FIN_WAIT1,
  TCP_FIN_WAIT2,
  TCP_TIME_WAIT,
  TCP_CLOSE,
  TCP_CLOSE_WAIT,
  TCP_LAST_ACK,
  TCP_LISTEN,
  TCP_CLOSING,
};

struct tcphdr {
  uint16_t th_sport;
  uint16_t th_dport;
  uint32_t th_seq;
  uint32_t th_ack;
  uint8_t th_off_x2;    // the data offset in the top four bits
  uint8_t th_flags;
  uint16_t th_win;
  uint16_t th_sum;
  uint16_t th_urp;
};

#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10
#define TH_URG  0x20
#endif
