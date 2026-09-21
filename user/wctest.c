//
// wctest: the tests for the interfaces added last to this library --
// wide characters, iconv, search, nl_types, strfmon, posix_spawn,
// ucontext and syslog. each group prints what it covered, and a
// failure says where it was and what it expected, so a bad run is a
// bug report on its own.
//
#include "user/user.h"
#include <wchar.h>
#include <wctype.h>
#include <iconv.h>
#include <search.h>
#include <nl_types.h>
#include <monetary.h>
#include <spawn.h>
#include <ucontext.h>
#include <syslog.h>

static int checks, failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
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

#define CHECKSTR(got, want)                                                                        \
  do {                                                                                             \
    checks++;                                                                                      \
    if (strcmp((got), (want)) != 0) {                                                              \
      printf("  FAIL %s:%d: got \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, (got), (want));      \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// the three characters the UTF-8 tests use: one byte, three bytes and
// four bytes, so every branch of the decoder sees traffic.
static const char *const s_ascii = "hi";
static const char *const s_cjk = "\xe4\xb8\xad";         // U+4E2D
static const char *const s_emoji = "\xf0\x9f\x98\x80";   // U+1F600

static void
test_mb(void)
{
  mbstate_t st;
  wchar_t wc;
  char buf[16];

  memset(&st, 0, sizeof(st));
  CHECK(mbsinit(&st));
  CHECKINT(mbrtowc(&wc, s_ascii, 2, &st), 1);
  CHECKINT(wc, 'h');
  CHECKINT(mbrtowc(&wc, "", 1, &st), 0); // a null character converts to nothing
  CHECKINT(wc, 0);

  CHECKINT(mbrtowc(&wc, s_cjk, 3, &st), 3);
  CHECKINT(wc, 0x4e2d);
  CHECKINT(mbrtowc(&wc, s_emoji, 4, &st), 4);
  CHECKINT(wc, 0x1f600);

  // the point of mbstate_t: a character arriving in two pieces.
  memset(&st, 0, sizeof(st));
  CHECKINT(mbrtowc(&wc, s_cjk, 2, &st), (size_t)-2);
  CHECK(!mbsinit(&st));
  CHECKINT(mbrtowc(&wc, s_cjk + 2, 1, &st), 1);
  CHECKINT(wc, 0x4e2d);
  CHECK(mbsinit(&st));

  // and a four-byte one split three bytes to one.
  CHECKINT(mbrtowc(&wc, s_emoji, 3, &st), (size_t)-2);
  CHECKINT(mbrtowc(&wc, s_emoji + 3, 1, &st), 1);
  CHECKINT(wc, 0x1f600);

  // the sequences that are not characters at all
  memset(&st, 0, sizeof(st));
  CHECKINT(mbrtowc(&wc, "\xff", 1, &st), (size_t)-1);
  CHECKINT(errno, EILSEQ);
  CHECKINT(mbrtowc(&wc, "\xc0\x80", 2, &st), (size_t)-1); // an overlong null
  CHECKINT(mbrtowc(&wc, "\xed\xa0\x80", 3, &st), (size_t)-1); // a surrogate
  CHECKINT(mbrlen(s_cjk, 3, &st), 3);

  CHECKINT(wcrtomb(buf, 0x4e2d, &st), 3);
  CHECK(memcmp(buf, s_cjk, 3) == 0);
  CHECKINT(wcrtomb(buf, 0x1f600, &st), 4);
  CHECK(memcmp(buf, s_emoji, 4) == 0);
  CHECKINT(wcrtomb(buf, 0xd800, &st), (size_t)-1);

  // a round trip through wide characters and back
  const char *src = "hi \xe4\xb8\xad \xf0\x9f\x98\x80";
  const char *p = src;
  wchar_t wide[16];
  memset(&st, 0, sizeof(st));
  CHECKINT(mbsrtowcs(wide, &p, 16, &st), 6);
  CHECK(p == 0);
  CHECKINT(wide[3], 0x4e2d);
  CHECKINT(wide[5], 0x1f600);
  CHECKINT(mbsrtowcs(0, &src, 0, &st), 6); // the length, without converting

  const wchar_t *wp = wide;
  char back[32];
  memset(&st, 0, sizeof(st));
  CHECKINT(wcsrtombs(back, &wp, sizeof(back), &st), strlen(src));
  CHECKSTR(back, src);

  CHECKINT(btowc('A'), 'A');
  CHECKINT(btowc(0xc3), WEOF);
  CHECKINT(btowc(EOF), WEOF);
  CHECKINT(wctob(L'A'), 'A');
  CHECKINT(wctob(0x4e2d), EOF);
  printf("mb: UTF-8 conversion, split sequences, round trips\n");
}

static void
test_wcs(void)
{
  wchar_t buf[32], *save;

  CHECKINT(wcslen(L"hello"), 5);
  CHECKINT(wcsnlen(L"hello", 3), 3);
  CHECK(wcscmp(wcscpy(buf, L"abc"), L"abc") == 0);
  wcsncpy(buf, L"abc", 6);
  CHECK(wmemcmp(buf, L"abc\0\0\0", 6) == 0);
  wcscpy(buf, L"ab");
  CHECK(wcscmp(wcscat(buf, L"cd"), L"abcd") == 0);
  CHECK(wcscmp(wcsncat(buf, L"efg", 2), L"abcdef") == 0);
  CHECK(wcscmp(L"a", L"b") < 0);
  CHECK(wcscmp(L"b", L"a") > 0);
  CHECK(wcsncmp(L"abcd", L"abce", 3) == 0);
  CHECK(wcscasecmp(L"AbC", L"aBc") == 0);
  CHECK(wcsncasecmp(L"ABX", L"abY", 2) == 0);
  CHECK(wcscoll(L"a", L"a") == 0);

  CHECKINT(*wcschr(L"hello", L'l'), L'l');
  CHECK(wcschr(L"hello", L'z') == 0);
  CHECK(wcsrchr(L"hello", L'l')[1] == L'o');
  CHECK(wcsstr(L"haystack", L"stack") != 0);
  CHECK(wcsstr(L"haystack", L"needle") == 0);
  CHECKINT(wcsspn(L"aabbcc", L"ab"), 4);
  CHECKINT(wcscspn(L"aabbcc", L"c"), 4);
  CHECK(*wcspbrk(L"hello", L"lo") == L'l');

  wcscpy(buf, L"a,b,,c");
  CHECK(wcscmp(wcstok(buf, L",", &save), L"a") == 0);
  CHECK(wcscmp(wcstok(0, L",", &save), L"b") == 0);
  CHECK(wcscmp(wcstok(0, L",", &save), L"c") == 0);
  CHECK(wcstok(0, L",", &save) == 0);

  wchar_t *dup = wcsdup(L"copy me");
  CHECK(dup && wcscmp(dup, L"copy me") == 0);
  free(dup);

  wmemset(buf, L'x', 4);
  buf[4] = 0;
  CHECK(wcscmp(buf, L"xxxx") == 0);
  wmemcpy(buf, L"abcd", 4);
  CHECK(wmemcmp(buf, L"abcd", 4) == 0);
  wmemmove(buf + 1, buf, 3);
  CHECK(wmemcmp(buf, L"aabc", 4) == 0);
  CHECK(wmemchr(L"abcd", L'c', 4) != 0);
  CHECK(wmemchr(L"abcd", L'z', 4) == 0);
  printf("wcs: the wide string family\n");
}

static void
test_wcwidth(void)
{
  CHECKINT(wcwidth(L'a'), 1);
  CHECKINT(wcwidth(L' '), 1);
  CHECKINT(wcwidth(0), 0);
  CHECKINT(wcwidth(L'\t'), -1);
  CHECKINT(wcwidth(0x7f), -1);
  CHECKINT(wcwidth(0x0301), 0);   // combining acute accent
  CHECKINT(wcwidth(0xfe0f), 0);   // a variation selector
  CHECKINT(wcwidth(0x4e2d), 2);   // a CJK ideograph
  CHECKINT(wcwidth(0xff21), 2);   // fullwidth A
  CHECKINT(wcwidth(0x1f600), 2);  // an emoji
  CHECKINT(wcwidth(0x00e9), 1);   // e with acute
  CHECKINT(wcswidth(L"ab", 2), 2);
  CHECKINT(wcswidth(L"a\x4e2d", 2), 3);
  CHECKINT(wcswidth(L"a\tb", 3), -1);
  printf("wcwidth: combining, wide and non-printing ranges\n");
}

static void
test_wctype(void)
{
  // ASCII has to agree with <ctype.h> exactly
  for (int c = 0; c < 128; c++) {
    if (!!iswalpha(c) != !!isalpha(c) || !!iswdigit(c) != !!isdigit(c) ||
        !!iswspace(c) != !!isspace(c) || !!iswpunct(c) != !!ispunct(c) ||
        !!iswupper(c) != !!isupper(c) || !!iswlower(c) != !!islower(c) ||
        !!iswcntrl(c) != !!iscntrl(c) || !!iswprint(c) != !!isprint(c) ||
        !!iswgraph(c) != !!isgraph(c) || !!iswxdigit(c) != !!isxdigit(c)) {
      printf("  FAIL %s:%d: wide and narrow classes differ for %d\n", __FILE__, __LINE__, c);
      failures++;
      break;
    }
  }
  checks++;

  CHECK(iswalpha(0x00e9));  // e with acute
  CHECK(iswlower(0x00e9));
  CHECK(iswupper(0x00c9));
  CHECK(!iswalpha(0x00d7)); // the multiplication sign is not a letter
  CHECK(iswpunct(0x00d7));
  CHECK(iswalpha(0x0141));  // L with stroke, in Latin Extended-A
  CHECK(iswalpha(0x4e2d));  // a CJK ideograph is a letter by block
  CHECK(!iswdigit(0x0660)); // only the ASCII digits are digits
  CHECK(iswspace(0x3000));  // the ideographic space
  CHECK(!iswspace(0x00a0)); // but not the no-break one
  CHECK(iswblank('\t') && !iswblank('\n'));

  CHECKINT(towupper(0x00e9), 0x00c9);
  CHECKINT(towlower(0x00c9), 0x00e9);
  CHECKINT(towupper(0x00ff), 0x0178);
  CHECKINT(towlower(0x0178), 0x00ff);
  CHECKINT(towupper(0x0101), 0x0100);
  CHECKINT(towlower(0x0100), 0x0101);
  CHECKINT(towlower(0x013a), 0x013a);
  CHECKINT(towupper(0x013a), 0x0139);
  CHECKINT(towupper(0x00df), 0x00df); // sharp s has no single capital
  CHECKINT(towlower(L'A'), L'a');

  CHECK(iswctype(L'a', wctype("alpha")));
  CHECK(!iswctype(L'1', wctype("alpha")));
  CHECK(iswctype(L'1', wctype("xdigit")));
  CHECKINT(wctype("nosuchclass"), 0);
  CHECK(!iswctype(L'a', wctype("nosuchclass")));
  CHECKINT(towctrans(L'a', wctrans("toupper")), L'A');
  CHECKINT(towctrans(L'A', wctrans("tolower")), L'a');
  CHECKINT(towctrans(L'a', wctrans("nosuch")), L'a');
  printf("wctype: ASCII classes, Latin-1 and Latin Extended-A case\n");
}

static void
test_iconv(void)
{
  char in[32], out[32];
  char *ip, *op;
  size_t il, ol;
  iconv_t cd;

  // UTF-8 to UTF-16LE, including a character that needs a surrogate pair
  cd = iconv_open("UTF-16LE", "UTF-8");
  CHECK(cd != (iconv_t)-1);
  strcpy(in, "hi\xe2\x82\xac\xf0\x9f\x98\x80"); // "hi", U+20AC, U+1F600
  ip = in;
  il = strlen(in);
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), 0);
  CHECKINT(op - out, 10);
  CHECK(memcmp(out, "h\0i\0\xac\x20\x3d\xd8\x00\xde", 10) == 0);
  CHECKINT(iconv_close(cd), 0);

  // and back again
  cd = iconv_open("utf_8", "utf-16le"); // the names are not case sensitive
  CHECK(cd != (iconv_t)-1);
  char utf16[10];
  memcpy(utf16, out, 10);
  ip = utf16;
  il = 10;
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), 0);
  *op = 0;
  CHECKSTR(out, in);
  iconv_close(cd);

  // UTF-32BE puts the code point in four bytes, biggest first
  cd = iconv_open("UTF-32BE", "UTF-8");
  ip = in;
  il = 1;
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), 0);
  CHECK(memcmp(out, "\0\0\0h", 4) == 0);
  iconv_close(cd);

  // Latin-1 holds the first 256 code points and nothing else, so a
  // character above them is substituted and counted.
  cd = iconv_open("ISO-8859-1", "UTF-8");
  strcpy(in, "a\xc3\xa9\xe2\x82\xac"); // "a", U+00E9, U+20AC
  ip = in;
  il = strlen(in);
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), 1);
  CHECKINT(op - out, 3);
  CHECK(memcmp(out, "a\xe9?", 3) == 0);
  iconv_close(cd);

  cd = iconv_open("UTF-8", "LATIN1");
  ip = in;
  strcpy(in, "a\xe9");
  il = 2;
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), 0);
  *op = 0;
  CHECKSTR(out, "a\xc3\xa9");
  iconv_close(cd);

  // ASCII refuses what it cannot represent on the way in
  cd = iconv_open("UTF-8", "ASCII");
  strcpy(in, "a\xe9");
  ip = in;
  il = 2;
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), (size_t)-1);
  CHECKINT(errno, EILSEQ);
  CHECKINT(ip - in, 1); // stopped on the offending byte
  iconv_close(cd);

  // the three errors POSIX names
  cd = iconv_open("UTF-16LE", "UTF-8");
  strcpy(in, "\xe2\x82"); // the first two bytes of a three-byte character
  ip = in;
  il = 2;
  op = out;
  ol = sizeof(out);
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), (size_t)-1);
  CHECKINT(errno, EINVAL);
  CHECKINT(il, 2); // nothing consumed: the caller brings the rest

  strcpy(in, "abc");
  ip = in;
  il = 3;
  op = out;
  ol = 3; // room for one character and half of another
  CHECKINT(iconv(cd, &ip, &il, &op, &ol), (size_t)-1);
  CHECKINT(errno, E2BIG);
  CHECKINT(il, 2);
  CHECKINT(ol, 1);
  CHECKINT(iconv(cd, 0, 0, 0, 0), 0); // the reset that has nothing to do
  iconv_close(cd);

  CHECK(iconv_open("NO-SUCH-CHARSET", "UTF-8") == (iconv_t)-1);
  CHECKINT(errno, EINVAL);
  printf("iconv: UTF-8, UTF-16, UTF-32, Latin-1, ASCII, both ways\n");
}

static int
intcmp(const void *a, const void *b)
{
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

static int walked;

static void
countnode(const void *node, VISIT v, int depth)
{
  (void)node;
  (void)depth;
  if (v == preorder || v == leaf)
    walked++;
}

struct qel {
  struct qel *next, *prev;
  int id;
};

static void
test_search(void)
{
  // the hash table
  CHECK(hcreate(32) != 0);
  static char k1[] = "alpha", k2[] = "beta", k3[] = "gamma";
  ENTRY e;
  e.key = k1;
  e.data = (void *)1;
  CHECK(hsearch(e, ENTER) != 0);
  e.key = k2;
  e.data = (void *)2;
  CHECK(hsearch(e, ENTER) != 0);
  e.key = k1;
  e.data = (void *)99;
  CHECKINT((long)hsearch(e, ENTER)->data, 1); // ENTER never replaces
  e.key = k2;
  ENTRY *found = hsearch(e, FIND);
  CHECK(found != 0);
  CHECKINT((long)found->data, 2);
  e.key = k3;
  CHECK(hsearch(e, FIND) == 0);
  hdestroy();

  // the same table through the _r form, which a library must use
  struct hsearch_data tab;
  memset(&tab, 0, sizeof(tab));
  CHECK(hcreate_r(8, &tab) != 0);
  ENTRY *ret = 0;
  e.key = k1;
  e.data = (void *)7;
  CHECK(hsearch_r(e, ENTER, &ret, &tab) != 0);
  CHECK(hsearch_r(e, FIND, &ret, &tab) != 0);
  CHECKINT((long)ret->data, 7);
  e.key = k3;
  CHECKINT(hsearch_r(e, FIND, &ret, &tab), 0);
  hdestroy_r(&tab);

  // the tree
  static int keys[64];
  void *root = 0;
  for (int i = 0; i < 64; i++) {
    // a scattered order, so the unbalanced tree gets some shape
    keys[i] = (i * 37) % 64;
    CHECK(tsearch(&keys[i], &root, intcmp) != 0);
  }
  for (int i = 0; i < 64; i++) {
    void **p = tfind(&keys[i], &root, intcmp);
    CHECK(p != 0 && **(int **)p == keys[i]);
  }
  int missing = 1000;
  CHECK(tfind(&missing, &root, intcmp) == 0);
  CHECK(tsearch(&keys[0], &root, intcmp) != 0); // already there, not added twice
  walked = 0;
  twalk(root, countnode);
  CHECKINT(walked, 64);
  CHECK(tdelete(&keys[5], &root, intcmp) != 0);
  CHECK(tfind(&keys[5], &root, intcmp) == 0);
  CHECK(tdelete(&missing, &root, intcmp) == 0);
  walked = 0;
  twalk(root, countnode);
  CHECKINT(walked, 63);
  tdestroy(root, 0);

  // the linear array
  int table[8] = {0};
  size_t nel = 0;
  int v = 11;
  CHECK(lsearch(&v, table, &nel, sizeof(int), intcmp) != 0);
  CHECKINT(nel, 1);
  v = 22;
  CHECK(lsearch(&v, table, &nel, sizeof(int), intcmp) != 0);
  CHECKINT(nel, 2);
  v = 11;
  CHECK(lsearch(&v, table, &nel, sizeof(int), intcmp) != 0);
  CHECKINT(nel, 2); // found, so nothing was added
  CHECK(lfind(&v, table, &nel, sizeof(int), intcmp) != 0);
  v = 33;
  CHECK(lfind(&v, table, &nel, sizeof(int), intcmp) == 0);

  // the two-pointer queue
  struct qel a = {0, 0, 1}, b = {0, 0, 2}, c = {0, 0, 3};
  insque(&a, 0);
  insque(&b, &a);
  insque(&c, &b);
  CHECKINT(a.next->id, 2);
  CHECKINT(c.prev->id, 2);
  CHECK(c.next == 0);
  remque(&b);
  CHECKINT(a.next->id, 3);
  CHECKINT(c.prev->id, 1);
  printf("search: hash table, tree, linear array, queues\n");
}

static void
test_catgets(void)
{
  nl_catd cat = catopen("wctest", 0);

  // there are no catalogues on this system, so the default string is
  // the only answer there can be.
  CHECK(cat == (nl_catd)-1);
  CHECKSTR(catgets(cat, 1, 1, "the default"), "the default");
  CHECKINT(catclose(cat), -1);
  printf("nl_types: catopen fails and catgets returns the default\n");
}

static void
test_strfmon(void)
{
  char buf[64];

  CHECKINT(strfmon(buf, sizeof(buf), "%n", 1234.567), 7);
  CHECKSTR(buf, "1234.57");
  CHECK(strfmon(buf, sizeof(buf), "%i", 1234.567) > 0);
  CHECKSTR(buf, "1234.57"); // the C locale has no currency symbol either way
  CHECK(strfmon(buf, sizeof(buf), "%.0n", 5.0) > 0);
  CHECKSTR(buf, "5");
  CHECK(strfmon(buf, sizeof(buf), "%.3n", 1.5) > 0);
  CHECKSTR(buf, "1.500");
  CHECK(strfmon(buf, sizeof(buf), "%n", -5.0) > 0);
  CHECKSTR(buf, "-5.00");
  CHECK(strfmon(buf, sizeof(buf), "%(n", -5.0) > 0);
  CHECKSTR(buf, "(5.00)");
  CHECK(strfmon(buf, sizeof(buf), "%(n", 5.0) > 0);
  CHECKSTR(buf, "5.00 "); // the column stays as wide as the negative form
  CHECK(strfmon(buf, sizeof(buf), "%11n", 1234.567) > 0);
  CHECKSTR(buf, "    1234.57");
  CHECK(strfmon(buf, sizeof(buf), "%-11n|", 1234.567) > 0);
  CHECKSTR(buf, "1234.57    |");
  CHECK(strfmon(buf, sizeof(buf), "%#5n", 5.0) > 0);
  CHECKSTR(buf, "    5.00");
  CHECK(strfmon(buf, sizeof(buf), "%=*#5n", 3.0) > 0);
  CHECKSTR(buf, "****3.00");
  CHECK(strfmon(buf, sizeof(buf), "cost: %n each", 2.5) > 0);
  CHECKSTR(buf, "cost: 2.50 each");
  CHECK(strfmon(buf, sizeof(buf), "100%%") > 0);
  CHECKSTR(buf, "100%");
  CHECKINT(strfmon(buf, 4, "%n", 1234.567), -1);
  CHECKINT(errno, E2BIG);
  CHECKINT(strfmon_l(buf, sizeof(buf), 0, "%n", 1.0), 4);
  CHECKSTR(buf, "1.00");
  printf("strfmon: signs, fills, widths and both precisions\n");
}

static void
test_spawn(void)
{
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t at;
  pid_t pid;
  int st;

  // run /echo with its output in a file, which tests the file actions
  // and gives something to check afterwards.
  CHECKINT(posix_spawn_file_actions_init(&fa), 0);
  CHECKINT(posix_spawn_file_actions_addopen(&fa, 1, "wctest.tmp",
                                            O_CREATE | O_WRONLY | O_TRUNC, 0666),
           0);
  char *argv[] = {"echo", "spawned", 0};
  CHECKINT(posix_spawn(&pid, "/bin/echo", &fa, 0, argv, environ), 0);
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st));
  CHECKINT(WEXITSTATUS(st), 0);
  int fd = open("wctest.tmp", O_RDONLY);
  CHECK(fd >= 0);
  char out[32];
  int n = read(fd, out, sizeof(out) - 1);
  close(fd);
  unlink("wctest.tmp");
  CHECK(n > 0);
  if (n > 0) {
    out[n] = 0;
    CHECKSTR(out, "spawned\n");
  }
  CHECKINT(posix_spawn_file_actions_destroy(&fa), 0);

  // the attributes: a new process group, the mask and the dispositions
  // put back, and vfork asked for.
  CHECKINT(posix_spawnattr_init(&at), 0);
  sigset_t def, mask, got;
  sigemptyset(&def);
  sigaddset(&def, SIGUSR1);
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR2);
  CHECKINT(posix_spawnattr_setsigdefault(&at, &def), 0);
  CHECKINT(posix_spawnattr_setsigmask(&at, &mask), 0);
  CHECKINT(posix_spawnattr_setpgroup(&at, 0), 0);
  short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP |
                POSIX_SPAWN_USEVFORK;
  CHECKINT(posix_spawnattr_setflags(&at, flags), 0);
  short back = 0;
  CHECKINT(posix_spawnattr_getflags(&at, &back), 0);
  CHECKINT(back, flags);
  pid_t pg = -1;
  CHECKINT(posix_spawnattr_getpgroup(&at, &pg), 0);
  CHECKINT(pg, 0);
  CHECKINT(posix_spawnattr_getsigmask(&at, &got), 0);
  CHECK(sigismember(&got, SIGUSR2));
  CHECKINT(posix_spawnattr_getsigdefault(&at, &got), 0);
  CHECK(sigismember(&got, SIGUSR1));

  // this one goes through PATH, which finds /echo
  char *quiet[] = {"echo", 0};
  CHECKINT(posix_spawnp(&pid, "echo", 0, &at, quiet, environ), 0);
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECKINT(posix_spawnattr_destroy(&at), 0);

  // a program that is not there is reported as the child's own errno,
  // through the pipe, rather than as a strange exit status.
  CHECKINT(posix_spawn(&pid, "/no/such/program", 0, 0, argv, environ), ENOENT);
  printf("spawn: file actions, attributes, PATH and the error pipe\n");
}

static ucontext_t mainctx, actx, bctx, retctx;
static char *astack, *bstack, *cstack;
static volatile int pings;
static volatile int argsum;

#define CORO_STACK 16384
#define PINGS 2000

static void
coro_a(void)
{
  for (int i = 0; i < PINGS; i++) {
    pings++;
    swapcontext(&actx, &bctx);
  }
}

static void
coro_b(void)
{
  for (int i = 0; i < PINGS; i++) {
    pings++;
    swapcontext(&bctx, &actx);
  }
}

static void
coro_args(int a, int b, int c)
{
  argsum = a + b + c;
}

static void
test_ucontext(void)
{
  volatile int passes = 0;
  ucontext_t uc;

  // setcontext of a context getcontext saved comes back out of
  // getcontext a second time, returning zero again. the check is kept
  // outside the macro because this line runs three times.
  int r = getcontext(&uc);
  passes++;
  if (passes < 3)
    setcontext(&uc);
  CHECKINT(r, 0);
  CHECKINT(passes, 3);

  // a function with arguments, which makecontext puts in place
  astack = malloc(CORO_STACK);
  bstack = malloc(CORO_STACK);
  cstack = malloc(CORO_STACK);
  CHECK(astack && bstack && cstack);

  ucontext_t cctx;
  CHECKINT(getcontext(&cctx), 0);
  cctx.uc_stack.ss_sp = cstack;
  cctx.uc_stack.ss_size = CORO_STACK;
  cctx.uc_link = &retctx;
  makecontext(&cctx, (void (*)(void))coro_args, 3, 11, 22, 33);
  CHECKINT(swapcontext(&retctx, &cctx), 0); // returns when coro_args does
  CHECKINT(argsum, 66);

  // two coroutines handing control back and forth
  CHECKINT(getcontext(&actx), 0);
  actx.uc_stack.ss_sp = astack;
  actx.uc_stack.ss_size = CORO_STACK;
  actx.uc_link = &mainctx;
  makecontext(&actx, coro_a, 0);

  CHECKINT(getcontext(&bctx), 0);
  bctx.uc_stack.ss_sp = bstack;
  bctx.uc_stack.ss_size = CORO_STACK;
  bctx.uc_link = &mainctx;
  makecontext(&bctx, coro_b, 0);

  pings = 0;
  CHECKINT(swapcontext(&mainctx, &actx), 0);
  CHECKINT(pings, 2 * PINGS);
  free(astack);
  free(bstack);
  free(cstack);
  printf("ucontext: %d switches between two coroutines\n", 2 * PINGS);
}

static void
test_syslog(void)
{
  openlog("wctest", LOG_PID, LOG_USER);
  CHECKINT(setlogmask(LOG_UPTO(LOG_WARNING)), 0xff);
  CHECKINT(setlogmask(0), LOG_UPTO(LOG_WARNING)); // a zero mask only asks
  syslog(LOG_DEBUG, "this one is below the mask and must not appear");
  CHECKINT(setlogmask(LOG_UPTO(LOG_DEBUG)), LOG_UPTO(LOG_WARNING));
  errno = ENOENT;
  syslog(LOG_INFO, "wctest ran %d checks so far, errno says %m", checks);
  CHECKINT(errno, ENOENT); // syslog leaves the caller's errno alone
  closelog();
  printf("syslog: one message above, to /dev/log or to standard error\n");
}

int
main(int argc, char *argv[])
{
  int only = argc > 1;

#define GROUP(name)                                                                                \
  if (!only || strcmp(argv[1], #name) == 0)                                                        \
  test_##name()

  GROUP(mb);
  GROUP(wcs);
  GROUP(wcwidth);
  GROUP(wctype);
  GROUP(iconv);
  GROUP(search);
  GROUP(catgets);
  GROUP(strfmon);
  GROUP(spawn);
  GROUP(ucontext);
  GROUP(syslog);

  printf("wctest: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("ALL TESTS PASSED\n");
  exit(failures != 0);
}
