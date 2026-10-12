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
  struct netif *rxdev;   // while it waits on the deferred receive queue
  char *head;
  uint len;
  uint32 arg;      // protocol-private (e.g. source ip for received UDP)
  uint16 arg16;
  uint8 arg_type;  // an ICMP error's type; zero for an ordinary datagram
  char buf[MBUF_SIZE];
};

struct netif;
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
extern struct spinlock netlock;

// ---- network namespaces ----
//
// every stack in the machine is one of these: the root namespace, which
// owns the physical NIC and the switch the guests are on, and later one
// per container, each with its own addresses, its own routes and its
// own idea of what "port 80" means. A packet belongs to the namespace
// of the interface it arrived on; a socket to the namespace of the
// process that made it.
//
// what is here is what was file-static in net.c: one stack's worth of
// state, and nothing that is shared between stacks.

#define ARP_ENTRIES 16
#define ARP_PENDING 4

struct arpent {
  uint32 ip;
  uint8 mac[ETHADDR_LEN];
  int valid;
  uint requested;                    // tick of the last request
  struct mbuf *pending[ARP_PENDING]; // packets waiting for the answer
};

struct dhcpstate {
  int state;        // 0 idle, 1 discovering, 2 requesting, 3 bound
  uint32 xid;
  uint32 offered, server;
  uint last_tx;
  int attempts;
};

// ---- interfaces and routes ----
//
// an interface is a place a frame can leave by and arrive at: the
// physical NIC, the loopback, and one end of the wire into a
// container. It knows its own addresses, so that a namespace with two
// of them can answer on both, and it knows how to send, so that the
// stack does not care whether the other end is a driver, the switch or
// another namespace.
#define NETIF_NAMELEN 8
#define NETIF_LOOPBACK 1
#define NETIF_ISOLATED 2   // no route out of the namespace through this one

struct netif {
  char name[NETIF_NAMELEN];
  uint8 mac[ETHADDR_LEN];
  uint32 ip, mask;
  int up, flags;
  struct netns *ns;                  // the namespace it belongs to
  int (*tx)(struct netif *, struct mbuf *); // how a frame leaves
  void *priv;                        // the driver's, the switch's, the wire's
  uint64 rx_packets, tx_packets, rx_dropped, tx_dropped;
  struct netif *next;
};

// longest prefix wins; a gateway of zero means the destination is on
// this interface's own subnet and is reached directly.
struct route {
  uint32 dst, mask, gw;
  struct netif *dev;
  int metric;
  struct route *next;
};

#define NNETNS 8       // network namespaces at once, besides the root's

struct netns {
  int ref;                           // processes (through nsproxy) pointing
  int id;                            // what /proc/<pid>/ns/net calls it
  struct netif *ifs;                 // its interfaces, lo first
  struct netif *lo;
  struct route *routes;              // longest prefix match
  struct netcfg cfg;                 // addresses and the interface's mac
  struct arpent arp[ARP_ENTRIES];
  uint16 ip_id;                      // the IP identification field
  struct mbuf *lo_head, *lo_tail;    // the loopback's queue
  int lo_delivering;
  struct dhcpstate dhcp;
  uint64 rx_packets, tx_packets, rx_dropped, tx_dropped, policy_dropped;
};

extern struct netns rootns;

// a namespace of its own for a container: a loopback and nothing else,
// until something attaches it to a gateway. netns_put takes the last
// reference away with the interfaces, the routes and the sockets.
struct netns *netns_new(void);
void netns_put(struct netns *ns);
void netns_hold(struct netns *ns);
int netns_id(struct netns *ns);
void netif_del(struct netif *dev);
void route_flush(struct netns *ns);

// the namespace a call belongs to: a process's own, or the root. The
// receive path uses the namespace of the interface instead, because a
// packet arrives without a process behind it.
struct netns *netns_cur(void);

// interfaces: made, named, given addresses, and found again
struct netif *netif_add(struct netns *ns, const char *name, int flags,
                        int (*tx)(struct netif *, struct mbuf *), void *priv);
struct netif *netif_find(struct netns *ns, const char *name);
void netif_set_addr(struct netif *dev, uint32 ip, uint32 mask);
void netif_rx(struct netif *dev, struct mbuf *m);  // a frame arrived here
void net_rx_dev(struct netif *dev, struct mbuf *m); // and the same, deferred

// routes: added, removed, and looked up. route_lookup answers with the
// interface to send by and the address to resolve (the destination
// itself when it is on-link, the gateway when it is not).
int route_add(struct netns *ns, uint32 dst, uint32 mask, uint32 gw,
              struct netif *dev, int metric);
void route_flush_dev(struct netns *ns, struct netif *dev);
struct netif *route_lookup(struct netns *ns, uint32 dst, uint32 *nexthop);
// ---- connection tracking ----
#define NCONNTRACK 256

#define CT_NEW     0   // seen one way only
#define CT_SYN     1   // TCP: a SYN went out, nothing has come back
#define CT_ESTAB   2   // both ends have spoken
#define CT_CLOSING 3   // a FIN has been seen
#define CT_CLOSED  4   // reset, or finished

#define CT_OUT 0       // a packet from inside
#define CT_IN  1       // a packet from outside

// the five numbers, plus whatever of the packet tells the state
struct ctkey {
  uint8 proto;
  uint8 flags;         // TCP flags, or the ICMP type
  uint32 src, dst;
  uint16 sport, dport;
};

struct ct {
  uint8 proto;         // 0 means the slot is free
  uint8 state;
  uint8 published;     // let in by a published port rather than by us
  // who said the first word: 0 for the host or the world beyond the
  // uplink, otherwise a machine's id plus one. A machine is answered
  // about what it said, and about nothing else.
  uint8 opener;
  uint8 icmp_type;     // ICMP: the type of the packet that started it
  uint32 src, dst;
  uint16 sport, dport;
  uint64 packets, replies;
  uint last;           // the tick it was last used
  uint expires;        // seconds left, filled in by ct_walk only
  uint64 seq;          // for least-recently-used
};

extern uint64 ct_created, ct_dropped, ct_early;
void ctinit(void);
int ct_track(const uint8 *ip, int len, int dir, int who);
int ct_open(const uint8 *ip, int len);
void ct_timer(void);
int ct_count(void);
void ct_walk(void (*fn)(const struct ct *, void *), void *arg);

void netif_walk(struct netns *ns, void (*fn)(const struct netif *, void *),
                void *arg);
int netif_tx(struct netif *dev, struct mbuf *m, const uint8 *dhost,
             uint16 type);
// what a program asks of the routing table. The same shape answers a
// question: for RT_GET the kernel fills in the route it would use.
#define RT_ADD 0
#define RT_DEL 1
#define RT_GET 2
// and an address on an interface, with the two routes that go with it:
// what DHCP does when it is answered, done by hand. `ip addr add'.
#define RT_ADDR 3

struct rtreq {
  uint32 dst, mask, gw;
  int metric;
  char dev[NETIF_NAMELEN];
};

// ---- gateways ----
//
// A gateway object is a router inside the host: a private subnet on one
// side, a port on the switch with its own address on the other, and
// translation between them. kernel/cgw.c has the whole of it; this is
// what a program says to make one.
#define CGW_NAMELEN 16

#define GW_ADD 0
#define GW_DEL 1
// and the three a container's runtime makes, from inside the network
// namespace it has just unshared and while it is still privileged
#define GW_ATTACH 2        // this namespace, behind that gateway, at addr
#define GW_PUB 3           // a port on the outside that is ours
#define GW_UNPUB 4

struct gwreq {
  uint32 addr, mask;       // the address it owns inside, and the subnet
  uint32 pub_ip;           // its address on the root subnet (0: chosen)
  char name[CGW_NAMELEN];
  // GW_ATTACH: the interface to make (empty: eth0)
  char ifname[NETIF_NAMELEN];
  // GW_PUB, GW_UNPUB: the port outside, the port inside, and which of
  // tcp (6) and udp (17) it is
  uint16 pport, cport;
  uint8 proto;
};

int route_del(struct netns *ns, uint32 dst, uint32 mask, uint32 gw);
void netns_configure(struct netns *ns, const char *ifname, uint32 ip,
                     uint32 mask, uint32 gw);
void route_walk(struct netns *ns,
                void (*fn)(const struct route *r, void *arg), void *arg);

// the root namespace's configuration, which is what the NIC, the
// switch and the DHCP client are talking about.
#define netcfg (rootns.cfg)

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
int net_tx_ip_ns(struct netns *ns, struct mbuf *m, uint32 dst, uint8 proto);
uint32 net_src_for(struct netns *ns, uint32 dst);
int net_tx_ip_ttl(struct mbuf *m, uint32 dst, uint8 proto, int ttl);
int net_tx_udp_ns(struct netns *ns, struct mbuf *m, uint32 dst, uint16 sport,
                  uint16 dport, int ttl);
int net_tx_icmp_echo_ns(struct netns *ns, uint32 dst, uint16 id, uint16 seq,
                        char *data, int n, int ttl);
uint16 ip_checksum(void *data, int len, uint32 seed);
uint16 l4_checksum(uint32 src, uint32 dst, uint8 proto, void *data, int len);
int net_policy_ok(uint32 dst, uint8 proto, uint16 dport, int from_guest);
void net_set_static(uint32 ip, uint32 mask, uint32 gw, uint32 dns);
void net_stats(struct netinfo *ni);
void arp_walk(void (*fn)(uint32 ip, const uint8 *mac, int valid, void *arg), void *arg);
// the physical NIC's and the switch's counters, which are the root
// namespace's: a container counts its own packets in its own stack.
#define net_rx_packets     (rootns.rx_packets)
#define net_tx_packets     (rootns.tx_packets)
#define net_rx_dropped     (rootns.rx_dropped)
#define net_tx_dropped     (rootns.tx_dropped)
#define net_policy_dropped (rootns.policy_dropped)

// pktcap.c: /dev/pktcap, which hands every frame to a reader
void pktcap_init(void);
void pktcap_register(void);
int pktcap_active(void);
void pktcap_frame(const void *frame, uint len);
void pktcap_stats(uint64 *got, uint64 *lost);

// sock.c
struct sock;
void sockinit(void);
int sock_udp_rx(struct netns *ns, struct mbuf *m, uint32 src, uint16 sport,
                uint16 dport);
int sock_icmp_rx(struct netns *ns, struct mbuf *m, uint32 src, uint16 id);
int sock_icmp_error(struct netns *ns, uint32 src, int type, int code,
                    uint16 id, uint16 seq);
int sock_udp_error(struct netns *ns, uint32 src, int type, int code,
                   uint16 sport);
void sock_timer(void);

// tcp.c
void tcp_rx(struct netns *ns, struct mbuf *m, uint32 src, uint32 dst);
void tcp_timer(void);
#endif
