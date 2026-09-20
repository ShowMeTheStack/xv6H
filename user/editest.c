//
// editest: the line editor's own tests. there is no pseudo-terminal on
// this system yet, so a test cannot pretend to be a person at a
// keyboard: instead the keys are written to a file and the editing
// engine is driven from it through __el_edit_from(), which reads keys
// from any descriptor and draws nothing. what comes back is the line
// those keys should have produced, which is the whole of what the
// editor promises. the history file functions are tested against a
// real file, and the plain path that a script takes is tested in a
// child whose standard input is a file.
//
// keys are written as three-digit octal escapes throughout, because a
// hexadecimal escape would swallow the letter that follows it.
//
#include "user/user.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <editline.h>
#include <sys/wait.h>

#define KEYFILE "editest.keys.tmp"
#define HISTFILE "editest.hist.tmp"
#define PLAINFILE "editest.plain.tmp"
#define OUTFILE "editest.out.tmp"

// the keys, named so that a test reads as a description of what was
// typed rather than as a row of numbers.
#define CTRL_A "\001"
#define CTRL_B "\002"
#define CTRL_C "\003"
#define CTRL_D "\004"
#define CTRL_E "\005"
#define CTRL_F "\006"
#define CTRL_G "\007"
#define CTRL_H "\010"
#define TAB "\011"
#define CTRL_K "\013"
#define CTRL_N "\016"
#define CTRL_P "\020"
#define CTRL_R "\022"
#define CTRL_T "\024"
#define CTRL_U "\025"
#define CTRL_W "\027"
#define ESC "\033"
#define BS "\177"
#define CR "\015"

static int checks, failures;

// keys are mostly control characters, so show them the way a manual
// does: a failure should name the keys that caused it.
static const char *
show(const char *s)
{
  static char out[256];
  size_t o = 0;

  for (; *s && o + 5 < sizeof(out); s++) {
    unsigned char c = *s;
    if (c == 033) {
      out[o++] = '\\';
      out[o++] = 'e';
    } else if (c == 015) {
      out[o++] = '\\';
      out[o++] = 'r';
    } else if (c < 32) {
      out[o++] = '^';
      out[o++] = '@' + c;
    } else if (c == 127) {
      out[o++] = '^';
      out[o++] = '?';
    } else {
      out[o++] = c;
    }
  }
  out[o] = 0;
  return out;
}

static void
fail(const char *what, const char *got, const char *want)
{
  printf("  FAIL %s: got %s, wanted \"%s\"\n", what, got ? got : "(null)", want);
  failures++;
}

// feed the editor a string of keys and give back the line it made.
static char *
run(const char *keys)
{
  FILE *f = fopen(KEYFILE, "w");
  int fd;
  char *line;

  if (f == 0)
    return 0;
  fwrite(keys, 1, strlen(keys), f);
  fclose(f);
  fd = open(KEYFILE, O_RDONLY);
  if (fd < 0)
    return 0;
  line = __el_edit_from(fd, "$ ");
  close(fd);
  return line;
}

// these keys should produce this line.
static void
expect(const char *keys, const char *want)
{
  char *got = run(keys);

  checks++;
  if (got == 0 || strcmp(got, want) != 0)
    fail(show(keys), got, want);
  el_free(got);
}

// these keys should be the end of the input.
static void
expect_eof(const char *keys)
{
  char *got = run(keys);

  checks++;
  if (got != 0)
    fail(show(keys), got, "(null)");
  el_free(got);
}

static void
check(const char *what, int cond)
{
  checks++;
  if (!cond) {
    printf("  FAIL %s\n", what);
    failures++;
  }
}

static void
check_str(const char *what, const char *got, const char *want)
{
  checks++;
  if (got == 0 || strcmp(got, want) != 0)
    fail(what, got, want);
}

// ---- the groups ----

static void
test_insert(void)
{
  expect("hello" CR, "hello");
  expect("hello\n", "hello");        // a newline accepts the line too
  expect(CR, "");
  expect("ac" CTRL_B "b" CR, "abc"); // insertion happens at the cursor
  expect_eof("");                    // nothing at all is the end of the input
  expect_eof(CTRL_D);                // and so is ctrl-D on an empty line

  // a line is allowed to be longer than any buffer the editor started
  // with, and longer than the terminal is wide
  char big[301], keys[320];
  memset(big, 'x', 300);
  big[300] = 0;
  snprintf(keys, sizeof(keys), "%s%s", big, CR);
  expect(keys, big);

  // the input ending in the middle of a line gives up what was typed
  expect("half", "half");
  printf("insert: ok\n");
}

static void
test_movement(void)
{
  expect("world" CTRL_A "hello " CR, "hello world");
  expect("abc" CTRL_A "x" CTRL_E "y" CR, "xabcy");
  expect("abc" CTRL_B CTRL_B "X" CR, "aXbc");
  expect("abc" CTRL_A CTRL_F "X" CR, "aXbc");

  // the arrow keys, in the xterm form and the vt100 form
  expect("abc" ESC "[D" ESC "[D" "X" CR, "aXbc");
  expect("abc" ESC "[D" ESC "[D" ESC "[C" "X" CR, "abXc");
  expect("abc" ESC "OD" "X" CR, "abXc");
  expect("abc" ESC "[A", "abc"); // no history: up leaves the line alone

  // home and end, in all the forms a terminal sends
  expect("abc" ESC "[H" "X" CR, "Xabc");
  expect("abc" ESC "[H" ESC "[F" "Y" CR, "abcY");
  expect("abc" ESC "[1~" "X" CR, "Xabc");
  expect("abc" ESC "[H" ESC "[4~" "Y" CR, "abcY");
  expect("abc" ESC "OH" "X" CR, "Xabc");
  expect("abc" ESC "[H" ESC "OF" "Y" CR, "abcY");

  // by words
  expect("one two" ESC "b" "X" CR, "one Xtwo");
  expect("one two" CTRL_A ESC "f" "X" CR, "oneX two");
  expect("one two" ESC "b" ESC "b" "X" CR, "Xone two");
  expect("one two" CTRL_A ESC "f" ESC "f" "X" CR, "one twoX");
  printf("movement: ok\n");
}

static void
test_delete(void)
{
  expect("abc" BS CR, "ab");
  expect("abc" CTRL_H CR, "ab");
  expect("abc" CTRL_A CTRL_D CR, "bc");  // ctrl-D deletes under the cursor
  expect("abc" CTRL_D CR, "abc");        // and does nothing at the end
  expect("abc" ESC "[H" ESC "[3~" CR, "bc");
  expect("one two" CTRL_W CR, "one ");
  expect("one two  " CTRL_W CR, "one ");
  expect("one two" CTRL_W CTRL_W CR, "");
  expect("one two" ESC BS CR, "one ");   // alt-backspace: the word before
  expect("one two" CTRL_A ESC "d" CR, " two");
  expect("foobar" CTRL_B CTRL_B CTRL_B CTRL_U CR, "bar");
  expect("foobar" CTRL_U CR, "");
  expect("foobar" CTRL_A CTRL_F CTRL_F CTRL_F CTRL_K CR, "foo");
  expect("ab" CTRL_T CR, "ba");          // at the end, the last two swap
  expect("abc" CTRL_A CTRL_F CTRL_T CR, "bac");
  expect("a" CTRL_T CR, "a");            // nothing to swap with
  printf("delete: ok\n");
}

static void
test_history(void)
{
  el_history_clear();
  check("empty history has no entries", el_history_count() == 0);
  check("an empty line is not history", el_history_add("") == 0);
  check("a line is added", el_history_add("one") == 1);
  check("a repeat is ignored", el_history_add("one") == 0);
  check("a different line is added", el_history_add("two") == 1);
  check("two entries", el_history_count() == 2);
  check_str("the oldest is first", el_history_get(0), "one");
  check_str("the newest is last", el_history_get(1), "two");
  check("out of range is nothing", el_history_get(-1) == 0 && el_history_get(2) == 0);

  el_history_add("three");
  expect(CTRL_P CR, "three");
  expect(CTRL_P CTRL_P CR, "two");
  expect(CTRL_P CTRL_P CTRL_P CTRL_P CR, "one"); // walking past the oldest stops
  expect(ESC "[A" ESC "[A" CR, "two");
  expect(CTRL_P CTRL_N CR, "");                  // and back down to a blank line
  expect("ab" CTRL_P CTRL_N CR, "ab");           // what was being typed comes back
  expect(CTRL_P ESC "[B" ESC "[B" CR, "");
  expect(CTRL_P CTRL_A "X" CR, "Xthree");        // a recalled line can be edited

  // the limit drops the oldest entries
  el_history_clear();
  el_history_max(2);
  el_history_add("a");
  el_history_add("b");
  el_history_add("c");
  check("the limit is kept", el_history_count() == 2);
  check_str("the oldest went", el_history_get(0), "b");
  el_history_max(500);
  printf("history: ok\n");
}

static void
test_search(void)
{
  el_history_clear();
  el_history_add("echo one");
  el_history_add("grep foo");
  el_history_add("echo two");

  expect(CTRL_R "foo" CR, "grep foo");
  expect(CTRL_R "echo" CR, "echo two");           // the newest match first
  expect(CTRL_R "echo" CTRL_R CR, "echo one");    // ctrl-R again goes further back
  expect(CTRL_R "echo" CTRL_R CTRL_R CR, "echo one"); // and stops at the oldest
  expect(CTRL_R "ec" "x" CR, "echo two");         // a failing letter keeps the match
  expect(CTRL_R "fool" BS CR, "grep foo");        // backspace searches again
  expect("ab" CTRL_R "echo" CTRL_G CR, "ab");     // ctrl-G puts the line back
  expect("ab" CTRL_R "echo" ESC, "ab");           // and so does escape
  expect(CTRL_R "grep" CTRL_A "X" CR, "Xgrep foo"); // any other key ends the search
  el_history_clear();
  expect(CTRL_R "nothing" CR, "");                // an empty history matches nothing
  printf("search: ok\n");
}

// a completer with a fixed vocabulary, so that the tab key can be
// tested without a file system behind it.
static const char *const words[] = {"hello", "help", "history", "zebra"};

static void
vocab_completer(const char *line, int point, char ***matches, int *nmatches)
{
  size_t start = point, wlen;
  char **m;
  int n = 0, i;

  while (start > 0 && line[start - 1] != ' ')
    start--;
  wlen = (size_t)point - start;
  m = malloc(sizeof(words) / sizeof(words[0]) * sizeof(char *));
  if (m == 0)
    return;
  for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++)
    if (strncmp(words[i], line + start, wlen) == 0)
      m[n++] = strdup(words[i]);
  if (n == 0) {
    free(m);
    m = 0;
  }
  *matches = m;
  *nmatches = n;
}

static void
test_complete(void)
{
  el_set_completer(vocab_completer);
  expect("zeb" TAB CR, "zebra");            // one match is filled in
  expect("he" TAB CR, "hel");               // several: as far as they agree
  expect("hello" TAB CR, "hello");
  expect("echo zeb" TAB CR, "echo zebra");  // only the word at the cursor
  expect("echo he" TAB "p" CR, "echo help");
  expect("xyz" TAB CR, "xyz");              // no match changes nothing
  expect("zeb" TAB " he" TAB CR, "zebra hel");
  el_set_completer(0);
  expect("ab" TAB "cd" CR, "abcd");         // without a completer, tab does nothing
  printf("complete: ok\n");
}

static void
test_multiline(void)
{
  // the two modes differ only in what is drawn; the line they produce
  // is the same, and nothing here is drawn at all.
  el_set_multiline(1);
  expect("hello" CTRL_A "say " CR, "say hello");
  expect("abc" BS BS CR, "a");
  el_set_multiline(0);
  printf("multiline: ok\n");
}

static void
test_histfile(void)
{
  int n;

  el_history_clear();
  el_history_add("first");
  el_history_add("second");
  el_history_add("third");
  check("saving works", el_history_save(HISTFILE) == 0);
  el_history_clear();
  check("the history is empty again", el_history_count() == 0);
  n = el_history_load(HISTFILE);
  check("loading reports what it read", n == 3);
  check("and the entries are there", el_history_count() == 3);
  check_str("in the order they were saved", el_history_get(0), "first");
  check_str("newest last", el_history_get(2), "third");
  check("a missing file says so", el_history_load("editest.nosuch.tmp") == -1);
  unlink(HISTFILE);
  el_history_clear();
  printf("histfile: ok\n");
}

// requirement four lives on the standard input, so it is tested in a
// child whose standard input is a file: the line must come back whole,
// escape bytes and all, with no editing.
static void
test_plain(void)
{
  static const char text[] = "a\033[Db\n";
  char got[64];
  FILE *f = fopen(PLAINFILE, "w");
  int pid, st, fd, n;

  if (f == 0) {
    check("the plain input file was written", 0);
    return;
  }
  fwrite(text, 1, strlen(text), f);
  fclose(f);

  // whatever this program has printed must leave with the parent, not
  // be inherited in a buffer and printed again by the child.
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    int in = open(PLAINFILE, O_RDONLY);
    FILE *out = fopen(OUTFILE, "w");
    char *line;

    if (in < 0 || out == 0)
      exit(1);
    dup2(in, 0);
    line = el_readline("$ ");
    if (line)
      fputs(line, out);
    fclose(out);
    el_free(line);
    exit(0);
  }
  if (pid < 0) {
    check("the child started", 0);
    return;
  }
  waitpid(pid, &st, 0);
  fd = open(OUTFILE, O_RDONLY);
  n = fd >= 0 ? (int)read(fd, got, sizeof(got) - 1) : -1;
  if (fd >= 0)
    close(fd);
  if (n < 0)
    n = 0;
  got[n] = 0;
  check_str("a line from a file is not edited", got, "a\033[Db");
  unlink(PLAINFILE);
  unlink(OUTFILE);
  printf("plain: ok\n");
}

static void
test_abandon(void)
{
  char *line = run("abc" CTRL_C);

  // ctrl-C gives up on the line, and an empty line is not the end of
  // the input: the caller must be able to tell the two apart.
  checks++;
  if (line == 0 || line[0] != 0)
    fail("^C", line, "");
  el_free(line);
  expect(CTRL_C, "");

  // an escape sequence nobody knows is dropped rather than typed in
  expect("ab" ESC "[5~" "c" CR, "abc");
  expect("ab" ESC "[200;99X" "c" CR, "abc");
  expect("ab" ESC "Z" "c" CR, "abc");
  expect("abc" ESC, "abc"); // a lone escape at the end is not a character
  printf("abandon: ok\n");
}

int
main(int argc, char *argv[])
{
  int only = argc > 1;

#define GROUP(name)                                                                                \
  if (!only || strcmp(argv[1], #name) == 0)                                                        \
  test_##name()

  GROUP(insert);
  GROUP(movement);
  GROUP(delete);
  GROUP(history);
  GROUP(search);
  GROUP(complete);
  GROUP(multiline);
  GROUP(histfile);
  GROUP(plain);
  GROUP(abandon);

  unlink(KEYFILE);
  printf("editline: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("ALL TESTS PASSED\n");
  exit(failures != 0);
}
