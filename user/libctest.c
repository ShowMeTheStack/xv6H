//
// libctest: the C library's own tests. every group reports what it
// checked, and a failure says what it expected and what it got, so a
// run that goes wrong is a bug report by itself.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include <dirent.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <locale.h>
#include <libgen.h>
#include <dlfcn.h>
#include <math.h>
#include "user/user.h"
#include <sys/stat.h>
#include <sys/wait.h>

static int failures, checks;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECKSTR(got, want)                                                                        \
  do {                                                                                             \
    checks++;                                                                                      \
    if (strcmp((got), (want)) != 0) {                                                              \
      printf("  FAIL %s:%d: got \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, (got), (want));      \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECKINT(got, want)                                                                        \
  do {                                                                                             \
    long g = (long)(got), w = (long)(want);                                                        \
    checks++;                                                                                      \
    if (g != w) {                                                                                  \
      printf("  FAIL %s:%d: got %ld, wanted %ld\n", __FILE__, __LINE__, g, w);                     \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static void
test_string(void)
{
  char buf[64];

  CHECKINT(strlen(""), 0);
  CHECKINT(strlen("hello"), 5);
  CHECKINT(strnlen("hello", 3), 3);
  CHECKSTR(strcpy(buf, "abc"), "abc");
  strncpy(buf, "abc", 6);
  CHECK(memcmp(buf, "abc\0\0\0", 6) == 0);
  CHECKINT(strlcpy(buf, "abcdef", 4), 6);
  CHECKSTR(buf, "abc");
  strcpy(buf, "ab");
  CHECKSTR(strcat(buf, "cd"), "abcd");
  CHECKINT(strlcat(buf, "ef", 6), 6);
  CHECK(strcmp("a", "b") < 0);
  CHECK(strcmp("b", "a") > 0);
  CHECK(strcmp("abc", "abc") == 0);
  CHECK(strncmp("abcd", "abce", 3) == 0);
  CHECK(strcasecmp("AbC", "aBc") == 0);
  CHECK(strncasecmp("AbCx", "aBcy", 3) == 0);
  CHECKSTR(strchr("hello", 'l'), "llo");
  CHECK(strchr("hello", 'z') == 0);
  CHECKSTR(strrchr("hello", 'l'), "lo");
  CHECKSTR(strstr("hello world", "wor"), "world");
  CHECK(strstr("hello", "xyz") == 0);
  CHECKSTR(strcasestr("Hello World", "WORLD"), "World");
  CHECKINT(strspn("abcxyz", "cba"), 3);
  CHECKINT(strcspn("abcxyz", "yx"), 3);
  CHECKSTR(strpbrk("abcxyz", "zx"), "xyz");

  strcpy(buf, "a,b,,c");
  char *save;
  CHECKSTR(strtok_r(buf, ",", &save), "a");
  CHECKSTR(strtok_r(0, ",", &save), "b");
  CHECKSTR(strtok_r(0, ",", &save), "c");
  CHECK(strtok_r(0, ",", &save) == 0);

  strcpy(buf, "a,b,,c");
  char *p = buf;
  CHECKSTR(strsep(&p, ","), "a");
  CHECKSTR(strsep(&p, ","), "b");
  CHECKSTR(strsep(&p, ","), "");
  CHECKSTR(strsep(&p, ","), "c");
  CHECK(strsep(&p, ",") == 0);

  // memmove must handle overlap in both directions
  strcpy(buf, "0123456789");
  memmove(buf + 2, buf, 8);
  CHECKSTR(buf, "0101234567");
  strcpy(buf, "0123456789");
  memmove(buf, buf + 2, 8);
  CHECKSTR(buf, "2345678989");

  // the word-at-a-time paths, at every alignment
  static char big[300], copy[300];
  for (int i = 0; i < 300; i++)
    big[i] = (char)(i * 7 + 1);
  for (int off = 0; off < 16; off++) {
    memset(copy, 0, sizeof(copy));
    memcpy(copy + off, big, 200);
    CHECK(memcmp(copy + off, big, 200) == 0);
    CHECK(copy[off + 200] == 0);
  }
  memset(copy, 'x', 100);
  for (int i = 0; i < 100; i++)
    CHECK(copy[i] == 'x');
  CHECK(memchr("abcdef", 'd', 6) != 0);
  CHECK(memchr("abcdef", 'z', 6) == 0);
  CHECKSTR((char *)memmem("hello world", 11, "o w", 3), "o world");

  char *dup = strdup("copy me");
  CHECKSTR(dup, "copy me");
  free(dup);
  dup = strndup("copy me", 4);
  CHECKSTR(dup, "copy");
  free(dup);

  CHECKSTR(strerror(ENOENT), "No such file or directory");
  CHECKSTR(strsignal(SIGKILL), "Killed");
  printf("string: %d checks\n", checks);
}

static void
test_ctype(void)
{
  CHECK(isalpha('a') && isalpha('Z') && !isalpha('0') && !isalpha(' '));
  CHECK(isdigit('7') && !isdigit('a'));
  CHECK(isxdigit('f') && isxdigit('F') && isxdigit('9') && !isxdigit('g'));
  CHECK(isspace(' ') && isspace('\t') && isspace('\n') && !isspace('a'));
  CHECK(isblank(' ') && isblank('\t') && !isblank('\n'));
  CHECK(ispunct('!') && !ispunct('a') && !ispunct(' '));
  CHECK(iscntrl('\n') && !iscntrl('a'));
  CHECK(isupper('A') && !isupper('a'));
  CHECK(islower('a') && !islower('A'));
  CHECK(isalnum('a') && isalnum('0') && !isalnum('!'));
  CHECK(isprint(' ') && isprint('a') && !isprint('\n'));
  CHECK(isgraph('a') && !isgraph(' '));
  CHECKINT(tolower('A'), 'a');
  CHECKINT(toupper('a'), 'A');
  CHECKINT(tolower('1'), '1');
  CHECK(!isalpha(EOF) && !isdigit(EOF) && !isspace(EOF));
  printf("ctype: ok\n");
}

static int
cmpint(const void *a, const void *b)
{
  int x = *(const int *)a, y = *(const int *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

static void
test_stdlib(void)
{
  CHECKINT(atoi("42"), 42);
  CHECKINT(atoi("-42"), -42);
  CHECKINT(atoi("  17abc"), 17);
  CHECKINT(atol("9999999999"), 9999999999L);

  char *end;
  CHECKINT(strtol("0x1f", &end, 0), 31);
  CHECKSTR(end, "");
  CHECKINT(strtol("0755", &end, 0), 493);
  CHECKINT(strtol("ff", &end, 16), 255);
  CHECKINT(strtol("-123xyz", &end, 10), -123);
  CHECKSTR(end, "xyz");
  CHECKINT(strtoul("18446744073709551615", &end, 10), (long)ULONG_MAX);
  errno = 0;
  CHECKINT(strtol("99999999999999999999999", &end, 10), LONG_MAX);
  CHECKINT(errno, ERANGE);

  CHECKINT(abs(-5), 5);
  CHECKINT(labs(-5L), 5);
  div_t d = div(17, 5);
  CHECKINT(d.quot, 3);
  CHECKINT(d.rem, 2);

  int v[9] = {5, 3, 9, 1, 7, 2, 8, 4, 6};
  qsort(v, 9, sizeof(int), cmpint);
  for (int i = 0; i < 9; i++)
    CHECKINT(v[i], i + 1);
  int key = 7;
  int *found = bsearch(&key, v, 9, sizeof(int), cmpint);
  CHECK(found && *found == 7);
  key = 100;
  CHECK(bsearch(&key, v, 9, sizeof(int), cmpint) == 0);

  // the same seed gives the same sequence, and a different one does not
  srand(12345);
  int a1 = rand(), a2 = rand();
  srand(12345);
  CHECKINT(rand(), a1);
  CHECKINT(rand(), a2);
  srand(999);
  CHECK(rand() != a1 || rand() != a2);
  for (int i = 0; i < 1000; i++) {
    int r = rand();
    CHECK(r >= 0 && r <= RAND_MAX);
  }

  setenv("LIBCTEST", "yes", 1);
  CHECKSTR(getenv("LIBCTEST"), "yes");
  setenv("LIBCTEST", "no", 0);
  CHECKSTR(getenv("LIBCTEST"), "yes");
  setenv("LIBCTEST", "no", 1);
  CHECKSTR(getenv("LIBCTEST"), "no");
  unsetenv("LIBCTEST");
  CHECK(getenv("LIBCTEST") == 0);

  // UTF-8 both ways
  char mb[4];
  int wc;
  CHECKINT(wctomb(mb, 0x263A), 3);
  CHECKINT(mbtowc(&wc, mb, 3), 3);
  CHECKINT(wc, 0x263A);
  CHECKINT(wctomb(mb, 'A'), 1);
  CHECKINT(mbtowc(&wc, "A", 1), 1);
  CHECKINT(wc, 'A');
  printf("stdlib: ok\n");
}

static void
test_malloc(void)
{
  // a spread of sizes, each written over and read back
  void *p[64];
  for (int i = 0; i < 64; i++) {
    size_t n = (i + 1) * 37;
    p[i] = malloc(n);
    CHECK(p[i] != 0);
    memset(p[i], i, n);
  }
  for (int i = 0; i < 64; i++) {
    unsigned char *q = p[i];
    size_t n = (i + 1) * 37;
    for (size_t j = 0; j < n; j++)
      if (q[j] != (unsigned char)i) {
        printf("  FAIL malloc block %d byte %ld was %d\n", i, (long)j, q[j]);
        failures++;
        break;
      }
    checks++;
  }
  for (int i = 0; i < 64; i += 2)
    free(p[i]);
  for (int i = 0; i < 64; i += 2) {
    p[i] = malloc(64);
    CHECK(p[i] != 0);
  }
  for (int i = 0; i < 64; i++)
    free(p[i]);

  char *big = malloc(2 << 20); // over the limit where a mapping is its own
  CHECK(big != 0);
  memset(big, 0x5a, 2 << 20);
  CHECK(big[0] == 0x5a && big[(2 << 20) - 1] == 0x5a);
  free(big);

  char *g = malloc(10);
  strcpy(g, "123456789");
  g = realloc(g, 1000);
  CHECKSTR(g, "123456789");
  memset(g + 9, 'x', 990);
  g[999] = 0;
  g = realloc(g, 20);
  CHECK(strncmp(g, "123456789", 9) == 0);
  free(g);
  CHECK(realloc(0, 10) != 0);

  int *z = calloc(100, sizeof(int));
  CHECK(z != 0);
  for (int i = 0; i < 100; i++)
    CHECKINT(z[i], 0);
  free(z);

  void *al;
  CHECKINT(posix_memalign(&al, 4096, 100), 0);
  CHECKINT((unsigned long)al % 4096, 0);
  memset(al, 1, 100);
  free(al);
  CHECK(malloc(0) != 0);
  free(0);
  printf("malloc: ok\n");
}

static void
test_printf(void)
{
  char buf[256];

  CHECKINT(snprintf(buf, sizeof(buf), "%d", 42), 2);
  CHECKSTR(buf, "42");
  snprintf(buf, sizeof(buf), "%5d|%-5d|%05d", 42, 42, 42);
  CHECKSTR(buf, "   42|42   |00042");
  snprintf(buf, sizeof(buf), "%+d %+d % d", 5, -5, 5);
  CHECKSTR(buf, "+5 -5  5");
  snprintf(buf, sizeof(buf), "%x %X %#x %o %#o", 255, 255, 255, 8, 8);
  CHECKSTR(buf, "ff FF 0xff 10 010");
  snprintf(buf, sizeof(buf), "%ld %lu %lld", -1L, 1UL, -1LL);
  CHECKSTR(buf, "-1 1 -1");
  snprintf(buf, sizeof(buf), "%s|%10s|%-10s|%.3s", "ab", "ab", "ab", "abcdef");
  CHECKSTR(buf, "ab|        ab|ab        |abc");
  snprintf(buf, sizeof(buf), "%c%c", 'h', 'i');
  CHECKSTR(buf, "hi");
  snprintf(buf, sizeof(buf), "%%");
  CHECKSTR(buf, "%");
  snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
  CHECKSTR(buf, "0x1234");
  snprintf(buf, sizeof(buf), "%*d|%.*d", 5, 7, 3, 7);
  CHECKSTR(buf, "    7|007");
  int n = 0;
  snprintf(buf, sizeof(buf), "abc%n", &n);
  CHECKINT(n, 3);

  // truncation reports what a longer buffer would have taken
  char small[5];
  CHECKINT(snprintf(small, sizeof(small), "abcdefgh"), 8);
  CHECKSTR(small, "abcd");
  CHECKINT(snprintf(0, 0, "%d-%d", 12, 34), 5);

  // floating point, where the digits must be exactly right
  snprintf(buf, sizeof(buf), "%f", 1.0);
  CHECKSTR(buf, "1.000000");
  snprintf(buf, sizeof(buf), "%.2f", 3.14159);
  CHECKSTR(buf, "3.14");
  snprintf(buf, sizeof(buf), "%.0f", 2.5);
  CHECKSTR(buf, "2"); // ties to even
  snprintf(buf, sizeof(buf), "%.0f", 3.5);
  CHECKSTR(buf, "4");
  snprintf(buf, sizeof(buf), "%.3f", 0.0005);
  CHECKSTR(buf, "0.001");
  snprintf(buf, sizeof(buf), "%e", 1234.5678);
  CHECKSTR(buf, "1.234568e+03");
  snprintf(buf, sizeof(buf), "%.2e", 0.000123);
  CHECKSTR(buf, "1.23e-04");
  snprintf(buf, sizeof(buf), "%g %g", 100000.0, 1000000.0);
  CHECKSTR(buf, "100000 1e+06");
  snprintf(buf, sizeof(buf), "%g", 0.0001);
  CHECKSTR(buf, "0.0001");
  snprintf(buf, sizeof(buf), "%g", 0.00001);
  CHECKSTR(buf, "1e-05");
  snprintf(buf, sizeof(buf), "%.17g", 0.1);
  CHECKSTR(buf, "0.10000000000000001");
  snprintf(buf, sizeof(buf), "%f %f", 0.0, -0.0);
  CHECKSTR(buf, "0.000000 -0.000000");
  snprintf(buf, sizeof(buf), "%.20f", 0.1);
  CHECKSTR(buf, "0.10000000000000000555");
  snprintf(buf, sizeof(buf), "%.0f", 1e22);
  CHECKSTR(buf, "10000000000000000000000");

  double inf = 1e308 * 10, nan = inf - inf;
  snprintf(buf, sizeof(buf), "%f %F %f", inf, -inf, nan);
  CHECKSTR(buf, "inf -INF nan");
  snprintf(buf, sizeof(buf), "%a", 1.0);
  CHECKSTR(buf, "0x1p+0");
  snprintf(buf, sizeof(buf), "%a", 0.5);
  CHECKSTR(buf, "0x1p-1");

  CHECKINT(asprintf(&(char *){0}, "x") >= 0, 1);
  char *ap;
  CHECKINT(asprintf(&ap, "%d-%s", 7, "seven"), 7);
  CHECKSTR(ap, "7-seven");
  free(ap);
  printf("printf: ok\n");
}

static void
test_scanf(void)
{
  int a, b;
  char s1[32], s2[32];
  double d;

  CHECKINT(sscanf("12 34", "%d %d", &a, &b), 2);
  CHECKINT(a, 12);
  CHECKINT(b, 34);
  CHECKINT(sscanf("0x1f", "%x", &a), 1);
  CHECKINT(a, 31);
  CHECKINT(sscanf("hello world", "%s %s", s1, s2), 2);
  CHECKSTR(s1, "hello");
  CHECKSTR(s2, "world");
  CHECKINT(sscanf("abc", "%2s", s1), 1);
  CHECKSTR(s1, "ab");
  CHECKINT(sscanf("3.25", "%lf", &d), 1);
  CHECK(d == 3.25);
  CHECKINT(sscanf("12abc", "%d%s", &a, s1), 2);
  CHECKINT(a, 12);
  CHECKSTR(s1, "abc");
  CHECKINT(sscanf("  42", "%d", &a), 1);
  CHECKINT(a, 42);
  CHECKINT(sscanf("a=5", "a=%d", &a), 1);
  CHECKINT(a, 5);
  CHECKINT(sscanf("xyz", "%d", &a), 0);
  CHECKINT(sscanf("1 2", "%d %*d %d", &a, &b), 1);
  CHECKINT(sscanf("[abc]", "[%[^]]]", s1), 1);
  CHECKSTR(s1, "abc");
  CHECKINT(sscanf("aab", "%[ab]", s1), 1);
  CHECKSTR(s1, "aab");
  int pos;
  CHECKINT(sscanf("1234 x", "%d %n", &a, &pos), 1);
  CHECKINT(pos, 5);
  printf("scanf: ok\n");
}

static void
test_stdio(void)
{
  unlink("libctest.tmp");
  FILE *f = fopen("libctest.tmp", "w");
  CHECK(f != 0);
  CHECKINT(fputs("line one\n", f), 9);
  fprintf(f, "value %d\n", 42);
  fputc('x', f);
  CHECKINT(fwrite("yz\n", 1, 3, f), 3);
  CHECKINT(fclose(f), 0);

  f = fopen("libctest.tmp", "r");
  CHECK(f != 0);
  char buf[64];
  CHECKSTR(fgets(buf, sizeof(buf), f), "line one\n");
  CHECKSTR(fgets(buf, sizeof(buf), f), "value 42\n");
  CHECKINT(fgetc(f), 'x');
  CHECKINT(ungetc('X', f), 'X');
  CHECKINT(fgetc(f), 'X');
  CHECKSTR(fgets(buf, sizeof(buf), f), "yz\n");
  CHECK(fgets(buf, sizeof(buf), f) == 0);
  CHECK(feof(f));
  clearerr(f);
  CHECK(!feof(f));
  rewind(f);
  CHECKINT(ftell(f), 0);
  CHECKINT(fread(buf, 1, 4, f), 4);
  CHECK(memcmp(buf, "line", 4) == 0);
  CHECKINT(ftell(f), 4);
  CHECKINT(fseek(f, 5, SEEK_SET), 0);
  CHECKINT(fgetc(f), 'o');
  CHECKINT(fseek(f, -1, SEEK_END), 0);
  CHECKINT(fgetc(f), '\n');
  fclose(f);

  // getline grows its buffer as it needs to
  f = fopen("libctest.tmp", "r");
  char *line = 0;
  size_t cap = 0;
  CHECKINT(getline(&line, &cap, f), 9);
  CHECKSTR(line, "line one\n");
  CHECKINT(getline(&line, &cap, f), 9);
  CHECKSTR(line, "value 42\n");
  free(line);
  fclose(f);

  // scanf through a stream
  f = fopen("libctest.tmp", "r");
  char word[16];
  int num;
  CHECKINT(fscanf(f, "%s %s value %d", word, buf, &num), 3);
  CHECKSTR(word, "line");
  CHECKINT(num, 42);
  fclose(f);

  // append, and reading after writing
  f = fopen("libctest.tmp", "a");
  fprintf(f, "appended\n");
  fclose(f);
  f = fopen("libctest.tmp", "r");
  int lines = 0;
  while (fgets(buf, sizeof(buf), f))
    lines++;
  CHECKINT(lines, 4);
  fclose(f);
  unlink("libctest.tmp");

  // a stream over memory
  char mem[64];
  f = fmemopen(mem, sizeof(mem), "w");
  CHECK(f != 0);
  fprintf(f, "in memory %d", 7);
  fflush(f);
  fclose(f);
  CHECK(strncmp(mem, "in memory 7", 11) == 0);

  char *out = 0;
  size_t outlen = 0;
  f = open_memstream(&out, &outlen);
  CHECK(f != 0);
  for (int i = 0; i < 100; i++)
    fprintf(f, "%d,", i);
  fclose(f);
  CHECK(out != 0);
  CHECK(outlen > 200);
  CHECK(strncmp(out, "0,1,2,", 6) == 0);
  free(out);

  // errors are reported, not hidden
  CHECK(fopen("/no/such/file", "r") == 0);
  f = fopen("libctest2.tmp", "w");
  CHECK(fgetc(f) == EOF);
  fclose(f);
  unlink("libctest2.tmp");
  printf("stdio: ok\n");
}

static void
test_time(void)
{
  struct tm tm;
  time_t t = 1758326400; // 2025-09-20 00:00:00 UTC, a Saturday

  gmtime_r(&t, &tm);
  CHECKINT(tm.tm_year + 1900, 2025);
  CHECKINT(tm.tm_mon + 1, 9);
  CHECKINT(tm.tm_mday, 20);
  CHECKINT(tm.tm_hour, 0);
  CHECKINT(tm.tm_wday, 6);
  CHECKINT(timegm(&tm), t);

  char buf[64];
  CHECK(strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) > 0);
  CHECKSTR(buf, "2025-09-20 00:00:00");
  strftime(buf, sizeof(buf), "%a %b %e %j %p", &tm);
  CHECKSTR(buf, "Sat Sep 20 263 AM");
  strftime(buf, sizeof(buf), "%F %T", &tm);
  CHECKSTR(buf, "2025-09-20 00:00:00");

  struct tm parsed;
  memset(&parsed, 0, sizeof(parsed));
  CHECK(strptime("2025-09-20 13:45:01", "%Y-%m-%d %H:%M:%S", &parsed) != 0);
  CHECKINT(parsed.tm_year + 1900, 2025);
  CHECKINT(parsed.tm_mon, 8);
  CHECKINT(parsed.tm_mday, 20);
  CHECKINT(parsed.tm_hour, 13);
  CHECKINT(parsed.tm_min, 45);

  // leap years, and the day before and after one
  time_t leap = 951782400; // 2000-02-29
  gmtime_r(&leap, &tm);
  CHECKINT(tm.tm_mon + 1, 2);
  CHECKINT(tm.tm_mday, 29);
  CHECKINT(tm.tm_year + 1900, 2000);

  // the epoch itself, and a time before it
  t = 0;
  gmtime_r(&t, &tm);
  CHECKINT(tm.tm_year + 1900, 1970);
  CHECKINT(tm.tm_yday, 0);
  t = -86400;
  gmtime_r(&t, &tm);
  CHECKINT(tm.tm_year + 1900, 1969);
  CHECKINT(tm.tm_mon + 1, 12);
  CHECKINT(tm.tm_mday, 31);

  // a time zone from TZ, with and without summer time
  setenv("TZ", "IST-5:30", 1);
  tzset();
  t = 1758326400;
  localtime_r(&t, &tm);
  CHECKINT(tm.tm_hour, 5);
  CHECKINT(tm.tm_min, 30);
  CHECKINT(mktime(&tm), t);
  setenv("TZ", "UTC", 1);
  tzset();

  // asctime's exact format
  gmtime_r(&t, &tm);
  asctime_r(&tm, buf);
  CHECKSTR(buf, "Sat Sep 20 00:00:00 2025\n");

  // the clock moves forward
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  ticksleep(3);
  clock_gettime(CLOCK_MONOTONIC, &b);
  CHECK(b.tv_sec > a.tv_sec || b.tv_nsec > a.tv_nsec);
  printf("time: ok\n");
}

static jmp_buf jb;
static volatile int jumped;

static void
do_jump(void)
{
  jumped = 1;
  longjmp(jb, 7);
}

static void
test_setjmp(void)
{
  volatile int stack_value = 99;
  int r = setjmp(jb);

  if (r == 0) {
    do_jump();
    CHECK(0); // never reached
  }
  CHECKINT(r, 7);
  CHECKINT(jumped, 1);
  CHECKINT(stack_value, 99);

  // longjmp(env, 0) still returns 1
  jumped = 0;
  r = setjmp(jb);
  if (r == 0) {
    jumped = 1;
    longjmp(jb, 0);
  }
  CHECKINT(r, 1);
  printf("setjmp: ok\n");
}

static volatile int sigcount;

static void
counter(int sig)
{
  (void)sig;
  sigcount++;
}

static void
test_signal(void)
{
  sigcount = 0;
  CHECK(signal(SIGUSR1, counter) != SIG_ERR);
  raise(SIGUSR1);
  CHECKINT(sigcount, 1);
  kill(getpid(), SIGUSR1);
  CHECKINT(sigcount, 2);

  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  CHECK(sigismember(&set, SIGUSR1));
  CHECK(!sigismember(&set, SIGUSR2));
  sigprocmask(SIG_BLOCK, &set, &old);
  raise(SIGUSR1);
  CHECKINT(sigcount, 2); // blocked, still pending
  sigset_t pend;
  sigpending(&pend);
  CHECK(sigismember(&pend, SIGUSR1));
  sigprocmask(SIG_SETMASK, &old, 0);
  CHECKINT(sigcount, 3);
  signal(SIGUSR1, SIG_DFL);
  printf("signal: ok\n");
}

static void *
adder(void *arg)
{
  long *v = arg;

  for (int i = 0; i < 1000; i++)
    __atomic_fetch_add(v, 1, __ATOMIC_RELAXED);
  return (void *)(*v > 0 ? 1L : 0L);
}

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t bar;
static pthread_key_t tkey;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_count, guarded, ready;

static void
once_fn(void)
{
  once_count++;
}

static void *
worker(void *arg)
{
  pthread_once(&once, once_fn);
  pthread_setspecific(tkey, arg);

  for (int i = 0; i < 500; i++) {
    pthread_mutex_lock(&mtx);
    guarded++;
    pthread_mutex_unlock(&mtx);
  }
  pthread_barrier_wait(&bar);

  pthread_mutex_lock(&mtx);
  while (!ready)
    pthread_cond_wait(&cnd, &mtx);
  pthread_mutex_unlock(&mtx);
  return pthread_getspecific(tkey);
}

static void
test_pthread(void)
{
  pthread_t t[4];
  long counter = 0;

  CHECKINT(pthread_create(&t[0], 0, adder, &counter), 0);
  void *ret;
  CHECKINT(pthread_join(t[0], &ret), 0);
  CHECKINT(counter, 1000);
  CHECKINT((long)ret, 1);

  CHECKINT(pthread_key_create(&tkey, 0), 0);
  CHECKINT(pthread_barrier_init(&bar, 0, 4), 0);
  guarded = 0;
  ready = 0;
  once_count = 0;
  for (long i = 0; i < 4; i++)
    CHECKINT(pthread_create(&t[i], 0, worker, (void *)(i + 1)), 0);
  ticksleep(20);
  pthread_mutex_lock(&mtx);
  ready = 1;
  pthread_cond_broadcast(&cnd);
  pthread_mutex_unlock(&mtx);
  for (long i = 0; i < 4; i++) {
    CHECKINT(pthread_join(t[i], &ret), 0);
    CHECKINT((long)ret, i + 1);
  }
  CHECKINT(guarded, 2000);
  CHECKINT(once_count, 1);
  pthread_key_delete(tkey);
  pthread_barrier_destroy(&bar);

  // a recursive mutex, and the error-checking one
  pthread_mutex_t rec;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&rec, &attr);
  CHECKINT(pthread_mutex_lock(&rec), 0);
  CHECKINT(pthread_mutex_lock(&rec), 0);
  CHECKINT(pthread_mutex_unlock(&rec), 0);
  CHECKINT(pthread_mutex_unlock(&rec), 0);
  CHECKINT(pthread_mutex_trylock(&rec), 0);
  pthread_mutex_unlock(&rec);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&rec, &attr);
  pthread_mutex_lock(&rec);
  CHECKINT(pthread_mutex_lock(&rec), EDEADLK);
  pthread_mutex_unlock(&rec);

  // read-write locks and semaphores
  pthread_rwlock_t rw;
  pthread_rwlock_init(&rw, 0);
  CHECKINT(pthread_rwlock_rdlock(&rw), 0);
  CHECKINT(pthread_rwlock_tryrdlock(&rw), 0);
  CHECKINT(pthread_rwlock_trywrlock(&rw), EBUSY);
  pthread_rwlock_unlock(&rw);
  pthread_rwlock_unlock(&rw);
  CHECKINT(pthread_rwlock_wrlock(&rw), 0);
  CHECKINT(pthread_rwlock_tryrdlock(&rw), EBUSY);
  pthread_rwlock_unlock(&rw);

  sem_t sem;
  sem_init(&sem, 0, 2);
  CHECKINT(sem_wait(&sem), 0);
  CHECKINT(sem_wait(&sem), 0);
  CHECKINT(sem_trywait(&sem), -1);
  CHECKINT(sem_post(&sem), 0);
  CHECKINT(sem_trywait(&sem), 0);
  int sv;
  sem_getvalue(&sem, &sv);
  CHECKINT(sv, 0);
  printf("pthread: ok\n");
}

// __thread variables: each thread gets its own copy, initialized from
// the program's image, and the main thread's copy is not disturbed.
static __thread int tls_int = 77;
static __thread char tls_buf[32] = "initial";
static __thread long tls_zero;

static void *
tls_worker(void *arg)
{
  long n = (long)arg;

  CHECKINT(tls_int, 77);
  CHECKSTR(tls_buf, "initial");
  CHECKINT(tls_zero, 0);
  tls_int = n;
  tls_zero = n * 100;
  snprintf(tls_buf, sizeof(tls_buf), "thread %ld", n);
  ticksleep(5);
  CHECKINT(tls_int, n);
  CHECKINT(tls_zero, n * 100);
  char want[32];
  snprintf(want, sizeof(want), "thread %ld", n);
  CHECKSTR(tls_buf, want);
  return 0;
}

static void
test_tls(void)
{
  pthread_t t[4];

  CHECKINT(tls_int, 77);
  CHECKSTR(tls_buf, "initial");
  tls_int = 5;
  strcpy(tls_buf, "main");
  for (long i = 1; i <= 4; i++)
    CHECKINT(pthread_create(&t[i - 1], 0, tls_worker, (void *)i), 0);
  for (int i = 0; i < 4; i++)
    pthread_join(t[i], 0);
  CHECKINT(tls_int, 5);
  CHECKSTR(tls_buf, "main");
  CHECKINT(tls_zero, 0);
  printf("tls: ok\n");
}

#include "user/divcases.h"

// binary128 division, which this system implements itself because the
// compiler's own routine cannot go into a shared library. every case
// was computed with exact rational arithmetic.
static void
test_softfloat(void)
{
  int bad = 0;

  for (unsigned i = 0; i < sizeof(divcases) / sizeof(divcases[0]); i++) {
    union {
      long double f;
      unsigned long w[2];
    } a, b, q;
    a.w[0] = divcases[i].a[0];
    a.w[1] = divcases[i].a[1];
    b.w[0] = divcases[i].b[0];
    b.w[1] = divcases[i].b[1];
    q.f = a.f / b.f;
    checks++;
    if (q.w[0] != divcases[i].q[0] || q.w[1] != divcases[i].q[1]) {
      if (bad++ < 4)
        printf("  FAIL divide case %u: got %016lx%016lx wanted %016lx%016lx\n", i, q.w[1], q.w[0],
               divcases[i].q[1], divcases[i].q[0]);
      failures++;
    }
  }

  // the values that are not numbers
  long double inf = 1.0L / 0.0L;
  CHECK(inf > 1e4000L);
  CHECK(1.0L / inf == 0.0L);
  CHECK(-1.0L / 0.0L < 0);
  long double nan = 0.0L / 0.0L;
  CHECK(nan != nan);
  CHECK((inf / inf) != (inf / inf));
  printf("softfloat: %d division cases\n", (int)(sizeof(divcases) / sizeof(divcases[0])));
}

// dynamic linking: the program is linked against the shared library,
// and can load another one while it runs.
static int
count_objects(struct dl_phdr_info *info, size_t size, void *data)
{
  (void)size;
  if (info->dlpi_name && info->dlpi_name[0])
    (*(int *)data)++;
  return 0;
}

static void
test_dlopen(void)
{
  // a statically linked program has no loader, and says so
  void *h = dlopen("libtest.so", RTLD_NOW);
  if (h == 0) {
    printf("dlopen: statically linked (%s)\n", dlerror());
    return;
  }

  int (*add)(int, int) = dlsym(h, "lib_add");
  const char *(*name)(void) = dlsym(h, "lib_name");
  int *counter = dlsym(h, "lib_counter");
  CHECK(add != 0);
  CHECK(name != 0);
  CHECK(counter != 0);
  if (add == 0 || name == 0 || counter == 0) {
    printf("dlopen: %s\n", dlerror());
    return;
  }
  CHECKINT(*counter, 100); // the library's initializer ran
  CHECKINT(add(2, 3), 5);
  CHECKINT(*counter, 101);
  CHECKSTR(name(), "libtest");
  CHECK(dlsym(h, "no_such_symbol") == 0);

  // a symbol of the program's own world, without a handle
  void *p = dlsym(RTLD_DEFAULT, "printf");
  CHECK(p == (void *)printf);

  // where an address came from
  Dl_info info;
  CHECKINT(dladdr((void *)add, &info), 1);
  CHECK(info.dli_fname != 0);
  CHECK(strstr(info.dli_fname, "libtest") != 0);
  CHECKSTR(info.dli_sname, "lib_add");

  int n = 0;
  dl_iterate_phdr(count_objects, &n);
  CHECK(n >= 2); // the library and the one we just loaded
  CHECKINT(dlclose(h), 0);
  printf("dlopen: ok, %d objects loaded\n", n);
}

static void
test_dirent(void)
{
  DIR *d = opendir("/");
  CHECK(d != 0);
  int found_init = 0, entries = 0;
  struct dirent *e;

  while ((e = readdir(d)) != 0) {
    entries++;
    if (strcmp(e->d_name, "init") == 0)
      found_init = 1;
  }
  CHECK(entries > 5);
  CHECK(found_init);
  rewinddir(d);
  CHECK(readdir(d) != 0);
  closedir(d);
  CHECK(opendir("/no/such/directory") == 0);
  CHECK(opendir("/init") == 0); // not a directory
  printf("dirent: %d entries in /\n", entries);
}

static void
test_getopt(void)
{
  char *argv[] = {"prog", "-a", "-b", "value", "-cd", "rest", 0};
  int argc = 6;
  int sawa = 0, sawb = 0, sawc = 0, sawd = 0;
  int c;

  optind = 1;
  opterr = 0;
  while ((c = getopt(argc, argv, "ab:cd")) != -1) {
    switch (c) {
    case 'a': sawa = 1; break;
    case 'b': sawb = 1; CHECKSTR(optarg, "value"); break;
    case 'c': sawc = 1; break;
    case 'd': sawd = 1; break;
    default: CHECK(0); break;
    }
  }
  CHECK(sawa && sawb && sawc && sawd);
  CHECKSTR(argv[optind], "rest");

  static const struct option longopts[] = {
      {"help", no_argument, 0, 'h'},
      {"file", required_argument, 0, 'f'},
      {0, 0, 0, 0},
  };
  char *argv2[] = {"prog", "--help", "--file=x.txt", "tail", 0};
  optind = 1;
  int idx;
  c = getopt_long(4, argv2, "hf:", longopts, &idx);
  CHECKINT(c, 'h');
  c = getopt_long(4, argv2, "hf:", longopts, &idx);
  CHECKINT(c, 'f');
  CHECKSTR(optarg, "x.txt");
  CHECKINT(getopt_long(4, argv2, "hf:", longopts, &idx), -1);
  CHECKSTR(argv2[optind], "tail");
  printf("getopt: ok\n");
}

static void
test_misc(void)
{
  char path[64];

  strcpy(path, "/usr/local/bin/prog");
  CHECKSTR(basename(path), "prog");
  strcpy(path, "/usr/local/bin/prog");
  CHECKSTR(dirname(path), "/usr/local/bin");
  strcpy(path, "prog");
  CHECKSTR(dirname(path), ".");
  strcpy(path, "/");
  CHECKSTR(basename(path), "/");

  CHECKSTR(setlocale(LC_ALL, "C"), "C");
  CHECKSTR(localeconv()->decimal_point, ".");
  CHECK(setlocale(LC_ALL, "fr_FR") == 0);

  CHECK(getcwd(path, sizeof(path)) != 0);
  CHECKSTR(path, "/");
  CHECK(isatty(0) || !isatty(0)); // just that it answers
  CHECKINT(access("/init", F_OK), 0);
  CHECKINT(access("/nothing-here", F_OK), -1);

  struct stat sb;
  CHECKINT(stat("/init", &sb), 0);
  CHECK(sb.size > 0);
  CHECK(S_ISREG(ST_MODE(sb)));
  CHECKINT(stat("/", &sb), 0);
  CHECK(S_ISDIR(ST_MODE(sb)));

  CHECKINT(sysconf(_SC_PAGESIZE), 4096);
  CHECK(sysconf(_SC_NPROCESSORS_ONLN) >= 1);
  printf("misc: ok\n");
}

static void
test_process(void)
{
  // system() and popen() both need a shell
  FILE *p = popen("echo from-popen", "r");
  CHECK(p != 0);
  char buf[64];
  CHECK(fgets(buf, sizeof(buf), p) != 0);
  CHECKSTR(buf, "from-popen\n");
  pclose(p);

  int st = system("echo from-system > /libctest3.tmp");
  CHECK(WIFEXITED(st));
  FILE *f = fopen("/libctest3.tmp", "r");
  CHECK(f != 0);
  CHECK(fgets(buf, sizeof(buf), f) != 0);
  CHECKSTR(buf, "from-system\n");
  fclose(f);
  unlink("/libctest3.tmp");
  printf("process: ok\n");
}

int
main(int argc, char *argv[])
{
  int only = argc > 1;

#define GROUP(name)                                                                                \
  if (!only || strcmp(argv[1], #name) == 0)                                                        \
  test_##name()

  GROUP(string);
  GROUP(ctype);
  GROUP(stdlib);
  GROUP(malloc);
  GROUP(printf);
  GROUP(scanf);
  GROUP(stdio);
  GROUP(time);
  GROUP(setjmp);
  GROUP(signal);
  GROUP(pthread);
  GROUP(tls);
  GROUP(softfloat);
  GROUP(dlopen);
  GROUP(dirent);
  GROUP(getopt);
  GROUP(misc);
  GROUP(process);

  printf("libctest: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("ALL TESTS PASSED\n");
  exit(failures != 0);
}
