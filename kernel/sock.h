#ifndef XV6_SOCK_H
#define XV6_SOCK_H
#include "types.h"
// sockets (sock.c)

#define NSOCK 32
#define SOCK_RXQ 16 // queued datagrams per socket

#include "socket.h"

struct tcpcb;

struct sock {
  struct spinlock lock;
  int type;
  int id;
  uint32 lip, rip;
  uint16 lport, rport;
  struct mbuf *rxq;   // received datagrams (UDP/ICMP)
  int rxcount;
  struct tcpcb *tcp;  // SOCK_STREAM
  int closed;
  int ttl;            // what to put in the IP header; 0 for the usual 64
  int want_errors;    // deliver ICMP errors here as well as replies
  uint deadline;      // a waiting receive gives up at this tick; 0 for none
};

// one socket as /proc/net sees it, copied out under the socket's lock
struct sockinfo {
  int index;
  int type;
  int state;      // a TCP state, or -1 for a socket that has none
  uint32 lip, rip;
  uint16 lport, rport;
  int rxcount;    // queued datagrams
  uint32 rxbytes, txbytes;
  int closed;
};

extern struct sock socks[NSOCK];
void sock_walk(void (*fn)(const struct sockinfo *si, void *arg), void *arg);
void sock_timer(void);
void sockclose(struct sock *s);
int sockread(struct sock *s, int user, uint64 addr, int n);
int sockwrite(struct sock *s, int user, uint64 addr, int n);
#endif
