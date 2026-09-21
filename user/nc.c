// nc: a connection, and the two directions of it on a terminal.
//
//   nc [-u] [-w SECONDS] [-v] [-n] HOST PORT      connect
//   nc -l [-u] [-p] PORT [-k] [-v]                listen
//   nc -z [-u] [-v] HOST PORT[-PORT]              only see whether it answers
//
//   -u  UDP rather than TCP
//   -l  listen instead of connecting
//   -k  keep listening after the first connection ends
//   -p  the port to listen on, for people who type it that way
//   -w  give up after this many seconds with nothing happening
//   -z  say whether the port answers and then stop
//   -v  say what is happening on standard error
//   -n  do not look up names
//
// what arrives is written to standard output and what is typed is
// sent, which is the whole of what nc is for.
#include "user/user.h"
#include "kernel/net.h"
#include "kernel/socket.h"

static int use_udp, do_listen, keep_open, quiet_scan, verbose, no_names;
static int timeout_secs = 0;

static void
say(const char *fmt, const char *a, int n)
{
  if (!verbose)
    return;
  if (a)
    fprintf(stderr, fmt, a, n);
  else
    fprintf(stderr, "%s", fmt);
}

// move bytes both ways until one end is done. `peer` is the socket and
// the terminal is 0 and 1.
static int
pump(int peer, uint32 raddr, int rport)
{
  struct pollfd pfd[2];
  char buf[4096];
  int stdin_open = 1;

  for (;;) {
    pfd[0].fd = peer;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = 0;
    pfd[1].events = stdin_open ? POLLIN : 0;
    pfd[1].revents = 0;

    int n = poll(pfd, stdin_open ? 2 : 1, timeout_secs ? timeout_secs * 1000 : -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "nc: poll: %s\n", strerror(errno));
      return 1;
    }
    if (n == 0) {
      say("nc: nothing happened for a while; closing\n", 0, 0);
      return 0;
    }

    if (pfd[0].revents & (POLLIN | POLLHUP)) {
      int r;
      if (use_udp) {
        uint32 from = 0;
        int fport = 0;
        r = recvfrom(peer, buf, sizeof(buf), &from, &fport, 0);
        if (r > 0 && raddr == 0) {
          // a server learns who it is talking to from the first packet
          raddr = from;
          rport = fport;
        }
      } else {
        r = read(peer, buf, sizeof(buf));
      }
      if (r <= 0)
        return 0;
      if (write(1, buf, r) != r)
        return 1;
    }

    if (stdin_open && (pfd[1].revents & (POLLIN | POLLHUP))) {
      int r = read(0, buf, sizeof(buf));
      if (r <= 0) {
        // nothing more will be typed; a TCP connection is left open to
        // read the answer, and there is nothing to shut down here
        stdin_open = 0;
        if (use_udp)
          return 0;
        continue;
      }
      int w;
      if (use_udp)
        w = sendto(peer, buf, r, raddr, rport);
      else
        w = write(peer, buf, r);
      if (w != r) {
        fprintf(stderr, "nc: write: %s\n", strerror(errno));
        return 1;
      }
    }
  }
}

static int
resolve(const char *host, uint32 *ip)
{
  if (parse_ip(host, ip) == 0)
    return 0;
  if (no_names) {
    fprintf(stderr, "nc: %s is not an address, and -n was given\n", host);
    return -1;
  }
  if (dns_resolve(host, ip) < 0) {
    fprintf(stderr, "nc: cannot resolve %s\n", host);
    return -1;
  }
  return 0;
}

static int
scan(const char *host, int from_port, int to_port)
{
  uint32 ip;
  int any = 0;

  if (resolve(host, &ip) < 0)
    return 1;
  for (int p = from_port; p <= to_port; p++) {
    int fd = socket(use_udp ? SOCK_DGRAM : SOCK_STREAM);
    if (fd < 0) {
      fprintf(stderr, "nc: socket: %s\n", strerror(errno));
      return 1;
    }
    int ok = connect(fd, ip, p) >= 0;
    if (ok) {
      any = 1;
      if (verbose)
        fprintf(stderr, "Connection to %s %d port [%s] succeeded!\n", host, p,
                use_udp ? "udp" : "tcp");
      else
        printf("%d open\n", p);
    } else if (verbose) {
      fprintf(stderr, "nc: connect to %s port %d (%s) failed: %s\n", host, p,
              use_udp ? "udp" : "tcp", strerror(errno));
    }
    close(fd);
  }
  return any ? 0 : 1;
}

static int
do_server(int port)
{
  int fd = socket(use_udp ? SOCK_DGRAM : SOCK_STREAM);

  if (fd < 0) {
    fprintf(stderr, "nc: socket: %s\n", strerror(errno));
    return 1;
  }
  if (bind(fd, port) < 0) {
    fprintf(stderr, "nc: bind %d: %s\n", port, strerror(errno));
    return 1;
  }
  if (use_udp) {
    if (verbose)
      fprintf(stderr, "Listening on 0.0.0.0 %d (udp)\n", port);
    int r = pump(fd, 0, 0);
    close(fd);
    return r;
  }
  if (listen(fd, 4) < 0) {
    fprintf(stderr, "nc: listen: %s\n", strerror(errno));
    return 1;
  }
  do {
    if (verbose)
      fprintf(stderr, "Listening on 0.0.0.0 %d\n", port);
    uint32 raddr = 0;
    int rport = 0;
    int c = accept(fd, &raddr, &rport);
    if (c < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "nc: accept: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    if (verbose) {
      char a[20];
      fprintf(stderr, "Connection received on %s %d\n", ip2str(raddr, a), rport);
    }
    pump(c, raddr, rport);
    close(c);
  } while (keep_open);
  close(fd);
  return 0;
}

static int
do_client(const char *host, int port)
{
  uint32 ip;

  if (resolve(host, &ip) < 0)
    return 1;
  int fd = socket(use_udp ? SOCK_DGRAM : SOCK_STREAM);
  if (fd < 0) {
    fprintf(stderr, "nc: socket: %s\n", strerror(errno));
    return 1;
  }
  if (!use_udp) {
    if (connect(fd, ip, port) < 0) {
      char a[20];
      fprintf(stderr, "nc: connect to %s port %d failed: %s\n",
              ip2str(ip, a), port, strerror(errno));
      close(fd);
      return 1;
    }
    if (verbose) {
      char a[20];
      fprintf(stderr, "Connection to %s %d port [tcp] succeeded!\n",
              ip2str(ip, a), port);
    }
  }
  int r = pump(fd, ip, port);
  close(fd);
  return r;
}

static void
usage(void)
{
  fprintf(stderr,
          "usage: nc [-u] [-v] [-n] [-w SEC] HOST PORT\n"
          "       nc -l [-u] [-k] [-v] [-p] PORT\n"
          "       nc -z [-u] [-v] HOST PORT[-PORT]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  int port_opt = -1, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    int took_value = 0;
    for (const char *p = argv[i] + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'u': use_udp = 1; break;
      case 'l': do_listen = 1; break;
      case 'k': keep_open = 1; break;
      case 'z': quiet_scan = 1; break;
      case 'v': verbose = 1; break;
      case 'n': no_names = 1; break;
      case 'w': case 'p': case 's': case 'q': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          usage();
        if (*p == 'w' || *p == 'q')
          timeout_secs = atoi(v);
        else if (*p == 'p')
          port_opt = atoi(v);
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }
  (void)say;

  if (do_listen) {
    int port = port_opt;
    if (port < 0 && i < argc)
      port = atoi(argv[i++]);
    if (port <= 0)
      usage();
    exit(do_server(port));
  }

  if (i + 1 > argc)
    usage();
  const char *host = argv[i++];
  if (i >= argc)
    usage();
  const char *ports = argv[i++];
  int from_port = atoi(ports), to_port = from_port;
  const char *dash = strchr(ports, '-');
  if (dash)
    to_port = atoi(dash + 1);
  if (from_port <= 0)
    usage();

  if (quiet_scan)
    exit(scan(host, from_port, to_port));
  exit(do_client(host, from_port));
}
