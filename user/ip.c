// ip, route and arp: the interface, where packets go, and who is who
// on the wire.
//
//   ip [-4] [-br] addr|link|route|neigh [show]
//   route [-n]                 the routing table
//   arp  [-n] [-a]             the address cache
//
// the state comes from /proc/net and from the interface itself; this
// program formats it, and setting an address is ifconfig's job.
#include "user/user.h"
#include "kernel/net.h"

static int numeric = 1, brief;

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

// the mask as a prefix length, which is how ip prints it
static int
prefix_len(uint32 mask)
{
  int n = 0;

  for (int i = 31; i >= 0; i--) {
    if (!(mask & (1u << i)))
      break;
    n++;
  }
  return n;
}

static void
mac_string(const uint8 *mac, char *out)
{
  static const char *hex = "0123456789abcdef";

  for (int i = 0; i < 6; i++) {
    out[3 * i] = hex[mac[i] >> 4];
    out[3 * i + 1] = hex[mac[i] & 15];
    out[3 * i + 2] = i == 5 ? 0 : ':';
  }
}

// ---- ip link / ip addr ----

static void
show_link(int with_addr)
{
  struct netinfo ni;
  char macs[18], a[24];

  if (netinfo(&ni) < 0) {
    fprintf(stderr, "ip: no network device\n");
    exit(1);
  }
  mac_string(ni.mac, macs);

  if (brief) {
    printf("%-10s %-10s ", "lo", "UNKNOWN");
    if (with_addr)
      printf("127.0.0.1/8");
    printf("\n");
    printf("%-10s %-10s ", "eth0", ni.up ? "UP" : "DOWN");
    if (with_addr && ni.up) {
      dotted(ni.ip, a, sizeof(a));
      printf("%s/%d", a, prefix_len(ni.mask));
    }
    printf("\n");
    return;
  }

  printf("1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 state UNKNOWN\n");
  printf("    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
  if (with_addr)
    printf("    inet 127.0.0.1/8 scope host lo\n");

  printf("2: eth0: <BROADCAST,MULTICAST%s> mtu 1500 state %s\n",
         ni.up ? ",UP,LOWER_UP" : "", ni.up ? "UP" : "DOWN");
  printf("    link/ether %s brd ff:ff:ff:ff:ff:ff\n", macs);
  if (with_addr && ni.up) {
    char b[24];
    dotted(ni.ip, a, sizeof(a));
    dotted((ni.ip & ni.mask) | ~ni.mask, b, sizeof(b));
    printf("    inet %s/%d brd %s scope global %s eth0\n", a,
           prefix_len(ni.mask), b, ni.dhcp ? "dynamic" : "");
  }
}

// ---- routes ----

struct route {
  char iface[16];
  uint32 dest, gw, mask;
  int flags;
};

static int
read_routes(struct route *out, int max)
{
  FILE *f = fopen("/proc/net/route", "r");
  char line[512];
  int n = 0;

  if (f == 0)
    return -1;
  int first = 1;
  while (fgets(line, sizeof(line), f) && n < max) {
    if (first) {
      first = 0;
      continue;
    }
    struct route *r = &out[n];
    const char *p = line;
    int k = 0;
    while (*p && *p != '\t' && *p != ' ' && k < 15)
      r->iface[k++] = *p++;
    r->iface[k] = 0;
    while (*p == '\t' || *p == ' ')
      p++;
    r->dest = addr_of(p);
    p += 8;
    while (*p == '\t' || *p == ' ')
      p++;
    r->gw = addr_of(p);
    p += 8;
    while (*p == '\t' || *p == ' ')
      p++;
    r->flags = (int)hexval(p, 4);
    // the flags, then refcnt, use and metric: four columns before the
    // mask
    for (int j = 0; j < 4; j++) {
      while (*p && *p != '\t')
        p++;
      while (*p == '\t')
        p++;
    }
    r->mask = addr_of(p);
    n++;
  }
  fclose(f);
  return n;
}

static void
show_route_ip(void)
{
  struct route r[16];
  int n = read_routes(r, 16);
  char d[24], g[24];

  if (n < 0) {
    fprintf(stderr, "ip: /proc/net/route: %s\n", strerror(errno));
    exit(1);
  }
  // the default route first, as ip prints it
  for (int i = 0; i < n; i++) {
    if (r[i].mask != 0)
      continue;
    dotted(r[i].gw, g, sizeof(g));
    printf("default via %s dev %s\n", g, r[i].iface);
  }
  for (int i = 0; i < n; i++) {
    if (r[i].mask == 0)
      continue;
    dotted(r[i].dest, d, sizeof(d));
    printf("%s/%d dev %s scope link\n", d, prefix_len(r[i].mask), r[i].iface);
  }
}

static void
show_route_classic(void)
{
  struct route r[16];
  int n = read_routes(r, 16);

  if (n < 0) {
    fprintf(stderr, "route: /proc/net/route: %s\n", strerror(errno));
    exit(1);
  }
  printf("Kernel IP routing table\n");
  printf("Destination     Gateway         Genmask         Flags Metric Ref"
         "    Use Iface\n");
  for (int i = 0; i < n; i++) {
    char d[24], g[24], m[24], fl[8];
    int fn = 0;

    if (r[i].mask == 0 && r[i].dest == 0 && !numeric)
      snprintf(d, sizeof(d), "default");
    else
      dotted(r[i].dest, d, sizeof(d));
    if (r[i].gw == 0 && !numeric)
      snprintf(g, sizeof(g), "*");
    else
      dotted(r[i].gw, g, sizeof(g));
    dotted(r[i].mask, m, sizeof(m));
    if (r[i].flags & 1)
      fl[fn++] = 'U';
    if (r[i].flags & 2)
      fl[fn++] = 'G';
    fl[fn] = 0;
    printf("%-15s %-15s %-15s %-5s %-6d %-3d %6d %s\n", d, g, m, fl, 0, 0, 0,
           r[i].iface);
  }
}

// ---- the address cache ----

static void
show_arp(int ip_form)
{
  FILE *f = fopen("/proc/net/arp", "r");
  char line[512];

  if (f == 0) {
    fprintf(stderr, "arp: /proc/net/arp: %s\n", strerror(errno));
    exit(1);
  }
  int first = 1, any = 0;
  if (!ip_form)
    printf("Address                  HWtype  HWaddress           Flags Mask"
           "            Iface\n");
  while (fgets(line, sizeof(line), f)) {
    if (first) {
      first = 0;
      continue;
    }
    // address \t type \t flags \t mac \t mask \t device
    char field[6][64];
    int n = 0, k = 0;
    for (const char *p = line; *p && n < 6; p++) {
      if (*p == '\t' || *p == '\n') {
        field[n][k] = 0;
        n++;
        k = 0;
        continue;
      }
      if (k < 63)
        field[n][k++] = *p;
    }
    if (n < 6)
      continue;
    any = 1;
    int flags = (int)strtol(field[2], 0, 0);
    if (ip_form)
      printf("%s dev %s lladdr %s %s\n", field[0], field[5], field[3],
             (flags & 2) ? "REACHABLE" : "INCOMPLETE");
    else
      printf("%-24s %-7s %-19s %-5s %-15s %s\n", field[0], "ether", field[3],
             (flags & 2) ? "C" : "", field[4], field[5]);
  }
  fclose(f);
  if (!any && !ip_form)
    ; // an empty cache prints only its title, as arp does
}

static void
usage(const char *me)
{
  if (strcmp(me, "route") == 0)
    fprintf(stderr, "usage: route [-n]\n");
  else if (strcmp(me, "arp") == 0)
    fprintf(stderr, "usage: arp [-n] [-a]\n");
  else
    fprintf(stderr, "usage: ip [-4] [-br] addr|link|route|neigh [show]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-br") == 0 || strcmp(argv[i], "-brief") == 0) {
      brief = 1;
      continue;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'n': numeric = 1; break;
      case 'a': break;         // arp -a, which is what it prints anyway
      case '4': break;
      case '6':
        fprintf(stderr, "%s: there is no IPv6 here\n", me);
        exit(1);
      case 'e': break;
      default: usage(me);
      }
    }
  }

  if (strcmp(me, "route") == 0) {
    if (i < argc) {
      fprintf(stderr, "route: adding and deleting routes is not supported;"
                      " use ifconfig to set the gateway\n");
      exit(1);
    }
    show_route_classic();
    exit(0);
  }
  if (strcmp(me, "arp") == 0) {
    show_arp(0);
    exit(0);
  }

  // ip: the object comes first, and "show" is what it does by default
  if (i >= argc)
    usage(me);
  const char *object = argv[i++];
  const char *verb = i < argc ? argv[i++] : "show";

  if (strcmp(verb, "show") != 0 && strcmp(verb, "list") != 0 &&
      strcmp(verb, "s") != 0 && strcmp(verb, "l") != 0) {
    fprintf(stderr, "ip: only `show' is supported; use ifconfig to change"
                    " the interface\n");
    exit(1);
  }

  if (strncmp(object, "a", 1) == 0)        // addr, address, a
    show_link(1);
  else if (strncmp(object, "l", 1) == 0)   // link, l
    show_link(0);
  else if (strncmp(object, "r", 1) == 0)   // route, r
    show_route_ip();
  else if (strncmp(object, "n", 1) == 0)   // neigh, n
    show_arp(1);
  else
    usage(me);
  exit(0);
}
