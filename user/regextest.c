//
// regextest: the pattern-matching library's own tests -- <regex.h>,
// <fnmatch.h>, <glob.h> and <wordexp.h>.
//
// every group prints what it checked, and a failure says where it was
// and what it expected, so a run that goes wrong is a bug report by
// itself. the glob group makes its own files in the working directory
// and takes them away again.
//
#include <regex.h>
#include <fnmatch.h>
#include <glob.h>
#include <wordexp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int checks, failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// ---- helpers ----

// compile `pat`, run it on `str`, and check the answer and the overall
// match against what the caller expects.
static void
m_(const char *pat, const char *str, int cflags, int eflags, int want, long so, long eo, int line)
{
  regex_t re;
  regmatch_t pm[10];
  int r = regcomp(&re, pat, cflags);

  checks++;
  if (r != 0) {
    char buf[64];
    regerror(r, &re, buf, sizeof(buf));
    printf("  FAIL %s:%d: /%s/ did not compile: %s\n", __FILE__, line, pat, buf);
    failures++;
    return;
  }
  for (int i = 0; i < 10; i++)
    pm[i].rm_so = pm[i].rm_eo = -2;
  r = regexec(&re, str, 10, pm, eflags);
  if (r != want) {
    printf("  FAIL %s:%d: /%s/ on \"%s\": got %d, wanted %d\n", __FILE__, line, pat, str, r, want);
    failures++;
  } else if (want == 0 && (pm[0].rm_so != so || pm[0].rm_eo != eo)) {
    printf("  FAIL %s:%d: /%s/ on \"%s\": matched [%ld,%ld), wanted [%ld,%ld)\n", __FILE__, line,
           pat, str, (long)pm[0].rm_so, (long)pm[0].rm_eo, so, eo);
    failures++;
  }
  regfree(&re);
}

// the same, checking one subexpression instead
static void
sub_(const char *pat, const char *str, int cflags, int n, long so, long eo, int line)
{
  regex_t re;
  regmatch_t pm[10];

  checks++;
  if (regcomp(&re, pat, cflags) != 0) {
    printf("  FAIL %s:%d: /%s/ did not compile\n", __FILE__, line, pat);
    failures++;
    return;
  }
  for (int i = 0; i < 10; i++)
    pm[i].rm_so = pm[i].rm_eo = -2;
  if (regexec(&re, str, 10, pm, 0) != 0) {
    printf("  FAIL %s:%d: /%s/ on \"%s\": no match\n", __FILE__, line, pat, str);
    failures++;
  } else if (pm[n].rm_so != so || pm[n].rm_eo != eo) {
    printf("  FAIL %s:%d: /%s/ on \"%s\": sub %d is [%ld,%ld), wanted [%ld,%ld)\n", __FILE__, line,
           pat, str, n, (long)pm[n].rm_so, (long)pm[n].rm_eo, so, eo);
    failures++;
  }
  regfree(&re);
}

static void
cerr_(const char *pat, int cflags, int want, int line)
{
  regex_t re;
  int r = regcomp(&re, pat, cflags);

  checks++;
  if (r != want) {
    printf("  FAIL %s:%d: regcomp(/%s/) gave %d, wanted %d\n", __FILE__, line, pat, r, want);
    failures++;
  }
  if (r == 0)
    regfree(&re);
}

#define M(pat, str, cf, so, eo) m_(pat, str, cf, 0, 0, so, eo, __LINE__)
#define ME(pat, str, cf, ef, so, eo) m_(pat, str, cf, ef, 0, so, eo, __LINE__)
#define NM(pat, str, cf) m_(pat, str, cf, 0, REG_NOMATCH, 0, 0, __LINE__)
#define NME(pat, str, cf, ef) m_(pat, str, cf, ef, REG_NOMATCH, 0, 0, __LINE__)
#define SUB(pat, str, cf, n, so, eo) sub_(pat, str, cf, n, so, eo, __LINE__)
#define CERR(pat, cf, e) cerr_(pat, cf, e, __LINE__)
#define E REG_EXTENDED

// ---- basic regular expressions ----

static void
test_bre(void)
{
  int before = checks;

  M("abc", "abc", 0, 0, 3);
  M("abc", "xxabcyy", 0, 2, 5);
  NM("abc", "ab", 0);
  NM("abc", "", 0);
  M("", "abc", 0, 0, 0);
  M("a.c", "abc", 0, 0, 3);
  M("a.c", "a.c", 0, 0, 3);
  NM("a.c", "ac", 0);
  M(".", "x", 0, 0, 1);
  NM(".", "", 0);

  // the star, which is greedy and may match nothing
  M("a*", "aaa", 0, 0, 3);
  M("a*", "bbb", 0, 0, 0);
  M("ba*", "xbaaay", 0, 1, 5);
  M("a*b", "aaab", 0, 0, 4);
  M(".*", "hello", 0, 0, 5);
  M("a.*c", "abcbc", 0, 0, 5);
  M("\\(ab\\)*", "ababab", 0, 0, 6);
  M("\\(ab\\)*c", "ababc", 0, 0, 5);

  // in a BRE the other repetition operators are ordinary characters
  M("a+", "a+", 0, 0, 2);
  M("a?", "a?", 0, 0, 2);
  M("a|b", "a|b", 0, 0, 3);
  M("*a", "*a", 0, 0, 2);
  M("^*a", "*a", 0, 0, 2);
  M("a{2}", "a{2}", 0, 0, 4);

  // anchors, which are only anchors where a BRE says they are
  M("^abc", "abcd", 0, 0, 3);
  NM("^abc", "xabc", 0);
  M("abc$", "xabc", 0, 1, 4);
  NM("abc$", "abcx", 0);
  M("^abc$", "abc", 0, 0, 3);
  M("^$", "", 0, 0, 0);
  M("a$b", "xa$b", 0, 1, 4);
  M("a^b", "xa^b", 0, 1, 4);
  M("$a", "x$a", 0, 1, 3);

  // grouping and the GNU alternation a BRE also understands
  M("\\(abc\\)", "abc", 0, 0, 3);
  M("a\\|b", "zb", 0, 1, 2);
  M("\\(a\\|b\\)c", "bc", 0, 0, 2);
  M("\\(\\(a\\)b\\)c", "abc", 0, 0, 3);
  M("a\\+", "aaa", 0, 0, 3);
  M("a\\?b", "ab", 0, 0, 2);

  // intervals, spelled with backslashed braces
  M("a\\{3\\}", "aaaa", 0, 0, 3);
  NM("a\\{3\\}", "aa", 0);
  M("a\\{2,\\}", "aaaa", 0, 0, 4);
  M("a\\{2,3\\}", "aaaa", 0, 0, 3);
  M("a\\{0,\\}", "b", 0, 0, 0);
  M("\\(ab\\)\\{2\\}", "ababab", 0, 0, 4);

  // escapes
  M("a\\.c", "a.c", 0, 0, 3);
  NM("a\\.c", "abc", 0);
  M("\\*", "*", 0, 0, 1);
  M("\\\\", "\\", 0, 0, 1);
  M("a\\tb", "a\tb", 0, 0, 3);
  M("\\w\\w", "ab", 0, 0, 2);
  M("\\s", " ", 0, 0, 1);
  M("\\S", " x", 0, 1, 2);
  M("\\W", "a+", 0, 1, 2);

  printf("regex BRE: %d checks\n", checks - before);
}

// ---- extended regular expressions ----

static void
test_ere(void)
{
  int before = checks;

  M("a+", "baaa", E, 1, 4);
  NM("a+", "b", E);
  M("a?", "b", E, 0, 0);
  M("a?b", "ab", E, 0, 2);
  M("ab?c", "ac", E, 0, 2);
  M("(ab)+", "ababab", E, 0, 6);
  M("(ab)+c", "ababc", E, 0, 5);
  M("a|b", "zb", E, 1, 2);
  M("abc|de", "xde", E, 1, 3);
  M("a|b|c|d", "xxd", E, 2, 3);
  M("(a|b)+", "abab", E, 0, 4);
  M("()", "a", E, 0, 0);
  M("(a)(b)(c)", "abc", E, 0, 3);
  M("((a|b)c)+", "acbc", E, 0, 4);

  // intervals
  M("a{3}", "aaaa", E, 0, 3);
  M("a{2,}", "aaaa", E, 0, 4);
  M("a{2,3}", "aaaaa", E, 0, 3);
  NM("a{2,3}", "a", E);
  M("(ab){2,3}", "ababab", E, 0, 6);
  M("a{0}b", "b", E, 0, 1);
  M("x{0,3}y", "xxy", E, 0, 3);
  M("a{1,2}b{1,2}", "aabb", E, 0, 4);
  M("a{", "a{", E, 0, 2);       // a brace that starts no interval
  M("a{x}", "a{x}", E, 0, 4);

  // stacked operators
  M("a**", "aaa", E, 0, 3);
  M("(a*)*", "aaa", E, 0, 3);
  M("(a?)+", "aa", E, 0, 2);
  M("(a+)+", "aaa", E, 0, 3);

  // anchors
  M("^a", "ab", E, 0, 1);
  NM("^a", "ba", E);
  M("a$", "ba", E, 1, 2);
  M("^$", "", E, 0, 0);
  M("$", "abc", E, 3, 3);
  M("^", "abc", E, 0, 0);
  M("(^a|b)", "b", E, 0, 1);

  // escaped metacharacters
  M("a\\+", "a+", E, 0, 2);
  M("\\(", "(", E, 0, 1);
  M("\\|", "|", E, 0, 1);
  M("\\{", "{", E, 0, 1);
  M("\\$", "$", E, 0, 1);
  M("a\\*b", "a*b", E, 0, 3);

  // the leftmost longest match POSIX asks for, not the first one found
  M("a|ab", "xab", E, 1, 3);
  M("ab|a", "xab", E, 1, 3);
  M("(a|ab)(c|bcd)", "abcd", E, 0, 4);
  M("(wee|week)(knights|nights)", "weeknights", E, 0, 10);
  M("x*", "xxx", E, 0, 3);
  M("(a*)(ab)?", "aaa", E, 0, 3);

  // patterns that make a backtracking matcher take for ever
  NM("a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", E);
  NM("(a*)*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", E);
  NM("(a|aa)+c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", E);
  M("(|a)*b", "aab", E, 0, 3);

  printf("regex ERE: %d checks\n", checks - before);
}

// ---- bracket expressions ----

static void
test_classes(void)
{
  int before = checks;

  M("[abc]", "xb", 0, 1, 2);
  NM("[abc]", "xyz", 0);
  M("[a-c]*", "abcd", 0, 0, 3);
  M("[^a-c]", "abx", 0, 2, 3);
  M("[^a-c]*", "xyzabc", 0, 0, 3);
  M("[]]", "]", 0, 0, 1);
  M("[]a]*", "]a]b", 0, 0, 3);
  M("[^]]*", "ab]", 0, 0, 2);
  M("[a-]*", "-a-b", 0, 0, 3);
  M("[-a]*", "-a-b", 0, 0, 3);
  M("[a^]*", "a^b", 0, 0, 2);
  M("[.]", ".", 0, 0, 1);
  NM("[.]", "x", 0);
  M("[*+?]*", "*+?a", 0, 0, 3);
  M("[0-9][0-9]", "ab42", 0, 2, 4);
  M("[A-Za-z]*", "abZ9", 0, 0, 3);

  // the named classes
  M("[[:alpha:]]*", "abc9", 0, 0, 3);
  M("[[:digit:]]*", "42x", 0, 0, 2);
  M("[[:alnum:]]*", "a9-", 0, 0, 2);
  M("[[:upper:]]*", "ABc", 0, 0, 2);
  M("[[:lower:]]*", "abC", 0, 0, 2);
  M("[[:space:]]*", " \t\nx", 0, 0, 3);
  M("[[:xdigit:]]*", "9afg", 0, 0, 3);
  M("[[:punct:]]*", "!,.a", 0, 0, 3);
  M("[[:print:]]*", "ab\x01", 0, 0, 2);
  M("[[:graph:]]*", "ab c", 0, 0, 2);
  M("[[:cntrl:]]*", "\x01\x02z", 0, 0, 2);
  M("[[:blank:]]*", " \tx", 0, 0, 2);
  M("[[:alpha:][:digit:]]*", "a9!", 0, 0, 2);
  M("[^[:digit:]]*", "ab9", 0, 0, 2);
  M("[[:digit:]abc]*", "9ax", 0, 0, 2);

  // collating elements and equivalence classes, of one character each
  M("[[.a.]]", "a", 0, 0, 1);
  M("[[=a=]]", "a", 0, 0, 1);
  M("[[.a.]-c]*", "abcd", 0, 0, 3);

  // a bracket expression in an ERE works the same way
  M("[abc]+", "xbcay", E, 1, 4);
  M("[^ ]+", " ab ", E, 1, 3);

  printf("regex bracket expressions: %d checks\n", checks - before);
}

// ---- submatches ----

static void
test_submatch(void)
{
  int before = checks;
  regex_t re;
  regmatch_t pm[10];

  SUB("\\(a*\\)\\(b*\\)", "aabbb", 0, 0, 0, 5);
  SUB("\\(a*\\)\\(b*\\)", "aabbb", 0, 1, 0, 2);
  SUB("\\(a*\\)\\(b*\\)", "aabbb", 0, 2, 2, 5);
  SUB("(a+)(b+)", "xaabbb", E, 1, 1, 3);
  SUB("(a+)(b+)", "xaabbb", E, 2, 3, 6);
  SUB("((a)(b))", "xab", E, 1, 1, 3);
  SUB("((a)(b))", "xab", E, 2, 1, 2);
  SUB("((a)(b))", "xab", E, 3, 2, 3);
  SUB("(a)|(b)", "b", E, 2, 0, 1);
  SUB("(a)|(b)", "b", E, 1, -1, -1);
  SUB("(a)(b)?", "a", E, 2, -1, -1);
  SUB("(a*)(a*)", "aaa", E, 1, 0, 3);   // the first one is greedy
  SUB("(a*)(a*)", "aaa", E, 2, 3, 3);
  SUB("(a|ab)(c|bcd)", "abcd", E, 1, 0, 1);
  SUB("(a|ab)(c|bcd)", "abcd", E, 2, 1, 4);
  SUB("(ab)+", "ababab", E, 1, 4, 6);   // the last time round the loop wins
  SUB("(a)(b)(c)(d)(e)(f)(g)(h)(i)", "abcdefghi", E, 9, 8, 9);
  SUB("x(y)?z", "xz", E, 1, -1, -1);
  SUB("(.*)/", "a/b/c/", E, 1, 0, 5);

  // a subexpression the caller did not ask about, and one that is not there
  CHECK(regcomp(&re, "(a)(b)", E) == 0);
  CHECK(re.re_nsub == 2);
  for (int i = 0; i < 10; i++)
    pm[i].rm_so = pm[i].rm_eo = -2;
  CHECK(regexec(&re, "ab", 5, pm, 0) == 0);
  CHECK(pm[3].rm_so == -1 && pm[3].rm_eo == -1);
  CHECK(pm[4].rm_so == -1);
  CHECK(regexec(&re, "ab", 0, 0, 0) == 0);
  regfree(&re);

  // REG_NOSUB, where the offsets are not reported at all
  CHECK(regcomp(&re, "(a)(b)", E | REG_NOSUB) == 0);
  pm[0].rm_so = -7;
  CHECK(regexec(&re, "zab", 3, pm, 0) == 0);
  CHECK(pm[0].rm_so == -7);
  CHECK(regexec(&re, "zz", 3, pm, 0) == REG_NOMATCH);
  regfree(&re);

  printf("regex submatches: %d checks\n", checks - before);
}

// ---- back-references ----

static void
test_backref(void)
{
  int before = checks;

  M("\\(a\\)\\1", "aa", 0, 0, 2);
  NM("\\(a\\)\\1", "ab", 0);
  M("\\(a*\\)\\1", "aaaa", 0, 0, 4);
  M("\\(ab\\)c\\1", "xabcab", 0, 1, 6);
  M("\\(.\\)\\1", "xaay", 0, 1, 3);
  NM("\\(.\\)\\1", "abc", 0);
  M("\\(.*\\)\\1", "abcabc", 0, 0, 6);
  M("\\(a\\)\\(b\\)\\2\\1", "abba", 0, 0, 4);
  M("\\(a\\)\\1*", "aaa", 0, 0, 3);
  M("^\\(.\\)\\(.\\)\\2\\1$", "abba", 0, 0, 4);
  NM("^\\(.\\)\\(.\\)\\2\\1$", "abab", 0);
  M("(a)\\1", "aa", E, 0, 2);
  M("(ab|cd)\\1", "cdcd", E, 0, 4);
  M("\\(a\\)\\1", "AA", REG_ICASE, 0, 2);
  M("\\(a\\)\\1", "aA", REG_ICASE, 0, 2);
  SUB("\\(a*\\)\\1", "aaaa", 0, 1, 0, 2);
  SUB("\\(a\\)\\(b\\)\\2\\1", "abba", 0, 2, 1, 2);

  // a back-reference to a group that matched nothing this time round
  NM("\\(a\\)\\?b\\1", "b", 0);

  printf("regex back-references: %d checks\n", checks - before);
}

// ---- the flags ----

static void
test_flags(void)
{
  int before = checks;

  // REG_ICASE
  M("abc", "xABC", REG_ICASE, 1, 4);
  M("ABC", "xabc", REG_ICASE, 1, 4);
  M("[a-c]+", "XABC", REG_ICASE | E, 1, 4);
  M("[^a-c]+", "abcX", REG_ICASE | E, 3, 4);
  M("[[:lower:]]+", "AB", REG_ICASE | E, 0, 2);
  NM("abc", "ABC", 0);

  // REG_NEWLINE: a newline ends a line for the anchors and hides from
  // the wildcards
  M("^b", "a\nb", REG_NEWLINE, 2, 3);
  M("a$", "a\nb", REG_NEWLINE, 0, 1);
  NM("^b", "a\nb", 0);
  NM("a$", "a\nb", 0);
  M(".*", "ab\ncd", REG_NEWLINE, 0, 2);
  M(".*", "ab\ncd", 0, 0, 5);
  M("[^x]*", "ab\ncd", REG_NEWLINE, 0, 2);
  M("[^x]*", "ab\ncd", 0, 0, 5);
  M("^$", "a\n\nb", REG_NEWLINE, 2, 2);

  // REG_NOTBOL and REG_NOTEOL
  ME("abc", "abc", 0, REG_NOTBOL, 0, 3);
  NME("^abc", "abc", 0, REG_NOTBOL);
  NME("abc$", "abc", 0, REG_NOTEOL);
  ME("^b", "a\nb", REG_NEWLINE, REG_NOTBOL, 2, 3);
  ME("a$", "a\nb", REG_NEWLINE, REG_NOTEOL, 0, 1);
  NME("^a", "a\nb", REG_NEWLINE, REG_NOTBOL);

  printf("regex flags: %d checks\n", checks - before);
}

// ---- the error returns ----

static void
test_errors(void)
{
  int before = checks;
  char buf[64];
  regex_t re;

  CERR("[abc", 0, REG_EBRACK);
  CERR("[]", 0, REG_EBRACK);
  CERR("[^]", E, REG_EBRACK);
  CERR("[[:alpha:]", E, REG_EBRACK);
  CERR("a\\", 0, REG_EESCAPE);
  CERR("a\\", E, REG_EESCAPE);
  CERR("(a", E, REG_EPAREN);
  CERR("a)", E, REG_EPAREN);
  CERR("\\(a", 0, REG_EPAREN);
  CERR("a{1", E, REG_EBRACE);
  CERR("a\\{1", 0, REG_EBRACE);
  CERR("a{2,1}", E, REG_BADBR);
  CERR("a\\{2,1\\}", 0, REG_BADBR);
  CERR("a{1000}", E, REG_BADBR);
  CERR("[z-a]", E, REG_ERANGE);
  CERR("[[:foo:]]", E, REG_ECTYPE);
  CERR("[[:foo:]]", 0, REG_ECTYPE);
  CERR("[[.ab.]]", E, REG_ECOLLATE);
  CERR("*a", E, REG_BADRPT);
  CERR("+a", E, REG_BADRPT);
  CERR("?a", E, REG_BADRPT);
  CERR("{1}a", E, REG_BADRPT);
  CERR("\\1", 0, REG_ESUBREG);
  CERR("\\(a\\)\\2", 0, REG_ESUBREG);
  CERR("(a)\\2", E, REG_ESUBREG);

  // the ones that are not errors
  CERR("*a", 0, 0);
  CERR("a{1,2}", E, 0);
  CERR("[a-]", 0, 0);
  CERR("a{x", E, 0);
  CERR("", 0, 0);
  CERR("()", E, 0);

  // regerror, which always says something and always terminates
  CHECK(regerror(REG_NOMATCH, 0, buf, sizeof(buf)) > 1);
  CHECK(strcmp(buf, "No match") == 0);
  CHECK(regerror(REG_ESPACE, 0, buf, sizeof(buf)) > 1);
  memset(buf, 'x', sizeof(buf));
  CHECK(regerror(REG_NOMATCH, 0, buf, 4) == 9);
  CHECK(strcmp(buf, "No ") == 0);
  CHECK(regerror(REG_NOMATCH, 0, 0, 0) == 9);
  CHECK(regcomp(&re, "[abc", 0) == REG_EBRACK);
  CHECK(regerror(REG_EBRACK, &re, buf, sizeof(buf)) > 1);

  printf("regex errors: %d checks\n", checks - before);
}

// ---- fnmatch ----

static void
fn_(const char *pat, const char *str, int flags, int want, int line)
{
  int r = fnmatch(pat, str, flags);

  checks++;
  if (r != want) {
    printf("  FAIL %s:%d: fnmatch(\"%s\", \"%s\", %d) = %d, wanted %d\n", __FILE__, line, pat, str,
           flags, r, want);
    failures++;
  }
}

#define FN(pat, str, flags) fn_(pat, str, flags, 0, __LINE__)
#define FNN(pat, str, flags) fn_(pat, str, flags, FNM_NOMATCH, __LINE__)

static void
test_fnmatch(void)
{
  int before = checks;

  FN("", "", 0);
  FNN("", "a", 0);
  FN("a", "a", 0);
  FNN("a", "b", 0);
  FN("*", "", 0);
  FN("*", "anything", 0);
  FN("a*", "abc", 0);
  FN("*c", "abc", 0);
  FN("a*c", "abc", 0);
  FN("a*c", "ac", 0);
  FNN("a*c", "abd", 0);
  FN("*.c", "hello.c", 0);
  FNN("*.c", "hello.h", 0);
  FN("?", "a", 0);
  FNN("?", "", 0);
  FNN("?", "ab", 0);
  FN("a?c", "abc", 0);
  FN("???", "abc", 0);
  FN("**", "abc", 0);
  FN("a**b", "axxb", 0);

  // the pattern that costs a naive matcher exponential time
  FNN("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", 0);
  FN("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", 0);

  // bracket expressions
  FN("[abc]", "b", 0);
  FNN("[abc]", "d", 0);
  FN("[!abc]", "d", 0);
  FN("[^abc]", "d", 0);
  FNN("[!abc]", "a", 0);
  FN("[a-c]", "b", 0);
  FNN("[a-c]", "d", 0);
  FN("[]]", "]", 0);
  FN("[-a]", "-", 0);
  FN("[a-]", "-", 0);
  FN("[[:digit:]]", "5", 0);
  FNN("[[:digit:]]", "a", 0);
  FN("[[:alpha:]][[:digit:]]", "a5", 0);
  FN("x[", "x[", 0);            // nothing closes it, so it is a character
  FN("a[bc]d", "acd", 0);

  // FNM_NOESCAPE
  FN("a\\*b", "a*b", 0);
  FNN("a\\*b", "axb", 0);
  FN("a\\*b", "a\\xb", FNM_NOESCAPE);
  FNN("a\\*b", "a*b", FNM_NOESCAPE);
  FN("\\\\", "\\", 0);

  // FNM_PATHNAME: no wildcard may cross a slash
  FN("*", "a/b", 0);
  FNN("*", "a/b", FNM_PATHNAME);
  FN("*/*", "a/b", FNM_PATHNAME);
  FNN("*/*", "a/b/c", FNM_PATHNAME);
  FN("*/*/*", "a/b/c", FNM_PATHNAME);
  FNN("a?c", "a/c", FNM_PATHNAME);
  FN("a?c", "a/c", 0);
  FNN("[/]", "/", FNM_PATHNAME);
  FN("a/b", "a/b", FNM_PATHNAME);

  // FNM_PERIOD: a leading period must be spelled out
  FN("*", ".x", 0);
  FNN("*", ".x", FNM_PERIOD);
  FN(".*", ".x", FNM_PERIOD);
  FNN("?x", ".x", FNM_PERIOD);
  FNN("[.]x", ".x", FNM_PERIOD);
  FN("a/.x", "a/.x", FNM_PERIOD | FNM_PATHNAME);
  FNN("a/*", "a/.x", FNM_PERIOD | FNM_PATHNAME);
  FN("a/*", "a/.x", FNM_PERIOD);       // without FNM_PATHNAME it is not leading
  FN("*", "x.y", FNM_PERIOD);

  // FNM_CASEFOLD
  FN("abc", "ABC", FNM_CASEFOLD);
  FN("ABC", "abc", FNM_CASEFOLD);
  FNN("abc", "ABC", 0);
  FN("[a-c]", "B", FNM_CASEFOLD);
  FN("[A-C]", "b", FNM_CASEFOLD);
  FN("*.C", "file.c", FNM_CASEFOLD);

  // FNM_LEADING_DIR
  FN("a", "a/b", FNM_LEADING_DIR);
  FNN("a", "ab", FNM_LEADING_DIR);
  FN("*", "a/b", FNM_LEADING_DIR | FNM_PATHNAME);
  FN("a/b", "a/b/c", FNM_LEADING_DIR);
  FNN("a/b", "a/bc", FNM_LEADING_DIR);

  printf("fnmatch: %d checks\n", checks - before);
}

// ---- glob ----

// the files this group makes for itself, and takes away again
static const char *const globdir = "regexdir";
static const char *const globfiles[] = {
  "regexdir/f1", "regexdir/f2", "regexdir/g1", "regexdir/.hid",
  "regexdir/sub/c1", "regexdir/sub/c2", "regexdir/sub2/c3",
};

static int
makefile(const char *path)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);

  if (fd < 0)
    return -1;
  close(fd);
  return 0;
}

static void
cleanup(void)
{
  for (int i = 0; i < (int)(sizeof(globfiles) / sizeof(globfiles[0])); i++)
    unlink(globfiles[i]);
  unlink("regexdir/sub");
  unlink("regexdir/sub2");
  rmdir("regexdir/sub");
  rmdir("regexdir/sub2");
  unlink(globdir);
  rmdir(globdir);
}

// the paths a call found, joined with spaces, so one strcmp checks the
// order as well as the contents
static void
joined(glob_t *g, char *buf, size_t n)
{
  buf[0] = 0;
  for (size_t i = 0; i < g->gl_pathc; i++) {
    if (i && strlen(buf) + 1 < n)
      strcat(buf, " ");
    if (strlen(buf) + strlen(g->gl_pathv[i]) + 1 < n)
      strcat(buf, g->gl_pathv[i]);
  }
}

static void
gl_(const char *pat, int flags, int want, const char *expect, int line)
{
  glob_t g;
  char buf[256];

  memset(&g, 0, sizeof(g));
  int r = glob(pat, flags, 0, &g);
  checks++;
  if (r != want) {
    printf("  FAIL %s:%d: glob(\"%s\") = %d, wanted %d\n", __FILE__, line, pat, r, want);
    failures++;
  } else if (r == 0) {
    joined(&g, buf, sizeof(buf));
    checks++;
    if (strcmp(buf, expect) != 0) {
      printf("  FAIL %s:%d: glob(\"%s\") found \"%s\", wanted \"%s\"\n", __FILE__, line, pat, buf,
             expect);
      failures++;
    }
  }
  globfree(&g);
}

#define GL(pat, flags, expect) gl_(pat, flags, 0, expect, __LINE__)
#define GLN(pat, flags, want) gl_(pat, flags, want, "", __LINE__)

static void
test_glob(void)
{
  int before = checks;
  glob_t g;

  // a pattern nothing can match needs no files at all
  GLN("no/such/thing*", 0, GLOB_NOMATCH);
  GL("no/such/thing*", GLOB_NOCHECK, "no/such/thing*");
  GLN("nosuchfile", 0, GLOB_NOMATCH);

  cleanup();
  if (mkdir(globdir, 0777) < 0 || mkdir("regexdir/sub", 0777) < 0 || mkdir("regexdir/sub2", 0777) < 0) {
    printf("glob: skipped, cannot make a directory here\n");
    cleanup();
    return;
  }
  int ok = 1;
  for (int i = 0; i < (int)(sizeof(globfiles) / sizeof(globfiles[0])); i++)
    if (makefile(globfiles[i]) < 0)
      ok = 0;
  if (!ok) {
    printf("glob: skipped, cannot make files here\n");
    cleanup();
    return;
  }

  GL("regexdir/f*", 0, "regexdir/f1 regexdir/f2");
  GL("regexdir/?1", 0, "regexdir/f1 regexdir/g1");
  GL("regexdir/[fg]1", 0, "regexdir/f1 regexdir/g1");
  GL("regexdir/[!f]1", 0, "regexdir/g1");
  GL("regexdir/f1", 0, "regexdir/f1");
  GL("regexdir/sub", 0, "regexdir/sub");
  GLN("regexdir/f3", 0, GLOB_NOMATCH);
  GLN("regexdir/z*", 0, GLOB_NOMATCH);

  // a leading period is not matched by a wildcard, as in a shell
  GL("regexdir/*", 0, "regexdir/f1 regexdir/f2 regexdir/g1 regexdir/sub regexdir/sub2");
  GL("regexdir/.*", 0, "regexdir/. regexdir/.. regexdir/.hid");

  // one component expanded after another
  GL("regexdir/*/c*", 0, "regexdir/sub/c1 regexdir/sub/c2 regexdir/sub2/c3");
  GL("regexdir/sub/c?", 0, "regexdir/sub/c1 regexdir/sub/c2");
  GL("regexdir/*/*", 0, "regexdir/sub/c1 regexdir/sub/c2 regexdir/sub2/c3");
  GLN("regexdir/f1/*", 0, GLOB_NOMATCH);   // not a directory, so not an error

  // the flags
  GL("regexdir/*", GLOB_ONLYDIR, "regexdir/sub regexdir/sub2");
  GL("regexdir/s*", GLOB_MARK, "regexdir/sub/ regexdir/sub2/");
  GL("regexdir/f*", GLOB_MARK, "regexdir/f1 regexdir/f2");
  GL("regexdir/f\\1", 0, "regexdir/f1");
  GLN("regexdir/f\\1", GLOB_NOESCAPE, GLOB_NOMATCH);   // the backslash is a character now

  memset(&g, 0, sizeof(g));
  CHECK(glob("regexdir/f*", GLOB_NOSORT, 0, &g) == 0);
  CHECK(g.gl_pathc == 2);
  CHECK((g.gl_flags & GLOB_MAGCHAR) != 0);
  globfree(&g);

  memset(&g, 0, sizeof(g));
  CHECK(glob("regexdir/f1", 0, 0, &g) == 0);
  CHECK(glob("regexdir/g1", GLOB_APPEND, 0, &g) == 0);
  CHECK(g.gl_pathc == 2);
  CHECK(strcmp(g.gl_pathv[0], "regexdir/f1") == 0);
  CHECK(strcmp(g.gl_pathv[1], "regexdir/g1") == 0);
  CHECK(g.gl_pathv[2] == 0);
  globfree(&g);

  memset(&g, 0, sizeof(g));
  g.gl_offs = 2;
  CHECK(glob("regexdir/f*", GLOB_DOOFFS, 0, &g) == 0);
  CHECK(g.gl_pathv[0] == 0 && g.gl_pathv[1] == 0);
  CHECK(strcmp(g.gl_pathv[2], "regexdir/f1") == 0);
  CHECK(g.gl_pathc == 2);
  globfree(&g);

  memset(&g, 0, sizeof(g));
  CHECK(glob("regexdir/f*", 0, 0, &g) == 0);
  globfree(&g);
  CHECK(g.gl_pathv == 0);   // globfree leaves nothing behind
  CHECK(g.gl_pathc == 0);

  // GLOB_NOCHECK hands back the pattern, metacharacters and all
  GL("regexdir/zz*", GLOB_NOCHECK, "regexdir/zz*");

  printf("glob: %d checks\n", checks - before);
}

// ---- wordexp ----

static void
we_(const char *words, int flags, int want, const char *expect, int line)
{
  wordexp_t we;
  char buf[256];

  memset(&we, 0, sizeof(we));
  int r = wordexp(words, &we, flags);
  checks++;
  if (r != want) {
    printf("  FAIL %s:%d: wordexp(\"%s\") = %d, wanted %d\n", __FILE__, line, words, r, want);
    failures++;
    if (r == 0)
      wordfree(&we);
    return;
  }
  if (r != 0)
    return;
  buf[0] = 0;
  for (size_t i = 0; i < we.we_wordc; i++) {
    if (i && strlen(buf) + 1 < sizeof(buf))
      strcat(buf, "|");
    if (strlen(buf) + strlen(we.we_wordv[i]) + 1 < sizeof(buf))
      strcat(buf, we.we_wordv[i]);
  }
  checks++;
  if (strcmp(buf, expect) != 0) {
    printf("  FAIL %s:%d: wordexp(\"%s\") gave \"%s\", wanted \"%s\"\n", __FILE__, line, words, buf,
           expect);
    failures++;
  }
  wordfree(&we);
}

#define WE(words, flags, expect) we_(words, flags, 0, expect, __LINE__)
#define WEE(words, flags, want) we_(words, flags, want, "", __LINE__)

static void
test_wordexp(void)
{
  int before = checks;
  wordexp_t we;

  setenv("RETESTVAR", "bar", 1);
  setenv("RETESTEMPTY", "", 1);
  setenv("RETESTTWO", "one two", 1);
  setenv("HOME", "/home/tester", 1);
  unsetenv("RETESTUNSET");

  // splitting
  WE("", 0, "");
  WE("   ", 0, "");
  WE("a", 0, "a");
  WE("a b c", 0, "a|b|c");
  WE("  a   b  ", 0, "a|b");
  WE("a\tb\nc", 0, "a|b|c");

  // quoting
  WE("'a b'", 0, "a b");
  WE("\"a b\"", 0, "a b");
  WE("''", 0, "");
  WE("\"\"", 0, "");
  WE("a'b'c", 0, "abc");
  WE("a\"b\"c", 0, "abc");
  WE("'a'\"b\"c", 0, "abc");
  WE("a\\ b", 0, "a b");
  WE("\\$HOME", 0, "$HOME");
  WE("\"\\$x\"", 0, "$x");
  WE("\"a\\\"b\"", 0, "a\"b");
  WE("'$RETESTVAR'", 0, "$RETESTVAR");

  // variables
  WE("$RETESTVAR", 0, "bar");
  WE("${RETESTVAR}", 0, "bar");
  WE("x$RETESTVAR", 0, "xbar");
  WE("${RETESTVAR}x", 0, "barx");
  WE("$RETESTVAR$RETESTVAR", 0, "barbar");
  WE("\"$RETESTVAR $RETESTVAR\"", 0, "bar bar");
  WE("$RETESTEMPTY", 0, "");
  WE("\"$RETESTEMPTY\"", 0, "");
  WE("a${RETESTEMPTY}b", 0, "ab");
  WE("$RETESTTWO", 0, "one|two");
  WE("\"$RETESTTWO\"", 0, "one two");
  WE("$RETESTUNSET", 0, "");
  WE("x${RETESTUNSET}y", 0, "xy");
  WEE("$RETESTUNSET", WRDE_UNDEF, WRDE_BADVAL);
  WE("a$", 0, "a$");
  WE("$ x", 0, "$|x");

  // the tilde
  WE("~", 0, "/home/tester");
  WE("~/x", 0, "/home/tester/x");
  WE("a~", 0, "a~");
  WE("'~'", 0, "~");

  // what is refused
  WEE("`ls`", 0, WRDE_CMDSUB);
  WEE("`ls`", WRDE_NOCMD, WRDE_CMDSUB);
  WEE("$(ls)", WRDE_NOCMD, WRDE_CMDSUB);
  WEE("\"`ls`\"", 0, WRDE_CMDSUB);
  WEE("a|b", 0, WRDE_BADCHAR);
  WEE("a&b", 0, WRDE_BADCHAR);
  WEE("a;b", 0, WRDE_BADCHAR);
  WEE("a<b", 0, WRDE_BADCHAR);
  WEE("a>b", 0, WRDE_BADCHAR);
  WEE("(a)", 0, WRDE_BADCHAR);
  WEE("{a}", 0, WRDE_BADCHAR);
  WEE("'unterminated", 0, WRDE_SYNTAX);
  WEE("\"unterminated", 0, WRDE_SYNTAX);
  WEE("trailing\\", 0, WRDE_SYNTAX);
  WEE("${}", 0, WRDE_SYNTAX);
  WEE("${a:-b}", 0, WRDE_SYNTAX);

  // globbing of the words, against the files the glob group left behind
  if (mkdir(globdir, 0777) == 0 && makefile("regexdir/f1") == 0 && makefile("regexdir/f2") == 0) {
    WE("regexdir/f*", 0, "regexdir/f1|regexdir/f2");
    WE("'regexdir/f*'", 0, "regexdir/f*");
    WE("\"regexdir/f*\"", 0, "regexdir/f*");
    WE("regexdir/f\\*", 0, "regexdir/f*");
    WE("regexdir/nomatch*", 0, "regexdir/nomatch*");
    setenv("RETESTDIR", "regexdir", 1);
    WE("$RETESTDIR/f1", 0, "regexdir/f1");
    unlink("regexdir/f1");
    unlink("regexdir/f2");
    unlink(globdir);
    rmdir(globdir);
  }

  // appending, and the reserved slots
  memset(&we, 0, sizeof(we));
  CHECK(wordexp("a b", &we, 0) == 0);
  CHECK(we.we_wordc == 2);
  CHECK(wordexp("c", &we, WRDE_APPEND) == 0);
  CHECK(we.we_wordc == 3);
  CHECK(strcmp(we.we_wordv[2], "c") == 0);
  CHECK(we.we_wordv[3] == 0);
  wordfree(&we);
  CHECK(we.we_wordv == 0);

  memset(&we, 0, sizeof(we));
  we.we_offs = 1;
  CHECK(wordexp("a", &we, WRDE_DOOFFS) == 0);
  CHECK(we.we_wordv[0] == 0);
  CHECK(strcmp(we.we_wordv[1], "a") == 0);
  wordfree(&we);

  memset(&we, 0, sizeof(we));
  CHECK(wordexp("a b", &we, 0) == 0);
  CHECK(wordexp("z", &we, WRDE_REUSE) == 0);
  CHECK(we.we_wordc == 1 && strcmp(we.we_wordv[0], "z") == 0);
  wordfree(&we);

  unsetenv("RETESTVAR");
  unsetenv("RETESTEMPTY");
  unsetenv("RETESTTWO");
  unsetenv("RETESTDIR");
  printf("wordexp: %d checks\n", checks - before);
}

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  test_bre();
  test_ere();
  test_classes();
  test_submatch();
  test_backref();
  test_flags();
  test_errors();
  test_fnmatch();
  test_glob();
  cleanup();
  test_wordexp();

  printf("regex: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("ALL TESTS PASSED\n");
  return failures != 0;
}
