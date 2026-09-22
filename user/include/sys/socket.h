#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <stddef.h>

// the address families this system has. AF_INET is the network, and
// AF_UNIX is the machine talking to itself through a name in the file
// system; there is no IPv6 here, and AF_INET6 is defined only so that
// a program that asks for it is refused rather than misunderstood.
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

// the kinds of socket. the numbers are Linux's.
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_SEQPACKET 5
// the flags socket() and accept4() take alongside the type
#define SOCK_NONBLOCK 0x0800
#define SOCK_CLOEXEC  0x0080

#define SOL_SOCKET 1

// setsockopt/getsockopt at SOL_SOCKET
#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_LINGER    13
#define SO_ACCEPTCONN 30
#define SO_PASSCRED  16
#define SO_PEERCRED  17
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

// shutdown(2)
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// send and recv flags. only these two mean anything here.
#define MSG_PEEK      0x0002
#define MSG_DONTWAIT  0x0040
#define MSG_NOSIGNAL  0x4000
#define MSG_TRUNC     0x0020
#define MSG_CTRUNC    0x0008
#define MSG_WAITALL   0x0100

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

// the address every call takes, whatever family it belongs to. a
// program fills in the one that matches its family and hands over a
// pointer to this; the kernel reads sa_family first and then knows
// which it really has.
struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

// big enough for any address this system has, and aligned for all of
// them, which is what a program uses when it does not yet know what
// it is going to be given
struct sockaddr_storage {
  sa_family_t ss_family;
  char __pad[126];
} __attribute__((aligned(8)));

// ---- ancillary data: sendmsg and recvmsg ----

struct msghdr {
  void *msg_name;            // the address, for a datagram
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  int msg_iovlen;
  void *msg_control;         // the ancillary data
  socklen_t msg_controllen;
  int msg_flags;
};

// cmsg_len is a size_t and not a socklen_t, as it is on Linux: it is
// what makes sizeof(struct cmsghdr) equal to CMSG_ALIGN of itself, so
// that CMSG_DATA and CMSG_LEN agree about where the data starts
struct cmsghdr {
  size_t cmsg_len;           // this header and its data
  int cmsg_level;
  int cmsg_type;
};

// the two kinds of ancillary data there are here: a descriptor passed
// from one process to another, and the identity of whoever sent it
#define SCM_RIGHTS      1
#define SCM_CREDENTIALS 2

struct ucred {
  int pid;
  unsigned int uid;
  unsigned int gid;
};

#define CMSG_ALIGN(n) (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_LEN(n)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_DATA(c)  ((unsigned char *)((struct cmsghdr *)(c) + 1))

#define CMSG_FIRSTHDR(m)                                                       \
  ((m)->msg_controllen >= sizeof(struct cmsghdr)                               \
       ? (struct cmsghdr *)(m)->msg_control                                    \
       : (struct cmsghdr *)0)

struct cmsghdr *__cmsg_nxthdr(struct msghdr *m, struct cmsghdr *c);
#define CMSG_NXTHDR(m, c) __cmsg_nxthdr(m, c)

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *addr, socklen_t alen);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *addr, socklen_t *alen);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int shutdown(int fd, int how);
#endif
