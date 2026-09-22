#ifndef XV6_NET_H
#define XV6_NET_H
#include "types.h"
//
// networking: packet buffers, protocol headers, configuration.
//

#define MBUF_SIZE 2048

// a packet buffer: one per packet, allocated with kalloc(); the payload
// starts at head and is len bytes; there is headroom below head for
// protocol headers to be prepended.
struct mbuf {
  struct mbuf *next;
  char *head;
  uint len;
  uint32 arg;      // protocol-private (e.g. source ip for received UDP)
  uint16 arg16;
  uint8 arg_type;  // an ICMP error's type; zero for an ordinary datagram
  char buf[MBUF_SIZE];
};

struct mbuf *mbufalloc(uint headroom);
void mbuffree(struct mbuf *m);
char *mbufpush(struct mbuf *m, uint n);  // prepend n bytes
char *mbufpull(struct mbuf *m, uint n);  // remove n bytes from the front
char *mbufput(struct mbuf *m, uint n);   // append n bytes

#define MBUF_HEADROOM 128

// ---- byte order ----
static inline uint16
bswap16(uint16 x)
{
  return (x << 8) | (x >> 8);
}
static inline uint32
bswap32(uint32 x)
{
  return ((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | (x >> 24);
}
// a user program that has included <netinet/in.h> already has these
// as functions, which are the same four swaps; do not shadow them
#ifndef htons
#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)
#endif

// ---- ethernet ----
#define ETHADDR_LEN 6
#define ETHTYPE_IP  0x0800
#define ETHTYPE_ARP 0x0806

struct eth {
  uint8 dhost[ETHADDR_LEN];
  uint8 shost[ETHADDR_LEN];
  uint16 type;
} __attribute__((packed));

// ---- arp ----
#define ARP_HRD_ETHER 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp {
  uint16 hrd;
  uint16 pro;
  uint8 hln;
  uint8 pln;
  uint16 op;
  uint8 sha[ETHADDR_LEN];
  uint32 sip;
  uint8 tha[ETHADDR_LEN];
  uint32 tip;
} __attribute__((packed));

// ---- ipv4 ----
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct ip {
  uint8 vhl;   // version << 4 | header length >> 2
  uint8 tos;
  uint16 len;
  uint16 id;
  uint16 off;
  uint8 ttl;
  uint8 pro;
  uint16 sum;
  uint32 src;
  uint32 dst;
} __attribute__((packed));

#define MAKE_IP(a, b, c, d) (((uint32)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

// ---- icmp ----
#define ICMP_ECHOREPLY 0
#define ICMP_UNREACH   3
#define ICMP_SRCQUENCH 4
#define ICMP_REDIRECT  5
#define ICMP_ECHO      8
#define ICMP_TIMXCEED  11
#define ICMP_PARAMPROB 12

struct icmp {
  uint8 type;
  uint8 code;
  uint16 sum;
  uint16 id;
  uint16 seq;
} __attribute__((packed));

// ---- udp ----
struct udp {
  uint16 sport;
  uint16 dport;
  uint16 ulen;
  uint16 sum;
} __attribute__((packed));

// ---- tcp ----
struct tcp {
  uint16 sport;
  uint16 dport;
  uint32 seq;
  uint32 ack;
  uint8 off;   // data offset << 4
  uint8 flags;
  uint16 win;
  uint16 sum;
  uint16 urp;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

// ---- configuration ----
struct netcfg {
  int up;             // interface configured
  int dhcp;           // configured by DHCP (else static)
  uint32 ip, mask, gw, dns;
  uint8 mac[ETHADDR_LEN];
};
extern struct netcfg netcfg;
extern struct spinlock netlock;

// user-visible interface information (netinfo system call).
struct netinfo {
  uint32 ip, mask, gw, dns;
  uint8 mac[6];
  int up, dhcp;
  uint64 rx_packets, tx_packets, rx_dropped, tx_dropped, policy_dropped;
};

char *mac2str(const uint8 *mac, char *buf); // 18 bytes

// virtio_net.c
int virtio_net_init(void);
void virtio_net_intr(int idx);
int virtio_net_tx(struct mbuf *m);
int virtio_net_present(void);

// bridge.c (hyp.h)
void bridge_rx_uplink(struct mbuf *m);
int bridge_tx_host(struct mbuf *m);
extern uint64 bridge_guest_blocked;

// net.c
void netinit(void);
void net_rx(struct mbuf *m);
void net_timer(void);
int net_tx_ip(struct mbuf *m, uint32 dst, uint8 proto);
int net_tx_ip_ttl(struct mbuf *m, uint32 dst, uint8 proto, int ttl);
int net_tx_udp(struct mbuf *m, uint32 dst, uint16 sport, uint16 dport, int ttl);
int net_tx_icmp_echo(uint32 dst, uint16 id, uint16 seq, char *data, int n, int ttl);
uint16 ip_checksum(void *data, int len, uint32 seed);
uint16 l4_checksum(uint32 src, uint32 dst, uint8 proto, void *data, int len);
int net_policy_ok(uint32 dst, uint8 proto, uint16 dport, int from_guest);
void net_set_static(uint32 ip, uint32 mask, uint32 gw, uint32 dns);
void net_stats(struct netinfo *ni);
void arp_walk(void (*fn)(uint32 ip, const uint8 *mac, int valid, void *arg), void *arg);
extern uint64 net_rx_packets, net_tx_packets, net_rx_dropped, net_tx_dropped, net_policy_dropped;

// pktcap.c: /dev/pktcap, which hands every frame to a reader
void pktcap_init(void);
void pktcap_register(void);
int pktcap_active(void);
void pktcap_frame(const void *frame, uint len);
void pktcap_stats(uint64 *got, uint64 *lost);

// sock.c
struct sock;
void sockinit(void);
int sock_udp_rx(struct mbuf *m, uint32 src, uint16 sport, uint16 dport);
int sock_icmp_rx(struct mbuf *m, uint32 src, uint16 id);
int sock_icmp_error(uint32 src, int type, int code, uint16 id, uint16 seq);
int sock_udp_error(uint32 src, int type, int code, uint16 sport);
void sock_timer(void);

// tcp.c
void tcp_rx(struct mbuf *m, uint32 src, uint32 dst);
void tcp_timer(void);
#endif
