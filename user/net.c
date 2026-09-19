// user-level network helpers: addresses, DNS, TCP connections.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "kernel/socket.h"
#include "user/user.h"

char *
ip2str(uint32 ip, char *buf)
{
  char *p = buf;
  for (int i = 3; i >= 0; i--) {
    int b = (ip >> (8 * i)) & 0xff;
    if (b >= 100) *p++ = '0' + b / 100;
    if (b >= 10) *p++ = '0' + (b / 10) % 10;
    *p++ = '0' + b % 10;
    if (i) *p++ = '.';
  }
  *p = 0;
  return buf;
}

int
parse_ip(const char *s, uint32 *ip)
{
  uint32 v = 0;
  for (int i = 0; i < 4; i++) {
    int n = 0, d = 0;
    while (*s >= '0' && *s <= '9') {
      n = n * 10 + (*s++ - '0');
      d++;
    }
    if (d == 0 || n > 255)
      return -1;
    v = (v << 8) | n;
    if (i < 3) {
      if (*s != '.')
        return -1;
      s++;
    }
  }
  if (*s && *s != '/')
    return -1;
  *ip = v;
  return 0;
}

// resolve a host name to an IPv4 address with the interface's DNS server.
int
dns_resolve(const char *name, uint32 *ip)
{
  if (parse_ip(name, ip) == 0)
    return 0;
  struct netinfo ni;
  if (netinfo(&ni) < 0 || ni.dns == 0)
    return -1;
  unsigned char q[512], r[512];
  int n = 0;
  static int id = 1;
  int xid = (id++ & 0x7fff) | 0x1000;
  q[n++] = xid >> 8; q[n++] = xid & 0xff;
  q[n++] = 1; q[n++] = 0;     // recursion desired
  q[n++] = 0; q[n++] = 1;     // one question
  q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
  const char *p = name;
  while (*p) {
    const char *dot = p;
    while (*dot && *dot != '.')
      dot++;
    int len = dot - p;
    if (len == 0 || len > 63 || n + len + 2 > 400)
      return -1;
    q[n++] = len;
    memmove(q + n, p, len);
    n += len;
    p = *dot ? dot + 1 : dot;
  }
  q[n++] = 0;
  q[n++] = 0; q[n++] = 1; // type A
  q[n++] = 0; q[n++] = 1; // class IN

  int fd = socket(SOCK_DGRAM);
  if (fd < 0)
    return -1;
  int rc = -1;
  for (int attempt = 0; attempt < 3 && rc < 0; attempt++) {
    if (sendto(fd, q, n, ni.dns, 53) < 0)
      break;
    uint32 src;
    int sport;
    int len = recvfrom(fd, r, sizeof(r), &src, &sport, 30);
    if (len < 12 || r[0] != (xid >> 8) || r[1] != (xid & 0xff))
      continue;
    int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
    int i = 12;
    // skip questions
    for (int k = 0; k < qd; k++) {
      while (i < len && r[i]) {
        if (r[i] & 0xc0) { i++; break; }
        i += r[i] + 1;
      }
      i += 5;
    }
    for (int k = 0; k < an && i + 12 <= len; k++) {
      // name (possibly compressed)
      if (r[i] & 0xc0) i += 2;
      else { while (i < len && r[i]) i += r[i] + 1; i++; }
      int type = (r[i] << 8) | r[i + 1];
      int rdlen = (r[i + 8] << 8) | r[i + 9];
      i += 10;
      if (type == 1 && rdlen == 4 && i + 4 <= len) {
        *ip = ((uint32)r[i] << 24) | ((uint32)r[i + 1] << 16) | ((uint32)r[i + 2] << 8) | r[i + 3];
        rc = 0;
        break;
      }
      i += rdlen;
    }
  }
  close(fd);
  return rc;
}

// connect a TCP stream to host:port; returns the socket fd or -1.
int
tcp_open(const char *host, int port)
{
  uint32 ip;
  if (dns_resolve(host, &ip) < 0) {
    fprintf(2, "cannot resolve %s\n", host);
    return -1;
  }
  int fd = socket(SOCK_STREAM);
  if (fd < 0)
    return -1;
  int rc = connect(fd, ip, port);
  if (rc < 0) {
    char b[20];
    fprintf(2, "connect to %s:%d failed%s\n", ip2str(ip, b), port, rc == -2 ? " (blocked by policy)" : "");
    close(fd);
    return -1;
  }
  return fd;
}

int
read_all(int fd, char *buf, int max)
{
  int total = 0, n;
  while (total < max && (n = read(fd, buf + total, max - total)) > 0)
    total += n;
  return total;
}
