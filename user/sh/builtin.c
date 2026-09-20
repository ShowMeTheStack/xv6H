//
// the built-in commands.
//
// a command is built in either because it must change the shell itself
// (cd, export, exit) or because running a program for it would cost
// more than the work is worth (echo, test, true). the special ones,
// which POSIX lists, differ in that a failure in them ends a
// non-interactive shell and their assignments outlive the command.
//
#include "sh.h"
#include <glob.h>

extern int sh_in_function(void);

// ---- small helpers ----

static int
opt_is(const char *s, const char *opt)
{
  return strcmp(s, opt) == 0;
}

// ---- cd, pwd ----

static int
bi_cd(int argc, char **argv)
{
  const char *dir;
  char *allocated = 0;

  if (argc < 2 || opt_is(argv[1], "--")) {
    dir = var_get("HOME");
    if (dir == 0) {
      sh_error("cd: HOME is not set");
      return 1;
    }
  } else if (opt_is(argv[1], "-")) {
    dir = var_get("OLDPWD");
    if (dir == 0) {
      sh_error("cd: OLDPWD is not set");
      return 1;
    }
    printf("%s\n", dir);
  } else {
    dir = argv[1];
  }

  char old[PATH_MAX];
  if (getcwd(old, sizeof(old)) == 0)
    old[0] = 0;
  if (chdir(dir) < 0) {
    sh_error("cd: %s: %s", dir, strerror(errno));
    free(allocated);
    return 1;
  }
  if (old[0])
    var_set("OLDPWD", old, 1);
  char now[PATH_MAX];
  if (getcwd(now, sizeof(now)))
    var_set("PWD", now, 1);
  free(allocated);
  return 0;
}

static int
bi_pwd(int argc, char **argv)
{
  char buf[PATH_MAX];

  (void)argc;
  (void)argv;
  if (getcwd(buf, sizeof(buf)) == 0) {
    sh_error("pwd: %s", strerror(errno));
    return 1;
  }
  printf("%s\n", buf);
  return 0;
}

// ---- echo, printf ----

static int
bi_echo(int argc, char **argv)
{
  int i = 1, newline = 1, escapes = 0;

  while (i < argc && argv[i][0] == '-' && argv[i][1] &&
         strspn(argv[i] + 1, "neE") == strlen(argv[i] + 1)) {
    for (char *p = argv[i] + 1; *p; p++) {
      if (*p == 'n')
        newline = 0;
      else if (*p == 'e')
        escapes = 1;
      else if (*p == 'E')
        escapes = 0;
    }
    i++;
  }
  for (; i < argc; i++) {
    if (!escapes) {
      fputs(argv[i], stdout);
    } else {
      for (char *p = argv[i]; *p; p++) {
        if (*p != '\\' || p[1] == 0) {
          putchar(*p);
          continue;
        }
        p++;
        switch (*p) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'f': putchar('\f'); break;
        case 'v': putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0': {
          int v = 0, n = 0;
          while (n < 3 && p[1] >= '0' && p[1] <= '7') {
            v = v * 8 + (*++p - '0');
            n++;
          }
          putchar(v);
          break;
        }
        case 'c':
          newline = 0;
          goto done;
        default:
          putchar('\\');
          putchar(*p);
          break;
        }
      }
    }
    if (i + 1 < argc)
      putchar(' ');
  }
done:
  if (newline)
    putchar('\n');
  fflush(stdout);
  return 0;
}

// printf(1): the format is reused until the arguments run out
static int
bi_printf(int argc, char **argv)
{
  if (argc < 2) {
    sh_error("printf: usage: printf FORMAT [ARGUMENT...]");
    return 1;
  }
  const char *fmt = argv[1];
  int arg = 2;

  do {
    for (const char *p = fmt; *p; p++) {
      if (*p == '\\' && p[1]) {
        p++;
        switch (*p) {
        case 'n': putchar('\n'); break;
        case 't': putchar('\t'); break;
        case 'r': putchar('\r'); break;
        case '\\': putchar('\\'); break;
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'f': putchar('\f'); break;
        case 'v': putchar('\v'); break;
        default: putchar('\\'); putchar(*p); break;
        }
        continue;
      }
      if (*p != '%') {
        putchar(*p);
        continue;
      }
      if (p[1] == '%') {
        putchar('%');
        p++;
        continue;
      }
      // gather the conversion specification
      char spec[32];
      int n = 0;
      spec[n++] = *p++;
      while (*p && strchr("-+ #0123456789.*", *p) && n < 28)
        spec[n++] = *p++;
      if (*p == 0)
        break;
      char conv = *p;
      spec[n++] = conv;
      spec[n] = 0;
      const char *a = arg < argc ? argv[arg++] : "";
      switch (conv) {
      case 'd': case 'i': {
        spec[n - 1] = 'l';
        spec[n] = 'd';
        spec[n + 1] = 0;
        printf(spec, strtol(a, 0, 0));
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        spec[n - 1] = 'l';
        spec[n] = conv;
        spec[n + 1] = 0;
        printf(spec, strtoul(a, 0, 0));
        break;
      }
      case 'c':
        putchar(a[0]);
        break;
      case 's':
        printf(spec, a);
        break;
      case 'f': case 'e': case 'g': case 'E': case 'G':
        printf(spec, strtod(a, 0));
        break;
      case 'b': { // the shell's own: escapes in the argument
        char *tmp[3] = {"echo", "-e", (char *)a};
        int saved = 1;
        (void)saved;
        for (const char *q = a; *q; q++) {
          if (*q == '\\' && q[1]) {
            q++;
            switch (*q) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case '\\': putchar('\\'); break;
            default: putchar('\\'); putchar(*q); break;
            }
          } else {
            putchar(*q);
          }
        }
        (void)tmp;
        break;
      }
      default:
        putchar('%');
        putchar(conv);
        break;
      }
    }
  } while (arg < argc && arg > 2);
  fflush(stdout);
  return 0;
}

// ---- true, false, colon ----

static int
bi_true(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  return 0;
}

static int
bi_false(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  return 1;
}

// ---- exit, return, break, continue ----

static int
bi_exit(int argc, char **argv)
{
  exiting = 1;
  exit_code = argc > 1 ? atoi(argv[1]) : last_status;
  return exit_code;
}

static int
bi_return(int argc, char **argv)
{
  if (!sh_in_function()) {
    // in a sourced file `return` leaves the file; at the top level it
    // is an error, which POSIX leaves to the shell
    exiting = 1;
    exit_code = argc > 1 ? atoi(argv[1]) : last_status;
    return exit_code;
  }
  function_return = 1;
  return argc > 1 ? atoi(argv[1]) : last_status;
}

static int
bi_break(int argc, char **argv)
{
  loop_break = argc > 1 ? atoi(argv[1]) : 1;
  if (loop_break < 1)
    loop_break = 1;
  return 0;
}

static int
bi_continue(int argc, char **argv)
{
  loop_continue = argc > 1 ? atoi(argv[1]) : 1;
  if (loop_continue < 1)
    loop_continue = 1;
  return 0;
}

// ---- variables ----

static int
bi_export(int argc, char **argv)
{
  int i = 1, unexport = 0;

  if (i < argc && opt_is(argv[i], "-p")) {
    var_dump(1);
    return 0;
  }
  if (i < argc && opt_is(argv[i], "-n")) {
    unexport = 1;
    i++;
  }
  if (i >= argc) {
    var_dump(1);
    return 0;
  }
  for (; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) {
      *eq = 0;
      var_set(argv[i], eq + 1, !unexport);
      var_export(argv[i], !unexport);
      *eq = '=';
    } else {
      var_export(argv[i], !unexport);
    }
  }
  return 0;
}

static int
bi_readonly(int argc, char **argv)
{
  if (argc < 2 || opt_is(argv[1], "-p")) {
    var_dump(0);
    return 0;
  }
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) {
      *eq = 0;
      var_set(argv[i], eq + 1, 0);
      var_readonly(argv[i], 1);
      *eq = '=';
    } else {
      var_readonly(argv[i], 1);
    }
  }
  return 0;
}

static int
bi_unset(int argc, char **argv)
{
  int functions = 0, i = 1;

  if (i < argc && opt_is(argv[i], "-f")) {
    functions = 1;
    i++;
  } else if (i < argc && opt_is(argv[i], "-v")) {
    i++;
  }
  for (; i < argc; i++) {
    if (functions)
      func_undefine(argv[i]);
    else
      var_unset(argv[i]);
  }
  return 0;
}

static int
bi_set(int argc, char **argv)
{
  if (argc == 1) {
    var_dump(0);
    return 0;
  }
  int i = 1;
  for (; i < argc; i++) {
    if (argv[i][0] != '-' && argv[i][0] != '+')
      break;
    if (opt_is(argv[i], "--")) {
      i++;
      break;
    }
    int on = argv[i][0] == '-';
    for (char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'e': opt_errexit = on; break;
      case 'x': opt_xtrace = on; break;
      case 'n': opt_noexec = on; break;
      case 'u': opt_nounset = on; break;
      case 'v': opt_verbose = on; break;
      case 'f': opt_noglob = on; break;
      case 'm': opt_monitor = on; break;
      case 'o':
        if (i + 1 < argc) {
          const char *name = argv[++i];
          if (strcmp(name, "errexit") == 0)
            opt_errexit = on;
          else if (strcmp(name, "xtrace") == 0)
            opt_xtrace = on;
          else if (strcmp(name, "nounset") == 0)
            opt_nounset = on;
          else if (strcmp(name, "noglob") == 0)
            opt_noglob = on;
          else if (strcmp(name, "monitor") == 0)
            opt_monitor = on;
          else if (strcmp(name, "verbose") == 0)
            opt_verbose = on;
          else
            sh_error("set: %s: unknown option", name);
        }
        break;
      default:
        sh_error("set: -%c: unknown option", *p);
        return 2;
      }
    }
  }
  if (i < argc || (argc > 1 && strcmp(argv[argc - 1], "--") == 0)) {
    // the rest become the positional parameters
    int n = argc - i;
    char **args = xmalloc((n + 1) * sizeof(char *));
    for (int k = 0; k < n; k++)
      args[k] = xstrdup(argv[i + k]);
    args[n] = 0;
    script_args = args;
    nscript_args = n;
  }
  return 0;
}

static int
bi_shift(int argc, char **argv)
{
  int n = argc > 1 ? atoi(argv[1]) : 1;

  if (n < 0 || n > nscript_args) {
    sh_error("shift: %d: cannot shift that many", n);
    return 1;
  }
  script_args += n;
  nscript_args -= n;
  return 0;
}

// read [-r] [-p prompt] NAME...
static int
bi_read(int argc, char **argv)
{
  int raw = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (opt_is(argv[i], "-r"))
      raw = 1;
    else if (opt_is(argv[i], "-p") && i + 1 < argc)
      fputs(argv[++i], stderr);
    else
      break;
  }

  char *line = 0;
  size_t cap = 0;
  ssize_t n = getline(&line, &cap, stdin);
  if (n <= 0) {
    free(line);
    return 1;
  }
  if (n > 0 && line[n - 1] == '\n')
    line[--n] = 0;
  if (!raw) {
    // a backslash removes the meaning of the next character
    char *w = line;
    for (char *p = line; *p; p++) {
      if (*p == '\\' && p[1])
        p++;
      *w++ = *p;
    }
    *w = 0;
  }

  const char *ifs = var_get("IFS");
  if (ifs == 0)
    ifs = " \t\n";
  char *p = line;
  int names = argc - i;
  if (names <= 0) {
    var_set("REPLY", line, 0);
    free(line);
    return 0;
  }
  for (int k = 0; k < names; k++) {
    while (*p && strchr(ifs, *p))
      p++;
    if (k == names - 1) {
      // the last name takes what is left, with the trailing space cut
      char *end = p + strlen(p);
      while (end > p && strchr(ifs, end[-1]))
        end--;
      *end = 0;
      var_set(argv[i + k], p, 0);
      break;
    }
    char *start = p;
    while (*p && !strchr(ifs, *p))
      p++;
    if (*p)
      *p++ = 0;
    var_set(argv[i + k], start, 0);
  }
  free(line);
  return 0;
}

// ---- test ----
//
// the expression language of test(1) and [ ]. the grammar is small
// enough to parse by hand, with the usual precedence: ! then -a then
// -o, and the primaries in between.

struct test_state {
  char **argv;
  int argc;
  int pos;
  int err;
};

static int test_or(struct test_state *t);

static int
file_type(const char *path, short *type, uint64 *size)
{
  struct stat st;

  if (stat(path, &st) < 0)
    return -1;
  if (type)
    *type = st.type;
  if (size)
    *size = st.size;
  return 0;
}

static int
test_primary(struct test_state *t)
{
  if (t->pos >= t->argc) {
    t->err = 1;
    return 0;
  }
  char *a = t->argv[t->pos];

  if (strcmp(a, "!") == 0) {
    t->pos++;
    return !test_primary(t);
  }
  if (strcmp(a, "(") == 0) {
    t->pos++;
    int v = test_or(t);
    if (t->pos < t->argc && strcmp(t->argv[t->pos], ")") == 0)
      t->pos++;
    else
      t->err = 1;
    return v;
  }

  // the unary operators
  if (a[0] == '-' && a[1] && a[2] == 0 && t->pos + 1 < t->argc) {
    char op = a[1];
    char *arg = t->argv[t->pos + 1];
    // -a and -o are binary; they are not unary operators here
    if (op != 'a' && op != 'o') {
      t->pos += 2;
      short type;
      uint64 size;
      switch (op) {
      case 'e':
        return file_type(arg, 0, 0) == 0;
      case 'f':
        return file_type(arg, &type, 0) == 0 && type == T_FILE;
      case 'd':
        return file_type(arg, &type, 0) == 0 && type == T_DIR;
      case 'c':
        return file_type(arg, &type, 0) == 0 && type == T_DEVICE;
      case 'b':
        return 0; // no block devices yet
      case 's':
        return file_type(arg, 0, &size) == 0 && size > 0;
      case 'r':
      case 'w':
      case 'x':
        return file_type(arg, 0, 0) == 0; // permissions arrive with users
      case 'z':
        return *arg == 0;
      case 'n':
        return *arg != 0;
      case 't': {
        int fd = atoi(arg);
        return isatty(fd);
      }
      case 'h':
      case 'L':
        return 0; // no symbolic links yet
      case 'p':
      case 'S':
      case 'g':
      case 'u':
      case 'k':
        return 0;
      default:
        t->err = 1;
        return 0;
      }
    }
  }

  // the binary operators
  if (t->pos + 2 < t->argc) {
    char *op = t->argv[t->pos + 1];
    char *rhs = t->argv[t->pos + 2];
    int handled = 1;
    int result = 0;
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
      result = strcmp(a, rhs) == 0;
    else if (strcmp(op, "!=") == 0)
      result = strcmp(a, rhs) != 0;
    else if (strcmp(op, "-eq") == 0)
      result = strtol(a, 0, 10) == strtol(rhs, 0, 10);
    else if (strcmp(op, "-ne") == 0)
      result = strtol(a, 0, 10) != strtol(rhs, 0, 10);
    else if (strcmp(op, "-lt") == 0)
      result = strtol(a, 0, 10) < strtol(rhs, 0, 10);
    else if (strcmp(op, "-le") == 0)
      result = strtol(a, 0, 10) <= strtol(rhs, 0, 10);
    else if (strcmp(op, "-gt") == 0)
      result = strtol(a, 0, 10) > strtol(rhs, 0, 10);
    else if (strcmp(op, "-ge") == 0)
      result = strtol(a, 0, 10) >= strtol(rhs, 0, 10);
    else if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0 || strcmp(op, "-ef") == 0)
      result = 0; // there are no file times yet
    else
      handled = 0;
    if (handled) {
      t->pos += 3;
      return result;
    }
  }

  // a bare word is true when it is not empty
  t->pos++;
  return *a != 0;
}

static int
test_and(struct test_state *t)
{
  int v = test_primary(t);

  while (t->pos < t->argc && strcmp(t->argv[t->pos], "-a") == 0) {
    t->pos++;
    int r = test_primary(t);
    v = v && r;
  }
  return v;
}

static int
test_or(struct test_state *t)
{
  int v = test_and(t);

  while (t->pos < t->argc && strcmp(t->argv[t->pos], "-o") == 0) {
    t->pos++;
    int r = test_and(t);
    v = v || r;
  }
  return v;
}

static int
bi_test(int argc, char **argv)
{
  int n = argc;

  if (strcmp(argv[0], "[") == 0) {
    if (n < 2 || strcmp(argv[n - 1], "]") != 0) {
      sh_error("[: missing ]");
      return 2;
    }
    n--;
  }
  if (n <= 1)
    return 1;
  struct test_state t = {argv, n, 1, 0};
  int v = test_or(&t);
  if (t.err || t.pos != n) {
    sh_error("%s: bad expression", argv[0]);
    return 2;
  }
  return v ? 0 : 1;
}

// ---- eval, exec, dot, source ----

static int
bi_eval(int argc, char **argv)
{
  if (argc < 2)
    return 0;
  int len = 0;
  for (int i = 1; i < argc; i++)
    len += strlen(argv[i]) + 1;
  char *text = xmalloc(len + 2);
  int n = 0;
  for (int i = 1; i < argc; i++) {
    int l = strlen(argv[i]);
    memmove(text + n, argv[i], l);
    n += l;
    text[n++] = ' ';
  }
  text[n++] = '\n';
  text[n] = 0;
  int r = run_string(text, 1);
  free(text);
  return r;
}

static int
bi_exec(int argc, char **argv)
{
  if (argc < 2)
    return 0; // the redirections have already been applied
  char **env = var_environ();
  char *program = 0;
  if (strchr(argv[1], '/')) {
    program = xstrdup(argv[1]);
  } else {
    const char *path = var_get("PATH");
    char buf[PATH_MAX];
    if (path == 0)
      path = "/:/bin";
    while (*path && program == 0) {
      const char *end = strchr(path, ':');
      int dlen = end ? (int)(end - path) : (int)strlen(path);
      snprintf(buf, sizeof(buf), "%.*s%s%s", dlen, path, dlen && path[dlen - 1] == '/' ? "" : "/",
               argv[1]);
      struct stat st;
      if (stat(buf, &st) == 0)
        program = xstrdup(buf);
      if (end == 0)
        break;
      path = end + 1;
    }
  }
  if (program == 0) {
    sh_error("exec: %s: not found", argv[1]);
    return 127;
  }
  fflush(0);
  execve(program, argv + 1, env);
  sh_error("exec: %s: %s", program, strerror(errno));
  free(program);
  return 126;
}

static int
bi_dot(int argc, char **argv)
{
  if (argc < 2) {
    sh_error("%s: usage: %s FILE", argv[0], argv[0]);
    return 2;
  }
  char *path = 0;
  if (strchr(argv[1], '/')) {
    path = xstrdup(argv[1]);
  } else {
    const char *p = var_get("PATH");
    char buf[PATH_MAX];
    if (p == 0)
      p = "/:/bin";
    while (*p && path == 0) {
      const char *end = strchr(p, ':');
      int dlen = end ? (int)(end - p) : (int)strlen(p);
      snprintf(buf, sizeof(buf), "%.*s%s%s", dlen, p, dlen && p[dlen - 1] == '/' ? "" : "/", argv[1]);
      struct stat st;
      if (stat(buf, &st) == 0)
        path = xstrdup(buf);
      if (end == 0)
        break;
      p = end + 1;
    }
    if (path == 0)
      path = xstrdup(argv[1]);
  }
  int r = run_file(path);
  free(path);
  return r;
}

// ---- jobs ----

static int
bi_jobs(int argc, char **argv)
{
  int mode = 0;

  for (int i = 1; i < argc; i++)
    if (argv[i][0] == '-')
      mode = argv[i][1];
  job_reap(0);
  job_list(mode);
  return 0;
}

static int
bi_fg(int argc, char **argv)
{
  struct job *j = job_find(argc > 1 ? argv[1] : 0);

  if (j == 0) {
    sh_error("fg: no such job");
    return 1;
  }
  return job_fg(j);
}

static int
bi_bg(int argc, char **argv)
{
  struct job *j = job_find(argc > 1 ? argv[1] : 0);

  if (j == 0) {
    sh_error("bg: no such job");
    return 1;
  }
  return job_bg(j);
}

static int
bi_wait(int argc, char **argv)
{
  if (argc > 1) {
    for (int i = 1; i < argc; i++) {
      int pid;
      if (argv[i][0] == '%') {
        struct job *j = job_find(argv[i]);
        if (j == 0) {
          sh_error("wait: %s: no such job", argv[i]);
          return 127;
        }
        job_wait(j);
        continue;
      }
      pid = atoi(argv[i]);
      int st;
      while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR) {
          trap_run_pending();
          continue;
        }
        return 127;
      }
      last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    return last_status;
  }
  // no argument: wait for everything
  int st;
  while (waitpid(-1, &st, 0) > 0 || errno == EINTR)
    trap_run_pending();
  job_reap(0);
  return 0;
}

static int
bi_kill(int argc, char **argv)
{
  int sig = SIGTERM, i = 1;

  if (i < argc && opt_is(argv[i], "-l")) {
    for (int s = 1; s < NSIG; s++) {
      const char *n = strsignal(s);
      if (n)
        printf("%2d) %s\n", s, n);
    }
    return 0;
  }
  if (i < argc && argv[i][0] == '-' && argv[i][1]) {
    const char *name = argv[i] + 1;
    if (strcmp(name, "s") == 0 && i + 1 < argc)
      name = argv[++i] + 0;
    int s = trap_signal_number(name);
    if (s < 0) {
      sh_error("kill: %s: invalid signal", name);
      return 1;
    }
    sig = s;
    i++;
  }
  if (i >= argc) {
    sh_error("kill: usage: kill [-SIGNAL] pid | %%job ...");
    return 1;
  }
  int bad = 0;
  for (; i < argc; i++) {
    int target;
    if (argv[i][0] == '%') {
      struct job *j = job_find(argv[i]);
      if (j == 0) {
        sh_error("kill: %s: no such job", argv[i]);
        bad = 1;
        continue;
      }
      target = -j->pgid;
    } else {
      target = atoi(argv[i]);
    }
    if (kill(target, sig) < 0) {
      sh_error("kill: %s: %s", argv[i], strerror(errno));
      bad = 1;
    }
  }
  return bad;
}

// ---- trap, times, umask, ulimit ----

static int
bi_trap(int argc, char **argv)
{
  if (argc == 1) {
    trap_list();
    return 0;
  }
  int i = 1;
  const char *action = 0;
  if (opt_is(argv[i], "-")) {
    i++;
  } else if (trap_signal_number(argv[i]) < 0 || isdigit((unsigned char)argv[i][0]) == 0) {
    action = argv[i++];
  } else {
    action = argv[i++];
  }
  for (; i < argc; i++) {
    int sig = trap_signal_number(argv[i]);
    if (sig < 0) {
      sh_error("trap: %s: invalid signal", argv[i]);
      return 1;
    }
    if (action == 0 || strcmp(action, "-") == 0)
      trap_set(sig, 0);
    else
      trap_set(sig, action);
  }
  return 0;
}

static int
bi_times(int argc, char **argv)
{
  struct tms t;

  (void)argc;
  (void)argv;
  if (times(&t) == (clock_t)-1)
    return 1;
  long hz = sysconf(_SC_CLK_TCK);
  printf("%ldm%lds %ldm%lds\n", t.tms_utime / hz / 60, (t.tms_utime / hz) % 60,
         t.tms_stime / hz / 60, (t.tms_stime / hz) % 60);
  printf("%ldm%lds %ldm%lds\n", t.tms_cutime / hz / 60, (t.tms_cutime / hz) % 60,
         t.tms_cstime / hz / 60, (t.tms_cstime / hz) % 60);
  return 0;
}

static int
bi_umask(int argc, char **argv)
{
  if (argc < 2) {
    int m = umask(0);
    umask(m);
    printf("%04o\n", m);
    return 0;
  }
  umask(strtol(argv[1], 0, 8));
  return 0;
}

static int
bi_ulimit(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("unlimited\n");
  return 0;
}

// ---- type, command, hash, alias ----

static int
bi_type(int argc, char **argv)
{
  int status = 0;

  for (int i = 1; i < argc; i++) {
    const char *name = argv[i];
    if (alias_get(name)) {
      printf("%s is an alias for %s\n", name, alias_get(name));
      continue;
    }
    if (func_find(name)) {
      printf("%s is a function\n", name);
      continue;
    }
    if (builtin_lookup(name)) {
      printf("%s is a shell builtin\n", name);
      continue;
    }
    const char *path = var_get("PATH");
    char buf[PATH_MAX];
    int found = 0;
    if (path == 0)
      path = "/:/bin";
    if (strchr(name, '/')) {
      struct stat st;
      if (stat(name, &st) == 0) {
        printf("%s is %s\n", name, name);
        continue;
      }
    } else {
      while (*path && !found) {
        const char *end = strchr(path, ':');
        int dlen = end ? (int)(end - path) : (int)strlen(path);
        snprintf(buf, sizeof(buf), "%.*s%s%s", dlen, path,
                 dlen && path[dlen - 1] == '/' ? "" : "/", name);
        struct stat st;
        if (stat(buf, &st) == 0) {
          printf("%s is %s\n", name, buf);
          found = 1;
        }
        if (end == 0)
          break;
        path = end + 1;
      }
    }
    if (!found) {
      fprintf(stderr, "%s: not found\n", name);
      status = 1;
    }
  }
  return status;
}

static int
bi_command(int argc, char **argv)
{
  int i = 1;

  while (i < argc && argv[i][0] == '-') {
    if (opt_is(argv[i], "-v") || opt_is(argv[i], "-V")) {
      char *args[3] = {"type", argc > i + 1 ? argv[i + 1] : "", 0};
      return bi_type(2, args);
    }
    if (opt_is(argv[i], "-p")) {
      i++;
      continue;
    }
    break;
  }
  if (i >= argc)
    return 0;
  // run the program, ignoring functions and built-ins
  fflush(0);
  int pid = fork();
  if (pid == 0) {
    char **env = var_environ();
    execvp(argv[i], argv + i);
    fprintf(stderr, "%s: %s\n", argv[i], strerror(errno));
    (void)env;
    _exit(127);
  }
  int st;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int
bi_hash(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  return 0; // nothing is remembered yet, so there is nothing to forget
}

static int
bi_alias(int argc, char **argv)
{
  if (argc == 1) {
    alias_list();
    return 0;
  }
  int status = 0;
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) {
      *eq = 0;
      alias_set(argv[i], eq + 1);
      *eq = '=';
    } else {
      const char *v = alias_get(argv[i]);
      if (v)
        printf("alias %s='%s'\n", argv[i], v);
      else {
        fprintf(stderr, "alias: %s: not found\n", argv[i]);
        status = 1;
      }
    }
  }
  return status;
}

static int
bi_unalias(int argc, char **argv)
{
  int status = 0;

  for (int i = 1; i < argc; i++)
    if (alias_unset(argv[i]) < 0) {
      fprintf(stderr, "unalias: %s: not found\n", argv[i]);
      status = 1;
    }
  return status;
}

// ---- getopts ----

static int
bi_getopts(int argc, char **argv)
{
  if (argc < 3) {
    sh_error("getopts: usage: getopts OPTSTRING NAME [ARG...]");
    return 2;
  }
  const char *spec = argv[1];
  const char *name = argv[2];
  char **args = argc > 3 ? argv + 3 : script_args;
  int nargs = argc > 3 ? argc - 3 : nscript_args;

  const char *optind_s = var_get("OPTIND");
  int optind_v = optind_s ? atoi(optind_s) : 1;
  const char *sub_s = var_get("_OPTSUB");
  int sub = sub_s ? atoi(sub_s) : 1;

  if (optind_v < 1)
    optind_v = 1;
  if (optind_v > nargs) {
    var_set_int("OPTIND", optind_v);
    return 1;
  }
  const char *arg = args[optind_v - 1];
  if (arg == 0 || arg[0] != '-' || arg[1] == 0) {
    var_set_int("OPTIND", optind_v);
    return 1;
  }
  if (strcmp(arg, "--") == 0) {
    var_set_int("OPTIND", optind_v + 1);
    return 1;
  }
  int c = arg[sub];
  if (c == 0) {
    var_set_int("OPTIND", optind_v + 1);
    var_set_int("_OPTSUB", 1);
    return bi_getopts(argc, argv);
  }
  const char *p = strchr(spec, c);
  char valbuf[2] = {c, 0};
  if (p == 0 || c == ':') {
    var_set(name, "?", 0);
    var_set("OPTARG", valbuf, 0);
    if (spec[0] != ':')
      fprintf(stderr, "%s: illegal option -- %c\n", script_name ? script_name : "sh", c);
    if (arg[sub + 1] == 0) {
      var_set_int("OPTIND", optind_v + 1);
      var_set_int("_OPTSUB", 1);
    } else {
      var_set_int("_OPTSUB", sub + 1);
    }
    return 0;
  }
  if (p[1] == ':') {
    const char *value;
    if (arg[sub + 1]) {
      value = arg + sub + 1;
      var_set_int("OPTIND", optind_v + 1);
    } else if (optind_v < nargs) {
      value = args[optind_v];
      var_set_int("OPTIND", optind_v + 2);
    } else {
      var_set(name, spec[0] == ':' ? ":" : "?", 0);
      var_set("OPTARG", valbuf, 0);
      if (spec[0] != ':')
        fprintf(stderr, "%s: option requires an argument -- %c\n",
                script_name ? script_name : "sh", c);
      var_set_int("OPTIND", optind_v + 1);
      return 0;
    }
    var_set(name, valbuf, 0);
    var_set("OPTARG", value, 0);
    var_set_int("_OPTSUB", 1);
    return 0;
  }
  var_set(name, valbuf, 0);
  var_unset("OPTARG");
  if (arg[sub + 1] == 0) {
    var_set_int("OPTIND", optind_v + 1);
    var_set_int("_OPTSUB", 1);
  } else {
    var_set_int("_OPTSUB", sub + 1);
  }
  return 0;
}

// ---- the table ----

static const struct {
  const char *name;
  builtin_fn fn;
  int special;   // POSIX's "special built-in"
} builtins[] = {
    {":", bi_true, 1},        {".", bi_dot, 1},       {"source", bi_dot, 0},
    {"alias", bi_alias, 0},   {"bg", bi_bg, 0},       {"break", bi_break, 1},
    {"cd", bi_cd, 0},         {"command", bi_command, 0}, {"continue", bi_continue, 1},
    {"echo", bi_echo, 0},     {"eval", bi_eval, 1},   {"exec", bi_exec, 1},
    {"exit", bi_exit, 1},     {"export", bi_export, 1}, {"false", bi_false, 0},
    {"fg", bi_fg, 0},         {"getopts", bi_getopts, 0}, {"hash", bi_hash, 0},
    {"jobs", bi_jobs, 0},     {"kill", bi_kill, 0},   {"printf", bi_printf, 0},
    {"pwd", bi_pwd, 0},       {"read", bi_read, 0},   {"readonly", bi_readonly, 1},
    {"return", bi_return, 1}, {"set", bi_set, 1},     {"shift", bi_shift, 1},
    {"test", bi_test, 0},     {"[", bi_test, 0},      {"times", bi_times, 1},
    {"trap", bi_trap, 1},     {"true", bi_true, 0},   {"type", bi_type, 0},
    {"ulimit", bi_ulimit, 0}, {"umask", bi_umask, 0}, {"unalias", bi_unalias, 0},
    {"unset", bi_unset, 1},   {"wait", bi_wait, 0},   {0, 0, 0},
};

builtin_fn
builtin_lookup(const char *name)
{
  for (int i = 0; builtins[i].name; i++)
    if (strcmp(name, builtins[i].name) == 0)
      return builtins[i].fn;
  return 0;
}

int
builtin_special(const char *name)
{
  for (int i = 0; builtins[i].name; i++)
    if (strcmp(name, builtins[i].name) == 0)
      return builtins[i].special;
  return 0;
}
