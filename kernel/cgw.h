#ifndef XV6_CGW_H
#define XV6_CGW_H
//
// gateway objects: a router inside the host, with a private subnet on
// one side and an ordinary port on the switch on the other. See
// kernel/cgw.c.
//
// This header is the kernel's; what a program needs (struct gwreq, and
// the operations) is in net.h, which user programs include.
//

#define NCGW 4            // gateways at once
#define NGWPORT 8         // network namespaces behind one gateway
#define NGWPUB 8          // ports it publishes for them
#define NGWNAT 128        // translated conversations per gateway
#define CGW_PENDING 8     // frames waiting for a hardware address
#define CGW_PORT_LOW 30000
#define CGW_PORT_HIGH 40000

// one translated conversation. The inside address and port, the port the
// gateway gave it outside, and the far end it is with.
struct gwnat {
  uint8 proto;            // 0: free
  uint8 published;        // it arrived because a port was published
  uint8 closing;          // a FIN or a reset has been seen
  uint32 iip;
  uint16 iport;
  uint16 pport;
  uint32 rip;
  uint16 rport;
  uint last;
  uint64 packets, replies;
};

// a container's end of the wire. The gateway holds the interface inside
// the namespace, which is where a frame for that address is delivered;
// there is no switch port and no virtual machine anywhere in it.
struct gwport {
  struct netns *ns;            // 0: free
  struct cgw *gw;
  struct netif *dev;
  uint32 ip;
  uint8 mac[ETHADDR_LEN];
};

// a port the gateway answers for on behalf of something behind it
struct gwpub {
  uint8 proto;                 // 0: free
  uint16 pport;                // the port outside
  uint32 ip;                   // and what it belongs to inside
  uint16 cport;
};

struct cgw {
  int used;
  char name[CGW_NAMELEN];
  uint32 addr, mask;      // its own address inside, and the subnet
  uint32 pub_ip;          // its address on the root subnet
  uint8 mac[ETHADDR_LEN];      // the port inside
  uint8 pub_mac[ETHADDR_LEN];  // the port on the switch
  // the one hardware address it needs to know outside, and the frames
  // waiting for it
  uint32 nh_ip;
  uint8 nh_mac[ETHADDR_LEN];
  int nh_valid;
  uint nh_asked;
  struct mbuf *pending[CGW_PENDING];
  uint32 pending_ip[CGW_PENDING];
  uint pending_at[CGW_PENDING];
  struct gwnat nat[NGWNAT];
  struct gwport ports[NGWPORT];
  struct gwpub pub[NGWPUB];
  uint16 next_port;
  uint64 rx, tx, snat, dnat, translated, dropped, natfull;
  struct spinlock lock;
};

struct vm;

void cgwinit(void);
int cgw_create(const char *name, uint32 addr, uint32 mask, uint32 pub_ip,
               struct cgw **out);
int cgw_destroy(struct cgw *g);
struct cgw *cgw_find(const char *name);
struct cgw *cgw_by_addr(uint32 ip);
struct cgw *cgw_by_pub(uint32 ip);
struct cgw *cgw_by_mac(const uint8 *mac, int *pub);
struct cgw *cgw_at(int i);
void cgw_rx_internal(struct cgw *g, struct vm *from, struct mbuf *m);

// the network namespaces behind a gateway: a container's interface is a
// port on it, the way a machine's switch port is.
struct netns;
struct netif;
int cgw_attach_ns(struct cgw *g, struct netns *ns, uint32 ip, const char *name);
void cgw_detach_ns(struct netns *ns);
int cgw_publish_ns(struct cgw *g, uint8 proto, uint16 pport, uint32 ip,
                   uint16 cport);
int cgw_unpublish_ns(struct cgw *g, uint8 proto, uint16 pport);
void cgw_rx_public(struct cgw *g, struct mbuf *m);
void cgw_arp_request(struct cgw *g, uint32 target);
void cgw_timer(void);
void cgw_walk(void (*fn)(const struct cgw *, void *), void *arg);

// the machines behind a gateway (kernel/hyp.c, which owns the table)
int cgw_members(struct cgw *g);
struct vm *cgw_member_by_ip(struct cgw *g, uint32 ip);
int cgw_published(struct cgw *g, uint8 proto, uint16 hport, struct vm **vm,
                  uint16 *gport);

// the switch (kernel/bridge.c)
void bridge_from_cgw(struct cgw *g, struct mbuf *m, int pub);
void bridge_to_vm(struct vm *vm, struct mbuf *m);

#endif
