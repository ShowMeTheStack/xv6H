#ifndef XV6_SOCKET_H
#define XV6_SOCKET_H
// socket types (shared with user programs).
#define SOCK_NONE   0
#define SOCK_DGRAM  1 // UDP
#define SOCK_STREAM 2 // TCP
#define SOCK_ICMP   3 // ping: send/receive ICMP echo
#endif
