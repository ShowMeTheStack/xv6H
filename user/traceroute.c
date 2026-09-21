// traceroute: the routers a packet passes on its way somewhere.
//
//   traceroute [-n] [-q TRIES] [-m MAXHOPS] [-f FIRSTHOP] [-w SECONDS] HOST
//
//   -n  do not turn addresses back into names
//   -q  probes per hop (three, as everywhere)
//   -m  how far to go before giving up
//   -f  the hop to start from
//   -w  how long to wait for each probe
//
// an ICMP echo is sent with a time to live of one, then two, and so
// on. a router that has to drop a packet because its time ran out
// says so, and saying so is what names the router. the destination
// answers the echo itself, and that is where the walk stops.
//
// behind QEMU's NAT there is one hop to the outside and the gateway
// answers for everything past it, so the list is short and honest
// rather than wrong.
#include "user/user.h"
#include "kernel/net.h"
#include "kernel/socket.h"

static int numeric, tries = 3, maxhops = 30, firsthop = 1, wait_secs = 2;

static void
usage(void)
{
  fprintf(stderr, "usage: traceroute [-n] [-q TRIES] [-m MAXHOPS]"
                  " [-f FIRSTHOP] [-w SECONDS] HOST\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    int took_value = 0;
    for (const char *p = argv[i] + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'n': numeric = 1; break;
      case 'I': break;   // ICMP probes, which is what this always sends
      case 'q': case 'm': case 'f': case 'w': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          usage();
        int n = atoi(v);
        if (*p == 'q') tries = n > 0 ? n : 3;
        else if (*p == 'm') maxhops = n > 0 ? n : 30;
        else if (*p == 'f') firsthop = n > 0 ? n : 1;
        else wait_secs = n > 0 ? n : 2;
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }
  if (i >= argc)
    usage();

  const char *host = argv[i];
  uint32 dest;
  if (dns_resolve(host, &dest) < 0) {
    fprintf(stderr, "traceroute: cannot resolve %s\n", host);
    exit(1);
  }
  char ds[20];
  printf("traceroute to %s (%s), %d hops max\n", host, ip2str(dest, ds),
         maxhops);

  int fd = socket(SOCK_ICMP);
  if (fd < 0) {
    fprintf(stderr, "traceroute: no network\n");
    exit(1);
  }
  // without this the socket is told only about the answers meant for
  // it, and every router's complaint is what this program is after
  if (sockopt(fd, SO_ICMP_ERRORS, 1) < 0) {
    fprintf(stderr, "traceroute: this kernel will not report ICMP errors\n");
    exit(1);
  }

  int arrived = 0;
  for (int hop = firsthop; hop <= maxhops && !arrived; hop++) {
    printf("%2d ", hop);
    if (sockopt(fd, SO_TTL, hop) < 0) {
      fprintf(stderr, "\ntraceroute: cannot set the time to live\n");
      exit(1);
    }
    uint32 last = 0;
    int answered = 0;

    for (int probe = 0; probe < tries; probe++) {
      char data[32];
      for (unsigned k = 0; k < sizeof(data); k++)
        data[k] = 'a' + k % 26;
      int seq = hop * 100 + probe;
      int t0 = uptime();
      if (sendto(fd, data, sizeof(data), dest, seq) < 0) {
        if (errno == EACCES) {
          printf("!X (the network policy refused it)\n");
          exit(2);
        }
        printf(" * ");
        continue;
      }

      // wait for something about this probe, ignoring anything about
      // an older one that is only now arriving
      int got = 0;
      for (;;) {
        uint32 from = 0;
        int port = 0;
        int n = recvfrom(fd, data, sizeof(data), &from, &port,
                         wait_secs * 100);
        if (n < 0)
          break;
        if (ICMP_SEQ(port) != seq)
          continue;   // an answer to a probe that had already timed out
        int type = ICMP_TYPE(port);
        int ms = (uptime() - t0) * 10;
        if (from != last) {
          char s[20];
          printf(" %s", ip2str(from, s));
          last = from;
        }
        printf("  %d ms", ms);
        if (type == ICMP_UNREACH)
          printf(" !H");
        if (type == ICMP_ECHOREPLY || from == dest)
          arrived = 1;
        got = 1;
        answered = 1;
        break;
      }
      if (!got)
        printf(" *");
    }
    if (!answered)
      printf(" (no answer)");
    printf("\n");
  }
  close(fd);
  (void)numeric;
  exit(0);
}
