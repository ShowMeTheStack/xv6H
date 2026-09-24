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
  // the network it is on: the namespace of the process that made it.
  // Two containers may both have a socket on port 80, and a packet is
  // delivered to the one whose namespace it arrived in.
  struct netns *ns;
  uint32 lip, rip;
  uint16 lport, rport;
  struct mbuf *rxq;   // received datagrams (UDP/ICMP)
  int rxcount;
  struct tcpcb *tcp;  // SOCK_STREAM
  int closed;
  int ttl;            // what to put in the IP header; 0 for the usual 64
  int want_errors;    // deliver ICMP errors here as well as replies
  uint deadline;      // a waiting receive gives up at this tick; 0 for none
  int rcvtimeo;       // SO_RCVTIMEO, in ticks; 0 to wait as long as it takes
  int sndtimeo;       // SO_SNDTIMEO, in ticks; what bounds connect(2)
  int shut_rd, shut_wr; // shutdown(2) has closed this direction
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
int sockshutdown(struct sock *s, int how);
struct sock *ipsock_new(int type);
int ipsock_bind(struct sock *s, int port);
int ipsock_connect(struct sock *s, uint32 ip, int port);
int ipsock_listen(struct sock *s, int backlog);
struct sock *ipsock_accept(struct sock *s, int *err);
int ipsock_sendto(struct sock *s, int user, uint64 buf, int n, uint32 dst,
                  int port);
int ipsock_recvfrom(struct sock *s, int user, uint64 buf, int n, uint32 *rip,
                    int *rport, int timeout, int nowait);
int ipsock_setopt(struct sock *s, int opt, int value);
int ipsock_getopt(struct sock *s, int opt, int *value);
int sockread(struct sock *s, int user, uint64 addr, int n);
int sockwrite(struct sock *s, int user, uint64 addr, int n);
#endif
