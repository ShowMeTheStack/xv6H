#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <sys/socket.h>
#include <stdint.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
  in_addr_t s_addr;      // in network order, as everything here is
};

struct sockaddr_in {
  sa_family_t sin_family;
  in_port_t sin_port;    // network order
  struct in_addr sin_addr;
  char sin_zero[8];
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

// the socket options that belong to IP and to TCP
#define IP_TTL        2
#define IP_RECVERR   11
#define TCP_NODELAY   1

// what an ICMP error says. a socket with IP_RECVERR turned on is
// given one of these alongside the message, as a control message at
// level IPPROTO_IP and type IP_RECVERR -- which is where Linux puts
// it too, in <linux/errqueue.h>. ee_type and ee_code are the ICMP
// type and code, so a traceroute reads the hop's answer from here
// and the sequence number from the address.
struct sock_extended_err {
  uint32_t ee_errno;
  uint8_t ee_origin;
  uint8_t ee_type;
  uint8_t ee_code;
  uint8_t ee_pad;
  uint32_t ee_info;
  uint32_t ee_data;
};

#define SO_EE_ORIGIN_NONE  0
#define SO_EE_ORIGIN_LOCAL 1
#define SO_EE_ORIGIN_ICMP  2

// the ICMP messages this system names
#define ICMP_ECHOREPLY   0
#define ICMP_UNREACH     3
#define ICMP_ECHO        8
#define ICMP_TIMXCEED   11

// this machine is little-endian, so these four are byte swaps. they
// are written out rather than taken from a header so that a program
// reading this file can see what network order means.
//
// kernel/net.h has the same four as macros over its own bswap16 and
// bswap32; a program that has included it already has them, and what
// they do is the same, so this file leaves them alone.
#ifndef htons
static inline uint16_t
htons(uint16_t x)
{
  return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint16_t
ntohs(uint16_t x)
{
  return htons(x);
}
static inline uint32_t
htonl(uint32_t x)
{
  return ((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) |
         (x >> 24);
}
static inline uint32_t
ntohl(uint32_t x)
{
  return htonl(x);
}
#endif
#endif
