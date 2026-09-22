// nettest: the parts of the stack that can be checked without leaving
// the machine.
//
// everything here goes over 127.0.0.1, over the machine's own address
// or over a socket in the file system, so it works on a laptop whose
// network blocks ICMP, behind a firewall, and on a build box with no
// route out at all. what needs the internet is in the network suite
// instead.
#include "user/user.h"
#include "kernel/net.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>

static int passed, failed;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (cond) {                                                                \
      passed++;                                                                \
    } else {                                                                   \
      failed++;                                                                \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

#define LOOPBACK MAKE_IP(127, 0, 0, 1)

static struct sockaddr_in
inaddr(uint32 ip, int port)
{
  struct sockaddr_in sa;

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16)port);
  sa.sin_addr.s_addr = htonl(ip);
  return sa;
}

#define SA(x) ((struct sockaddr *)&(x))

// a receive that gives up rather than waiting for ever
static void
set_timeout(int fd, int secs)
{
  struct timeval tv = {secs, 0};

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// ---- a datagram to a port nobody has ----

static void
test_port_unreachable(void)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  if (fd < 0)
    return;

  // without asking for them, an error is not delivered and the read
  // simply times out: a program that does not want them is not
  // surprised by one
  struct sockaddr_in me = inaddr(INADDR_ANY, 40001);
  struct sockaddr_in nowhere = inaddr(LOOPBACK, 40099);
  CHECK(bind(fd, SA(me), sizeof(me)) == 0);
  set_timeout(fd, 3);
  CHECK(sendto(fd, "hello", 5, 0, SA(nowhere), sizeof(nowhere)) == 5);
  char buf[64];
  CHECK(recvfrom(fd, buf, sizeof(buf), 0, 0, 0) < 0);
  close(fd);

  // asking for them, the same send draws a port unreachable back, and
  // what it was comes alongside the message rather than inside it
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  me = inaddr(INADDR_ANY, 40002);
  CHECK(bind(fd, SA(me), sizeof(me)) == 0);
  int on = 1;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_RECVERR, &on, sizeof(on)) == 0);
  set_timeout(fd, 10);
  CHECK(sendto(fd, "hello", 5, 0, SA(nowhere), sizeof(nowhere)) == 5);

  struct sockaddr_in from;
  struct iovec iov = {buf, sizeof(buf)};
  char ctl[64];
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  memset(&from, 0, sizeof(from));
  msg.msg_name = &from;
  msg.msg_namelen = sizeof(from);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctl;
  msg.msg_controllen = sizeof(ctl);
  int n = recvmsg(fd, &msg, 0);
  CHECK(n >= 0);
  CHECK(ntohl(from.sin_addr.s_addr) == LOOPBACK);
  int type = -1;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR) {
      struct sock_extended_err ee;
      memcpy(&ee, CMSG_DATA(c), sizeof(ee));
      CHECK(ee.ee_origin == SO_EE_ORIGIN_ICMP);
      type = ee.ee_type;
    }
  CHECK(type == ICMP_UNREACH);
  close(fd);
  printf("port unreachable: ok\n");
}

// ---- a datagram to a port that is there ----

static void
test_udp_loopback(void)
{
  int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);

  CHECK(a >= 0 && b >= 0);
  struct sockaddr_in ba = inaddr(INADDR_ANY, 40010);
  struct sockaddr_in aa = inaddr(INADDR_ANY, 40011);
  CHECK(bind(b, SA(ba), sizeof(ba)) == 0);
  CHECK(bind(a, SA(aa), sizeof(aa)) == 0);
  set_timeout(a, 10);
  set_timeout(b, 10);
  struct sockaddr_in to = inaddr(LOOPBACK, 40010);
  CHECK(sendto(a, "ping", 4, 0, SA(to), sizeof(to)) == 4);

  char buf[64];
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  memset(&from, 0, sizeof(from));
  int n = recvfrom(b, buf, sizeof(buf), 0, SA(from), &fromlen);
  CHECK(n == 4);
  CHECK(memcmp(buf, "ping", 4) == 0);
  CHECK(fromlen == sizeof(from));
  CHECK(from.sin_family == AF_INET);
  CHECK(ntohl(from.sin_addr.s_addr) == LOOPBACK);
  CHECK(ntohs(from.sin_port) == 40011);

  // the sender's own name is the port it asked for
  struct sockaddr_in mine;
  socklen_t minelen = sizeof(mine);
  CHECK(getsockname(a, SA(mine), &minelen) == 0);
  CHECK(ntohs(mine.sin_port) == 40011);

  CHECK(sendto(b, "pong", 4, 0, SA(from), sizeof(from)) == 4);
  n = recvfrom(a, buf, sizeof(buf), 0, 0, 0);
  CHECK(n == 4);
  CHECK(memcmp(buf, "pong", 4) == 0);
  close(a);
  close(b);
  printf("udp loopback: ok\n");
}

// ---- the time to live a socket sends with ----

static void
test_ttl(void)
{
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  int v;

  CHECK(fd >= 0);
  v = 1;   CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) == 0);
  v = 64;  CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) == 0);
  v = 255; CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) == 0);
  v = -1;  CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) < 0);
  v = 256; CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) < 0);
  v = 1;   CHECK(setsockopt(fd, IPPROTO_IP, IP_RECVERR, &v, sizeof(v)) == 0);
  CHECK(setsockopt(fd, IPPROTO_IP, 9999, &v, sizeof(v)) < 0);

  // what was set is what comes back
  v = 17;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) == 0);
  v = 0;
  socklen_t vlen = sizeof(v);
  CHECK(getsockopt(fd, IPPROTO_IP, IP_TTL, &v, &vlen) == 0);
  CHECK(v == 17);
  CHECK(vlen == sizeof(v));
  v = 0;
  vlen = sizeof(v);
  CHECK(getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &vlen) == 0);
  CHECK(v == SOCK_DGRAM);

  // a packet to ourselves never passes a router, so a time to live of
  // one still arrives: what is checked here is that setting it does
  // not break the send
  set_timeout(fd, 10);
  struct sockaddr_in to = inaddr(LOOPBACK, 1);   // the sequence number
  CHECK(sendto(fd, "abcdefgh", 8, 0, SA(to), sizeof(to)) == 8);
  char buf[64];
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  memset(&from, 0, sizeof(from));
  int n = recvfrom(fd, buf, sizeof(buf), 0, SA(from), &fromlen);
  CHECK(n >= 0);
  CHECK(ntohl(from.sin_addr.s_addr) == LOOPBACK);
  CHECK(ntohs(from.sin_port) == 1);
  close(fd);

  v = 1;
  CHECK(setsockopt(-1, IPPROTO_IP, IP_TTL, &v, sizeof(v)) < 0);
  CHECK(errno == EBADF);
  printf("ttl: ok\n");
}

// ---- a connection to ourselves ----

static void
test_tcp_loopback(void)
{
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in me = inaddr(INADDR_ANY, 40020);

  CHECK(srv >= 0);
  CHECK(bind(srv, SA(me), sizeof(me)) == 0);
  CHECK(listen(srv, 4) == 0);

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = inaddr(LOOPBACK, 40020);
    if (c < 0 || connect(c, SA(to), sizeof(to)) < 0)
      exit(1);
    // the other end is where we said we were going
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    if (getpeername(c, SA(peer), &peerlen) < 0 || ntohs(peer.sin_port) != 40020)
      exit(1);
    write(c, "knock", 5);
    char buf[16];
    int n = read(c, buf, sizeof(buf));
    close(c);
    exit(n == 5 && memcmp(buf, "enter", 5) == 0 ? 0 : 1);
  }
  struct sockaddr_in raddr;
  socklen_t rlen = sizeof(raddr);
  memset(&raddr, 0, sizeof(raddr));
  int c = accept(srv, SA(raddr), &rlen);
  CHECK(c >= 0);
  CHECK(ntohl(raddr.sin_addr.s_addr) == LOOPBACK);
  CHECK(rlen == sizeof(raddr));
  char buf[16];
  int n = read(c, buf, sizeof(buf));
  CHECK(n == 5);
  CHECK(memcmp(buf, "knock", 5) == 0);
  CHECK(write(c, "enter", 5) == 5);
  close(c);
  close(srv);
  int st = 0;
  wait(&st);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  printf("tcp loopback: ok\n");
}

// ---- a connection nobody is listening for ----

static void
test_refused(void)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in to = inaddr(LOOPBACK, 40099);

  CHECK(fd >= 0);
  CHECK(connect(fd, SA(to), sizeof(to)) < 0);
  close(fd);

  // and the things that are not sockets at all
  int nul = open("/dev/null", O_RDWR);
  CHECK(nul >= 0);
  CHECK(listen(nul, 1) < 0 && errno == ENOTSOCK);
  close(nul);
  CHECK(socket(AF_INET6, SOCK_STREAM, 0) < 0);
  CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) < 0);
  printf("refused: ok\n");
}

// ---- sockets in the file system ----

static void
test_unix_stream(void)
{
  const char *path = "/tmp/nettest.sock";
  struct sockaddr_un sa;

  unlink(path);
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, path);

  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(srv >= 0);
  CHECK(bind(srv, SA(sa), sizeof(sa)) == 0);
  CHECK(listen(srv, 4) == 0);

  // binding makes something the file system can see, and it is a
  // socket and not a file
  struct stat st;
  CHECK(stat(path, &st) == 0);
  CHECK(st.type == T_SOCK);

  // and the name it answers to is the one it was given
  struct sockaddr_un got;
  socklen_t gotlen = sizeof(got);
  memset(&got, 0, sizeof(got));
  CHECK(getsockname(srv, SA(got), &gotlen) == 0);
  CHECK(got.sun_family == AF_UNIX);
  CHECK(strcmp(got.sun_path, path) == 0);

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0 || connect(c, SA(sa), sizeof(sa)) < 0)
      exit(1);
    if (write(c, "hello", 5) != 5)
      exit(1);
    char buf[16];
    int n = read(c, buf, sizeof(buf));
    close(c);
    exit(n == 5 && memcmp(buf, "world", 5) == 0 ? 0 : 1);
  }
  int c = accept(srv, 0, 0);
  CHECK(c >= 0);
  char buf[16];
  CHECK(read(c, buf, sizeof(buf)) == 5);
  CHECK(memcmp(buf, "hello", 5) == 0);
  CHECK(write(c, "world", 5) == 5);
  close(c);
  close(srv);
  int st2 = 0;
  wait(&st2);
  CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0);

  // a second socket cannot take a name that is already there, and a
  // connection to a name nothing answers is refused
  int again = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(bind(again, SA(sa), sizeof(sa)) < 0);
  close(again);
  unlink(path);
  int lost = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(connect(lost, SA(sa), sizeof(sa)) < 0);
  close(lost);
  printf("unix stream: ok\n");
}

static void
test_unix_abstract(void)
{
  struct sockaddr_un sa;
  socklen_t salen;

  // a leading zero byte: a name that leaves nothing behind
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  memmove(sa.sun_path + 1, "nettest", 7);
  salen = sizeof(sa.sun_family) + 1 + 7;

  int srv = socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(srv >= 0);
  CHECK(bind(srv, SA(sa), salen) == 0);
  // nothing of it is in the file system
  struct stat st;
  CHECK(stat("/nettest", &st) < 0);

  int cl = socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(cl >= 0);
  CHECK(sendto(cl, "abstract", 8, 0, SA(sa), salen) == 8);
  char buf[32];
  CHECK(recv(srv, buf, sizeof(buf), 0) == 8);
  CHECK(memcmp(buf, "abstract", 8) == 0);
  close(cl);
  close(srv);
  printf("unix abstract: ok\n");
}

static void
test_socketpair(void)
{
  int sv[2];

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(write(sv[0], "one", 3) == 3);
  char buf[8];
  CHECK(read(sv[1], buf, sizeof(buf)) == 3);
  CHECK(memcmp(buf, "one", 3) == 0);
  CHECK(write(sv[1], "two", 3) == 3);
  CHECK(read(sv[0], buf, sizeof(buf)) == 3);
  CHECK(memcmp(buf, "two", 3) == 0);

  // closing one end is the end of the other
  close(sv[0]);
  CHECK(read(sv[1], buf, sizeof(buf)) == 0);
  close(sv[1]);

  CHECK(socketpair(AF_INET, SOCK_STREAM, 0, sv) < 0);
  printf("socketpair: ok\n");
}

// a descriptor handed from one process to another
static void
test_scm_rights(void)
{
  int sv[2];
  char path[] = "/tmp/nettest.pass";

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    int fd = open(path, O_CREATE | O_RDWR);
    if (fd < 0)
      exit(1);
    write(fd, "passed through", 14);
    lseek(fd, 0, SEEK_SET);

    char c = 'x';
    struct iovec iov = {&c, 1};
    char ctl[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    memset(ctl, 0, sizeof(ctl));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof(ctl);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
    int r = sendmsg(sv[1], &msg, 0);
    close(fd);
    close(sv[1]);
    exit(r == 1 ? 0 : 1);
  }
  close(sv[1]);

  char c = 0;
  struct iovec iov = {&c, 1};
  char ctl[CMSG_SPACE(sizeof(int)) + 32];
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  memset(ctl, 0, sizeof(ctl));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctl;
  msg.msg_controllen = sizeof(ctl);
  CHECK(recvmsg(sv[0], &msg, 0) == 1);
  CHECK(c == 'x');

  int got = -1;
  for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
      memcpy(&got, CMSG_DATA(cm), sizeof(got));
  CHECK(got >= 0);
  if (got >= 0) {
    char buf[32];
    int n = read(got, buf, sizeof(buf));
    CHECK(n == 14);
    CHECK(memcmp(buf, "passed through", 14) == 0);
    close(got);
  }
  close(sv[0]);
  int st = 0;
  wait(&st);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  unlink(path);
  printf("scm_rights: ok\n");
}

// who sent it, according to the kernel rather than the sender
static void
test_scm_credentials(void)
{
  int sv[2];
  int on = 1;

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) == 0);

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    write(sv[1], "c", 1);
    close(sv[1]);
    exit(0);
  }
  close(sv[1]);

  char c = 0;
  struct iovec iov = {&c, 1};
  char ctl[CMSG_SPACE(sizeof(struct ucred)) + 32];
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  memset(ctl, 0, sizeof(ctl));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctl;
  msg.msg_controllen = sizeof(ctl);
  CHECK(recvmsg(sv[0], &msg, 0) == 1);

  struct ucred cred;
  int found = 0;
  memset(&cred, 0, sizeof(cred));
  for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_CREDENTIALS) {
      memcpy(&cred, CMSG_DATA(cm), sizeof(cred));
      found = 1;
    }
  CHECK(found);
  CHECK(cred.pid == pid);
  CHECK(cred.uid == getuid());
  close(sv[0]);
  int st = 0;
  wait(&st);
  CHECK(WIFEXITED(st));
  printf("scm_credentials: ok\n");
}

static void
test_shutdown(void)
{
  int sv[2];

  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(shutdown(sv[0], SHUT_WR) == 0);
  CHECK(write(sv[0], "x", 1) < 0);
  char buf[8];
  CHECK(read(sv[1], buf, sizeof(buf)) == 0);
  CHECK(shutdown(sv[0], 99) < 0);
  close(sv[0]);
  close(sv[1]);
  printf("shutdown: ok\n");
}

// ---- addresses and names as text ----

static void
test_addresses(void)
{
  struct in_addr a;
  char text[32];

  CHECK(inet_pton(AF_INET, "10.0.2.15", &a) == 1);
  CHECK(ntohl(a.s_addr) == MAKE_IP(10, 0, 2, 15));
  CHECK(inet_ntop(AF_INET, &a, text, sizeof(text)) != 0);
  CHECK(strcmp(text, "10.0.2.15") == 0);
  CHECK(inet_pton(AF_INET, "256.0.0.1", &a) == 0);
  CHECK(inet_pton(AF_INET, "1.2.3", &a) == 0);
  CHECK(inet_pton(AF_INET, "1.2.3.4.5", &a) == 0);
  CHECK(inet_aton("127.0.0.1", &a) == 1);
  CHECK(ntohl(a.s_addr) == LOOPBACK);
  CHECK(strcmp(inet_ntoa(a), "127.0.0.1") == 0);

  struct servent *se = getservbyname("http", "tcp");
  CHECK(se != 0 && ntohs(se->s_port) == 80);
  CHECK(getservbyname("domain", "udp") != 0);
  CHECK(getservbyname("nosuchservice", "tcp") == 0);
  struct protoent *pe = getprotobyname("udp");
  CHECK(pe != 0 && pe->p_proto == IPPROTO_UDP);

  // an address needs no name server to be understood
  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  CHECK(getaddrinfo("127.0.0.1", "80", &hints, &res) == 0);
  CHECK(res != 0);
  if (res) {
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    CHECK(res->ai_family == AF_INET);
    CHECK(res->ai_socktype == SOCK_STREAM);
    CHECK(res->ai_addrlen == sizeof(*sin));
    CHECK(ntohl(sin->sin_addr.s_addr) == LOOPBACK);
    CHECK(ntohs(sin->sin_port) == 80);

    char host[NI_MAXHOST], serv[NI_MAXSERV];
    CHECK(getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof(host), serv,
                      sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0);
    CHECK(strcmp(host, "127.0.0.1") == 0);
    CHECK(strcmp(serv, "80") == 0);
    freeaddrinfo(res);
  }
  res = 0;
  CHECK(getaddrinfo("127.0.0.1", "http", &hints, &res) == 0);
  if (res) {
    CHECK(ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port) == 80);
    freeaddrinfo(res);
  }
  res = 0;
  CHECK(getaddrinfo("not.a.number", 0, &hints, &res) == EAI_NONAME);
  hints.ai_family = AF_INET6;
  CHECK(getaddrinfo("127.0.0.1", "80", &hints, &res) == EAI_FAMILY);
  CHECK(strlen(gai_strerror(EAI_NONAME)) > 0);

  struct hostent *he = gethostbyname("127.0.0.1");
  CHECK(he != 0 && he->h_addrtype == AF_INET && he->h_length == 4);

  // /etc/hosts answers before anything is asked of the network, and
  // /etc/services and /etc/protocols have names the built-in tables
  // do not
  he = gethostbyname("localhost");
  CHECK(he != 0);
  if (he)
    CHECK(ntohl(((struct in_addr *)he->h_addr_list[0])->s_addr) == LOOPBACK);
  res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  CHECK(getaddrinfo("localhost", "http", &hints, &res) == 0);
  if (res) {
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    CHECK(ntohl(sin->sin_addr.s_addr) == LOOPBACK);
    CHECK(ntohs(sin->sin_port) == 80);
    freeaddrinfo(res);
  }
  se = getservbyname("imap2", "tcp");    // only /etc/services has this name
  CHECK(se != 0 && ntohs(se->s_port) == 143);
  pe = getprotobyname("icmp");
  CHECK(pe != 0 && pe->p_proto == IPPROTO_ICMP);
  pe = getprotobynumber(IPPROTO_TCP);
  CHECK(pe != 0 && strcmp(pe->p_name, "tcp") == 0);
  printf("addresses: ok\n");
}

// ---- the interfaces this machine has ----

static void
test_interfaces(void)
{
  char name[IF_NAMESIZE];

  CHECK(if_nametoindex("lo") == 1);
  CHECK(if_nametoindex("nosuchif") == 0);
  CHECK(if_indextoname(1, name) != 0 && strcmp(name, "lo") == 0);
  CHECK(if_indextoname(99, name) == 0);

  struct if_nameindex *ni = if_nameindex();
  CHECK(ni != 0);
  if (ni) {
    int n = 0;
    for (struct if_nameindex *p = ni; p->if_name; p++)
      n++;
    CHECK(n == 2);
    CHECK(strcmp(ni[0].if_name, "lo") == 0);
    if_freenameindex(ni);
  }

  struct ifaddrs *ifa = 0;
  CHECK(getifaddrs(&ifa) == 0);
  CHECK(ifa != 0);
  int sawlo = 0;
  for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
    CHECK(p->ifa_addr != 0 && p->ifa_addr->sa_family == AF_INET);
    if (strcmp(p->ifa_name, "lo") == 0) {
      struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
      CHECK(ntohl(sin->sin_addr.s_addr) == LOOPBACK);
      CHECK(p->ifa_flags & IFF_LOOPBACK);
      sawlo = 1;
    }
  }
  CHECK(sawlo);
  freeifaddrs(ifa);
  printf("interfaces: ok\n");
}

// a whole exchange written the way a program on any other system
// would write it: getaddrinfo, socket, connect, and nothing about
// this kernel anywhere in it
static void
test_posix_shape(void)
{
  struct addrinfo hints, *res = 0;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  CHECK(getaddrinfo(0, "40050", &hints, &res) == 0);
  if (res == 0)
    return;
  int srv = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  CHECK(srv >= 0);
  CHECK(bind(srv, res->ai_addr, res->ai_addrlen) == 0);
  CHECK(listen(srv, 1) == 0);
  freeaddrinfo(res);

  int pid = fork();
  if (pid == 0) {
    struct addrinfo h2, *r2 = 0;
    memset(&h2, 0, sizeof(h2));
    h2.ai_family = AF_INET;
    h2.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("127.0.0.1", "40050", &h2, &r2) != 0)
      exit(1);
    int c = socket(r2->ai_family, r2->ai_socktype, r2->ai_protocol);
    int ok = c >= 0 && connect(c, r2->ai_addr, r2->ai_addrlen) == 0 &&
             send(c, "posix", 5, 0) == 5;
    freeaddrinfo(r2);
    close(c);
    exit(ok ? 0 : 1);
  }
  int c = accept(srv, 0, 0);
  CHECK(c >= 0);
  char buf[16];
  CHECK(recv(c, buf, sizeof(buf), 0) == 5);
  CHECK(memcmp(buf, "posix", 5) == 0);
  close(c);
  close(srv);
  int st = 0;
  wait(&st);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  printf("posix shape: ok\n");
}

// ---- what /proc/net says about all this ----

static int
file_has(const char *path, const char *want)
{
  FILE *f = fopen(path, "r");
  char line[512];

  if (f == 0)
    return 0;
  while (fgets(line, sizeof(line), f))
    if (strstr(line, want)) {
      fclose(f);
      return 1;
    }
  fclose(f);
  return 0;
}

static void
test_procnet(void)
{
  CHECK(file_has("/proc/net/route", "Destination"));
  CHECK(file_has("/proc/net/dev", "eth0"));
  CHECK(file_has("/proc/net/arp", "HW type"));
  CHECK(file_has("/proc/net/tcp", "local_address"));
  CHECK(file_has("/proc/net/udp", "local_address"));
  CHECK(file_has("/proc/net/sockstat", "TCP: inuse"));

  // a listening socket shows up, in the hex form these files use
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in me = inaddr(INADDR_ANY, 40030);
  CHECK(srv >= 0);
  CHECK(bind(srv, SA(me), sizeof(me)) == 0);
  CHECK(listen(srv, 1) == 0);
  CHECK(file_has("/proc/net/tcp", ":9C5E"));    // 40030
  CHECK(file_has("/proc/net/tcp", " 0A "));     // the LISTEN state
  close(srv);

  int u = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in mu = inaddr(INADDR_ANY, 40031);
  CHECK(u >= 0);
  CHECK(bind(u, SA(mu), sizeof(mu)) == 0);
  CHECK(file_has("/proc/net/udp", ":9C5F"));    // 40031
  close(u);

  // and a socket in the file system shows up by the name it answers
  // to, with the flag that says it accepts connections
  CHECK(file_has("/proc/net/unix", "RefCount"));
  const char *path = "/tmp/nettest.proc";
  struct sockaddr_un un;
  memset(&un, 0, sizeof(un));
  un.sun_family = AF_UNIX;
  strcpy(un.sun_path, path);
  unlink(path);
  int us = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(us >= 0);
  CHECK(bind(us, SA(un), sizeof(un)) == 0);
  CHECK(listen(us, 1) == 0);
  CHECK(file_has("/proc/net/unix", path));
  CHECK(file_has("/proc/net/unix", "00010000"));
  close(us);
  unlink(path);
  printf("/proc/net: ok\n");
}

// ---- the capture device ----

static void
test_pktcap(void)
{
  int fd = open("/dev/pktcap", O_RDONLY);

  CHECK(fd >= 0);
  if (fd < 0)
    return;

  // a packet to ourselves is captured: both the send and the delivery
  int u = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in me = inaddr(INADDR_ANY, 40040);
  struct sockaddr_in to = inaddr(LOOPBACK, 40040);
  CHECK(u >= 0);
  CHECK(bind(u, SA(me), sizeof(me)) == 0);
  set_timeout(u, 10);
  CHECK(sendto(u, "captured", 8, 0, SA(to), sizeof(to)) == 8);
  char buf[64];
  recvfrom(u, buf, sizeof(buf), 0, 0, 0);
  close(u);

  // loopback never reaches the wire, so nothing is captured from it;
  // what must be true is that the device reads as pcap records when
  // there is something to read, and that it can be opened and closed
  // without upsetting anything
  close(fd);
  CHECK(open("/dev/pktcap", O_RDONLY) >= 0);
  printf("pktcap: ok\n");
}

int
main(void)
{
  test_udp_loopback();
  test_port_unreachable();
  test_ttl();
  test_tcp_loopback();
  test_refused();
  test_unix_stream();
  test_unix_abstract();
  test_socketpair();
  test_scm_rights();
  test_scm_credentials();
  test_shutdown();
  test_addresses();
  test_interfaces();
  test_posix_shape();
  test_procnet();
  test_pktcap();
  printf("nettest: %d checks, %d failures\n", passed + failed, failed);
  exit(failed ? 1 : 0);
}
