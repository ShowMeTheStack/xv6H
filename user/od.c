// od, hexdump and xxd: bytes, in a form a person can read.
//
//   od [-bcdosvx] [-A a|d|o|x|n] [-j SKIP] [-N COUNT] [-t TYPE...] [FILE...]
//   hexdump [-C] [-n LENGTH] [-s SKIP] [FILE...]
//   xxd [-l LENGTH] [-s SKIP] [FILE]
//
// od's shape is POSIX's: -t says how to read the bytes and how wide
// each reading is, and more than one -t prints more than one line for
// the same sixteen bytes. hexdump -C and xxd print the two forms
// people actually type, which are hex bytes with the text beside them.
//
// a run of identical lines is printed once with a `*` under it, as
// every od does; -v prints them all.
#include "user/user.h"

#define PER_LINE 16
#define MAXSPEC 8

enum { A_OCT, A_DEC, A_HEX, A_NONE };
enum { T_OCTAL, T_HEX, T_DEC, T_UDEC, T_CHAR, T_NAMED, T_FLOAT };

struct spec {
  int type;
  int size;   // bytes per item
};

static int addr_base = A_OCT;
static struct spec specs[MAXSPEC];
static int nspec;
static int show_all;      // -v
static int mode_canon;    // hexdump -C
static int mode_xxd;
static const char *me = "od";

// the names od -a gives the bytes that have them
static const char *named[] = {
  "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
  "bs",  "ht",  "nl",  "vt",  "ff",  "cr",  "so",  "si",
  "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
  "can", "em",  "sub", "esc", "fs",  "gs",  "rs",  "us",
  "sp"
};

static void
print_address(long long off, int trailing)
{
  switch (addr_base) {
  case A_OCT: printf("%07llo", off); break;
  case A_DEC: printf("%07lld", off); break;
  case A_HEX: printf("%06llx", off); break;
  case A_NONE:
    return; // -A n prints no address, and no final one either
  }
  if (trailing)
    printf("\n");
}

// the bytes of one item, least significant first, as this machine
// stores them
static uint64_t
item(const unsigned char *p, int size)
{
  uint64_t v = 0;

  for (int i = size - 1; i >= 0; i--)
    v = (v << 8) | p[i];
  return v;
}

static void
print_char(int c)
{
  switch (c) {
  case '\0': printf(" \\0"); return;
  case '\a': printf(" \\a"); return;
  case '\b': printf(" \\b"); return;
  case '\f': printf(" \\f"); return;
  case '\n': printf(" \\n"); return;
  case '\r': printf(" \\r"); return;
  case '\t': printf(" \\t"); return;
  case '\v': printf(" \\v"); return;
  }
  if (c >= 0x20 && c < 0x7f)
    printf("  %c", c);
  else
    printf("%03o", c);
}

static void
print_line(const struct spec *s, const unsigned char *buf, int n)
{
  for (int i = 0; i < n || (i == 0 && n == 0); ) {
    if (i >= n)
      break;
    int size = s->size;
    if (i + size > n)
      size = n - i;   // a short tail at the end of the file
    switch (s->type) {
    case T_OCTAL:
      printf(" %0*llo", size * 8 / 3 + 1, (unsigned long long)item(buf + i, size));
      break;
    case T_HEX:
      printf(" %0*llx", size * 2, (unsigned long long)item(buf + i, size));
      break;
    case T_UDEC:
      printf(" %*llu", size * 5 / 2 + 1, (unsigned long long)item(buf + i, size));
      break;
    case T_DEC: {
      uint64_t v = item(buf + i, size);
      long long sv;
      if (size == 1) sv = (long long)(signed char)v;
      else if (size == 2) sv = (long long)(short)v;
      else if (size == 4) sv = (long long)(int)v;
      else sv = (long long)v;
      printf(" %*lld", size * 5 / 2 + 1, sv);
      break;
    }
    case T_CHAR:
      printf(" ");
      print_char(buf[i]);
      size = 1;
      break;
    case T_NAMED: {
      int c = buf[i] & 0x7f;
      if (c <= 0x20)
        printf(" %3s", named[c]);
      else if (c == 0x7f)
        printf(" %3s", "del");
      else
        printf("   %c", c);
      size = 1;
      break;
    }
    case T_FLOAT: {
      uint64_t v = item(buf + i, size);
      if (size == 4) {
        float fv;
        memmove(&fv, &v, 4);
        printf(" %14.7e", (double)fv);
      } else {
        double dv;
        memmove(&dv, &v, 8);
        printf(" %21.14e", dv);
      }
      break;
    }
    }
    i += size;
  }
  printf("\n");
}

static void
print_canon(long long off, const unsigned char *buf, int n)
{
  printf("%08llx  ", off);
  for (int i = 0; i < PER_LINE; i++) {
    if (i < n)
      printf("%02x ", buf[i]);
    else
      printf("   ");
    if (i == 7)
      printf(" ");
  }
  printf(" |");
  for (int i = 0; i < n; i++)
    putchar(buf[i] >= 0x20 && buf[i] < 0x7f ? buf[i] : '.');
  printf("|\n");
}

static void
print_xxd(long long off, const unsigned char *buf, int n)
{
  printf("%08llx: ", off);
  for (int i = 0; i < PER_LINE; i += 2) {
    if (i < n)
      printf("%02x", buf[i]);
    else
      printf("  ");
    if (i + 1 < n)
      printf("%02x ", buf[i + 1]);
    else
      printf("   ");
  }
  printf(" ");
  for (int i = 0; i < n; i++)
    putchar(buf[i] >= 0x20 && buf[i] < 0x7f ? buf[i] : '.');
  printf("\n");
}

// od reads its files as if they were one stream, so the state of a
// dump outlives any single file
struct dumper {
  long long offset;
  long long skip;      // bytes still to be thrown away
  long long left;      // bytes still to be printed, or -1 for all
  unsigned char line[PER_LINE];
  int n;               // bytes gathered for the current line
  unsigned char last[PER_LINE];
  int had_last;
  int starring;        // a `*` has been printed for the current run
};

static void
flush_line(struct dumper *d)
{
  if (d->n == 0)
    return;
  if (!show_all && d->had_last && d->n == PER_LINE &&
      memcmp(d->line, d->last, PER_LINE) == 0) {
    if (!d->starring) {
      printf("*\n");
      d->starring = 1;
    }
    d->offset += d->n;
    d->n = 0;
    return;
  }
  d->starring = 0;
  if (mode_canon)
    print_canon(d->offset, d->line, d->n);
  else if (mode_xxd)
    print_xxd(d->offset, d->line, d->n);
  else
    for (int s = 0; s < nspec; s++) {
      if (s == 0)
        print_address(d->offset, 0);
      else if (addr_base != A_NONE)
        printf("       ");
      print_line(&specs[s], d->line, d->n);
    }
  memmove(d->last, d->line, PER_LINE);
  d->had_last = (d->n == PER_LINE);
  d->offset += d->n;
  d->n = 0;
}

static void
dump(struct dumper *d, FILE *f)
{
  int c;

  while ((c = fgetc(f)) != EOF) {
    if (d->skip > 0) {
      d->skip--;
      continue;
    }
    if (d->left == 0)
      return;
    d->line[d->n++] = (unsigned char)c;
    if (d->left > 0)
      d->left--;
    if (d->n == PER_LINE)
      flush_line(d);
  }
}

static int
parse_type(const char *p)
{
  // -t takes one or more TYPE[SIZE] groups, e.g. "x1" or "c" or "d4"
  while (*p) {
    if (nspec >= MAXSPEC) {
      fprintf(stderr, "%s: too many -t formats\n", me);
      return -1;
    }
    struct spec *s = &specs[nspec];
    switch (*p++) {
    case 'o': s->type = T_OCTAL; s->size = 4; break;
    case 'x': s->type = T_HEX; s->size = 4; break;
    case 'd': s->type = T_DEC; s->size = 4; break;
    case 'u': s->type = T_UDEC; s->size = 4; break;
    case 'f': s->type = T_FLOAT; s->size = 8; break;
    case 'c': s->type = T_CHAR; s->size = 1; break;
    case 'a': s->type = T_NAMED; s->size = 1; break;
    default:
      fprintf(stderr, "%s: unknown format '%c'\n", me, p[-1]);
      return -1;
    }
    if (*p == 'C') { s->size = 1; p++; }
    else if (*p == 'S') { s->size = 2; p++; }
    else if (*p == 'I' || *p == 'L') { s->size = 4; p++; }
    else if (*p >= '1' && *p <= '9') {
      s->size = 0;
      while (*p >= '0' && *p <= '9')
        s->size = s->size * 10 + (*p++ - '0');
      if (s->size != 1 && s->size != 2 && s->size != 4 && s->size != 8) {
        fprintf(stderr, "%s: bad size %d\n", me, s->size);
        return -1;
      }
    }
    if (s->type == T_CHAR || s->type == T_NAMED)
      s->size = 1;
    if (s->type == T_FLOAT && s->size != 4 && s->size != 8)
      s->size = 8;
    nspec++;
  }
  return 0;
}

// a count may be written in hex or with a b/k/m suffix
static long long
parse_count(const char *p)
{
  long long v = 0;
  int base = 10;

  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
  } else if (p[0] == '0' && p[1]) {
    base = 8;
    p++;
  }
  while (*p) {
    int d;
    if (*p >= '0' && *p <= '9')
      d = *p - '0';
    else if (base == 16 && *p >= 'a' && *p <= 'f')
      d = *p - 'a' + 10;
    else if (base == 16 && *p >= 'A' && *p <= 'F')
      d = *p - 'A' + 10;
    else
      break;
    v = v * base + d;
    p++;
  }
  if (*p == 'b')
    v *= 512;
  else if (*p == 'k' || *p == 'K')
    v *= 1024;
  else if (*p == 'm' || *p == 'M')
    v *= 1024 * 1024;
  return v;
}

static void
usage(void)
{
  fprintf(stderr,
          "usage: %s [-bcdosvx] [-A a|d|o|x|n] [-j SKIP] [-N COUNT]"
          " [-t TYPE] [FILE...]\n", me);
  exit(1);
}

int
main(int argc, char *argv[])
{
  struct dumper d;
  long long skip = 0, count = -1;
  int i = 1;

  me = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  if (strcmp(me, "hexdump") == 0)
    mode_canon = 1;
  else if (strcmp(me, "xxd") == 0)
    mode_xxd = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    // the options that take a value, whether joined or separate
    if (a[1] == 'A' || a[1] == 'j' || a[1] == 'N' || a[1] == 't' ||
        a[1] == 'n' || a[1] == 'l' || a[1] == 's') {
      const char *val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
      if (val == 0)
        usage();
      switch (a[1]) {
      case 'A':
        addr_base = *val == 'd' ? A_DEC : *val == 'x' ? A_HEX
                    : *val == 'n' ? A_NONE : A_OCT;
        break;
      case 'j':
      case 's':
        skip = parse_count(val);
        break;
      case 'N':
      case 'n':
      case 'l':
        count = parse_count(val);
        break;
      case 't':
        if (parse_type(val) < 0)
          exit(1);
        break;
      }
      continue;
    }
    for (const char *p = a + 1; *p; p++) {
      switch (*p) {
      case 'b': parse_type("o1"); break;
      case 'c': parse_type("c"); break;
      case 'd': parse_type("u2"); break;
      case 'o': parse_type("o2"); break;
      case 'x': parse_type("x2"); break;
      case 'a': parse_type("a"); break;
      case 'v': show_all = 1; break;
      case 'C': mode_canon = 1; break;
      default: usage();
      }
    }
  }
  if (nspec == 0 && !mode_canon && !mode_xxd)
    parse_type("o2"); // what od prints when nobody says otherwise

  memset(&d, 0, sizeof(d));
  d.skip = skip;
  d.left = count;
  // a skipped byte still counts towards the address, as it does in od,
  // so the first line printed is labelled with where it really is
  d.offset = skip;

  int status = 0;
  if (i >= argc) {
    dump(&d, stdin);
  } else {
    for (; i < argc; i++) {
      FILE *f = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
      if (f == 0) {
        fprintf(stderr, "%s: %s: %s\n", me, argv[i], strerror(errno));
        status = 1;
        continue;
      }
      dump(&d, f);
      if (f != stdin)
        fclose(f);
    }
  }
  flush_line(&d);
  if (!mode_canon && !mode_xxd)
    print_address(d.offset, 1);
  exit(status);
}
