//
// the shell.
//
// it reads a line, parses it, runs it, and repeats. interactively the
// line comes from the editor, with history kept between runs; from a
// script or from -c it comes from the text itself. the shell owns the
// terminal and hands it to each foreground job, which is what makes
// ctrl-C and ctrl-Z reach the job rather than the shell.
//
#include "sh.h"
#include <editline.h>

int last_status;
int shell_pid;
int last_bg_pid;
int interactive;
int shell_tty = -1;
struct termios saved_tty;
int shell_pgid;
char **script_args;
int nscript_args;
char *script_name;
int opt_errexit, opt_xtrace, opt_noexec, opt_nounset, opt_verbose, opt_noglob, opt_monitor;

void *
xmalloc(size_t n)
{
  void *p = malloc(n);

  if (p == 0) {
    fprintf(stderr, "sh: out of memory\n");
    exit(2);
  }
  return p;
}

char *
xstrdup(const char *s)
{
  char *p = strdup(s ? s : "");

  if (p == 0) {
    fprintf(stderr, "sh: out of memory\n");
    exit(2);
  }
  return p;
}

void
sh_error(const char *fmt, ...)
{
  va_list ap;

  fflush(stdout);
  fprintf(stderr, "%s: ", script_name ? script_name : "sh");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  fflush(stderr);
}

// ---- running text ----

// a command whose first word is an alias is rewritten once, which is
// as far as POSIX asks a shell to go
static char *
apply_alias(const char *text)
{
  int i = 0;

  while (text[i] == ' ' || text[i] == '\t')
    i++;
  int start = i;
  while (text[i] && !strchr(" \t\n;|&<>()", text[i]))
    i++;
  if (i == start)
    return 0;
  char name[64];
  int len = i - start;
  if (len >= (int)sizeof(name))
    return 0;
  memmove(name, text + start, len);
  name[len] = 0;
  const char *value = alias_get(name);
  if (value == 0)
    return 0;

  int n = strlen(value) + strlen(text + i) + 2;
  char *out = xmalloc(n);
  snprintf(out, n, "%s%s", value, text + i);
  return out;
}

int
run_string(const char *text, int report_syntax)
{
  char *aliased = apply_alias(text);
  struct parser *p = parser_new(aliased ? aliased : text, 0);
  int eof = 0;

  for (;;) {
    struct cmd *c = parse_command(p, &eof);
    if (c == 0) {
      if (parser_error(p) && report_syntax)
        last_status = 2;
      break;
    }
    if (opt_noexec) {
      cmd_free(c);
      continue;
    }
    run_cmd(c);
    cmd_free(c);
    trap_run_pending();
    if (exiting || function_return || loop_break || loop_continue)
      break;
  }
  parser_free(p);
  free(aliased);
  return last_status;
}

int
run_file(const char *path)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0) {
    sh_error("%s: %s", path, strerror(errno));
    return 127;
  }
  int cap = 4096, len = 0;
  char *text = xmalloc(cap);
  for (;;) {
    if (len + 1024 > cap) {
      cap *= 2;
      char *t = xmalloc(cap);
      memmove(t, text, len);
      free(text);
      text = t;
    }
    int n = read(fd, text + len, cap - len - 1);
    if (n <= 0)
      break;
    len += n;
  }
  text[len] = 0;
  close(fd);

  // a script may begin with #! naming this shell; the kernel already
  // handled it, and the line is a comment to us
  int r = run_string(text, 1);
  free(text);
  return r;
}

// ---- reading a line interactively ----
//
// a line that ends inside a quote or a compound command is continued
// with the PS2 prompt until it is complete.

static int
needs_more(const char *text)
{
  int quote = 0, depth = 0, escaped = 0;
  int word_start = 1;
  int nk = 0; // how many compound commands are still open

  for (const char *p = text; *p; p++) {
    if (escaped) {
      escaped = 0;
      continue;
    }
    if (*p == '\\' && quote != '\'') {
      escaped = 1;
      continue;
    }
    if (quote) {
      if (*p == quote)
        quote = 0;
      continue;
    }
    if (*p == '\'' || *p == '"') {
      quote = *p;
      continue;
    }
    if (*p == '#' && word_start) {
      while (p[1] && p[1] != '\n')
        p++;
      continue;
    }
    if (*p == '(') {
      depth++;
      word_start = 1;
      continue;
    }
    if (*p == ')') {
      depth--;
      word_start = 1;
      continue;
    }
    if (word_start && isalpha((unsigned char)*p)) {
      // the compound commands that need a closing word
      static const char *const openers[] = {"if", "while", "until", "for", "case", "{", 0};
      static const char *const closers[] = {"fi", "done", "done", "done", "esac", "}", 0};
      int len = 0;
      while (isalpha((unsigned char)p[len]))
        len++;
      for (int i = 0; openers[i]; i++) {
        if ((int)strlen(openers[i]) == len && memcmp(p, openers[i], len) == 0 &&
            (p[len] == ' ' || p[len] == '\n' || p[len] == '\t' || p[len] == 0)) {
          nk++;
          break;
        }
      }
      for (int i = 0; closers[i]; i++) {
        if ((int)strlen(closers[i]) == len && memcmp(p, closers[i], len) == 0 && nk > 0) {
          nk--;
          break;
        }
      }
      p += len - 1;
      word_start = 0;
      continue;
    }
    word_start = (*p == '\n' || *p == ';' || *p == '&' || *p == '|' || *p == ' ' || *p == '\t');
  }
  // a line ending in a pipe or a logical operator also continues
  const char *end = text + strlen(text);
  while (end > text && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
    end--;
  if (end > text && (end[-1] == '|' || end[-1] == '&') && !(end - 1 > text && end[-2] == '|'))
    return 1;
  if (end > text && end[-1] == '\\')
    return 1;
  return quote || depth > 0 || nk > 0;
}

static char *
prompt_of(const char *name, const char *fallback)
{
  const char *p = var_get(name);

  return xstrdup(p ? p : fallback);
}

static void
interactive_loop(void)
{
  char *history = 0;
  const char *home = var_get("HOME");

  if (home) {
    int n = strlen(home) + 16;
    history = xmalloc(n);
    snprintf(history, n, "%s%s.sh_history", home, home[strlen(home) - 1] == '/' ? "" : "/");
    el_history_load(history);
  }

  for (;;) {
    job_reap(1);
    trap_run_pending();
    if (exiting)
      break;

    char *ps1 = prompt_of("PS1", "$ ");
    char *line = el_readline(ps1);
    free(ps1);
    if (line == 0) {
      printf("\n");
      break;
    }

    // keep reading while the command is not finished
    while (needs_more(line)) {
      char *ps2 = prompt_of("PS2", "> ");
      char *more = el_readline(ps2);
      free(ps2);
      if (more == 0)
        break;
      int n = strlen(line) + strlen(more) + 2;
      char *joined = xmalloc(n);
      snprintf(joined, n, "%s\n%s", line, more);
      free(line);
      free(more);
      line = joined;
    }

    if (*line) {
      el_history_add(line);
      int n = strlen(line);
      char *text = xmalloc(n + 2);
      memmove(text, line, n);
      text[n] = '\n';
      text[n + 1] = 0;
      run_string(text, 1);
      free(text);
    }
    free(line);
  }

  if (history) {
    el_history_save(history);
    free(history);
  }
}

// ---- the terminal ----

static void
setup_terminal(void)
{
  int fd;

  // make sure the three standard descriptors are open
  while ((fd = open("/console", O_RDWR)) >= 0) {
    if (fd >= 3) {
      close(fd);
      break;
    }
  }
  if (fd < 0)
    while ((fd = open("console", O_RDWR)) >= 0) {
      if (fd >= 3) {
        close(fd);
        break;
      }
    }

  if (!isatty(0))
    return;
  shell_tty = 0;
  interactive = 1;
  opt_monitor = 1;

  // the shell leads its own process group and holds the terminal
  shell_pgid = getpid();
  if (getpgrp() != shell_pgid)
    setpgid(0, shell_pgid);
  ioctl(shell_tty, TIOCSCTTY, 0);
  tcsetpgrp(shell_tty, shell_pgid);
  tcgetattr(shell_tty, &saved_tty);

  // the signals a foreground job takes are not for the shell
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
}

int
main(int argc, char *argv[])
{
  extern char **environ;
  int i = 1;
  const char *command = 0;

  shell_pid = getpid();
  script_name = argv[0];
  var_init(environ);
  jobs_init();
  var_set_int("OPTIND", 1);

  // the options a shell takes
  for (; i < argc; i++) {
    if (argv[i][0] != '-' || argv[i][1] == 0)
      break;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv[i], "-c") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "sh: -c needs an argument\n");
        return 2;
      }
      command = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "-s") == 0)
      continue;
    if (strcmp(argv[i], "-i") == 0) {
      interactive = 1;
      continue;
    }
    if (strcmp(argv[i], "-l") == 0)
      continue;
    for (char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'e': opt_errexit = 1; break;
      case 'x': opt_xtrace = 1; break;
      case 'n': opt_noexec = 1; break;
      case 'u': opt_nounset = 1; break;
      case 'v': opt_verbose = 1; break;
      case 'f': opt_noglob = 1; break;
      case 'm': opt_monitor = 1; break;
      default:
        fprintf(stderr, "sh: -%c: unknown option\n", *p);
        return 2;
      }
    }
  }

  if (command) {
    // sh -c COMMAND [name [args...]]
    if (i < argc) {
      script_name = argv[i++];
      script_args = argv + i;
      nscript_args = argc - i;
    } else {
      script_args = 0;
      nscript_args = 0;
    }
    int n = strlen(command);
    char *text = xmalloc(n + 2);
    memmove(text, command, n);
    text[n] = '\n';
    text[n + 1] = 0;
    run_string(text, 1);
    free(text);
    trap_on_exit();
    return exiting ? exit_code : last_status;
  }

  if (i < argc) {
    // a script: its name is $0 and the rest are the arguments
    script_name = argv[i];
    script_args = argv + i + 1;
    nscript_args = argc - i - 1;
    int r = run_file(argv[i]);
    trap_on_exit();
    return exiting ? exit_code : r;
  }

  setup_terminal();
  script_args = 0;
  nscript_args = 0;
  if (interactive) {
    interactive_loop();
  } else {
    // a script on the standard input
    int cap = 4096, len = 0;
    char *text = xmalloc(cap);
    for (;;) {
      if (len + 1024 > cap) {
        cap *= 2;
        char *t = xmalloc(cap);
        memmove(t, text, len);
        free(text);
        text = t;
      }
      int n = read(0, text + len, cap - len - 1);
      if (n <= 0)
        break;
      len += n;
    }
    text[len] = 0;
    run_string(text, 1);
    free(text);
  }
  trap_on_exit();
  return exiting ? exit_code : last_status;
}
