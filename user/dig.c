// dig and host: ask a name server a question.
//
//   dig [@SERVER] NAME [TYPE] [+short] [+noall] [+answer] [-x ADDR] [-p PORT]
//   host [-t TYPE] NAME [SERVER]
//
// the types this knows are A, AAAA, CNAME, MX, TXT, NS, PTR and SOA.
// -x asks the reverse question: the name of an address.
//
// the query is built and the answer taken apart here rather than in
// the library, because the library's resolver only ever wants an
// address and dig is the program for everything else.
#include "user/user.h"
#include "kernel/net.h"
#include "kernel/socket.h"

#define T_A     1
#define T_NS    2
#define T_CNAME 5
#define T_SOA   6
#define T_PTR   12
#define T_MX    15
#define T_TXT   16
#define T_AAAA  28
#define T_ANY   255

static int short_form, is_host, answer_only;
static uint32 server;
static int port = 53;

static const struct {
  const char *name;
  int type;
} types[] = {
  {"A", T_A}, {"NS", T_NS}, {"CNAME", T_CNAME}, {"SOA", T_SOA},
  {"PTR", T_PTR}, {"MX", T_MX}, {"TXT", T_TXT}, {"AAAA", T_AAAA},
  {"ANY", T_ANY}, {0, 0}
};

static const char *
type_name(int t)
{
  for (int i = 0; types[i].name; i++)
    if (types[i].type == t)
      return types[i].name;
  return "TYPE?";
}

static int
type_of(const char *s)
{
  for (int i = 0; types[i].name; i++)
    if (strcasecmp(s, types[i].name) == 0)
      return types[i].type;
  return -1;
}

// a name into the wire form: each label with its length in front
static int
put_name(unsigned char *q, int n, const char *name)
{
  const char *p = name;

  while (*p) {
    const char *dot = p;
    while (*dot && *dot != '.')
      dot++;
    int len = dot - p;
    if (len == 0 || len > 63 || n + len + 2 > 400)
      return -1;
    q[n++] = (unsigned char)len;
    memmove(q + n, p, len);
    n += len;
    p = *dot ? dot + 1 : dot;
  }
  q[n++] = 0;
  return n;
}

// a name out of the wire form, following the compression pointers.
// returns how many bytes of the message the name itself took.
static int
get_name(const unsigned char *r, int len, int at, char *out, int size)
{
  int n = 0, used = 0, hops = 0, i = at;

  out[0] = 0;
  for (;;) {
    if (i >= len)
      return -1;
    unsigned c = r[i];
    if ((c & 0xc0) == 0xc0) {
      if (i + 1 >= len)
        return -1;
      if (used == 0)
        used = i + 2 - at;
      i = ((c & 0x3f) << 8) | r[i + 1];
      if (++hops > 20)
        return -1;   // a pointer loop
      continue;
    }
    if (c == 0) {
      i++;
      if (used == 0)
        used = i - at;
      break;
    }
    if (i + 1 + (int)c > len)
      return -1;
    if (n > 0 && n < size - 1)
      out[n++] = '.';
    for (unsigned k = 0; k < c; k++)
      if (n < size - 1)
        out[n++] = r[i + 1 + k];
    i += c + 1;
  }
  out[n] = 0;
  return used;
}

static void
print_ip(const unsigned char *b)
{
  printf("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void
print_ipv6(const unsigned char *b)
{
  for (int i = 0; i < 16; i += 2) {
    printf("%x", (b[i] << 8) | b[i + 1]);
    if (i < 14)
      putchar(':');
  }
}

// the address written backwards under in-addr.arpa, which is how a
// reverse question is asked
static void
reverse_name(uint32 ip, char *out, int size)
{
  snprintf(out, size, "%u.%u.%u.%u.in-addr.arpa", ip & 0xff, (ip >> 8) & 0xff,
           (ip >> 16) & 0xff, (ip >> 24) & 0xff);
}

static int
ask(const char *name, int qtype)
{
  unsigned char q[512], r[2048];
  struct netinfo ni;
  int n = 0;
  static int seq = 1;

  if (server == 0) {
    if (netinfo(&ni) < 0 || ni.dns == 0) {
      fprintf(stderr, "dig: no name server is configured\n");
      return 1;
    }
    server = ni.dns;
  }

  int xid = (seq++ & 0x7fff) | 0x2000;
  q[n++] = xid >> 8;
  q[n++] = xid & 0xff;
  q[n++] = 1;           // recursion desired
  q[n++] = 0;
  q[n++] = 0; q[n++] = 1;   // one question
  q[n++] = 0; q[n++] = 0;   // no answers
  q[n++] = 0; q[n++] = 0;   // no authority
  q[n++] = 0; q[n++] = 0;   // no additional
  n = put_name(q, n, name);
  if (n < 0) {
    fprintf(stderr, "dig: %s is not a name that can be asked about\n", name);
    return 1;
  }
  q[n++] = qtype >> 8;
  q[n++] = qtype & 0xff;
  q[n++] = 0;
  q[n++] = 1;           // class IN

  int fd = socket(SOCK_DGRAM);
  if (fd < 0) {
    fprintf(stderr, "dig: socket: %s\n", strerror(errno));
    return 1;
  }
  int len = -1;
  for (int attempt = 0; attempt < 3 && len < 0; attempt++) {
    if (sendto(fd, q, n, server, port) < 0) {
      fprintf(stderr, "dig: sendto: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    uint32 src;
    int sport;
    int got = recvfrom(fd, r, sizeof(r), &src, &sport, 30);
    if (got < 12 || r[0] != (xid >> 8) || r[1] != (xid & 0xff))
      continue;
    len = got;
  }
  close(fd);
  if (len < 0) {
    if (!short_form)
      printf(";; no answer from the server\n");
    return 1;
  }

  int rcode = r[3] & 15;
  int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
  int ns = (r[8] << 8) | r[9], ar = (r[10] << 8) | r[11];
  char nm[256];

  if (!short_form && !answer_only && !is_host) {
    static const char *rcodes[] = {"NOERROR", "FORMERR", "SERVFAIL",
                                   "NXDOMAIN", "NOTIMP", "REFUSED"};
    char s[20];
    printf(";; ->>HEADER<<- opcode: QUERY, status: %s, id: %d\n",
           rcode < 6 ? rcodes[rcode] : "?", xid);
    printf(";; flags:%s%s; QUERY: %d, ANSWER: %d, AUTHORITY: %d, "
           "ADDITIONAL: %d\n\n",
           (r[2] & 0x80) ? " qr" : "", (r[3] & 0x80) ? " ra" : "", qd, an, ns,
           ar);
    printf(";; QUESTION SECTION:\n;%s.\t\tIN\t%s\n\n", name, type_name(qtype));
    printf(";; SERVER: %s#%d\n\n", ip2str(server, s), port);
  }

  int i = 12;
  for (int k = 0; k < qd; k++) {
    int used = get_name(r, len, i, nm, sizeof(nm));
    if (used < 0)
      return 1;
    i += used + 4;
  }

  if (rcode != 0) {
    if (is_host)
      printf("Host %s not found: %d\n", name, rcode);
    else if (!short_form)
      printf(";; the server answered with an error\n");
    return 1;
  }
  if (an == 0) {
    if (is_host)
      printf("%s has no %s record\n", name, type_name(qtype));
    else if (!short_form)
      printf(";; ANSWER SECTION: (none)\n");
    return 1;
  }

  if (!short_form && !is_host)
    printf(";; ANSWER SECTION:\n");

  for (int k = 0; k < an && i + 10 <= len; k++) {
    int used = get_name(r, len, i, nm, sizeof(nm));
    if (used < 0)
      break;
    i += used;
    int type = (r[i] << 8) | r[i + 1];
    long ttl = ((long)r[i + 4] << 24) | (r[i + 5] << 16) | (r[i + 6] << 8) |
               r[i + 7];
    int rdlen = (r[i + 8] << 8) | r[i + 9];
    i += 10;
    if (i + rdlen > len)
      break;

    if (!short_form && !is_host)
      printf("%s.\t%ld\tIN\t%s\t", nm, ttl, type_name(type));

    switch (type) {
    case T_A:
      if (rdlen == 4) {
        if (is_host)
          printf("%s has address ", nm);
        print_ip(r + i);
        printf("\n");
      }
      break;
    case T_AAAA:
      if (rdlen == 16) {
        if (is_host)
          printf("%s has IPv6 address ", nm);
        print_ipv6(r + i);
        printf("\n");
      }
      break;
    case T_CNAME:
    case T_NS:
    case T_PTR: {
      char target[256];
      if (get_name(r, len, i, target, sizeof(target)) < 0)
        break;
      if (is_host)
        printf("%s %s %s\n", nm,
               type == T_PTR ? "domain name pointer"
                             : (type == T_NS ? "name server" : "is an alias for"),
               target);
      else
        printf("%s.\n", target);
      break;
    }
    case T_MX: {
      char target[256];
      int pref = (r[i] << 8) | r[i + 1];
      if (get_name(r, len, i + 2, target, sizeof(target)) < 0)
        break;
      if (is_host)
        printf("%s mail is handled by %d %s\n", nm, pref, target);
      else
        printf("%d %s.\n", pref, target);
      break;
    }
    case T_TXT: {
      // one or more counted strings
      int at = i, end = i + rdlen;
      printf("\"");
      while (at < end) {
        int slen = r[at++];
        for (int c = 0; c < slen && at < end; c++)
          putchar(r[at++]);
      }
      printf("\"\n");
      break;
    }
    case T_SOA: {
      char mname[256], rname[256];
      int u1 = get_name(r, len, i, mname, sizeof(mname));
      if (u1 < 0)
        break;
      int u2 = get_name(r, len, i + u1, rname, sizeof(rname));
      if (u2 < 0)
        break;
      const unsigned char *v = r + i + u1 + u2;
      printf("%s. %s. %u %u %u %u %u\n", mname, rname,
             (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3],
             (v[4] << 24) | (v[5] << 16) | (v[6] << 8) | v[7],
             (v[8] << 24) | (v[9] << 16) | (v[10] << 8) | v[11],
             (v[12] << 24) | (v[13] << 16) | (v[14] << 8) | v[15],
             (v[16] << 24) | (v[17] << 16) | (v[18] << 8) | v[19]);
      break;
    }
    default:
      printf("(%d bytes)\n", rdlen);
      break;
    }
    i += rdlen;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  const char *name = 0;
  int qtype = -1, i = 1;
  char revbuf[80];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  is_host = strcmp(me, "host") == 0;

  for (; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '@') {
      if (parse_ip(a + 1, &server) < 0 && dns_resolve(a + 1, &server) < 0) {
        fprintf(stderr, "%s: cannot find the server %s\n", me, a + 1);
        exit(1);
      }
      continue;
    }
    if (a[0] == '+') {
      if (strcmp(a, "+short") == 0)
        short_form = 1;
      else if (strcmp(a, "+noall") == 0)
        answer_only = 1;
      else if (strcmp(a, "+answer") == 0)
        answer_only = 1;
      continue;
    }
    if (strcmp(a, "-x") == 0 && i + 1 < argc) {
      uint32 ip;
      if (parse_ip(argv[++i], &ip) < 0) {
        fprintf(stderr, "%s: %s is not an address\n", me, argv[i]);
        exit(1);
      }
      reverse_name(ip, revbuf, sizeof(revbuf));
      name = revbuf;
      qtype = T_PTR;
      continue;
    }
    if (strcmp(a, "-t") == 0 && i + 1 < argc) {
      qtype = type_of(argv[++i]);
      if (qtype < 0) {
        fprintf(stderr, "%s: unknown type %s\n", me, argv[i]);
        exit(1);
      }
      continue;
    }
    if (strcmp(a, "-p") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
      continue;
    }
    if (a[0] == '-' && a[1]) {
      fprintf(stderr, "usage: %s [@SERVER] NAME [TYPE] [+short] [-x ADDR]\n", me);
      exit(1);
    }
    if (name == 0) {
      name = a;
      continue;
    }
    int t = type_of(a);
    if (t >= 0 && qtype < 0) {
      qtype = t;
      continue;
    }
    // host NAME SERVER: the second name is the server to ask
    if (is_host && parse_ip(a, &server) < 0)
      dns_resolve(a, &server);
  }

  if (name == 0) {
    fprintf(stderr, "usage: %s [@SERVER] NAME [TYPE] [+short] [-x ADDR]\n", me);
    exit(1);
  }
  if (qtype < 0) {
    // host with a plain address asks the reverse question, as it does
    uint32 ip;
    if (is_host && parse_ip(name, &ip) == 0) {
      reverse_name(ip, revbuf, sizeof(revbuf));
      name = revbuf;
      qtype = T_PTR;
    } else {
      qtype = T_A;
    }
  }
  exit(ask(name, qtype));
}
