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
};

extern struct sock socks[NSOCK];
void sockclose(struct sock *s);
int sockread(struct sock *s, int user, uint64 addr, int n);
int sockwrite(struct sock *s, int user, uint64 addr, int n);
