#ifndef XV6_SOCKET_H
#define XV6_SOCKET_H
//
// what the kernel's socket layer calls things. the numbers for the
// kinds of socket are the ones user programs pass to socket(), so
// that sockapi.c does not have to translate a value into another
// value that means the same thing.
//

// address families
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10 // named so it can be refused, not so it can be used

// kinds of socket. SOCK_ICMP is this kernel's own: it is what
// socket(AF_INET, SOCK_DGRAM or SOCK_RAW, IPPROTO_ICMP) becomes.
#define SOCK_NONE   0 // a free slot in the table
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_ICMP   4
#define SOCK_SEQPACKET 5
#define SOCK_NTYPES 6

// the flags socket() and accept4() take above the type
#define SOCK_NONBLOCK 0x0800
#define SOCK_CLOEXEC  0x0080

// the flags a message carries, the same numbers sys/socket.h uses
#define MSG_PEEK      0x0002
#define MSG_CTRUNC    0x0008
#define MSG_TRUNC     0x0020
#define MSG_DONTWAIT  0x0040
#define MSG_WAITALL   0x0100
#define MSG_NOSIGNAL  0x4000

// shutdown(2)
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// the levels setsockopt and getsockopt take
#define SOL_SOCKET 1

// options at SOL_SOCKET, with Linux's numbers
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_PASSCRED  16
#define SO_PEERCRED  17
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_ACCEPTCONN 30

// options at IPPROTO_IP
#define IP_TTL        2
#define IP_RECVERR   11

// ancillary data
#define SCM_RIGHTS      1
#define SCM_CREDENTIALS 2

// the knobs struct sock itself has. ipsock_setopt and ipsock_getopt
// take these; the POSIX names above turn into them in sockapi.c.
#define SO_TTL            1 // the time to live IP packets go out with
#define SO_ICMP_ERRORS    2 // deliver ICMP errors here as well as replies
#define SO_RCVTIMEO_TICKS 3 // a receive gives up after this many ticks
#define SO_SNDTIMEO_TICKS 4 // a connect gives up after this many ticks

// what an ICMP socket's recvfrom puts in its `port`: the sequence
// number of the probe, and above it the ICMP type when the message is
// an error rather than a reply. an echo reply is type 0, so a port
// with nothing above the low sixteen bits is an ordinary answer.
#define ICMP_SEQ(port)  ((port) & 0xffff)
#define ICMP_TYPE(port) (((port) >> 16) & 0xff)
#endif
