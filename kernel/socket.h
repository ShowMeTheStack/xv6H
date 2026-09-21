#ifndef XV6_SOCKET_H
#define XV6_SOCKET_H
// socket types (shared with user programs).
#define SOCK_NONE   0
#define SOCK_DGRAM  1 // UDP
#define SOCK_STREAM 2 // TCP
#define SOCK_ICMP   3 // ping: send/receive ICMP echo

// sockopt(fd, option, value)
#define SO_TTL        1 // the time to live IP packets go out with
#define SO_ICMP_ERRORS 2 // deliver ICMP errors to this socket as well

// what an ICMP socket's recvfrom puts in its `port`: the sequence
// number of the probe, and above it the ICMP type when the message is
// an error rather than a reply. an echo reply is type 0, so a port
// with nothing above the low sixteen bits is an ordinary answer.
#define ICMP_SEQ(port)  ((port) & 0xffff)
#define ICMP_TYPE(port) (((port) >> 16) & 0xff)
#endif
