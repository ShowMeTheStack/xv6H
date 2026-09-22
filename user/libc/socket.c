//
// the parts of the socket interface that are not system calls: the
// three or four wrappers the kernel does not need to see, addresses
// as text, and the name lookups.
//
// getaddrinfo is the one worth reading. a program that wants to reach
// somewhere should not have to know whether what it was given is a
// name or a number, nor build a sockaddr by hand; it asks for a host
// and a service and is handed a list of addresses ready for connect.
// what comes back is one address here, because this system has one
// family and one address per name, but the shape is the real one, so
// a program written against it is written against the real thing.
//
#include "kernel/types.h"
#include "kernel/net.h"   // struct netinfo, for getifaddrs
#include "user/user.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <ifaddrs.h>

int h_errno;

// ---- the calls that are only shapes over other calls ----

int
accept(int fd, struct sockaddr *addr, socklen_t *len)
{
  return accept4(fd, addr, len, 0);
}

ssize_t
send(int fd, const void *buf, size_t n, int flags)
{
  return sendto(fd, buf, n, flags, 0, 0);
}

ssize_t
recv(int fd, void *buf, size_t n, int flags)
{
  return recvfrom(fd, buf, n, flags, 0, 0);
}

struct cmsghdr *
__cmsg_nxthdr(struct msghdr *m, struct cmsghdr *c)
{
  if (c->cmsg_len < sizeof(struct cmsghdr))
    return 0;
  char *next = (char *)c + CMSG_ALIGN(c->cmsg_len);
  char *end = (char *)m->msg_control + m->msg_controllen;
  if (next + sizeof(struct cmsghdr) > end)
    return 0;
  return (struct cmsghdr *)next;
}

// ---- addresses as text ----

int
inet_pton(int af, const char *src, void *dst)
{
  uint32 v = 0;

  if (af != AF_INET) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  for (int i = 0; i < 4; i++) {
    int n = 0, d = 0;
    while (*src >= '0' && *src <= '9') {
      n = n * 10 + (*src++ - '0');
      if (++d > 3 || n > 255)
        return 0;
    }
    if (d == 0)
      return 0;
    v = (v << 8) | n;
    if (i < 3) {
      if (*src != '.')
        return 0;
      src++;
    }
  }
  if (*src)
    return 0;
  *(in_addr_t *)dst = htonl(v);
  return 1;
}

const char *
inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
  char tmp[16];
  int n = 0;
  uint32 v;

  if (af != AF_INET) {
    errno = EAFNOSUPPORT;
    return 0;
  }
  v = ntohl(*(const in_addr_t *)src);
  for (int i = 3; i >= 0; i--) {
    int b = (v >> (i * 8)) & 0xff;
    if (b >= 100)
      tmp[n++] = '0' + b / 100;
    if (b >= 10)
      tmp[n++] = '0' + (b / 10) % 10;
    tmp[n++] = '0' + b % 10;
    if (i)
      tmp[n++] = '.';
  }
  tmp[n++] = 0;
  if ((socklen_t)n > size) {
    errno = ENOSPC;
    return 0;
  }
  memcpy(dst, tmp, n);
  return dst;
}

int
inet_aton(const char *s, struct in_addr *out)
{
  return inet_pton(AF_INET, s, &out->s_addr) == 1;
}

in_addr_t
inet_addr(const char *s)
{
  struct in_addr a;

  if (!inet_aton(s, &a))
    return INADDR_NONE;
  return a.s_addr;
}

char *
inet_ntoa(struct in_addr in)
{
  static char buf[16];

  inet_ntop(AF_INET, &in.s_addr, buf, sizeof(buf));
  return buf;
}

// ---- interfaces ----

// this machine has one interface and the loopback every machine has.
// the numbering is the order they are named in, which is all an index
// has ever promised to be.
static const char *const ifnames[] = {"lo", "eth0", 0};

unsigned int
if_nametoindex(const char *name)
{
  for (int i = 0; ifnames[i]; i++)
    if (strcmp(ifnames[i], name) == 0)
      return i + 1;
  errno = ENODEV;
  return 0;
}

char *
if_indextoname(unsigned int index, char *name)
{
  if (index == 0 || index > 2) {
    errno = ENXIO;
    return 0;
  }
  strcpy(name, ifnames[index - 1]);
  return name;
}

struct if_nameindex *
if_nameindex(void)
{
  int n = 0;
  while (ifnames[n])
    n++;
  // the table and the names it points at, in one allocation, so that
  // if_freenameindex is a single free
  int size = (n + 1) * sizeof(struct if_nameindex) + n * IF_NAMESIZE;
  struct if_nameindex *p = malloc(size);
  if (p == 0)
    return 0;
  char *names = (char *)(p + n + 1);
  for (int i = 0; i < n; i++) {
    p[i].if_index = i + 1;
    p[i].if_name = names + i * IF_NAMESIZE;
    strcpy(p[i].if_name, ifnames[i]);
  }
  p[n].if_index = 0;
  p[n].if_name = 0;
  return p;
}

void
if_freenameindex(struct if_nameindex *p)
{
  free(p);
}

// one node of a getifaddrs list: the entry and the three addresses it
// points at, together, so freeifaddrs is one free per node
struct ifa_node {
  struct ifaddrs ifa;
  struct sockaddr_in addr, netmask, broad;
  char name[IF_NAMESIZE];
};

static struct ifa_node *
ifa_make(const char *name, unsigned flags, uint32 ip, uint32 mask)
{
  struct ifa_node *n = malloc(sizeof(*n));

  if (n == 0)
    return 0;
  memset(n, 0, sizeof(*n));
  strcpy(n->name, name);
  n->addr.sin_family = AF_INET;
  n->addr.sin_addr.s_addr = htonl(ip);
  n->netmask.sin_family = AF_INET;
  n->netmask.sin_addr.s_addr = htonl(mask);
  n->broad.sin_family = AF_INET;
  n->broad.sin_addr.s_addr = htonl(ip | ~mask);
  n->ifa.ifa_name = n->name;
  n->ifa.ifa_flags = flags;
  n->ifa.ifa_addr = (struct sockaddr *)&n->addr;
  n->ifa.ifa_netmask = (struct sockaddr *)&n->netmask;
  n->ifa.ifa_broadaddr = (struct sockaddr *)&n->broad;
  return n;
}

int
getifaddrs(struct ifaddrs **ifap)
{
  struct netinfo ni;
  struct ifa_node *lo, *eth;

  if (ifap == 0) {
    errno = EINVAL;
    return -1;
  }
  *ifap = 0;
  memset(&ni, 0, sizeof(ni));
  netinfo(&ni);   // fails when there is no interface; loopback is still true

  lo = ifa_make("lo", IFF_UP | IFF_LOOPBACK | IFF_RUNNING, 0x7f000001,
                0xff000000);
  if (lo == 0) {
    errno = ENOMEM;
    return -1;
  }
  *ifap = &lo->ifa;
  if (ni.ip) {
    eth = ifa_make("eth0",
                   (ni.up ? IFF_UP | IFF_RUNNING : 0) | IFF_BROADCAST |
                       IFF_MULTICAST,
                   ni.ip, ni.mask);
    if (eth == 0) {
      freeifaddrs(*ifap);
      *ifap = 0;
      errno = ENOMEM;
      return -1;
    }
    lo->ifa.ifa_next = &eth->ifa;
  }
  return 0;
}

void
freeifaddrs(struct ifaddrs *ifa)
{
  while (ifa) {
    struct ifaddrs *next = ifa->ifa_next;
    free(ifa);
    ifa = next;
  }
}

// ---- services and protocols ----

// the handful of names this system knows. /etc/services would be the
// place for these, and getservbyname reads it when it is there; the
// table is what answers when it is not.
struct svc {
  const char *name;
  int port;
  const char *proto;
};

static const struct svc services[] = {
    {"echo", 7, "udp"},      {"echo", 7, "tcp"},
    {"discard", 9, "udp"},   {"discard", 9, "tcp"},
    {"daytime", 13, "tcp"},  {"ftp", 21, "tcp"},
    {"ssh", 22, "tcp"},      {"telnet", 23, "tcp"},
    {"smtp", 25, "tcp"},     {"time", 37, "udp"},
    {"domain", 53, "udp"},   {"domain", 53, "tcp"},
    {"tftp", 69, "udp"},     {"http", 80, "tcp"},
    {"pop3", 110, "tcp"},    {"ntp", 123, "udp"},
    {"imap", 143, "tcp"},    {"snmp", 161, "udp"},
    {"https", 443, "tcp"},   {"syslog", 514, "udp"},
    {0, 0, 0},
};

struct proto {
  const char *name;
  int number;
};

static const struct proto protos[] = {
    {"ip", IPPROTO_IP},   {"icmp", IPPROTO_ICMP}, {"tcp", IPPROTO_TCP},
    {"udp", IPPROTO_UDP}, {"raw", IPPROTO_RAW},   {0, 0},
};

// /etc/services, when there is one. the fields are the ones the file
// has always had: name, port/proto, and then aliases.
static int
services_file(const char *name, int want, const char *proto, char *outname,
              int outsize, int *outport, char *outproto, int protosize)
{
  FILE *f = fopen("/etc/services", "r");
  char line[256];
  int found = 0;

  if (f == 0)
    return 0;
  while (!found && fgets(line, sizeof(line), f)) {
    char *p = line, *sname, *portstr;
    char *hash = strchr(line, '#');
    if (hash)
      *hash = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    sname = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p == 0)
      continue;
    *p++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    portstr = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n')
      p++;
    *p = 0;
    char *slash = strchr(portstr, '/');
    if (slash == 0)
      continue;
    *slash++ = 0;
    int port = atoi(portstr);
    if (proto && strcmp(slash, proto) != 0)
      continue;
    if (name ? strcmp(sname, name) == 0 : port == want) {
      strncpy(outname, sname, outsize - 1);
      outname[outsize - 1] = 0;
      strncpy(outproto, slash, protosize - 1);
      outproto[protosize - 1] = 0;
      *outport = port;
      found = 1;
    }
  }
  fclose(f);
  return found;
}

static struct servent *
servent(const char *name, int port, const char *proto)
{
  static struct servent se;
  static char namebuf[64], protobuf[16];
  static char *aliases[1] = {0};

  if (services_file(name, port, proto, namebuf, sizeof(namebuf), &port,
                    protobuf, sizeof(protobuf))) {
    se.s_name = namebuf;
    se.s_aliases = aliases;
    se.s_port = htons((uint16)port);
    se.s_proto = protobuf;
    return &se;
  }
  for (const struct svc *s = services; s->name; s++) {
    if (proto && strcmp(s->proto, proto) != 0)
      continue;
    if (name ? strcmp(s->name, name) != 0 : s->port != port)
      continue;
    strcpy(namebuf, s->name);
    strcpy(protobuf, s->proto);
    se.s_name = namebuf;
    se.s_aliases = aliases;
    se.s_port = htons((uint16)s->port);
    se.s_proto = protobuf;
    return &se;
  }
  return 0;
}

struct servent *
getservbyname(const char *name, const char *proto)
{
  return servent(name, 0, proto);
}

struct servent *
getservbyport(int port, const char *proto)
{
  return servent(0, ntohs((uint16)port), proto);
}

// /etc/protocols, when there is one: a name, a number, and any other
// names for the same thing
static int
protocols_file(const char *name, int number, char *outname, int outsize,
               int *outnum)
{
  FILE *f = fopen("/etc/protocols", "r");
  char line[256];
  int found = 0;

  if (f == 0)
    return 0;
  while (!found && fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash)
      *hash = 0;
    char *p = line, *pname;
    while (*p == ' ' || *p == '\t')
      p++;
    pname = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n')
      p++;
    if (*p == 0 || *p == '\n')
      continue;
    *p++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    int n = atoi(p);
    if (name ? strcmp(pname, name) == 0 : n == number) {
      strncpy(outname, pname, outsize - 1);
      outname[outsize - 1] = 0;
      *outnum = n;
      found = 1;
    }
  }
  fclose(f);
  return found;
}

static struct protoent *
protoent(const char *name, int number)
{
  static struct protoent pe;
  static char namebuf[32];
  static char *aliases[1] = {0};

  if (protocols_file(name, number, namebuf, sizeof(namebuf), &number)) {
    pe.p_name = namebuf;
    pe.p_aliases = aliases;
    pe.p_proto = number;
    return &pe;
  }
  for (const struct proto *p = protos; p->name; p++) {
    if (name ? strcmp(p->name, name) != 0 : p->number != number)
      continue;
    strcpy(namebuf, p->name);
    pe.p_name = namebuf;
    pe.p_aliases = aliases;
    pe.p_proto = p->number;
    return &pe;
  }
  return 0;
}

struct protoent *
getprotobyname(const char *name)
{
  return protoent(name, -1);
}

struct protoent *
getprotobynumber(int proto)
{
  return protoent(0, proto);
}

// ---- names to addresses ----

struct hostent *
gethostbyname(const char *name)
{
  static struct hostent he;
  static char namebuf[256];
  static struct in_addr addr;
  static char *addrlist[2];
  static char *aliases[1] = {0};
  uint32 ip;

  if (name == 0) {
    h_errno = HOST_NOT_FOUND;
    return 0;
  }
  if (parse_ip(name, &ip) < 0 && dns_resolve(name, &ip) < 0) {
    h_errno = HOST_NOT_FOUND;
    return 0;
  }
  strncpy(namebuf, name, sizeof(namebuf) - 1);
  namebuf[sizeof(namebuf) - 1] = 0;
  addr.s_addr = htonl(ip);
  addrlist[0] = (char *)&addr;
  addrlist[1] = 0;
  he.h_name = namebuf;
  he.h_aliases = aliases;
  he.h_addrtype = AF_INET;
  he.h_length = 4;
  he.h_addr_list = addrlist;
  return &he;
}

struct hostent *
gethostbyaddr(const void *addr, socklen_t len, int type)
{
  static struct hostent he;
  static char namebuf[32];
  static struct in_addr a;
  static char *addrlist[2];
  static char *aliases[1] = {0};

  if (type != AF_INET || len != 4) {
    h_errno = NO_RECOVERY;
    return 0;
  }
  a = *(const struct in_addr *)addr;
  // there is no reverse lookup here; the answer is the address itself,
  // written out, which is what a caller printing h_name wants
  inet_ntop(AF_INET, &a.s_addr, namebuf, sizeof(namebuf));
  addrlist[0] = (char *)&a;
  addrlist[1] = 0;
  he.h_name = namebuf;
  he.h_aliases = aliases;
  he.h_addrtype = AF_INET;
  he.h_length = 4;
  he.h_addr_list = addrlist;
  return &he;
}

// one addrinfo and the sockaddr it points at, in one allocation, so
// that freeaddrinfo is one free per node
struct ai_node {
  struct addrinfo ai;
  struct sockaddr_in sin;
  char canon[256];
};

int
getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
            struct addrinfo **res)
{
  struct ai_node *n;
  uint32 ip = 0;
  int port = 0;
  int family = AF_UNSPEC, socktype = 0, protocol = 0, flags = 0;

  if (res == 0)
    return EAI_FAIL;
  *res = 0;
  if (hints) {
    family = hints->ai_family;
    socktype = hints->ai_socktype;
    protocol = hints->ai_protocol;
    flags = hints->ai_flags;
  }
  if (family != AF_UNSPEC && family != AF_INET)
    return EAI_FAMILY;
  if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
    return EAI_SOCKTYPE;
  if (node == 0 && service == 0)
    return EAI_NONAME;

  if (service) {
    if (service[0] >= '0' && service[0] <= '9') {
      port = atoi(service);
    } else if (flags & AI_NUMERICSERV) {
      return EAI_NONAME;
    } else {
      const char *proto = socktype == SOCK_DGRAM ? "udp" : "tcp";
      struct servent *se = getservbyname(service, proto);
      if (se == 0)
        return EAI_SERVICE;
      port = ntohs((uint16)se->s_port);
    }
    if (port < 0 || port > 65535)
      return EAI_SERVICE;
  }

  if (node == 0) {
    // no host: the address to bind to, which for AI_PASSIVE is every
    // interface and otherwise is this machine
    ip = (flags & AI_PASSIVE) ? 0 : 0x7f000001;
  } else if (parse_ip(node, &ip) == 0) {
    // a number, and nothing had to be looked up
  } else if (flags & AI_NUMERICHOST) {
    return EAI_NONAME;
  } else if (dns_resolve(node, &ip) < 0) {
    return EAI_NONAME;
  }

  n = malloc(sizeof(*n));
  if (n == 0)
    return EAI_MEMORY;
  memset(n, 0, sizeof(*n));
  n->sin.sin_family = AF_INET;
  n->sin.sin_port = htons((uint16)port);
  n->sin.sin_addr.s_addr = htonl(ip);
  n->ai.ai_family = AF_INET;
  n->ai.ai_socktype = socktype ? socktype : SOCK_STREAM;
  n->ai.ai_protocol = protocol;
  n->ai.ai_addrlen = sizeof(n->sin);
  n->ai.ai_addr = (struct sockaddr *)&n->sin;
  n->ai.ai_next = 0;
  if (flags & AI_CANONNAME) {
    if (node)
      strncpy(n->canon, node, sizeof(n->canon) - 1);
    else
      inet_ntop(AF_INET, &n->sin.sin_addr.s_addr, n->canon, sizeof(n->canon));
    n->ai.ai_canonname = n->canon;
  }
  *res = &n->ai;
  return 0;
}

void
freeaddrinfo(struct addrinfo *res)
{
  while (res) {
    struct addrinfo *next = res->ai_next;
    free(res);
    res = next;
  }
}

const char *
gai_strerror(int code)
{
  switch (code) {
  case 0: return "no error";
  case EAI_BADFLAGS: return "bad flags";
  case EAI_NONAME: return "name or service not known";
  case EAI_AGAIN: return "temporary failure in name resolution";
  case EAI_FAIL: return "non-recoverable failure in name resolution";
  case EAI_FAMILY: return "address family not supported";
  case EAI_SOCKTYPE: return "socket type not supported";
  case EAI_SERVICE: return "service not supported for this socket type";
  case EAI_MEMORY: return "out of memory";
  case EAI_SYSTEM: return "system error";
  case EAI_OVERFLOW: return "buffer overflow";
  }
  return "unknown error";
}

int
getnameinfo(const struct sockaddr *addr, socklen_t alen, char *host,
            socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
  const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

  if (addr == 0 || alen < sizeof(*sin) || addr->sa_family != AF_INET)
    return EAI_FAMILY;
  if (host && hostlen > 0) {
    // there is no reverse lookup, so a caller that insists on a name
    // is told so rather than handed a number pretending to be one
    if (flags & NI_NAMEREQD)
      return EAI_NONAME;
    if (inet_ntop(AF_INET, &sin->sin_addr.s_addr, host, hostlen) == 0)
      return EAI_OVERFLOW;
  }
  if (serv && servlen > 0) {
    int port = ntohs(sin->sin_port);
    struct servent *se = 0;
    if (!(flags & NI_NUMERICSERV))
      se = getservbyport(sin->sin_port, (flags & NI_DGRAM) ? "udp" : "tcp");
    if (se) {
      if (strlen(se->s_name) + 1 > servlen)
        return EAI_OVERFLOW;
      strcpy(serv, se->s_name);
    } else {
      char tmp[8];
      int n = 0, d = port;
      if (d == 0)
        tmp[n++] = '0';
      while (d) {
        tmp[n++] = '0' + d % 10;
        d /= 10;
      }
      if ((socklen_t)n + 1 > servlen)
        return EAI_OVERFLOW;
      for (int i = 0; i < n; i++)
        serv[i] = tmp[n - 1 - i];
      serv[n] = 0;
    }
  }
  return 0;
}
