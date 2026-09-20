//
// https [-v] URL [file]: fetch https://host[:port]/path over TLS 1.2 and
// print the body (or save it to a file). the server's certificate chain
// is verified against the root CAs embedded at build time (tls/roots.pem,
// tls/trust_anchors.c). TLS is BearSSL (tls/bearssl), built as an xv6
// user library.
//
// the certificate validity check uses the real-time clock (clock_gettime,
// the RTC); on a machine without one, or one whose clock is earlier than
// the build, the build date (user/tls_date.h) plus uptime stands in.
// entropy for the client random and ECDHE key comes from the cycle, time
// and instret counters read at several points, mixed by BearSSL's
// HMAC-DRBG. that is honest enough for an experiment, not for secrets.
//
// the same file builds on Linux (static, musl) for the Linux guest, where
// it uses sockets, time() and getrandom().
//
// exit status 0 if the server answered 200, 1 otherwise.
//
#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define oprintf(...) fprintf(stdout, __VA_ARGS__)
typedef uint64_t uint64;

static int
plat_connect(const char *host, int port)
{
  struct addrinfo hints, *res = 0, *a;
  char ps[16];
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(ps, sizeof(ps), "%d", port);
  if (getaddrinfo(host, ps, &hints, &res) != 0) {
    eprintf("https: cannot resolve %s\n", host);
    return -1;
  }
  int fd = -1;
  for (a = res; a; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype, 0);
    if (fd < 0)
      continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0)
    eprintf("https: connect to %s:%d failed\n", host, port);
  return fd;
}

#ifndef TLS_BUILD_SECS
#define TLS_BUILD_SECS 0
#endif

// a Linux guest has no real-time clock either: its clock starts at 1970.
// when the clock is earlier than the build date, use the build date plus
// the time since boot.
static void
plat_time(uint32_t *days, uint32_t *secs)
{
  time_t t = time(0);
  if (t < (time_t)TLS_BUILD_SECS) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t = (time_t)TLS_BUILD_SECS + ts.tv_sec;
  }
  *days = (uint32_t)(t / 86400) + 719528; // unix epoch in days since Jan 1st, 0 AD
  *secs = (uint32_t)(t % 86400);
}

// a guest without entropy sources may take minutes to initialise its
// random pool, and getrandom() would block that long: ask for whatever
// the kernel has now, and add the clock.
#ifndef GRND_INSECURE
#define GRND_INSECURE 0x0004
#endif
static void
plat_entropy(br_ssl_engine_context *eng)
{
  unsigned char buf[32];
  ssize_t n = getrandom(buf, sizeof(buf), GRND_INSECURE);
  if (n != (ssize_t)sizeof(buf))
    n = getrandom(buf, sizeof(buf), GRND_NONBLOCK);
  if (n != (ssize_t)sizeof(buf)) {
    int fd = open("/dev/urandom", O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
    if (fd >= 0)
      close(fd);
  }
  if (n > 0)
    br_ssl_engine_inject_entropy(eng, buf, n);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  br_ssl_engine_inject_entropy(eng, &ts, sizeof(ts));
}

static int
plat_open_out(const char *path)
{
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

static int
plat_read(int fd, void *buf, int n)
{
  int r;
  do {
    r = read(fd, buf, n);
  } while (r < 0 && errno == EINTR);
  return r;
}

#define plat_write write
#define plat_close close
#define plat_exit exit
#else
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/net.h"
#include "kernel/time.h"
#include "user/user.h"
#include "user/tls_date.h"
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define oprintf(...) fprintf(stdout, __VA_ARGS__)
#define plat_connect tcp_open
#define plat_read read
#define plat_write write
#define plat_close close
#define plat_exit exit

static void
plat_time(uint32_t *days, uint32_t *secs)
{
  struct timespec ts;
  uint64 s;
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec >= (long)TLS_BUILD_SECS)
    s = ts.tv_sec;
  else
    s = (uint64)TLS_BUILD_SECS + uptime() / 10; // no RTC: build date, 10 ticks per second
  *days = (uint32_t)(s / 86400) + 719528;
  *secs = (uint32_t)(s % 86400);
}

// the user-readable counters: cycle, time, instret.
static void
plat_entropy(br_ssl_engine_context *eng)
{
  uint64 v[4];
  asm volatile("rdcycle %0" : "=r"(v[0]));
  asm volatile("rdtime %0" : "=r"(v[1]));
  asm volatile("rdinstret %0" : "=r"(v[2]));
  v[3] = ((uint64)getpid() << 32) ^ (uint64)uptime();
  br_ssl_engine_inject_entropy(eng, v, sizeof(v));
}

static int
plat_open_out(const char *path)
{
  return open(path, O_WRONLY | O_CREATE | O_TRUNC);
}
#endif

extern const br_x509_trust_anchor TAs[];
extern const size_t TAs_NUM;

static int
sock_read(void *ctx, unsigned char *buf, size_t len)
{
  int n = plat_read(*(int *)ctx, buf, (int)len);
  return n <= 0 ? -1 : n;
}

static int
sock_write(void *ctx, const unsigned char *buf, size_t len)
{
  int n = plat_write(*(int *)ctx, buf, (int)len);
  return n <= 0 ? -1 : n;
}

static const char *
errname(int err)
{
  static const struct {
    int code;
    const char *name;
  } names[] = {
    {BR_ERR_BAD_PARAM, "bad parameter"},
    {BR_ERR_BAD_STATE, "bad state"},
    {BR_ERR_UNSUPPORTED_VERSION, "unsupported protocol version"},
    {BR_ERR_BAD_VERSION, "bad protocol version"},
    {BR_ERR_BAD_MAC, "bad record MAC"},
    {BR_ERR_NO_RANDOM, "no entropy"},
    {BR_ERR_UNEXPECTED, "unexpected message"},
    {BR_ERR_BAD_HANDSHAKE, "bad handshake"},
    {BR_ERR_BAD_CIPHER_SUITE, "bad cipher suite"},
    {BR_ERR_BAD_SIGNATURE, "bad signature"},
    {BR_ERR_IO, "connection closed"},
    {BR_ERR_X509_BAD_SIGNATURE, "certificate: bad signature"},
    {BR_ERR_X509_EXPIRED, "certificate expired or not yet valid (the clock is the build date)"},
    {BR_ERR_X509_BAD_SERVER_NAME, "certificate does not match the server name"},
    {BR_ERR_X509_NOT_TRUSTED, "certificate chain not trusted (no embedded root signs it)"},
    {BR_ERR_X509_NOT_CA, "certificate: issuer is not a CA"},
    {BR_ERR_X509_EMPTY_CHAIN, "certificate: empty chain"},
    {BR_ERR_X509_UNSUPPORTED, "certificate: unsupported"},
    {BR_ERR_X509_WEAK_PUBLIC_KEY, "certificate: weak public key"},
    {BR_ERR_X509_LIMIT_EXCEEDED, "certificate: limit exceeded"},
    {BR_ERR_X509_BAD_TIME, "certificate: bad time"},
    {BR_ERR_X509_CRITICAL_EXTENSION, "certificate: unknown critical extension"},
  };
  static char buf[48];
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (names[i].code == err)
      return names[i].name;
  if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256) {
    const char *s = "received fatal alert ";
    int a = err - BR_ERR_RECV_FATAL_ALERT, i = 0;
    while (*s)
      buf[i++] = *s++;
    if (a >= 100)
      buf[i++] = '0' + a / 100;
    if (a >= 10)
      buf[i++] = '0' + (a / 10) % 10;
    buf[i++] = '0' + a % 10;
    buf[i] = 0;
    return buf;
  }
  if (err >= BR_ERR_SEND_FATAL_ALERT)
    return "sent fatal alert";
  return "TLS error";
}

static const char *
suitename(uint16_t id)
{
  switch (id) {
  case 0xC02B: return "ECDHE-ECDSA-AES128-GCM-SHA256";
  case 0xC02C: return "ECDHE-ECDSA-AES256-GCM-SHA384";
  case 0xC02F: return "ECDHE-RSA-AES128-GCM-SHA256";
  case 0xC030: return "ECDHE-RSA-AES256-GCM-SHA384";
  case 0xCCA8: return "ECDHE-RSA-CHACHA20-POLY1305";
  case 0xCCA9: return "ECDHE-ECDSA-CHACHA20-POLY1305";
  case 0xC023: return "ECDHE-ECDSA-AES128-SHA256";
  case 0xC027: return "ECDHE-RSA-AES128-SHA256";
  case 0x009C: return "RSA-AES128-GCM-SHA256";
  case 0x009D: return "RSA-AES256-GCM-SHA384";
  default: return "other suite";
  }
}

static int
my_atoi(const char *s)
{
  int n = 0;
  while (*s >= '0' && *s <= '9')
    n = n * 10 + (*s++ - '0');
  return n;
}

static void
my_cat(char *dst, int *len, const char *s)
{
  while (*s)
    dst[(*len)++] = *s++;
  dst[*len] = 0;
}

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;

int
main(int argc, char *argv[])
{
  int verbose = 0, ai = 1;
  if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v') {
    verbose = 1;
    ai = 2;
  }
  if (argc <= ai) {
    eprintf("usage: https [-v] https://host[:port]/path [file]\n");
    plat_exit(1);
  }
  char *url = argv[ai];
  const char *pfx = "https://";
  for (int i = 0; pfx[i]; i++) {
    if (url[i] != pfx[i]) {
      eprintf("https: only https:// urls\n");
      plat_exit(1);
    }
  }
  char host[128], *path;
  char *h = url + 8;
  int i = 0;
  while (h[i] && h[i] != '/' && h[i] != ':' && i < 127) {
    host[i] = h[i];
    i++;
  }
  host[i] = 0;
  int port = 443;
  if (h[i] == ':')
    port = my_atoi(h + i + 1);
  path = 0;
  for (char *p = h; *p; p++) {
    if (*p == '/') {
      path = p;
      break;
    }
  }
  if (path == 0)
    path = "/";

  // the TLS client: TLS 1.2 only, all of BearSSL's suites, the embedded
  // trust anchors, the current (approximate) date.
  br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
  br_ssl_engine_set_versions(&sc.eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
  uint32_t days, secs;
  plat_time(&days, &secs);
  br_x509_minimal_set_time(&xc, days, secs);
  plat_entropy(&sc.eng);

  int fd = plat_connect(host, port);
  if (fd < 0)
    plat_exit(1);
  plat_entropy(&sc.eng); // the connection's timing adds a little more

  if (!br_ssl_client_reset(&sc, host, 0)) {
    eprintf("https: client reset failed: %s\n", errname(br_ssl_engine_last_error(&sc.eng)));
    plat_exit(1);
  }
  br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);

  char req[512];
  int n = 0;
  my_cat(req, &n, "GET ");
  my_cat(req, &n, path);
  my_cat(req, &n, " HTTP/1.0\r\nHost: ");
  my_cat(req, &n, host);
  my_cat(req, &n, "\r\nUser-Agent: xv6-https\r\nConnection: close\r\n\r\n");
  if (br_sslio_write_all(&ioc, req, n) < 0 || br_sslio_flush(&ioc) < 0) {
    eprintf("https: handshake with %s failed: %s\n", host, errname(br_ssl_engine_last_error(&sc.eng)));
    plat_exit(1);
  }
  if (verbose) {
    const br_ssl_session_parameters *sp = &sc.eng.session;
    eprintf("https: TLS %d.%d, %s (0x%x)\n", (sp->version >> 8) - 2, (sp->version & 0xff) - 1,
            suitename(sp->cipher_suite), sp->cipher_suite);
  }

  int out = 1;
  if (argc > ai + 1) {
    out = plat_open_out(argv[ai + 1]);
    if (out < 0) {
      eprintf("https: cannot create %s\n", argv[ai + 1]);
      plat_exit(1);
    }
  }

  static char buf[4096], hdr[4096];
  int total = 0, header_done = 0, status = 0, hlen = 0;
  for (;;) {
    int r = br_sslio_read(&ioc, buf, sizeof(buf));
    if (r < 0)
      break;
    char *p = buf;
    int len = r;
    if (!header_done) {
      // accumulate the header until the blank line.
      int take = len;
      if (hlen + take > (int)sizeof(hdr) - 1)
        take = sizeof(hdr) - 1 - hlen;
      for (int k = 0; k < take; k++)
        hdr[hlen + k] = buf[k];
      hlen += take;
      hdr[hlen] = 0;
      int end = -1;
      for (int k = 0; k + 3 < hlen; k++) {
        if (hdr[k] == '\r' && hdr[k + 1] == '\n' && hdr[k + 2] == '\r' && hdr[k + 3] == '\n') {
          end = k + 4;
          break;
        }
      }
      if (end < 0) {
        if (hlen >= (int)sizeof(hdr) - 1) {
          eprintf("https: response header too long\n");
          plat_exit(1);
        }
        continue;
      }
      // "HTTP/1.x NNN ..."
      if (hlen >= 12)
        status = my_atoi(hdr + 9);
      if (verbose) {
        for (int k = 0; k < end; k++)
          if (hdr[k] != '\r')
            plat_write(2, hdr + k, 1);
      }
      header_done = 1;
      // body bytes that arrived in this same read (and any excess from
      // the last accumulation: hlen may exceed end).
      p = hdr + end;
      len = hlen - end;
    }
    if (len > 0) {
      if (plat_write(out, p, len) != len) {
        eprintf("https: write failed\n");
        plat_exit(1);
      }
      total += len;
    }
  }
  int err = br_ssl_engine_last_error(&sc.eng);
  // a clean close_notify leaves BR_ERR_OK; many servers just close the
  // TCP connection after the response (BR_ERR_IO), which is fine too.
  if (!header_done) {
    eprintf("https: %s: %s\n", host, errname(err));
    plat_exit(1);
  }
  if (err != BR_ERR_OK && err != BR_ERR_IO)
    eprintf("https: warning: %s\n", errname(err));
  if (out != 1)
    plat_close(out);
  if (total > 0 && out == 1)
    plat_write(1, "\n", 1);
  eprintf("https: %s:%d%s -> HTTP %d, %d body bytes, verified against embedded roots\n", host, port, path,
          status, total);
  plat_close(fd);
  plat_exit(status == 200 ? 0 : 1);
}
