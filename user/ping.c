// ping host [count]: ICMP echo.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: ping host [count]\n");
    exit(1);
  }
  int count = argc > 2 ? atoi(argv[2]) : 4;
  uint32 ip;
  if (dns_resolve(argv[1], &ip) < 0) {
    fprintf(stderr, "ping: cannot resolve %s\n", argv[1]);
    exit(1);
  }
  char s[20];
  // uptime() counts timer ticks, and how many there are in a second
  // is a setting of this kernel's (/sys/kernel/tick_hz)
  long hz = sysconf(_SC_CLK_TCK);
  if (hz <= 0)
    hz = 100;
  printf("PING %s (%s)\n", argv[1], ip2str(ip, s));
  // an ICMP socket: what Linux calls a ping socket, and what this
  // kernel gives a program that asks for datagrams of ICMP
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  if (fd < 0) {
    fprintf(stderr, "ping: no network\n");
    exit(1);
  }
  struct timeval tv = {2, 0};   // two seconds for an answer
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(ip);
  // a reply that is slow is not a reply that is lost. Each probe is
  // waited for, but one that has not come by then stays outstanding:
  // if it arrives while a later probe is in the air, or in the drain
  // after the last one, it counts. That is what the numbers at the end
  // mean, and it is what ping has always meant by them.
  int *sent = malloc((count + 1) * sizeof(int));
  char *got = malloc(count + 1);
  if (sent == 0 || got == 0) {
    fprintf(stderr, "ping: out of memory\n");
    exit(1);
  }
  memset(got, 0, count + 1);
  int ok = 0, outstanding = 0;
  struct sockaddr_in from;
  socklen_t fromlen;
  char reply[64];

  // count a reply, whichever probe it belongs to.
  #define TAKE(n)                                                            \
    do {                                                                     \
      int q = ntohs(from.sin_port);                                           \
      if (q >= 1 && q <= count && !got[q]) {                                  \
        got[q] = 1;                                                           \
        ok++;                                                                 \
        outstanding--;                                                        \
        printf("%d bytes from %s: seq %d time %d ms\n", (n),                  \
               ip2str(ntohl(from.sin_addr.s_addr), s), q,                     \
               (int)((uptime() - sent[q]) * 1000 / hz));                      \
      }                                                                       \
    } while (0)

  for (int seq = 1; seq <= count; seq++) {
    char data[32];
    for (int i = 0; i < sizeof(data); i++)
      data[i] = 'a' + i % 26;
    sent[seq] = uptime();
    // an echo socket carries the sequence number where the port goes:
    // there is no port in an ICMP message, and the sequence is what
    // an answer has to be matched against
    to.sin_port = htons((uint16)seq);
    int rc = sendto(fd, data, sizeof(data), 0, (struct sockaddr *)&to,
                    sizeof(to));
    if (rc < 0 && errno == EACCES) {
      printf("ping: %s is blocked by the network policy\n", s);
      exit(2);
    }
    if (rc < 0) {
      printf("ping: %s\n", strerror(errno));
      exit(1);
    }
    outstanding++;
    // wait for this one's reply, taking any other that turns up first
    while (!got[seq]) {
      fromlen = sizeof(from);
      int n = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from,
                       &fromlen);
      if (n < 0)
        break;                      // nothing within the two seconds
      TAKE(n);
    }
    if (seq < count && got[seq])
      ticksleep((int)hz);           // a second between probes
  }

  // the drain: anything still outstanding has two more seconds to
  // arrive, which is the receive timeout on the socket.
  while (outstanding > 0) {
    fromlen = sizeof(from);
    int n = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from,
                     &fromlen);
    if (n < 0)
      break;
    TAKE(n);
  }
  for (int seq = 1; seq <= count; seq++)
    if (!got[seq])
      printf("seq %d: timeout\n", seq);
  printf("%d packets transmitted, %d received\n", count, ok);
  exit(ok ? 0 : 1);
}
