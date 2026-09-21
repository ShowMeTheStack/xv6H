// xargs: read words and put them on a command line.
//
//   xargs [-0] [-r] [-t] [-p] [-n MAX] [-L LINES] [-I STR] [-s SIZE]
//         [-E EOF] [-a FILE] [COMMAND [ARG...]]
//
//   -0        the input is separated by zero bytes, not by blanks
//   -r        do not run the command at all when there is no input
//   -t        print each command before running it
//   -p        print it and ask before running it
//   -n MAX    at most this many arguments per command
//   -L LINES  one command per this many input lines
//   -I STR    replace STR in the arguments with one input line
//   -s SIZE   at most this many bytes of arguments per command
//   -E EOF    stop reading at a line holding just this word
//   -a FILE   read from this file rather than standard input
//
// with no command it runs echo, which is what xargs has always done.
// the exit status is the command's, or 123 when a command failed, or
// 124 when one exited because of a signal.
#include "user/user.h"

#define MAXARGS 1024
#define MAXBYTES 65536

static int null_sep, no_run_if_empty, trace, prompt;
static long max_args, max_lines, max_bytes = MAXBYTES;
static const char *replace_str;
static const char *eof_word;
static int status;

static char *fixed[MAXARGS];
static int nfixed;

static void
usage(void)
{
  fprintf(stderr, "usage: xargs [-0rtp] [-n MAX] [-L N] [-I STR] [-s SIZE]"
                  " [-E EOF] [-a FILE] [COMMAND [ARG...]]\n");
  exit(1);
}

static int
ask(char **argv)
{
  fprintf(stderr, "%s", argv[0]);
  for (int i = 1; argv[i]; i++)
    fprintf(stderr, " %s", argv[i]);
  fprintf(stderr, " ?...");
  int c = fgetc(stdin), first = c;
  while (c != '\n' && c != EOF)
    c = fgetc(stdin);
  return first == 'y' || first == 'Y';
}

static void
run(char **argv)
{
  if (argv[nfixed] == 0 && no_run_if_empty)
    return;
  if (trace || prompt) {
    if (prompt) {
      if (!ask(argv))
        return;
    } else {
      fprintf(stderr, "%s", argv[0]);
      for (int i = 1; argv[i]; i++)
        fprintf(stderr, " %s", argv[i]);
      fprintf(stderr, "\n");
    }
  }
  int pid = fork();
  if (pid < 0) {
    fprintf(stderr, "xargs: fork: %s\n", strerror(errno));
    exit(125);
  }
  if (pid == 0) {
    execvp(argv[0], argv);
    fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  if (WIFSIGNALED(st)) {
    fprintf(stderr, "xargs: %s ended on a signal\n", argv[0]);
    exit(124);
  }
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
  if (code == 126 || code == 127)
    exit(code);
  if (code == 255) {
    fprintf(stderr, "xargs: %s exited with 255; stopping\n", argv[0]);
    exit(124);
  }
  if (code != 0)
    status = 123;
}

// one word from the input: separated by blanks (and newlines) unless
// -0 says zero bytes, with quotes and backslashes honoured the way
// xargs does. returns 0 at the end of the input.
static int
read_word(FILE *f, char *buf, int size, int *saw_newline)
{
  int n = 0, c;

  *saw_newline = 0;
  if (null_sep) {
    while ((c = fgetc(f)) != EOF && c != 0)
      if (n < size - 1)
        buf[n++] = (char)c;
    buf[n] = 0;
    return n > 0 || c == 0;
  }

  // skip the blanks in front
  while ((c = fgetc(f)) != EOF && (c == ' ' || c == '\t' || c == '\n'))
    if (c == '\n')
      *saw_newline = 1;
  if (c == EOF)
    return 0;

  int quote = 0;
  for (;;) {
    if (c == EOF)
      break;
    if (quote) {
      if (c == quote) {
        quote = 0;
        c = fgetc(f);
        continue;
      }
    } else {
      if (c == ' ' || c == '\t')
        break;
      if (c == '\n') {
        *saw_newline = 1;
        break;
      }
      if (c == '\'' || c == '"') {
        quote = c;
        c = fgetc(f);
        continue;
      }
      if (c == '\\') {
        c = fgetc(f);
        if (c == EOF)
          break;
      }
    }
    if (n < size - 1)
      buf[n++] = (char)c;
    c = fgetc(f);
  }
  buf[n] = 0;
  return 1;
}

// -I: the command is run once per input line, with every occurrence of
// the string replaced by that line
static void
run_replacing(FILE *f)
{
  static char line[MAXBYTES];
  char *argv[MAXARGS];
  static char built[MAXARGS][MAXBYTES];

  while (fgets(line, sizeof(line), f)) {
    int n = strlen(line);
    if (n > 0 && line[n - 1] == '\n')
      line[--n] = 0;
    if (eof_word && strcmp(line, eof_word) == 0)
      break;
    if (n == 0)
      continue;
    for (int i = 0; i < nfixed; i++) {
      // every occurrence in the argument, not only a whole one
      const char *src = fixed[i];
      char *dst = built[i];
      int room = MAXBYTES - 1, len = strlen(replace_str);
      while (*src && room > 0) {
        if (strncmp(src, replace_str, len) == 0) {
          int ln = strlen(line);
          if (ln > room)
            ln = room;
          memmove(dst, line, ln);
          dst += ln;
          room -= ln;
          src += len;
        } else {
          *dst++ = *src++;
          room--;
        }
      }
      *dst = 0;
      argv[i] = built[i];
    }
    argv[nfixed] = 0;
    run(argv);
  }
}

int
main(int argc, char *argv[])
{
  FILE *in = stdin;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case '0': null_sep = 1; break;
      case 'r': no_run_if_empty = 1; break;
      case 't': trace = 1; break;
      case 'p': prompt = 1; break;
      case 'x': break; // stop if the line is too long; this always does
      case 'n': case 'L': case 'l': case 'I': case 'i': case 's':
      case 'E': case 'e': case 'a': {
        const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (val == 0)
          usage();
        switch (*p) {
        case 'n': max_args = atol(val); break;
        case 'L': case 'l': max_lines = atol(val); break;
        case 'I': case 'i': replace_str = val; break;
        case 's': max_bytes = atol(val); break;
        case 'E': case 'e': eof_word = val; break;
        case 'a':
          in = fopen(val, "r");
          if (in == 0) {
            fprintf(stderr, "xargs: %s: %s\n", val, strerror(errno));
            exit(1);
          }
          break;
        }
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }

  if (i >= argc) {
    fixed[nfixed++] = "echo";
  } else {
    for (; i < argc && nfixed < MAXARGS - 2; i++)
      fixed[nfixed++] = argv[i];
  }
  if (max_bytes > MAXBYTES)
    max_bytes = MAXBYTES;

  if (replace_str) {
    run_replacing(in);
    exit(status);
  }

  char **built = malloc(MAXARGS * sizeof(char *));
  if (built == 0) {
    fprintf(stderr, "xargs: out of memory\n");
    exit(1);
  }
  for (int k = 0; k < nfixed; k++)
    built[k] = fixed[k];
  int n = nfixed;
  long bytes = 0, lines = 0;
  for (int k = 0; k < nfixed; k++)
    bytes += strlen(fixed[k]) + 1;

  static char word[MAXBYTES];
  int any = 0, saw_newline;
  while (read_word(in, word, sizeof(word), &saw_newline)) {
    if (eof_word && strcmp(word, eof_word) == 0)
      break;
    if (word[0] == 0 && !null_sep) {
      if (saw_newline)
        lines++;
      continue;
    }
    any = 1;
    long need = (long)strlen(word) + 1;
    // this word would make the command line too long, or there are
    // already as many arguments as were asked for: run what there is
    if (n > nfixed && (bytes + need > max_bytes ||
                       (max_args > 0 && n - nfixed >= max_args))) {
      built[n] = 0;
      run(built);
      n = nfixed;
      bytes = 0;
      for (int k = 0; k < nfixed; k++)
        bytes += strlen(fixed[k]) + 1;
    }
    if (n >= MAXARGS - 2) {
      built[n] = 0;
      run(built);
      n = nfixed;
    }
    built[n] = strdup(word);
    if (built[n] == 0) {
      fprintf(stderr, "xargs: out of memory\n");
      exit(1);
    }
    n++;
    bytes += need;
    if (saw_newline) {
      lines++;
      if (max_lines > 0 && lines >= max_lines) {
        built[n] = 0;
        run(built);
        n = nfixed;
        lines = 0;
        bytes = 0;
        for (int k = 0; k < nfixed; k++)
          bytes += strlen(fixed[k]) + 1;
      }
    }
  }
  if (n > nfixed || (!any && !no_run_if_empty)) {
    built[n] = 0;
    run(built);
  }
  if (in != stdin)
    fclose(in);
  exit(status);
}
