// ping host [count]: ICMP echo.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "kernel/socket.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(2, "usage: ping host [count]\n");
    exit(1);
  }
  int count = argc > 2 ? atoi(argv[2]) : 4;
  uint32 ip;
  if (dns_resolve(argv[1], &ip) < 0) {
    fprintf(2, "ping: cannot resolve %s\n", argv[1]);
    exit(1);
  }
  char s[20];
  printf("PING %s (%s)\n", argv[1], ip2str(ip, s));
  int fd = socket(SOCK_ICMP);
  if (fd < 0) {
    fprintf(2, "ping: no network\n");
    exit(1);
  }
  int ok = 0;
  for (int seq = 1; seq <= count; seq++) {
    char data[32];
    for (int i = 0; i < sizeof(data); i++)
      data[i] = 'a' + i % 26;
    int t0 = uptime();
    int rc = sendto(fd, data, sizeof(data), ip, seq);
    if (rc == -2) {
      printf("ping: %s is blocked by the network policy\n", s);
      exit(2);
    }
    if (rc < 0) {
      printf("ping: send failed\n");
      exit(1);
    }
    uint32 src;
    int rseq;
    char reply[64];
    int n;
    // wait up to 2 s for the reply with this sequence number
    while ((n = recvfrom(fd, reply, sizeof(reply), &src, &rseq, 20)) >= 0 && rseq != seq)
      ;
    if (n < 0) {
      printf("seq %d: timeout\n", seq);
    } else {
      printf("%d bytes from %s: seq %d time %d ms\n", n, ip2str(src, s), seq, (uptime() - t0) * 100);
      ok++;
    }
    if (seq < count)
      pause(10);
  }
  printf("%d packets transmitted, %d received\n", count, ok);
  exit(ok ? 0 : 1);
}
