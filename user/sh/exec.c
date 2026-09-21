//
// running commands.
//
// the tree the parser built is walked here. a simple command is either
// a built-in, a function, or a program; a pipeline is a chain of
// children joined by pipes, all in one process group; a compound
// command is a walk of its parts. redirections are applied in the
// child, or saved and undone in the shell when a built-in needs them.
//
#include "sh.h"

int loop_break, loop_continue, function_return, exiting, exit_code;

// how deep we are inside a function, so that `return` knows whether it
// has anything to return from
static int function_depth;

// ---- redirections ----
//
// a built-in runs in the shell itself, so its redirections have to be
// undone afterwards. each one saves the descriptor it replaces.

struct saved_fd {
  int fd;
  int saved;   // where the old descriptor went, or -1 if there was none
};

static int
open_redir(struct redir *r, char *file)
{
  switch (r->kind) {
  case R_IN:
    return open(file, O_RDONLY);
  case R_OUT:
  case R_CLOBBER:
    return open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  case R_APPEND:
    return open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
  case R_RDWR:
    return open(file, O_RDWR | O_CREAT, 0666);
  default:
    return -1;
  }
}

// the text of a here-document, in a temporary file the child reads
static int
heredoc_fd(struct redir *r)
{
  char name[32];
  static int counter;

  snprintf(name, sizeof(name), "/tmp.here%d.%d", shell_pid, counter++);
  int fd = open(name, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) {
    // without /tmp, fall back to the working directory
    snprintf(name, sizeof(name), "here%d.%d", shell_pid, counter++);
    fd = open(name, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      sh_error("cannot make a here-document: %s", strerror(errno));
      return -1;
    }
  }
  char *text = r->heredoc ? r->heredoc : "";
  char *expanded = 0;
  if (!r->heredoc_quoted) {
    expanded = expand_here(text);
    text = expanded;
  }
  int len = strlen(text);
  if (write(fd, text, len) != len)
    sh_error("cannot write a here-document: %s", strerror(errno));
  free(expanded);
  close(fd);
  fd = open(name, O_RDONLY);
  unlink(name); // it lives only as long as the descriptor
  return fd;
}

// a here-string is the word, with a newline after it
static int
herestr_fd(struct redir *r)
{
  struct redir copy = *r;
  char *text = expand_to_string(r->word, 1);
  int n = strlen(text);
  char *with_nl = xmalloc(n + 2);

  memmove(with_nl, text, n);
  with_nl[n] = '\n';
  with_nl[n + 1] = 0;
  free(text);
  copy.heredoc = with_nl;
  copy.heredoc_quoted = 1;
  int fd = heredoc_fd(&copy);
  free(with_nl);
  return fd;
}

// apply one redirection. `saves` is where to record what to undo, or 0
// in a child, which never has to undo anything.
static int
apply_redir(struct redir *r, struct saved_fd *saves, int *nsaves)
{
  int fd = -1;

  if (r->kind == R_HEREDOC) {
    fd = heredoc_fd(r);
  } else if (r->kind == R_HERESTR) {
    fd = herestr_fd(r);
  } else if (r->kind == R_DUPIN || r->kind == R_DUPOUT) {
    char *target = expand_to_string(r->word, 0);
    if (strcmp(target, "-") == 0) {
      if (saves) {
        saves[*nsaves].fd = r->fd;
        saves[*nsaves].saved = dup(r->fd);
        (*nsaves)++;
      }
      close(r->fd);
      free(target);
      return 0;
    }
    if (!isdigit((unsigned char)target[0])) {
      sh_error("%s: bad file descriptor", target);
      free(target);
      return -1;
    }
    fd = atoi(target);
    free(target);
    if (saves) {
      saves[*nsaves].fd = r->fd;
      saves[*nsaves].saved = dup(r->fd);
      (*nsaves)++;
    }
    if (dup2(fd, r->fd) < 0) {
      sh_error("%d: bad file descriptor", fd);
      return -1;
    }
    return 0;
  } else {
    char *file = expand_to_string(r->word, 0);
    fd = open_redir(r, file);
    if (fd < 0) {
      sh_error("%s: %s", file, strerror(errno));
      free(file);
      return -1;
    }
    free(file);
  }

  if (fd < 0)
    return -1;
  if (saves) {
    saves[*nsaves].fd = r->fd;
    saves[*nsaves].saved = dup(r->fd);
    (*nsaves)++;
  }
  if (fd != r->fd) {
    dup2(fd, r->fd);
    close(fd);
  }
  return 0;
}

static int
apply_redirs(struct redir *list, struct saved_fd *saves, int *nsaves)
{
  for (struct redir *r = list; r; r = r->next)
    if (apply_redir(r, saves, nsaves) < 0)
      return -1;
  return 0;
}

static void
undo_redirs(struct saved_fd *saves, int nsaves)
{
  for (int i = nsaves - 1; i >= 0; i--) {
    if (saves[i].saved >= 0) {
      dup2(saves[i].saved, saves[i].fd);
      close(saves[i].saved);
    } else {
      close(saves[i].fd);
    }
  }
}

// ---- finding a program ----

static char *
find_program(const char *name)
{
  if (strchr(name, '/'))
    return xstrdup(name);

  const char *path = var_get("PATH");
  if (path == 0)
    path = "/:/bin";
  int namelen = strlen(name);
  while (*path) {
    const char *end = strchr(path, ':');
    int dirlen = end ? (int)(end - path) : (int)strlen(path);
    char *full = xmalloc(dirlen + namelen + 2);
    memmove(full, path, dirlen);
    int n = dirlen;
    if (n == 0 || full[n - 1] != '/')
      full[n++] = '/';
    memmove(full + n, name, namelen);
    full[n + namelen] = 0;
    struct stat st;
    if (stat(full, &st) == 0 && st.type != T_DIR)
      return full;
    free(full);
    if (end == 0)
      break;
    path = end + 1;
  }
  return 0;
}

// ---- running a simple command ----

static void
trace(char **argv)
{
  if (!opt_xtrace)
    return;
  fprintf(stderr, "+");
  for (int i = 0; argv[i]; i++)
    fprintf(stderr, " %s", argv[i]);
  fprintf(stderr, "\n");
}

// the assignments in front of a command, which apply to the shell when
// there is no command and to the child's environment when there is
static void
do_assignments(struct word *assigns, int export_them)
{
  for (struct word *a = assigns; a; a = a->next) {
    char *text = expand_to_string(a, 0);
    char *eq = strchr(text, '=');
    if (eq) {
      *eq = 0;
      var_set(text, eq + 1, export_them);
    }
    free(text);
  }
}

// what a child does once it has been forked: its own group, its
// redirections, and then the program.
static void
child_exec(struct cmd *c, char **argv, int pgid, int background)
{
  if (opt_monitor) {
    int me = getpid();
    setpgid(0, pgid ? pgid : me);
    if (!background && interactive)
      give_terminal(pgid ? pgid : me);
  }
  trap_reset_for_child();
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);

  if (apply_redirs(c->redirs, 0, 0) < 0)
    _exit(1);
  if (argv == 0 || argv[0] == 0)
    _exit(0);

  do_assignments(c->assigns, 1);

  // a function or built-in reached through a pipeline runs here, in
  // the child, and its exit status is the child's
  struct func *f = func_find(argv[0]);
  if (f) {
    int saved_n = nscript_args;
    char **saved_a = script_args;
    int n = 0;
    while (argv[n])
      n++;
    script_args = argv + 1;
    nscript_args = n - 1;
    run_cmd(f->body);
    script_args = saved_a;
    nscript_args = saved_n;
    _exit(last_status);
  }
  builtin_fn fn = builtin_lookup(argv[0]);
  if (fn) {
    int n = 0;
    while (argv[n])
      n++;
    _exit(fn(n, argv));
  }

  char *program = find_program(argv[0]);
  if (program == 0) {
    fprintf(stderr, "%s: command not found\n", argv[0]);
    _exit(127);
  }
  char **env = var_environ();
  execve(program, argv, env);
  fprintf(stderr, "%s: %s\n", program, strerror(errno));
  _exit(errno == ENOENT ? 127 : 126);
}

// the text of a command, for the jobs listing
static char *
command_text(struct cmd *c)
{
  struct buf {
    char b[128];
  } tmp;
  int n = 0;

  tmp.b[0] = 0;
  for (struct word *w = c ? c->words : 0; w && n < 100; w = w->next) {
    int len = strlen(w->text);
    if (n + len + 2 >= (int)sizeof(tmp.b))
      break;
    if (n)
      tmp.b[n++] = ' ';
    memmove(tmp.b + n, w->text, len);
    n += len;
    tmp.b[n] = 0;
  }
  return xstrdup(tmp.b);
}

static int
run_simple(struct cmd *c, int background)
{
  struct wordlist wl;

  wl_init(&wl);
  expand_word_list(c->words, &wl, 0);

  // nothing but assignments: they apply to the shell itself
  if (wl.n == 0) {
    struct saved_fd saves[16];
    int nsaves = 0;
    int r = apply_redirs(c->redirs, saves, &nsaves);
    if (r == 0)
      do_assignments(c->assigns, 0);
    undo_redirs(saves, nsaves);
    wl_free(&wl);
    return r < 0 ? 1 : 0;
  }
  trace(wl.words);

  // a special built-in, or one that must change the shell itself,
  // runs here rather than in a child
  builtin_fn fn = builtin_lookup(wl.words[0]);
  struct func *f = func_find(wl.words[0]);
  if ((fn || f) && !background) {
    struct saved_fd saves[16];
    int nsaves = 0;
    int status = 0;

    if (apply_redirs(c->redirs, saves, &nsaves) < 0) {
      undo_redirs(saves, nsaves);
      wl_free(&wl);
      return 1;
    }
    if (f) {
      // a function's arguments replace the positional ones
      int saved_n = nscript_args;
      char **saved_a = script_args;
      char *saved_name = script_name;
      script_args = wl.words + 1;
      nscript_args = wl.n - 1;
      function_depth++;
      do_assignments(c->assigns, 0);
      run_cmd(f->body);
      function_depth--;
      function_return = 0;
      status = last_status;
      script_args = saved_a;
      nscript_args = saved_n;
      script_name = saved_name;
    } else {
      // assignments in front of a built-in last only for it, except
      // for the special ones, where they last
      if (builtin_special(wl.words[0])) {
        do_assignments(c->assigns, 0);
        status = fn(wl.n, wl.words);
      } else {
        do_assignments(c->assigns, 0);
        status = fn(wl.n, wl.words);
      }
    }
    undo_redirs(saves, nsaves);
    wl_free(&wl);
    return status;
  }

  // a program, or a built-in in the background: fork
  fflush(0);
  int pid = fork();
  if (pid < 0) {
    sh_error("fork: %s", strerror(errno));
    wl_free(&wl);
    return 1;
  }
  if (pid == 0)
    child_exec(c, wl.words, 0, background);

  char *text = command_text(c);
  struct job *j = job_new(opt_monitor ? pid : 0, text, background);
  free(text);
  job_add_pid(j, pid);
  if (opt_monitor)
    setpgid(pid, pid); // the parent sets it too, against the race

  int status;
  if (background) {
    last_bg_pid = pid;
    printf("[%d] %d\n", j->id, pid);
    status = 0;
  } else {
    if (opt_monitor)
      give_terminal(pid);
    job_wait(j);
    status = last_status;
  }
  wl_free(&wl);
  return status;
}

// ---- pipelines ----

// the commands of a pipeline, flattened left to right
static int
collect_pipeline(struct cmd *c, struct cmd **out, int max)
{
  if (c->kind != C_PIPE) {
    if (max < 1)
      return 0;
    out[0] = c;
    return 1;
  }
  int n = collect_pipeline(c->left, out, max);
  if (n < max)
    out[n++] = c->right;
  return n;
}

static int
run_pipeline(struct cmd *c, int background)
{
  struct cmd *parts[MAXPROCS];
  int n = collect_pipeline(c, parts, MAXPROCS);
  int prev_read = -1, pgid = 0;
  char *text = command_text(parts[0]);
  struct job *j = job_new(0, text, background);

  free(text);
  fflush(0);
  for (int i = 0; i < n; i++) {
    int fds[2] = {-1, -1};
    if (i + 1 < n && pipe(fds) < 0) {
      sh_error("pipe: %s", strerror(errno));
      break;
    }
    int pid = fork();
    if (pid < 0) {
      sh_error("fork: %s", strerror(errno));
      break;
    }
    if (pid == 0) {
      if (prev_read >= 0) {
        dup2(prev_read, 0);
        close(prev_read);
      }
      if (fds[1] >= 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
      }
      if (parts[i]->kind == C_SIMPLE) {
        struct wordlist wl;
        wl_init(&wl);
        expand_word_list(parts[i]->words, &wl, 0);
        child_exec(parts[i], wl.words, pgid, background);
      } else {
        // a compound command in a pipeline runs in its own child
        if (opt_monitor) {
          int me = getpid();
          setpgid(0, pgid ? pgid : me);
        }
        trap_reset_for_child();
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (apply_redirs(parts[i]->redirs, 0, 0) < 0)
          _exit(1);
        run_cmd(parts[i]);
        _exit(last_status);
      }
    }
    if (pgid == 0)
      pgid = pid;
    if (opt_monitor)
      setpgid(pid, pgid);
    job_add_pid(j, pid);

    if (prev_read >= 0)
      close(prev_read);
    if (fds[1] >= 0)
      close(fds[1]);
    prev_read = fds[0];
  }
  if (prev_read >= 0)
    close(prev_read);

  j->pgid = pgid;
  if (background) {
    last_bg_pid = j->pids[j->npids - 1];
    printf("[%d] %d\n", j->id, j->pgid);
    return 0;
  }
  if (opt_monitor)
    give_terminal(pgid);
  job_wait(j);
  return last_status;
}

// ---- the walk ----

int
run_cmd(struct cmd *c)
{
  if (c == 0 || exiting)
    return last_status;

  switch (c->kind) {
  case C_SIMPLE:
    last_status = run_simple(c, c->background);
    break;

  case C_PIPE:
    last_status = run_pipeline(c, c->background);
    break;

  case C_NOT:
    run_cmd(c->body);
    last_status = !last_status;
    break;

  case C_LIST:
    run_cmd(c->left);
    if (loop_break || loop_continue || function_return || exiting)
      break;
    run_cmd(c->right);
    break;

  case C_AND:
    run_cmd(c->left);
    if (last_status == 0 && !loop_break && !loop_continue && !function_return && !exiting)
      run_cmd(c->right);
    break;

  case C_OR:
    run_cmd(c->left);
    if (last_status != 0 && !loop_break && !loop_continue && !function_return && !exiting)
      run_cmd(c->right);
    break;

  case C_GROUP: {
    struct saved_fd saves[16];
    int nsaves = 0;
    if (apply_redirs(c->redirs, saves, &nsaves) < 0) {
      undo_redirs(saves, nsaves);
      last_status = 1;
      break;
    }
    run_cmd(c->body);
    undo_redirs(saves, nsaves);
    break;
  }

  case C_SUBSHELL: {
    fflush(0);
    int pid = fork();
    if (pid < 0) {
      sh_error("fork: %s", strerror(errno));
      last_status = 1;
      break;
    }
    if (pid == 0) {
      if (opt_monitor)
        setpgid(0, 0);
      trap_reset_for_child();
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      if (apply_redirs(c->redirs, 0, 0) < 0)
        _exit(1);
      interactive = 0;
      run_cmd(c->body);
      _exit(last_status);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
      trap_run_pending();
    last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    break;
  }

  case C_IF: {
    struct saved_fd saves[16];
    int nsaves = 0;
    if (apply_redirs(c->redirs, saves, &nsaves) < 0) {
      undo_redirs(saves, nsaves);
      last_status = 1;
      break;
    }
    run_cmd(c->cond);
    if (last_status == 0) {
      last_status = 0;
      run_cmd(c->then_part);
    } else if (c->else_part) {
      last_status = 0;
      run_cmd(c->else_part);
    } else {
      last_status = 0;
    }
    undo_redirs(saves, nsaves);
    break;
  }

  case C_WHILE:
  case C_UNTIL: {
    struct saved_fd saves[16];
    int nsaves = 0;
    if (apply_redirs(c->redirs, saves, &nsaves) < 0) {
      undo_redirs(saves, nsaves);
      last_status = 1;
      break;
    }
    last_status = 0;
    for (;;) {
      run_cmd(c->cond);
      int go = (c->kind == C_WHILE) ? (last_status == 0) : (last_status != 0);
      if (!go || exiting || function_return)
        break;
      last_status = 0;
      run_cmd(c->body);
      if (loop_break) {
        loop_break--;
        break;
      }
      if (loop_continue) {
        loop_continue--;
        if (loop_continue)
          break;
      }
      if (exiting || function_return)
        break;
    }
    undo_redirs(saves, nsaves);
    break;
  }

  case C_FOR: {
    struct saved_fd saves[16];
    int nsaves = 0;
    if (apply_redirs(c->redirs, saves, &nsaves) < 0) {
      undo_redirs(saves, nsaves);
      last_status = 1;
      break;
    }
    struct wordlist wl;
    wl_init(&wl);
    if (c->has_items) {
      expand_word_list(c->items, &wl, 0);
    } else {
      for (int i = 0; i < nscript_args; i++)
        wl_add(&wl, xstrdup(script_args[i]));
    }
    last_status = 0;
    for (int i = 0; i < wl.n; i++) {
      var_set(c->var, wl.words[i], 0);
      run_cmd(c->body);
      if (loop_break) {
        loop_break--;
        break;
      }
      if (loop_continue) {
        loop_continue--;
        if (loop_continue)
          break;
      }
      if (exiting || function_return)
        break;
    }
    wl_free(&wl);
    undo_redirs(saves, nsaves);
    break;
  }

  case C_CASE: {
    struct saved_fd saves[16];
    int nsaves = 0;
    if (apply_redirs(c->redirs, saves, &nsaves) < 0) {
      undo_redirs(saves, nsaves);
      last_status = 1;
      break;
    }
    char *subject = expand_to_string(c->subject, 0);
    last_status = 0;
    for (struct case_item *it = c->items_case; it; it = it->next) {
      int matched = 0;
      for (struct word *p = it->patterns; p && !matched; p = p->next) {
        char *pattern = expand_to_string(p, 0);
        if (pattern_match(pattern, subject))
          matched = 1;
        free(pattern);
      }
      if (matched) {
        run_cmd(it->body);
        break;
      }
    }
    free(subject);
    undo_redirs(saves, nsaves);
    break;
  }

  case C_FUNCTION:
    func_define(c->fname, c->body);
    c->body = 0; // the function owns it now
    last_status = 0;
    break;
  }

  if (opt_errexit && last_status != 0 && !exiting) {
    // -e: leave, unless the command was a condition
    if (c->kind == C_SIMPLE || c->kind == C_PIPE) {
      exiting = 1;
      exit_code = last_status;
    }
  }
  return last_status;
}

int
run_cmd_status(struct cmd *c)
{
  run_cmd(c);
  return last_status;
}

// `return` is only meaningful inside a function or a sourced file
int
sh_in_function(void)
{
  return function_depth;
}
