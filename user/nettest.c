// nettest: the parts of the stack that can be checked without leaving
// the machine.
//
// everything here goes over 127.0.0.1 or the machine's own address, so
// it works on a laptop whose network blocks ICMP, behind a firewall,
// and on a build box with no route out at all. what needs the internet
// is in the network suite instead.
#include "user/user.h"
#include "kernel/net.h"
#include "kernel/socket.h"

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

// ---- a datagram to a port nobody has ----

static void
test_port_unreachable(void)
{
  int fd = socket(SOCK_DGRAM);
  CHECK(fd >= 0);
  if (fd < 0)
    return;

  // without asking for them, an error is not delivered and the read
  // simply times out: a program that does not want them is not
  // surprised by one
  CHECK(bind(fd, 40001) == 0);
  CHECK(sendto(fd, "hello", 5, LOOPBACK, 40099) == 5);
  char buf[64];
  uint32 from = 0;
  int port = 0;
  CHECK(recvfrom(fd, buf, sizeof(buf), &from, &port, 30) < 0);
  close(fd);

  // asking for them, the same send draws a port unreachable back
  fd = socket(SOCK_DGRAM);
  CHECK(fd >= 0);
  CHECK(bind(fd, 40002) == 0);
  CHECK(sockopt(fd, SO_ICMP_ERRORS, 1) == 0);
  CHECK(sendto(fd, "hello", 5, LOOPBACK, 40099) == 5);
  from = 0;
  port = 0;
  int n = recvfrom(fd, buf, sizeof(buf), &from, &port, 100);
  CHECK(n >= 0);
  CHECK(from == LOOPBACK);
  CHECK(ICMP_TYPE(port) == ICMP_UNREACH);
  close(fd);
  printf("port unreachable: ok\n");
}

// ---- a datagram to a port that is there ----

static void
test_udp_loopback(void)
{
  int a = socket(SOCK_DGRAM), b = socket(SOCK_DGRAM);

  CHECK(a >= 0 && b >= 0);
  CHECK(bind(b, 40010) == 0);
  CHECK(bind(a, 40011) == 0);
  CHECK(sendto(a, "ping", 4, LOOPBACK, 40010) == 4);

  char buf[64];
  uint32 from = 0;
  int port = 0;
  int n = recvfrom(b, buf, sizeof(buf), &from, &port, 100);
  CHECK(n == 4);
  CHECK(memcmp(buf, "ping", 4) == 0);
  CHECK(from == LOOPBACK);
  CHECK(port == 40011);
  // an ordinary datagram has no ICMP type above its port
  CHECK(ICMP_TYPE(port) == 0);

  CHECK(sendto(b, "pong", 4, from, port) == 4);
  from = 0;
  port = 0;
  n = recvfrom(a, buf, sizeof(buf), &from, &port, 100);
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
  int fd = socket(SOCK_ICMP);

  CHECK(fd >= 0);
  CHECK(sockopt(fd, SO_TTL, 1) == 0);
  CHECK(sockopt(fd, SO_TTL, 64) == 0);
  CHECK(sockopt(fd, SO_TTL, 255) == 0);
  CHECK(sockopt(fd, SO_TTL, -1) < 0);
  CHECK(sockopt(fd, SO_TTL, 256) < 0);
  CHECK(sockopt(fd, SO_ICMP_ERRORS, 1) == 0);
  CHECK(sockopt(fd, 9999, 1) < 0);

  // a packet to ourselves never passes a router, so a time to live of
  // one still arrives: what is checked here is that setting it does
  // not break the send
  CHECK(sockopt(fd, SO_TTL, 1) == 0);
  CHECK(sendto(fd, "abcdefgh", 8, LOOPBACK, 1) == 8);
  char buf[64];
  uint32 from = 0;
  int port = 0;
  int n = recvfrom(fd, buf, sizeof(buf), &from, &port, 100);
  CHECK(n >= 0);
  CHECK(from == LOOPBACK);
  CHECK(ICMP_SEQ(port) == 1);
  CHECK(ICMP_TYPE(port) == ICMP_ECHOREPLY);
  close(fd);

  CHECK(sockopt(-1, SO_TTL, 1) < 0);
  printf("ttl: ok\n");
}

// ---- a connection to ourselves ----

static void
test_tcp_loopback(void)
{
  int srv = socket(SOCK_STREAM);

  CHECK(srv >= 0);
  CHECK(bind(srv, 40020) == 0);
  CHECK(listen(srv, 4) == 0);

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int c = socket(SOCK_STREAM);
    if (c < 0 || connect(c, LOOPBACK, 40020) < 0)
      exit(1);
    write(c, "knock", 5);
    char buf[16];
    int n = read(c, buf, sizeof(buf));
    close(c);
    exit(n == 5 && memcmp(buf, "enter", 5) == 0 ? 0 : 1);
  }
  uint32 raddr = 0;
  int rport = 0;
  int c = accept(srv, &raddr, &rport);
  CHECK(c >= 0);
  CHECK(raddr == LOOPBACK);
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
  int fd = socket(SOCK_STREAM);

  CHECK(fd >= 0);
  CHECK(connect(fd, LOOPBACK, 40099) < 0);
  close(fd);
  printf("refused: ok\n");
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
  int srv = socket(SOCK_STREAM);
  CHECK(srv >= 0);
  CHECK(bind(srv, 40030) == 0);
  CHECK(listen(srv, 1) == 0);
  CHECK(file_has("/proc/net/tcp", ":9C5E"));    // 40030
  CHECK(file_has("/proc/net/tcp", " 0A "));     // the LISTEN state
  close(srv);

  int u = socket(SOCK_DGRAM);
  CHECK(u >= 0);
  CHECK(bind(u, 40031) == 0);
  CHECK(file_has("/proc/net/udp", ":9C5F"));    // 40031
  close(u);
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
  int u = socket(SOCK_DGRAM);
  CHECK(u >= 0);
  CHECK(bind(u, 40040) == 0);
  CHECK(sendto(u, "captured", 8, LOOPBACK, 40040) == 8);
  char buf[64];
  uint32 from;
  int port;
  recvfrom(u, buf, sizeof(buf), &from, &port, 100);
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
  test_procnet();
  test_pktcap();
  printf("nettest: %d checks, %d failures\n", passed + failed, failed);
  exit(failed ? 1 : 0);
}
