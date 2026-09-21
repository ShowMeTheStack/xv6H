// netstat and ss: the sockets, the routes and the interfaces.
//
//   netstat [-a] [-n] [-t] [-u] [-l] [-p] [-r] [-i] [-s]
//   ss      [-a] [-n] [-t] [-u] [-l] [-p] [-s]
//
//   -a  every socket, not only the connected ones
//   -l  only the ones listening
//   -t  TCP    -u  UDP
//   -n  addresses and ports as numbers
//   -p  which process holds it
//   -r  the routing table (netstat)
//   -i  the interfaces (netstat)
//   -s  a summary by protocol
//
// everything here is read out of /proc/net, which is where the kernel
// writes it, in the same shape Linux writes it: these two programs
// know nothing that a person with cat does not.
#include "user/user.h"

static int all, numeric, want_tcp, want_udp, listening, show_pids;
static int is_ss;

static const char *
state_name(int st)
{
  // the numbers /proc/net/tcp uses, which are Linux's
  switch (st) {
  case 1: return "ESTABLISHED";
  case 2: return "SYN_SENT";
  case 3: return "SYN_RECV";
  case 4: return "FIN_WAIT1";
  case 5: return "FIN_WAIT2";
  case 6: return "TIME_WAIT";
  case 7: return "CLOSE";
  case 8: return "CLOSE_WAIT";
  case 9: return "LAST_ACK";
  case 10: return "LISTEN";
  case 11: return "CLOSING";
  }
  return "UNKNOWN";
}

static unsigned
hexval(const char *s, int n)
{
  unsigned v = 0;

  for (int i = 0; i < n; i++) {
    int c = s[i];
    if (c >= '0' && c <= '9')
      v = v * 16 + (c - '0');
    else if (c >= 'a' && c <= 'f')
      v = v * 16 + (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v = v * 16 + (c - 'A' + 10);
    else
      break;
  }
  return v;
}

// /proc/net writes an address as the four bytes in the order the
// machine holds them, so reading it back is a byte swap
static uint32
addr_of(const char *hex)
{
  unsigned v = hexval(hex, 8);

  return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) |
         (v >> 24);
}

static void
dotted(uint32 ip, char *out, int size)
{
  snprintf(out, size, "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
           (ip >> 8) & 0xff, ip & 0xff);
}

// a port as a name when there is one and -n was not asked for
static void
port_name(int port, char *out, int size)
{
  static const struct {
    int port;
    const char *name;
  } known[] = {
    {7, "echo"}, {20, "ftp-data"}, {21, "ftp"}, {22, "ssh"}, {23, "telnet"},
    {25, "smtp"}, {53, "domain"}, {67, "bootps"}, {68, "bootpc"},
    {69, "tftp"}, {80, "http"}, {110, "pop3"}, {123, "ntp"}, {143, "imap"},
    {161, "snmp"}, {443, "https"}, {514, "syslog"}, {587, "submission"},
    {993, "imaps"}, {995, "pop3s"}, {3128, "squid"}, {8080, "http-alt"},
    {0, 0}
  };

  if (port == 0) {
    snprintf(out, size, "*");
    return;
  }
  if (!numeric)
    for (int i = 0; known[i].name; i++)
      if (known[i].port == port) {
        snprintf(out, size, "%s", known[i].name);
        return;
      }
  snprintf(out, size, "%d", port);
}

static void
addr_port(uint32 ip, int port, char *out, int size)
{
  char a[24], p[24];

  if (ip == 0 && !numeric)
    snprintf(a, sizeof(a), "0.0.0.0");
  else
    dotted(ip, a, sizeof(a));
  port_name(port, p, sizeof(p));
  snprintf(out, size, "%s:%s", a, p);
}

// which process has this socket open. /proc/PID/status says how many
// sockets a process holds but not which; the inode column of
// /proc/net is the socket's slot, and a process's fd list is not kept
// here, so the answer is the best this system can give.
static void
who_has(int slot, char *out, int size)
{
  (void)slot;
  snprintf(out, size, "-");
}

struct row {
  const char *proto;
  uint32 lip, rip;
  int lport, rport;
  int state;
  unsigned txq, rxq;
  int slot;
};

static int
parse_row(const char *line, const char *proto, struct row *r)
{
  // "  0: 0100007F:0050 00000000:0000 0A 00000000:00000000 ... inode"
  const char *p = line;

  while (*p == ' ')
    p++;
  if (*p < '0' || *p > '9')
    return -1;
  while (*p >= '0' && *p <= '9')
    p++;
  if (*p != ':')
    return -1;
  p++;
  while (*p == ' ')
    p++;

  r->proto = proto;
  r->lip = addr_of(p);
  p += 8;
  if (*p != ':')
    return -1;
  r->lport = (int)hexval(p + 1, 4);
  p += 5;
  while (*p == ' ')
    p++;
  r->rip = addr_of(p);
  p += 8;
  if (*p != ':')
    return -1;
  r->rport = (int)hexval(p + 1, 4);
  p += 5;
  while (*p == ' ')
    p++;
  r->state = (int)hexval(p, 2);
  p += 2;
  while (*p == ' ')
    p++;
  r->txq = hexval(p, 8);
  p += 8;
  if (*p == ':')
    r->rxq = hexval(p + 1, 8);
  // the last number on the line is the slot
  const char *last = line + strlen(line);
  while (last > line && (last[-1] == '\n' || last[-1] == ' '))
    last--;
  const char *start = last;
  while (start > line && start[-1] >= '0' && start[-1] <= '9')
    start--;
  r->slot = atoi(start);
  return 0;
}

static int header_done;

static void
print_row(const struct row *r)
{
  char local[64], remote[64], pid[32];

  if (listening && r->state != 10 && strcmp(r->proto, "tcp") == 0)
    return;
  if (!all && !listening && strcmp(r->proto, "tcp") == 0 && r->state == 10)
    return;

  addr_port(r->lip, r->lport, local, sizeof(local));
  if (r->rport == 0 && r->rip == 0)
    snprintf(remote, sizeof(remote), "0.0.0.0:*");
  else
    addr_port(r->rip, r->rport, remote, sizeof(remote));
  who_has(r->slot, pid, sizeof(pid));

  if (is_ss) {
    if (!header_done) {
      printf("Netid State      Recv-Q Send-Q Local Address:Port"
             "        Peer Address:Port\n");
      header_done = 1;
    }
    printf("%-5s %-10s %-6u %-6u %-24s %s", r->proto,
           strcmp(r->proto, "tcp") == 0 ? state_name(r->state) : "UNCONN",
           r->rxq, r->txq, local, remote);
  } else {
    if (!header_done) {
      printf("Active Internet connections%s\n",
             all ? " (servers and established)"
                 : (listening ? " (only servers)" : " (w/o servers)"));
      printf("Proto Recv-Q Send-Q Local Address           Foreign Address"
             "         State\n");
      header_done = 1;
    }
    printf("%-5s %6u %6u %-23s %-23s %s", r->proto, r->rxq, r->txq, local,
           remote, strcmp(r->proto, "tcp") == 0 ? state_name(r->state) : "");
  }
  if (show_pids)
    printf(" %s", pid);
  printf("\n");
}

static void
read_sockets(const char *path, const char *proto)
{
  FILE *f = fopen(path, "r");
  char line[512];

  if (f == 0)
    return;
  int first = 1;
  while (fgets(line, sizeof(line), f)) {
    if (first) {
      first = 0;
      continue; // the column titles
    }
    struct row r;
    memset(&r, 0, sizeof(r));
    if (parse_row(line, proto, &r) == 0)
      print_row(&r);
  }
  fclose(f);
}

static void
show_routes(void)
{
  FILE *f = fopen("/proc/net/route", "r");
  char line[512];

  if (f == 0) {
    fprintf(stderr, "netstat: /proc/net/route: %s\n", strerror(errno));
    return;
  }
  printf("Kernel IP routing table\n");
  printf("Destination     Gateway         Genmask         Flags   MSS Window"
         "  irtt Iface\n");
  int first = 1;
  while (fgets(line, sizeof(line), f)) {
    if (first) {
      first = 0;
      continue;
    }
    // iface \t dest \t gw \t flags \t ... \t mask
    char iface[16];
    int n = 0;
    const char *p = line;
    while (*p && *p != '\t' && *p != ' ' && n < 15)
      iface[n++] = *p++;
    iface[n] = 0;
    while (*p == '\t' || *p == ' ')
      p++;
    uint32 dest = addr_of(p);
    p += 8;
    while (*p == '\t' || *p == ' ')
      p++;
    uint32 gw = addr_of(p);
    p += 8;
    while (*p == '\t' || *p == ' ')
      p++;
    int flags = (int)hexval(p, 4);
    // skip flags, refcnt, use, metric to reach the mask
    // from the flags, the columns to step over are refcnt, use and
    // metric -- four in all, counting the flags themselves
    for (int k = 0; k < 4; k++) {
      while (*p && *p != '\t')
        p++;
      while (*p == '\t')
        p++;
    }
    uint32 mask = addr_of(p);

    char d[24], g[24], m[24];
    dotted(dest, d, sizeof(d));
    dotted(gw, g, sizeof(g));
    dotted(mask, m, sizeof(m));
    char fl[8];
    int fn = 0;
    if (flags & 1)
      fl[fn++] = 'U';
    if (flags & 2)
      fl[fn++] = 'G';
    fl[fn] = 0;
    printf("%-15s %-15s %-15s %-7s %3d %-6d %5d %s\n", d, g, m, fl, 0, 0, 0,
           iface);
  }
  fclose(f);
}

static void
show_interfaces(void)
{
  FILE *f = fopen("/proc/net/dev", "r");
  char line[512];

  if (f == 0) {
    fprintf(stderr, "netstat: /proc/net/dev: %s\n", strerror(errno));
    return;
  }
  printf("Kernel Interface table\n");
  printf("Iface   MTU    RX-OK RX-ERR RX-DRP RX-OVR    TX-OK TX-ERR TX-DRP"
         " TX-OVR Flg\n");
  int skip = 2;
  while (fgets(line, sizeof(line), f)) {
    if (skip > 0) {
      skip--;
      continue;
    }
    char *colon = strchr(line, ':');
    if (colon == 0)
      continue;
    *colon = 0;
    char *name = line;
    while (*name == ' ')
      name++;
    // bytes packets errs drop ... and the same again for transmit
    long v[16];
    int n = 0;
    char *p = colon + 1;
    while (n < 16) {
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p < '0' || *p > '9')
        break;
      v[n++] = atol(p);
      while (*p >= '0' && *p <= '9')
        p++;
    }
    if (n < 12)
      continue;
    printf("%-7s %-6d %6ld %6ld %6ld %6ld %8ld %6ld %6ld %6ld %s\n", name, 1500,
           v[1], v[2], v[3], v[4], v[9], v[10], v[11], v[11],
           strcmp(name, "lo") == 0 ? "LRU" : "BMRU");
  }
  fclose(f);
}

static void
show_summary(void)
{
  FILE *f = fopen("/proc/net/sockstat", "r");
  char line[256];

  if (f == 0) {
    fprintf(stderr, "netstat: /proc/net/sockstat: %s\n", strerror(errno));
    return;
  }
  while (fgets(line, sizeof(line), f))
    fputs(line, stdout);
  fclose(f);
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int routes = 0, interfaces = 0, summary = 0, i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  is_ss = strcmp(me, "ss") == 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'a': all = 1; break;
      case 'n': numeric = 1; break;
      case 't': want_tcp = 1; break;
      case 'u': want_udp = 1; break;
      case 'l': listening = 1; break;
      case 'p': show_pids = 1; break;
      case 'r': routes = 1; break;
      case 'i': interfaces = 1; break;
      case 's': summary = 1; break;
      case 'e': case 'o': case 'w': case 'x': case '4': break;
      default:
        fprintf(stderr, "usage: %s [-a] [-n] [-t] [-u] [-l] [-p]%s\n", me,
                is_ss ? " [-s]" : " [-r] [-i] [-s]");
        exit(1);
      }
    }
  }

  if (routes) {
    show_routes();
    exit(0);
  }
  if (interfaces) {
    show_interfaces();
    exit(0);
  }
  if (summary) {
    show_summary();
    exit(0);
  }
  if (!want_tcp && !want_udp)
    want_tcp = want_udp = 1;
  if (want_tcp)
    read_sockets("/proc/net/tcp", "tcp");
  if (want_udp)
    read_sockets("/proc/net/udp", "udp");
  exit(0);
}
