// tcpdump: what is going over the wire.
//
//   tcpdump [-i IFACE] [-n] [-c COUNT] [-s SNAPLEN] [-q] [-e] [-x] [-X]
//           [-t] [-w FILE] [-r FILE] [EXPRESSION]
//
//   -n  do not turn addresses into names
//   -c  stop after this many packets
//   -s  how much of each packet to keep
//   -q  one short line per packet
//   -e  print the ethernet header too
//   -x  the bytes in hex; -X hex and text
//   -t  no timestamps
//   -w  write a pcap file instead of printing
//   -r  read a pcap file instead of the wire
//
// the expression is the familiar subset:
//
//   host H        net N/M      port P        src H      dst H
//   tcp  udp  icmp  arp  ip
//   and  or  not  ( )
//
// the packets come from /dev/pktcap, which hands out pcap records --
// so -w is a copy with a file header in front, and -r reads back what
// anyone else wrote.
#include "user/user.h"
#include "kernel/net.h"

#define SNAP 1600

static int numeric, count_limit, snaplen = SNAP, brief, show_eth;
static int hex_dump, hex_and_text, no_time;
static const char *write_file, *read_file;
static long long shown, captured;
static int stop;

// ---- the expression ----

enum {
  E_TRUE, E_AND, E_OR, E_NOT,
  E_HOST, E_SRC, E_DST, E_NET, E_PORT, E_SRCPORT, E_DSTPORT,
  E_PROTO_TCP, E_PROTO_UDP, E_PROTO_ICMP, E_PROTO_ARP, E_PROTO_IP
};

struct expr {
  int kind;
  struct expr *a, *b;
  uint32 addr, mask;
  int port;
};

static struct expr pool[128];
static int npool;
static char **words;
static int nwords, at;

static struct expr *
node(int kind)
{
  if (npool >= 128) {
    fprintf(stderr, "tcpdump: the expression is too long\n");
    exit(1);
  }
  struct expr *e = &pool[npool++];
  memset(e, 0, sizeof(*e));
  e->kind = kind;
  return e;
}

static const char *
peek(void)
{
  return at < nwords ? words[at] : 0;
}

static struct expr *parse_or(void);

static struct expr *
parse_primary(void)
{
  const char *w = peek();

  if (w == 0)
    return 0;
  if (strcmp(w, "(") == 0) {
    at++;
    struct expr *e = parse_or();
    if (peek() && strcmp(peek(), ")") == 0)
      at++;
    return e;
  }
  if (strcmp(w, "not") == 0 || strcmp(w, "!") == 0) {
    at++;
    struct expr *e = node(E_NOT);
    e->a = parse_primary();
    return e;
  }
  if (strcmp(w, "tcp") == 0) { at++; return node(E_PROTO_TCP); }
  if (strcmp(w, "udp") == 0) { at++; return node(E_PROTO_UDP); }
  if (strcmp(w, "icmp") == 0) { at++; return node(E_PROTO_ICMP); }
  if (strcmp(w, "arp") == 0) { at++; return node(E_PROTO_ARP); }
  if (strcmp(w, "ip") == 0) { at++; return node(E_PROTO_IP); }

  int kind = -1;
  int direction = 0; // 1 src, 2 dst
  if (strcmp(w, "src") == 0) {
    direction = 1;
    at++;
    w = peek();
  } else if (strcmp(w, "dst") == 0) {
    direction = 2;
    at++;
    w = peek();
  }
  if (w == 0)
    return 0;

  if (strcmp(w, "host") == 0) {
    at++;
    kind = direction == 1 ? E_SRC : direction == 2 ? E_DST : E_HOST;
  } else if (strcmp(w, "net") == 0) {
    at++;
    kind = E_NET;
  } else if (strcmp(w, "port") == 0) {
    at++;
    kind = direction == 1 ? E_SRCPORT : direction == 2 ? E_DSTPORT : E_PORT;
  } else {
    fprintf(stderr, "tcpdump: cannot make sense of `%s'\n", w);
    exit(1);
  }

  const char *v = peek();
  if (v == 0) {
    fprintf(stderr, "tcpdump: the expression ends too soon\n");
    exit(1);
  }
  at++;
  struct expr *e = node(kind);
  if (kind == E_PORT || kind == E_SRCPORT || kind == E_DSTPORT) {
    e->port = atoi(v);
    return e;
  }
  char addr[64];
  snprintf(addr, sizeof(addr), "%s", v);
  char *slash = strchr(addr, '/');
  int bits = 32;
  if (slash) {
    *slash = 0;
    bits = atoi(slash + 1);
  }
  if (parse_ip(addr, &e->addr) < 0 && dns_resolve(addr, &e->addr) < 0) {
    fprintf(stderr, "tcpdump: cannot find %s\n", addr);
    exit(1);
  }
  e->mask = bits >= 32 ? 0xffffffffu : (bits <= 0 ? 0 : ~0u << (32 - bits));
  if (kind == E_NET)
    e->addr &= e->mask;
  return e;
}

static struct expr *
parse_and(void)
{
  struct expr *left = parse_primary();

  while (left && peek() &&
         (strcmp(peek(), "and") == 0 || strcmp(peek(), "&&") == 0)) {
    at++;
    struct expr *e = node(E_AND);
    e->a = left;
    e->b = parse_primary();
    left = e;
  }
  return left;
}

static struct expr *
parse_or(void)
{
  struct expr *left = parse_and();

  while (left && peek() &&
         (strcmp(peek(), "or") == 0 || strcmp(peek(), "||") == 0)) {
    at++;
    struct expr *e = node(E_OR);
    e->a = left;
    e->b = parse_and();
    left = e;
  }
  return left;
}

// ---- what a packet is ----

struct packet {
  int is_ip, is_arp;
  int proto;                // IPPROTO_*, when it is IP
  uint32 src, dst;
  int sport, dport;
};

static void
look(const unsigned char *p, int len, struct packet *k)
{
  memset(k, 0, sizeof(*k));
  if (len < 14)
    return;
  int type = (p[12] << 8) | p[13];
  const unsigned char *q = p + 14;
  int qlen = len - 14;

  if (type == ETHTYPE_ARP) {
    k->is_arp = 1;
    if (qlen >= 28) {
      k->src = (q[14] << 24) | (q[15] << 16) | (q[16] << 8) | q[17];
      k->dst = (q[24] << 24) | (q[25] << 16) | (q[26] << 8) | q[27];
    }
    return;
  }
  if (type != ETHTYPE_IP || qlen < 20)
    return;
  k->is_ip = 1;
  int ihl = (q[0] & 0xf) * 4;
  k->proto = q[9];
  k->src = (q[12] << 24) | (q[13] << 16) | (q[14] << 8) | q[15];
  k->dst = (q[16] << 24) | (q[17] << 16) | (q[18] << 8) | q[19];
  if (ihl < 20 || qlen < ihl)
    return;
  const unsigned char *l4 = q + ihl;
  int l4len = qlen - ihl;
  if ((k->proto == IPPROTO_TCP || k->proto == IPPROTO_UDP) && l4len >= 4) {
    k->sport = (l4[0] << 8) | l4[1];
    k->dport = (l4[2] << 8) | l4[3];
  }
}

static int
matches(const struct expr *e, const struct packet *k)
{
  if (e == 0)
    return 1;
  switch (e->kind) {
  case E_TRUE: return 1;
  case E_AND: return matches(e->a, k) && matches(e->b, k);
  case E_OR: return matches(e->a, k) || matches(e->b, k);
  case E_NOT: return !matches(e->a, k);
  case E_HOST: return k->src == e->addr || k->dst == e->addr;
  case E_SRC: return k->src == e->addr;
  case E_DST: return k->dst == e->addr;
  case E_NET:
    return (k->src & e->mask) == e->addr || (k->dst & e->mask) == e->addr;
  case E_PORT: return k->sport == e->port || k->dport == e->port;
  case E_SRCPORT: return k->sport == e->port;
  case E_DSTPORT: return k->dport == e->port;
  case E_PROTO_TCP: return k->is_ip && k->proto == IPPROTO_TCP;
  case E_PROTO_UDP: return k->is_ip && k->proto == IPPROTO_UDP;
  case E_PROTO_ICMP: return k->is_ip && k->proto == IPPROTO_ICMP;
  case E_PROTO_ARP: return k->is_arp;
  case E_PROTO_IP: return k->is_ip;
  }
  return 1;
}

// ---- printing ----

static void
dotted(uint32 ip, char *out, int size)
{
  snprintf(out, size, "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
           (ip >> 8) & 0xff, ip & 0xff);
}

static void
print_time(uint32 secs, uint32 usecs)
{
  if (no_time)
    return;
  printf("%02u:%02u:%02u.%06u ", (secs / 3600) % 24, (secs / 60) % 60,
         secs % 60, usecs);
}

static void
print_mac(const unsigned char *m)
{
  for (int i = 0; i < 6; i++)
    printf("%02x%s", m[i], i < 5 ? ":" : "");
}

static void
print_hex(const unsigned char *p, int len)
{
  for (int i = 0; i < len; i += 16) {
    printf("\t0x%04x:  ", i);
    for (int k = 0; k < 16; k += 2) {
      if (i + k < len)
        printf("%02x", p[i + k]);
      else
        printf("  ");
      if (i + k + 1 < len)
        printf("%02x ", p[i + k + 1]);
      else
        printf("   ");
    }
    if (hex_and_text) {
      printf(" ");
      for (int k = 0; k < 16 && i + k < len; k++)
        putchar(p[i + k] >= 0x20 && p[i + k] < 0x7f ? p[i + k] : '.');
    }
    printf("\n");
  }
}

static const char *
tcp_flags(int f, char *out)
{
  int n = 0;

  if (f & TCP_FIN) out[n++] = 'F';
  if (f & TCP_SYN) out[n++] = 'S';
  if (f & TCP_RST) out[n++] = 'R';
  if (f & TCP_PSH) out[n++] = 'P';
  if (f & TCP_ACK) out[n++] = '.';
  if (n == 0)
    out[n++] = '.';
  out[n] = 0;
  return out;
}

static void
print_packet(uint32 secs, uint32 usecs, const unsigned char *p, int len,
             int caplen)
{
  struct packet k;
  char a[24], b[24];

  look(p, caplen, &k);
  print_time(secs, usecs);

  if (show_eth) {
    print_mac(p + 6);
    printf(" > ");
    print_mac(p);
    printf(", ethertype 0x%04x, length %d: ", (p[12] << 8) | p[13], len);
  }

  if (k.is_arp) {
    dotted(k.src, a, sizeof(a));
    dotted(k.dst, b, sizeof(b));
    const unsigned char *q = p + 14;
    int op = caplen >= 14 + 8 ? ((q[6] << 8) | q[7]) : 0;
    if (op == ARP_OP_REQUEST)
      printf("ARP, Request who-has %s tell %s, length %d\n", b, a, len - 14);
    else
      printf("ARP, Reply %s is-at ", a), print_mac(q + 8),
          printf(", length %d\n", len - 14);
    goto bytes;
  }
  if (!k.is_ip) {
    printf("unknown ethertype 0x%04x, length %d\n", (p[12] << 8) | p[13], len);
    goto bytes;
  }

  dotted(k.src, a, sizeof(a));
  dotted(k.dst, b, sizeof(b));
  const unsigned char *q = p + 14;
  int ihl = (q[0] & 0xf) * 4;
  int iplen = (q[2] << 8) | q[3];
  const unsigned char *l4 = q + ihl;
  int l4len = iplen - ihl;

  switch (k.proto) {
  case IPPROTO_TCP: {
    char fl[8];
    int off = caplen >= 14 + ihl + 13 ? (l4[12] >> 4) * 4 : 20;
    unsigned seq = (l4[4] << 24) | (l4[5] << 16) | (l4[6] << 8) | l4[7];
    unsigned ack = (l4[8] << 24) | (l4[9] << 16) | (l4[10] << 8) | l4[11];
    int payload = l4len - off;
    if (payload < 0)
      payload = 0;
    printf("IP %s.%d > %s.%d: Flags [%s], seq %u", a, k.sport, b, k.dport,
           tcp_flags(l4[13], fl), seq);
    if (payload > 0)
      printf(":%u", seq + payload);
    if (l4[13] & TCP_ACK)
      printf(", ack %u", ack);
    printf(", win %d, length %d\n", (l4[14] << 8) | l4[15], payload);
    break;
  }
  case IPPROTO_UDP:
    printf("IP %s.%d > %s.%d: UDP, length %d\n", a, k.sport, b, k.dport,
           l4len - 8 > 0 ? l4len - 8 : 0);
    break;
  case IPPROTO_ICMP: {
    int type = caplen > 14 + ihl ? l4[0] : -1;
    const char *what = type == ICMP_ECHO ? "echo request"
                       : type == ICMP_ECHOREPLY ? "echo reply"
                       : type == ICMP_TIMXCEED ? "time exceeded"
                       : type == ICMP_UNREACH ? "unreachable"
                                              : "type ?";
    printf("IP %s > %s: ICMP %s", a, b, what);
    if ((type == ICMP_ECHO || type == ICMP_ECHOREPLY) && caplen >= 14 + ihl + 8)
      printf(", id %d, seq %d", (l4[4] << 8) | l4[5], (l4[6] << 8) | l4[7]);
    printf(", length %d\n", l4len);
    break;
  }
  default:
    printf("IP %s > %s: protocol %d, length %d\n", a, b, k.proto, l4len);
    break;
  }

bytes:
  if (hex_dump || hex_and_text)
    print_hex(p, caplen);
}

// ---- the pcap file header ----

static void
write_u32(FILE *f, uint32 v)
{
  fputc(v & 0xff, f);
  fputc((v >> 8) & 0xff, f);
  fputc((v >> 16) & 0xff, f);
  fputc((v >> 24) & 0xff, f);
}

static void
write_pcap_header(FILE *f)
{
  write_u32(f, 0xa1b2c3d4);   // magic, little endian
  fputc(2, f); fputc(0, f);   // version 2
  fputc(4, f); fputc(0, f);   // .4
  write_u32(f, 0);            // no time zone correction
  write_u32(f, 0);            // no accuracy figure
  write_u32(f, (uint32)snaplen);
  write_u32(f, 1);            // ethernet
}

static uint32
read_u32(FILE *f, int *ok)
{
  int a = fgetc(f), b = fgetc(f), c = fgetc(f), d = fgetc(f);

  if (d == EOF) {
    *ok = 0;
    return 0;
  }
  return (uint32)a | ((uint32)b << 8) | ((uint32)c << 16) | ((uint32)d << 24);
}

static void
on_signal(int sig)
{
  (void)sig;
  stop = 1;
}

static void
usage(void)
{
  fprintf(stderr, "usage: tcpdump [-i IFACE] [-n] [-c COUNT] [-s LEN] [-qexX]"
                  " [-t] [-w FILE] [-r FILE] [EXPRESSION]\n");
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
      case 'q': brief = 1; break;
      case 'e': show_eth = 1; break;
      case 'x': hex_dump = 1; break;
      case 'X': hex_and_text = 1; break;
      case 't': no_time = 1; break;
      case 'l': case 'U': case 'v': break;
      case 'i': case 'c': case 's': case 'w': case 'r': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          usage();
        switch (*p) {
        case 'i': break;   // there is one interface, and lo is not captured
        case 'c': count_limit = atoi(v); break;
        case 's': snaplen = atoi(v) > 0 ? atoi(v) : SNAP; break;
        case 'w': write_file = v; break;
        case 'r': read_file = v; break;
        }
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }
  (void)numeric;
  (void)brief;

  words = argv + i;
  nwords = argc - i;
  at = 0;
  struct expr *filter = parse_or();

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  FILE *out = 0;
  if (write_file) {
    out = strcmp(write_file, "-") == 0 ? stdout : fopen(write_file, "w");
    if (out == 0) {
      fprintf(stderr, "tcpdump: %s: %s\n", write_file, strerror(errno));
      exit(1);
    }
    write_pcap_header(out);
  }

  FILE *in;
  if (read_file) {
    in = strcmp(read_file, "-") == 0 ? stdin : fopen(read_file, "r");
    if (in == 0) {
      fprintf(stderr, "tcpdump: %s: %s\n", read_file, strerror(errno));
      exit(1);
    }
    int ok = 1;
    uint32 magic = read_u32(in, &ok);
    if (!ok || (magic != 0xa1b2c3d4 && magic != 0xd4c3b2a1)) {
      fprintf(stderr, "tcpdump: %s is not a pcap file\n", read_file);
      exit(1);
    }
    for (int k = 0; k < 5; k++)
      read_u32(in, &ok);   // version, zone, accuracy, snaplen, link type
  } else {
    in = fopen("/dev/pktcap", "r");
    if (in == 0) {
      fprintf(stderr, "tcpdump: /dev/pktcap: %s\n", strerror(errno));
      if (errno == EACCES)
        fprintf(stderr, "tcpdump: only root may watch the wire\n");
      exit(1);
    }
    if (!write_file)
      fprintf(stderr, "tcpdump: listening on eth0, link-type EN10MB,"
                      " capture size %d bytes\n", snaplen);
  }

  static unsigned char buf[SNAP];
  while (!stop) {
    int ok = 1;
    uint32 secs = read_u32(in, &ok);
    uint32 usecs = read_u32(in, &ok);
    uint32 caplen = read_u32(in, &ok);
    uint32 len = read_u32(in, &ok);
    if (!ok)
      break;
    if (caplen > sizeof(buf))
      break;
    if (fread(buf, 1, caplen, in) != caplen)
      break;
    captured++;

    struct packet k;
    look(buf, (int)caplen, &k);
    if (!matches(filter, &k))
      continue;

    shown++;
    if (out) {
      uint32 keep = caplen > (uint32)snaplen ? (uint32)snaplen : caplen;
      write_u32(out, secs);
      write_u32(out, usecs);
      write_u32(out, keep);
      write_u32(out, len);
      fwrite(buf, 1, keep, out);
      fflush(out);
    } else {
      print_packet(secs, usecs, buf, (int)len, (int)caplen);
      fflush(stdout);
    }
    if (count_limit && shown >= count_limit)
      break;
  }

  if (out && out != stdout)
    fclose(out);
  if (in != stdin)
    fclose(in);
  if (!read_file)
    fprintf(stderr, "\n%lld packets captured\n%lld packets shown\n", captured,
            shown);
  exit(0);
}
