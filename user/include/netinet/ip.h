#ifndef _NETINET_IP_H
#define _NETINET_IP_H
#include <netinet/in.h>

// an IPv4 header as it goes over the wire. a program that reads a
// capture from /dev/pktcap walks these.
struct iphdr {
  uint8_t ip_vhl;       // version in the top four bits, length in words below
  uint8_t ip_tos;
  uint16_t ip_len;
  uint16_t ip_id;
  uint16_t ip_off;
  uint8_t ip_ttl;
  uint8_t ip_p;
  uint16_t ip_sum;
  uint32_t ip_src;
  uint32_t ip_dst;
};

#define IP_VERSION(h) ((h)->ip_vhl >> 4)
#define IP_HLEN(h)    (((h)->ip_vhl & 0x0f) * 4)

#define IPVERSION  4
#define IPDEFTTL   64
#define IP_MAXPACKET 65535

// the fragment offset field, and the two flags above it
#define IP_RF 0x8000
#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFMASK 0x1fff

// the type of service bits, for a program that wants to read them
#define IPTOS_LOWDELAY    0x10
#define IPTOS_THROUGHPUT  0x08
#define IPTOS_RELIABILITY 0x04
#define IPTOS_MINCOST     0x02
#endif
