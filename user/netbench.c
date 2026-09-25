//
// netbench: what this network stack costs, on the host and inside a guest.
//
//   netbench [-t secs] [-p port] [-s SIZE] [-v] [-l | -c HOST] [test ...]
//
// tests:
//   stream    bytes a second through one TCP connection, one way
//   rr        one byte there and back: round trips a second, and the latency
//   connect   connect, exchange a byte, close: connections a second
//   udp       datagrams a second, one way, and how many arrived
//
// With neither -l nor -c it does the whole thing to itself over the
// loopback address, forking a server: that measures the stack with no
// device under it. `-l' listens and `-c HOST' connects, which is how the
// interesting number is taken -- the same test run between a host and its
// guest goes through the virtual switch and the guest's virtio-net, and
// the difference between the two is what a guest pays for its network.
//
// Like schedbench, this builds and runs on Linux too, because a number
// with nothing to compare it against says very little.
//
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT_DEFAULT 5555

static double secs = 3.0;
static int port = PORT_DEFAULT;
static int bufsize = 4096;
static int verbose = 0;
static const char *peer_host = 0;   // -c
static int listen_only = 0;         // -l

static double
now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// A failed system call, or a peer that went away. errno means nothing in
// the second case, and printing it there once said "Operation not
// permitted" about a connection that had simply closed.
static void
die(const char *what)
{
  fprintf(stderr, "netbench: %s: %s\n", what, strerror(errno));
  exit(1);
}

static void
died(const char *what, int rc)
{
  if (rc < 0)
    die(what);
  fprintf(stderr, "netbench: %s: the other end closed\n", what);
  exit(1);
}

// read exactly n bytes, or return 0 at end of file
static int
readn(int fd, char *buf, int n)
{
  int got = 0;

  while (got < n) {
    int k = read(fd, buf + got, n - got);
    if (k <= 0)
      return got > 0 ? got : k;
    got += k;
  }
  return got;
}

static int
tcp_listen(void)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    die("socket");
  struct sockaddr_in me;
  memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_port = htons((unsigned short)port);
  me.sin_addr.s_addr = htonl(0);   // any
  if (bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0)
    die("bind");
  if (listen(fd, 4) < 0)
    die("listen");
  return fd;
}

static int
tcp_connect(const char *host)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    die("socket");
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &to.sin_addr) != 1) {
    fprintf(stderr, "netbench: %s is not an address\n", host);
    exit(1);
  }
  if (connect(fd, (struct sockaddr *)&to, sizeof(to)) < 0)
    die("connect");
  return fd;
}

// ---- the server side of each test ----
//
// One process, one connection at a time, and it does whatever the client
// asks of it: for `stream' it reads until end of file, for `rr' it echoes
// every byte back. Which of the two it is doing is decided by the first
// byte the client sends, so that the two ends need agree on nothing else.
static void
serve(int quit_after)
{
  int lfd = tcp_listen();
  char *buf = malloc(bufsize);

  if (buf == 0)
    die("malloc");
  for (int conns = 0; quit_after == 0 || conns < quit_after; conns++) {
    struct sockaddr_in peer;
    unsigned int plen = sizeof(peer);
    int c = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (c < 0)
      die("accept");
    char kind;
    if (read(c, &kind, 1) != 1) {
      close(c);
      continue;
    }
    if (kind == 'S') {
      // drain until the client is done
      while (read(c, buf, bufsize) > 0)
        ;
    } else if (kind == 'R') {
      // echo one byte at a time, for as long as it keeps coming
      char b;
      while (read(c, &b, 1) == 1)
        if (write(c, &b, 1) != 1)
          break;
    } else if (kind == 'C') {
      char b = 'c';
      write(c, &b, 1);
    }
    close(c);
  }
  close(lfd);
  free(buf);
}

// ---- the client side ----

static void
t_stream(const char *host)
{
  int fd = tcp_connect(host);
  char *buf = malloc(bufsize);

  if (buf == 0)
    die("malloc");
  memset(buf, 'x', bufsize);
  buf[0] = 'S';
  double t0 = now(), end = t0 + secs;
  long long bytes = 0;
  while (now() < end) {
    // fifty buffers between clock readings: the clock is a system call
    // and this loop is meant to be measuring the network.
    for (int i = 0; i < 50; i++) {
      int k = write(fd, buf, bufsize);
      if (k <= 0)
        goto done;
      bytes += k;
      buf[0] = 'x';
    }
  }
done:
  {
    double dt = now() - t0;
    close(fd);
    printf("netbench: stream %.2f MB/s (%lld bytes in %.2fs, %d-byte writes)\n",
           bytes / dt / 1048576.0, bytes, dt, bufsize);
  }
  free(buf);
}

static void
t_rr(const char *host)
{
  int fd = tcp_connect(host);
  char b = 'R';

  // the first byte says which test this is, and the server consumes it as
  // that rather than echoing it: there is nothing to wait for here, and
  // waiting for one is a client that never starts.
  {
    int rc = write(fd, &b, 1);
    if (rc != 1)
      died("write", rc);
  }
  double t0 = now(), end = t0 + secs;
  long long trips = 0;
  while (now() < end) {
    for (int i = 0; i < 20; i++) {
      b = 'a';
      if (write(fd, &b, 1) != 1)
        goto done;
      if (readn(fd, &b, 1) != 1)
        goto done;
      trips++;
    }
  }
done:
  {
    double dt = now() - t0;
    close(fd);
    printf("netbench: rr %.0f round trips/s (%.1f us each)\n",
           trips / dt, trips ? dt * 1e6 / trips : 0.0);
  }
}

static void
t_connect(const char *host)
{
  double t0 = now(), end = t0 + secs;
  long long n = 0;

  while (now() < end) {
    int fd = tcp_connect(host);
    char b = 'C';
    if (write(fd, &b, 1) == 1 && readn(fd, &b, 1) == 1)
      n++;
    close(fd);
  }
  {
    double dt = now() - t0;
    printf("netbench: connect %.0f connections/s (%.0f us each)\n",
           n / dt, n ? dt * 1e6 / n : 0.0);
  }
}

static void
t_udp(const char *host)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    die("socket");
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &to.sin_addr) != 1)
    die("address");
  char *buf = malloc(bufsize);
  if (buf == 0)
    die("malloc");
  memset(buf, 'u', bufsize);
  double t0 = now(), end = t0 + secs;
  long long sent = 0, bytes = 0;
  while (now() < end) {
    for (int i = 0; i < 50; i++) {
      int k = sendto(fd, buf, bufsize, 0, (struct sockaddr *)&to, sizeof(to));
      if (k <= 0)
        goto done;
      sent++;
      bytes += k;
    }
  }
done:
  {
    double dt = now() - t0;
    close(fd);
    printf("netbench: udp %.2f MB/s (%lld datagrams of %d bytes in %.2fs)\n",
           bytes / dt / 1048576.0, sent, bufsize, dt);
  }
  free(buf);
}

static void
usage(void)
{
  fprintf(stderr,
          "usage: netbench [-t secs] [-p port] [-s size] [-v] [-l | -c HOST]"
          " [stream|rr|connect|udp ...]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  const char *tests[8];
  int ntests = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
      secs = atof(argv[++i]);
    else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
      port = atoi(argv[++i]);
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
      bufsize = atoi(argv[++i]);
    else if (strcmp(argv[i], "-v") == 0)
      verbose = 1;
    else if (strcmp(argv[i], "-l") == 0)
      listen_only = 1;
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
      peer_host = argv[++i];
    else if (argv[i][0] == '-')
      usage();
    else if (ntests < 8)
      tests[ntests++] = argv[i];
  }
  if (bufsize <= 0 || bufsize > (1 << 20))
    usage();
  if (ntests == 0) {
    tests[ntests++] = "stream";
    tests[ntests++] = "rr";
  }

  if (listen_only) {
    printf("netbench: listening on port %d\n", port);
    serve(0);
    return 0;
  }

  const char *host = peer_host;
  int pid = -1;
  if (host == 0) {
    // to ourselves: a server of our own, and the loopback address. This
    // is the stack with no device underneath it, which is the number the
    // guest's own figure has to be read against.
    host = "127.0.0.1";
    pid = fork();
    if (pid < 0)
      die("fork");
    if (pid == 0) {
      // for as long as the tests take: `connect' makes thousands of them,
      // and a bound here is a server that stops answering halfway through
      // and a client that reports "connection refused" about its own
      // scaffolding. The parent kills it when it is done.
      serve(0);
      exit(0);
    }
    // give it a moment to reach listen()
    struct timespec ts = {0, 200 * 1000 * 1000};
    nanosleep(&ts, 0);
  }

  for (int i = 0; i < ntests; i++) {
    if (strcmp(tests[i], "stream") == 0)
      t_stream(host);
    else if (strcmp(tests[i], "rr") == 0)
      t_rr(host);
    else if (strcmp(tests[i], "connect") == 0)
      t_connect(host);
    else if (strcmp(tests[i], "udp") == 0)
      t_udp(host);
    else {
      fprintf(stderr, "netbench: no test called %s\n", tests[i]);
      if (pid > 0)
        kill(pid, SIGKILL);
      return 1;
    }
  }
  if (pid > 0) {
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
  }
  return 0;
}
