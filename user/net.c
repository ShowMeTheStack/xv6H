// user-level network helpers: addresses, DNS, TCP connections.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

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

// ---- where a name is looked up ----

// /etc/hosts: an address, a name, and any number of other names for
// it. a machine's own names live here and nowhere else, and looking
// here first is what makes a resolver work before the network does.
static int
hosts_lookup(const char *name, uint32 *ip)
{
  FILE *f = fopen("/etc/hosts", "r");
  char line[256];

  if (f == 0)
    return -1;
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash)
      *hash = 0;
    char *p = line;
    char *fields[8];
    int nf = 0;
    while (*p && nf < 8) {
      while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
      if (*p == 0)
        break;
      fields[nf++] = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n')
        p++;
      if (*p)
        *p++ = 0;
    }
    if (nf < 2)
      continue;
    for (int i = 1; i < nf; i++)
      if (strcmp(fields[i], name) == 0 && parse_ip(fields[0], ip) == 0) {
        fclose(f);
        return 0;
      }
  }
  fclose(f);
  return -1;
}

// /etc/resolv.conf: which servers to ask and what to try adding to a
// name that has no dots in it
struct resolv_conf {
  uint32 ns[3];
  int nns;
  char search[4][64];
  int nsearch;
};

static void
read_resolv_conf(struct resolv_conf *rc)
{
  FILE *f = fopen("/etc/resolv.conf", "r");
  char line[256];

  memset(rc, 0, sizeof(*rc));
  if (f == 0)
    return;
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash)
      *hash = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    char *key = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n')
      p++;
    if (*p)
      *p++ = 0;
    if (strcmp(key, "nameserver") == 0) {
      while (*p == ' ' || *p == '\t')
        p++;
      char *v = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n')
        p++;
      *p = 0;
      uint32 a;
      if (rc->nns < 3 && parse_ip(v, &a) == 0)
        rc->ns[rc->nns++] = a;
    } else if (strcmp(key, "search") == 0 || strcmp(key, "domain") == 0) {
      // `domain' names one, `search' names a list; both end up here
      while (*p && rc->nsearch < 4) {
        while (*p == ' ' || *p == '\t')
          p++;
        if (*p == 0 || *p == '\n')
          break;
        char *v = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
          p++;
        if (*p)
          *p++ = 0;
        strncpy(rc->search[rc->nsearch], v, sizeof(rc->search[0]) - 1);
        rc->search[rc->nsearch][sizeof(rc->search[0]) - 1] = 0;
        rc->nsearch++;
      }
    }
  }
  fclose(f);
}

// one question to one server, with retries
static int
dns_query(uint32 nameserver, const char *name, uint32 *ip)
{
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

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_in server;
  memset(&server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_port = htons(53);
  server.sin_addr.s_addr = htonl(nameserver);
  // three seconds per attempt, which is what this has always waited
  struct timeval tv = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int rc = -1;
  for (int attempt = 0; attempt < 3 && rc < 0; attempt++) {
    if (sendto(fd, q, n, 0, (struct sockaddr *)&server, sizeof(server)) < 0)
      break;
    int len = recvfrom(fd, r, sizeof(r), 0, 0, 0);
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

// a name to an address: a number as it stands, then /etc/hosts, then
// the servers /etc/resolv.conf names -- or the one the interface was
// given, when there is no such file. a name with a dot in it is asked
// for as it stands first and then with each search domain after it; a
// name with none is tried the other way round, which is the rule
// every resolver has followed since /etc/resolv.conf was invented.
int
dns_resolve(const char *name, uint32 *ip)
{
  struct resolv_conf rc;

  if (parse_ip(name, ip) == 0)
    return 0;
  if (hosts_lookup(name, ip) == 0)
    return 0;

  read_resolv_conf(&rc);
  if (rc.nns == 0) {
    struct netinfo ni;
    if (netinfo(&ni) == 0 && ni.dns)
      rc.ns[rc.nns++] = ni.dns;
  }
  if (rc.nns == 0)
    return -1;

  int dotted = strchr(name, '.') != 0;
  for (int s = 0; s < rc.nns; s++) {
    if (dotted && dns_query(rc.ns[s], name, ip) == 0)
      return 0;
    for (int k = 0; k < rc.nsearch; k++) {
      char full[256];
      int n = strlen(name), m = strlen(rc.search[k]);
      if (n + m + 2 > (int)sizeof(full))
        continue;
      memmove(full, name, n);
      full[n] = '.';
      memmove(full + n + 1, rc.search[k], m + 1);
      if (dns_query(rc.ns[s], full, ip) == 0)
        return 0;
    }
    if (!dotted && dns_query(rc.ns[s], name, ip) == 0)
      return 0;
  }
  return -1;
}

// connect a TCP stream to host:port; returns the socket fd or -1.
int
tcp_open(const char *host, int port)
{
  uint32 ip;
  if (dns_resolve(host, &ip) < 0) {
    fprintf(stderr, "cannot resolve %s\n", host);
    return -1;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons((uint16)port);
  to.sin_addr.s_addr = htonl(ip);
  int rc = connect(fd, (struct sockaddr *)&to, sizeof(to));
  if (rc < 0) {
    char b[20];
    fprintf(stderr, "connect to %s:%d failed%s\n", ip2str(ip, b), port, (rc < 0 && errno == EACCES) ? " (blocked by policy)" : "");
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
