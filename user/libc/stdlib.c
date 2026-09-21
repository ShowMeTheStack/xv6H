//
// <stdlib.h>, apart from the allocator (malloc.c), the floating point
// conversions (strtod.c) and the environment (env.c).
//
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/random.h>
#include "libc.h"

// ---- integer conversion ----

// the shared core: skips space, takes a sign, works out the base when
// it was given as zero, and accumulates with an overflow check.
static unsigned long
strtox(const char *s, char **end, int base, int *neg, int *overflow, unsigned long limit)
{
  const char *start = s;
  unsigned long v = 0;
  int any = 0;

  *neg = 0;
  *overflow = 0;
  while (isspace((unsigned char)*s))
    s++;
  if (*s == '+' || *s == '-')
    *neg = (*s++ == '-');
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
    s += 2;
    base = 16;
  } else if (base == 0) {
    base = (*s == '0') ? 8 : 10;
  }
  if (base == 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && (s[2] == '0' || s[2] == '1'))
    s += 2;

  unsigned long cutoff = limit / (unsigned long)base;
  int cutlim = (int)(limit % (unsigned long)base);
  for (;; s++) {
    int c = (unsigned char)*s, d;
    if (isdigit(c))
      d = c - '0';
    else if (isalpha(c))
      d = tolower(c) - 'a' + 10;
    else
      break;
    if (d >= base)
      break;
    any = 1;
    if (v > cutoff || (v == cutoff && d > cutlim))
      *overflow = 1;
    else
      v = v * (unsigned long)base + (unsigned long)d;
  }
  if (end)
    *end = (char *)(any ? s : start);
  if (*overflow)
    return limit;
  return v;
}

long
strtol(const char *s, char **end, int base)
{
  int neg, over;
  // the accumulation is unsigned; the range check needs the sign,
  // which strtox reports, so it happens here instead.
  unsigned long v = strtox(s, end, base, &neg, &over, ULONG_MAX);
  unsigned long limit = neg ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
  if (over || v > limit) {
    errno = ERANGE;
    return neg ? LONG_MIN : LONG_MAX;
  }
  return neg ? -(long)v : (long)v;
}

long long
strtoll(const char *s, char **end, int base)
{
  return strtol(s, end, base);
}

unsigned long
strtoul(const char *s, char **end, int base)
{
  int neg, over;
  unsigned long v = strtox(s, end, base, &neg, &over, ULONG_MAX);

  if (over) {
    errno = ERANGE;
    return ULONG_MAX;
  }
  return neg ? -v : v;
}

unsigned long long
strtoull(const char *s, char **end, int base)
{
  return strtoul(s, end, base);
}

long
strtoimax(const char *s, char **end, int base)
{
  return strtol(s, end, base);
}

unsigned long
strtoumax(const char *s, char **end, int base)
{
  return strtoul(s, end, base);
}

int
atoi(const char *s)
{
  return (int)strtol(s, 0, 10);
}

long
atol(const char *s)
{
  return strtol(s, 0, 10);
}

long long
atoll(const char *s)
{
  return strtoll(s, 0, 10);
}

// ---- arithmetic ----

int
abs(int v)
{
  return v < 0 ? -v : v;
}

long
labs(long v)
{
  return v < 0 ? -v : v;
}

long long
llabs(long long v)
{
  return v < 0 ? -v : v;
}

long
imaxabs(long v)
{
  return v < 0 ? -v : v;
}

div_t
div(int a, int b)
{
  div_t r = {a / b, a % b};
  return r;
}

ldiv_t
ldiv(long a, long b)
{
  ldiv_t r = {a / b, a % b};
  return r;
}

lldiv_t
lldiv(long long a, long long b)
{
  lldiv_t r = {a / b, a % b};
  return r;
}

// ---- searching and sorting ----

void *
bsearch(const void *key, const void *base, size_t n, size_t size,
        int (*cmp)(const void *, const void *))
{
  const char *b = base;

  while (n) {
    size_t mid = n / 2;
    const char *p = b + mid * size;
    int c = cmp(key, p);
    if (c == 0)
      return (void *)p;
    if (c > 0) {
      b = p + size;
      n -= mid + 1;
    } else {
      n = mid;
    }
  }
  return 0;
}

static void
swapbytes(char *a, char *b, size_t n)
{
  while (n--) {
    char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

// heapsort: qsort must not be quadratic on adversarial input and must
// not allocate, and a sift-down heap gives both with n log n worst case.
static void
siftdown(char *base, size_t size, size_t root, size_t n, int (*cmp)(const void *, const void *, void *),
         void *arg)
{
  for (;;) {
    size_t child = 2 * root + 1;
    if (child >= n)
      return;
    if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size, arg) < 0)
      child++;
    if (cmp(base + root * size, base + child * size, arg) >= 0)
      return;
    swapbytes(base + root * size, base + child * size, size);
    root = child;
  }
}

void
qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg)
{
  char *b = base;

  if (n < 2 || size == 0)
    return;
  for (size_t i = n / 2; i-- > 0;)
    siftdown(b, size, i, n, cmp, arg);
  for (size_t i = n; i-- > 1;) {
    swapbytes(b, b + i * size, size);
    siftdown(b, size, 0, i, cmp, arg);
  }
}

static int
cmp_noarg(const void *a, const void *b, void *arg)
{
  int (*cmp)(const void *, const void *) = arg;
  return cmp(a, b);
}

void
qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
  qsort_r(base, n, size, cmp_noarg, (void *)cmp);
}

// ---- pseudo-random numbers ----

// the same additive generator glibc uses for random(), TYPE_3: a
// trinomial x^31 + x^3 + 1 over a 34-entry table. rand() is the same
// sequence, which is far better than the ANSI-C linear congruence.
static uint32_t rtab[34];
static int rfront, rrear;
static int rinit;

void
srandom(unsigned seed)
{
  if (seed == 0)
    seed = 1;
  rtab[0] = seed;
  for (int i = 1; i < 31; i++) {
    // the recurrence is r[i] = (16807 * r[i-1]) % 2147483647, computed
    // without overflow by splitting the multiplication (Schrage).
    long hi = rtab[i - 1] / 127773;
    long lo = rtab[i - 1] % 127773;
    long word = 16807 * lo - 2836 * hi;
    if (word < 0)
      word += 2147483647;
    rtab[i] = word;
  }
  for (int i = 31; i < 34; i++)
    rtab[i] = rtab[i - 31];
  rfront = 2;
  rrear = 0;
  rinit = 1;
  for (int i = 0; i < 310; i++)
    random();
}

long
random(void)
{
  if (!rinit)
    srandom(1);
  rtab[rfront] += rtab[rrear];
  uint32_t v = (rtab[rfront] >> 1) & 0x7fffffff;
  if (++rfront >= 34)
    rfront = 0;
  if (++rrear >= 34)
    rrear = 0;
  return v;
}

char *
initstate(unsigned seed, char *state, size_t n)
{
  (void)n;
  srandom(seed);
  return state;
}

char *
setstate(char *state)
{
  return state;
}

int
rand(void)
{
  return (int)random();
}

void
srand(unsigned seed)
{
  srandom(seed);
}

int
rand_r(unsigned *seed)
{
  // the POSIX example generator, which needs no shared state.
  *seed = *seed * 1103515245u + 12345u;
  return (int)((*seed >> 16) & 0x7fff);
}

// the 48-bit linear congruence of drand48 and its relatives.
static unsigned short x48[3] = {0x330e, 0xabcd, 0x1234};
static unsigned short a48[3] = {0xe66d, 0xdeec, 0x0005};
static unsigned short c48 = 0xb;

static uint64_t
next48(unsigned short s[3])
{
  uint64_t x = (uint64_t)s[0] | ((uint64_t)s[1] << 16) | ((uint64_t)s[2] << 32);
  uint64_t a = (uint64_t)a48[0] | ((uint64_t)a48[1] << 16) | ((uint64_t)a48[2] << 32);

  x = (a * x + c48) & 0xffffffffffffUL;
  s[0] = x & 0xffff;
  s[1] = (x >> 16) & 0xffff;
  s[2] = (x >> 32) & 0xffff;
  return x;
}

double
erand48(unsigned short s[3])
{
  return (double)next48(s) / 281474976710656.0; // 2^48
}

double
drand48(void)
{
  return erand48(x48);
}

long
nrand48(unsigned short s[3])
{
  return (long)(next48(s) >> 17);
}

long
lrand48(void)
{
  return nrand48(x48);
}

long
jrand48(unsigned short s[3])
{
  return (long)(int32_t)(next48(s) >> 16);
}

long
mrand48(void)
{
  return jrand48(x48);
}

void
srand48(long seed)
{
  x48[0] = 0x330e;
  x48[1] = seed & 0xffff;
  x48[2] = (seed >> 16) & 0xffff;
}

unsigned short *
seed48(unsigned short s[3])
{
  static unsigned short old[3];
  memcpy(old, x48, sizeof(old));
  memcpy(x48, s, sizeof(x48));
  return old;
}

void
lcong48(unsigned short p[7])
{
  memcpy(x48, p, sizeof(x48));
  memcpy(a48, p + 3, sizeof(a48));
  c48 = p[6];
}

// ---- leaving the program ----

// the handlers atexit() registered, run in reverse order. the table
// holds the 32 the standard asks for; more than that is refused, which
// is what a program is told to expect.
#define MAXATEXIT 32
static struct {
  void (*fn)(void *);
  void *arg;
  int cxa;
} atexits[MAXATEXIT];
static int natexit;
static void (*quickexits[MAXATEXIT])(void);
static int nquickexit;

int
atexit(void (*fn)(void))
{
  if (natexit >= MAXATEXIT)
    return -1;
  atexits[natexit].fn = (void (*)(void *))fn;
  atexits[natexit].arg = 0;
  atexits[natexit].cxa = 0;
  natexit++;
  return 0;
}

int
__cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
  (void)dso;
  if (natexit >= MAXATEXIT)
    return -1;
  atexits[natexit].fn = fn;
  atexits[natexit].arg = arg;
  atexits[natexit].cxa = 1;
  natexit++;
  return 0;
}

int
at_quick_exit(void (*fn)(void))
{
  if (nquickexit >= MAXATEXIT)
    return -1;
  quickexits[nquickexit++] = fn;
  return 0;
}

void
exit(int status)
{
  while (natexit > 0) {
    natexit--;
    if (atexits[natexit].cxa)
      atexits[natexit].fn(atexits[natexit].arg);
    else
      ((void (*)(void))atexits[natexit].fn)();
  }
  __stdio_exit(); // flush and close every stream
  _exit(status);
}

void
quick_exit(int status)
{
  while (nquickexit > 0)
    quickexits[--nquickexit]();
  _exit(status);
}

void
_Exit(int status)
{
  _exit(status);
}

void
abort(void)
{
  // the standard way: raise SIGABRT with the handler reset if it
  // returns, so an ignored signal still ends the program.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGABRT);
  sigprocmask(SIG_UNBLOCK, &set, 0);
  raise(SIGABRT);
  signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
  _exit(127);
}

// ---- running another program ----

int
system(const char *cmd)
{
  if (cmd == 0)
    return 1; // a shell is always available

  int pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    char *argv[] = {"sh", "-c", (char *)cmd, 0};
    execv("/bin/sh", argv);
    _exit(127);
  }
  int st;
  // POSIX says to ignore these while waiting
  void (*oint)(int) = signal(SIGINT, SIG_IGN);
  void (*oquit)(int) = signal(SIGQUIT, SIG_IGN);
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  signal(SIGINT, oint);
  signal(SIGQUIT, oquit);
  return st;
}

// ---- temporary files and paths ----

// replace the trailing XXXXXX with something unlikely to exist.
static int
fill_template(char *tmpl, int len)
{
  static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  int n = strlen(tmpl);

  if (n < len || strcmp(tmpl + n - len, "XXXXXX") != 0) {
    errno = EINVAL;
    return -1;
  }
  unsigned long r;
  if (getrandom(&r, sizeof(r), 0) != sizeof(r)) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    r = ts.tv_nsec ^ ((unsigned long)getpid() << 20);
  }
  for (int i = 0; i < len; i++) {
    tmpl[n - len + i] = set[r % 62];
    r /= 62;
    r = r * 6364136223846793005UL + 1442695040888963407UL;
  }
  return 0;
}

int
mkstemp(char *tmpl)
{
  for (int try = 0; try < 100; try++) {
    if (fill_template(tmpl, 6) < 0)
      return -1;
    int fd = open(tmpl, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0)
      return fd;
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

char *
mkdtemp(char *tmpl)
{
  for (int try = 0; try < 100; try++) {
    if (fill_template(tmpl, 6) < 0)
      return 0;
    if (mkdir(tmpl, 0777) == 0)
      return tmpl;
    if (errno != EEXIST)
      return 0;
  }
  errno = EEXIST;
  return 0;
}

char *
mktemp(char *tmpl)
{
  if (fill_template(tmpl, 6) < 0)
    return 0;
  return tmpl;
}

char *
tmpnam(char *s)
{
  static char buf[L_tmpnam];
  char *p = s ? s : buf;

  strcpy(p, "/tmp/tmpXXXXXX");
  return mktemp(p);
}

// the absolute path with no `.`, no `..` and no symbolic link left in
// it. each component is resolved in turn: when one turns out to be a
// link, what it points at is put in front of whatever is left of the
// path and the walk carries on. forty expansions is as far as this
// goes, which is where a loop of links is called a loop.
char *
realpath(const char *path, char *out)
{
  char rest[PATH_MAX], link[PATH_MAX], joined[PATH_MAX];
  char *buf = out ? out : malloc(PATH_MAX);
  size_t blen;
  int links = 0;

  if (buf == 0)
    return 0;
  if (path == 0 || path[0] == 0) {
    errno = ENOENT;
    goto fail;
  }
  if (path[0] != '/') {
    if (getcwd(rest, sizeof(rest)) == 0)
      goto fail;
    size_t n = strlen(rest);
    if (n + 1 + strlen(path) + 1 > sizeof(rest)) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    if (n > 1)
      rest[n++] = '/';
    strcpy(rest + n, path);
  } else {
    if (strlen(path) + 1 > sizeof(rest)) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    strcpy(rest, path);
  }

  buf[0] = '/';
  buf[1] = 0;
  blen = 1;

  char *p = rest;
  while (*p) {
    while (*p == '/')
      p++;
    if (*p == 0)
      break;
    char *comp = p;
    while (*p && *p != '/')
      p++;
    size_t len = p - comp;

    if (len == 1 && comp[0] == '.')
      continue;
    if (len == 2 && comp[0] == '.' && comp[1] == '.') {
      while (blen > 1 && buf[blen - 1] != '/')
        blen--;
      if (blen > 1)
        blen--;
      buf[blen] = 0;
      continue;
    }
    if (blen + (blen > 1 ? 1 : 0) + len + 1 > PATH_MAX) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    if (blen > 1)
      buf[blen++] = '/';
    memcpy(buf + blen, comp, len);
    blen += len;
    buf[blen] = 0;

    struct stat st;
    if (lstat(buf, &st) < 0)
      goto fail;
    if (st.type != T_SYMLINK)
      continue;
    if (++links > 40) {
      errno = ELOOP;
      goto fail;
    }
    ssize_t n = readlink(buf, link, sizeof(link) - 1);
    if (n < 0)
      goto fail;
    link[n] = 0;
    // what the link points at, then the rest of the path. `p` is at
    // the separator after this component, or at the end.
    if (strlen(link) + strlen(p) + 1 > sizeof(joined)) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    strcpy(joined, link);
    strcat(joined, p);
    strcpy(rest, joined);
    p = rest;
    if (link[0] == '/') {
      blen = 1;
      buf[1] = 0;
    } else {
      // a relative link is resolved against the directory it sits in,
      // so the component just appended comes back off
      while (blen > 1 && buf[blen - 1] != '/')
        blen--;
      if (blen > 1)
        blen--;
      buf[blen] = 0;
    }
  }

  struct stat st;
  if (stat(buf, &st) < 0)
    goto fail;
  return buf;

fail:
  if (out == 0)
    free(buf);
  return 0;
}

int
getsubopt(char **optp, char *const *keys, char **value)
{
  char *p = *optp, *end;

  *value = 0;
  if (*p == 0)
    return -1;
  end = strchr(p, ',');
  if (end)
    *end++ = 0;
  else
    end = p + strlen(p);
  *optp = end;

  char *eq = strchr(p, '=');
  size_t klen = eq ? (size_t)(eq - p) : strlen(p);
  for (int i = 0; keys[i]; i++) {
    if (strlen(keys[i]) == klen && memcmp(keys[i], p, klen) == 0) {
      if (eq)
        *value = eq + 1;
      return i;
    }
  }
  *value = p;
  return -1;
}

// ---- multibyte characters: UTF-8 ----

int
mbtowc(int *wc, const char *s, size_t n)
{
  if (s == 0)
    return 0;
  if (n == 0)
    return -1;

  unsigned char c = s[0];
  int len, v;
  if (c < 0x80) {
    if (wc)
      *wc = c;
    return c ? 1 : 0;
  } else if ((c & 0xe0) == 0xc0) {
    len = 2;
    v = c & 0x1f;
  } else if ((c & 0xf0) == 0xe0) {
    len = 3;
    v = c & 0x0f;
  } else if ((c & 0xf8) == 0xf0) {
    len = 4;
    v = c & 0x07;
  } else {
    errno = EILSEQ;
    return -1;
  }
  if (n < (size_t)len)
    return -1;
  for (int i = 1; i < len; i++) {
    if ((s[i] & 0xc0) != 0x80) {
      errno = EILSEQ;
      return -1;
    }
    v = (v << 6) | (s[i] & 0x3f);
  }
  if (wc)
    *wc = v;
  return len;
}

int
mblen(const char *s, size_t n)
{
  return mbtowc(0, s, n);
}

int
wctomb(char *s, int wc)
{
  if (s == 0)
    return 0;
  if (wc < 0x80) {
    s[0] = wc;
    return 1;
  }
  if (wc < 0x800) {
    s[0] = 0xc0 | (wc >> 6);
    s[1] = 0x80 | (wc & 0x3f);
    return 2;
  }
  if (wc < 0x10000) {
    s[0] = 0xe0 | (wc >> 12);
    s[1] = 0x80 | ((wc >> 6) & 0x3f);
    s[2] = 0x80 | (wc & 0x3f);
    return 3;
  }
  if (wc < 0x110000) {
    s[0] = 0xf0 | (wc >> 18);
    s[1] = 0x80 | ((wc >> 12) & 0x3f);
    s[2] = 0x80 | ((wc >> 6) & 0x3f);
    s[3] = 0x80 | (wc & 0x3f);
    return 4;
  }
  errno = EILSEQ;
  return -1;
}

size_t
mbstowcs(int *d, const char *s, size_t n)
{
  size_t i = 0;

  while (i < n || d == 0) {
    int wc, len = mbtowc(&wc, s, 4);
    if (len < 0)
      return (size_t)-1;
    if (d)
      d[i] = wc;
    i++;
    if (wc == 0)
      return i - 1;
    s += len;
  }
  return i;
}

size_t
wcstombs(char *d, const int *s, size_t n)
{
  size_t used = 0;
  char tmp[4];

  for (; *s; s++) {
    int len = wctomb(tmp, *s);
    if (len < 0)
      return (size_t)-1;
    if (d) {
      if (used + len > n)
        break;
      memcpy(d + used, tmp, len);
    }
    used += len;
  }
  if (d && used < n)
    d[used] = 0;
  return used;
}
