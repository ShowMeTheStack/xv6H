// find: walk a tree and do something with the names.
//
//   find [PATH...] [EXPRESSION]
//
// tests:
//   -name PATTERN     the last component matches (shell wildcards)
//   -path PATTERN     the whole path matches
//   -iname PATTERN    -name, ignoring case
//   -type f|d|l|c|b   what kind of file it is
//   -size N[ckMG]     size, with + for more and - for less
//   -newer FILE       changed more recently than FILE
//   -mtime N          changed N days ago, with + and -
//   -mmin N           changed N minutes ago, with + and -
//   -perm [-]MODE     the permission bits, -MODE for "at least these"
//   -user NAME        owned by them
//   -group NAME       in that group
//   -links N          that many links
//   -inum N           that inode
//   -empty            an empty file or an empty directory
//   -maxdepth N       go no deeper; -mindepth N say nothing shallower
//   -prune            do not walk into this directory
//   -false / -true
//
// actions:
//   -print (the default), -print0, -ls, -delete, -quit
//   -exec CMD {} ;    run it once per file
//   -exec CMD {} +    run it once with as many files as fit
//   -ok CMD {} ;      -exec, but ask first
//
// the expression is read as a little grammar: -a (or nothing) binds
// tighter than -o, ! negates, and \( \) group. an action anywhere in
// the expression means there is no implied -print.
#include "user/user.h"
#include <pwd.h>
#include <grp.h>
#include <fnmatch.h>

#define MAXARGS 256

enum {
  N_AND, N_OR, N_NOT,
  N_NAME, N_INAME, N_PATH, N_TYPE, N_SIZE, N_NEWER, N_MTIME, N_MMIN,
  N_PERM, N_USER, N_GROUP, N_LINKS, N_INUM, N_EMPTY, N_PRUNE,
  N_TRUE, N_FALSE,
  N_PRINT, N_PRINT0, N_LS, N_DELETE, N_EXEC, N_OK, N_QUIT
};

struct node {
  int kind;
  struct node *left, *right;
  const char *text;
  long long num;
  int sign;          // -1 for a leading -, +1 for a leading +, 0 for neither
  int type;
  char **argv;       // -exec
  int argc;
  int plus;          // -exec ... +
  int brace;         // which argument is {}
};

static struct node *pool;
static int npool, maxpool;
static char **words;
static int nwords, at;

static int maxdepth = 1 << 30, mindepth;
static int has_action;
static int status;
static int quit_now;
static uint64 now;

// -exec ... + gathers names until the batch is full
struct batch {
  struct node *n;
  char *args[MAXARGS];
  int nargs;
  struct batch *next;
};
static struct batch *batches;

static void
die(const char *msg, const char *what)
{
  fprintf(stderr, "find: %s%s%s\n", msg, what ? ": " : "", what ? what : "");
  exit(2);
}

static struct node *
new_node(int kind)
{
  if (npool == maxpool) {
    maxpool = maxpool ? maxpool * 2 : 64;
    pool = realloc(pool, maxpool * sizeof(*pool));
    if (pool == 0)
      die("out of memory", 0);
  }
  struct node *n = &pool[npool++];
  memset(n, 0, sizeof(*n));
  n->kind = kind;
  return n;
}

static const char *
peek(void)
{
  return at < nwords ? words[at] : 0;
}

static const char *
next_word(void)
{
  return at < nwords ? words[at++] : 0;
}

// a number that may carry a leading + or -, meaning "more than" or
// "less than", which is how find has always read them
static long long
signed_number(const char *s, int *sign)
{
  *sign = 0;
  if (*s == '+') {
    *sign = 1;
    s++;
  } else if (*s == '-') {
    *sign = -1;
    s++;
  }
  long long v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (*s++ - '0');
  return v;
}

static int
compare_signed(long long have, long long want, int sign)
{
  if (sign > 0)
    return have > want;
  if (sign < 0)
    return have < want;
  return have == want;
}

static struct node *parse_or(void);

static struct node *
parse_primary(void)
{
  const char *w = peek();

  if (w == 0)
    return 0;

  if (strcmp(w, "(") == 0 || strcmp(w, "\\(") == 0) {
    next_word();
    struct node *inner = parse_or();
    const char *close = next_word();
    if (close == 0 || (strcmp(close, ")") != 0 && strcmp(close, "\\)") != 0))
      die("expected )", 0);
    return inner;
  }
  if (strcmp(w, "!") == 0 || strcmp(w, "-not") == 0) {
    next_word();
    struct node *n = new_node(N_NOT);
    n->left = parse_primary();
    if (n->left == 0)
      die("! wants something to negate", 0);
    return n;
  }

  // the tests and actions that take a value
  static const struct {
    const char *name;
    int kind;
    int takes_value;
  } table[] = {
    { "-name", N_NAME, 1 },     { "-iname", N_INAME, 1 },
    { "-path", N_PATH, 1 },     { "-wholename", N_PATH, 1 },
    { "-type", N_TYPE, 1 },     { "-size", N_SIZE, 1 },
    { "-newer", N_NEWER, 1 },   { "-mtime", N_MTIME, 1 },
    { "-mmin", N_MMIN, 1 },     { "-perm", N_PERM, 1 },
    { "-user", N_USER, 1 },     { "-group", N_GROUP, 1 },
    { "-links", N_LINKS, 1 },   { "-inum", N_INUM, 1 },
    { "-empty", N_EMPTY, 0 },   { "-prune", N_PRUNE, 0 },
    { "-true", N_TRUE, 0 },     { "-false", N_FALSE, 0 },
    { "-print", N_PRINT, 0 },   { "-print0", N_PRINT0, 0 },
    { "-ls", N_LS, 0 },         { "-delete", N_DELETE, 0 },
    { "-quit", N_QUIT, 0 },
    { 0, 0, 0 }
  };

  for (int i = 0; table[i].name; i++) {
    if (strcmp(w, table[i].name) != 0)
      continue;
    next_word();
    struct node *n = new_node(table[i].kind);
    if (table[i].takes_value) {
      const char *v = next_word();
      if (v == 0)
        die("missing value for", w);
      n->text = v;
      switch (n->kind) {
      case N_TYPE: n->type = v[0]; break;
      case N_SIZE: {
        int sign;
        long long v2 = signed_number(v, &sign);
        const char *p = v;
        while (*p == '+' || *p == '-' || (*p >= '0' && *p <= '9'))
          p++;
        long long unit = 512; // find counts 512-byte blocks by default
        switch (*p) {
        case 'c': unit = 1; break;
        case 'k': unit = 1024; break;
        case 'M': unit = 1024 * 1024; break;
        case 'G': unit = 1024LL * 1024 * 1024; break;
        case 'b': case 0: unit = 512; break;
        }
        n->num = v2;
        n->sign = sign;
        n->type = (int)unit;
        break;
      }
      case N_MTIME:
      case N_MMIN:
      case N_LINKS:
      case N_INUM:
        n->num = signed_number(v, &n->sign);
        break;
      case N_PERM:
        if (v[0] == '-' || v[0] == '/') {
          n->sign = -1;
          v++;
        }
        n->num = strtol(v, 0, 8);
        break;
      }
    }
    if (n->kind >= N_PRINT)
      has_action = 1;
    if (n->kind == N_DELETE)
      has_action = 1;
    return n;
  }

  if (strcmp(w, "-maxdepth") == 0 || strcmp(w, "-mindepth") == 0) {
    next_word();
    const char *v = next_word();
    if (v == 0)
      die("missing value for", w);
    if (strcmp(w, "-maxdepth") == 0)
      maxdepth = atoi(v);
    else
      mindepth = atoi(v);
    return new_node(N_TRUE);
  }

  if (strcmp(w, "-exec") == 0 || strcmp(w, "-ok") == 0) {
    int is_ok = strcmp(w, "-ok") == 0;
    next_word();
    struct node *n = new_node(is_ok ? N_OK : N_EXEC);
    n->argv = malloc(MAXARGS * sizeof(char *));
    if (n->argv == 0)
      die("out of memory", 0);
    n->brace = -1;
    for (;;) {
      const char *a = next_word();
      if (a == 0)
        die("-exec wants a ; or a + at the end", 0);
      if (strcmp(a, ";") == 0 || strcmp(a, "\\;") == 0)
        break;
      if (strcmp(a, "+") == 0) {
        n->plus = 1;
        break;
      }
      if (strcmp(a, "{}") == 0)
        n->brace = n->argc;
      if (n->argc >= MAXARGS - 2)
        die("-exec command is too long", 0);
      n->argv[n->argc++] = (char *)a;
    }
    n->argv[n->argc] = 0;
    if (n->plus && n->brace != n->argc - 1)
      die("-exec ... + wants {} just before the +", 0);
    has_action = 1;
    return n;
  }

  return 0; // not part of the expression
}

static struct node *
parse_and(void)
{
  struct node *left = parse_primary();

  if (left == 0)
    return 0;
  for (;;) {
    const char *w = peek();
    if (w && (strcmp(w, "-a") == 0 || strcmp(w, "-and") == 0))
      next_word();
    else if (w == 0 || strcmp(w, "-o") == 0 || strcmp(w, "-or") == 0 ||
             strcmp(w, ")") == 0 || strcmp(w, "\\)") == 0)
      return left;
    struct node *right = parse_primary();
    if (right == 0)
      return left;
    struct node *n = new_node(N_AND);
    n->left = left;
    n->right = right;
    left = n;
  }
}

static struct node *
parse_or(void)
{
  struct node *left = parse_and();

  if (left == 0)
    return 0;
  while (peek() && (strcmp(peek(), "-o") == 0 || strcmp(peek(), "-or") == 0)) {
    next_word();
    struct node *right = parse_and();
    if (right == 0)
      die("-o wants something after it", 0);
    struct node *n = new_node(N_OR);
    n->left = left;
    n->right = right;
    left = n;
  }
  return left;
}

// ---- running the expression ----

static void
print_ls(const char *path, const struct stat *st)
{
  static const char bits[] = "rwxrwxrwx";
  char perms[12];

  perms[0] = st->type == T_DIR ? 'd'
             : st->type == T_SYMLINK ? 'l'
             : st->type == T_DEVICE
                   ? ((st->rdev >> 8) == STAT_DEV_BLOCK ? 'b' : 'c')
                   : '-';
  for (int i = 0; i < 9; i++)
    perms[1 + i] = (st->mode & (0400 >> i)) ? bits[i] : '-';
  perms[10] = 0;
  printf("%6u %6llu %s %3d %5u %5u %9llu %s\n", st->ino,
         (unsigned long long)(st->blocks / 2), perms, st->nlink, st->uid,
         st->gid, (unsigned long long)st->size, path);
}

static int
run_command(char **argv)
{
  int pid = fork();

  if (pid < 0) {
    fprintf(stderr, "find: fork: %s\n", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    execvp(argv[0], argv);
    fprintf(stderr, "find: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void
flush_batch(struct batch *b)
{
  if (b->nargs == b->n->argc - 1)
    return; // nothing gathered beyond the command itself
  b->args[b->nargs] = 0;
  if (run_command(b->args) != 0)
    status = 1;
  for (int i = b->n->argc - 1; i < b->nargs; i++)
    free(b->args[i]);
  b->nargs = b->n->argc - 1;
}

static struct batch *
batch_for(struct node *n)
{
  for (struct batch *b = batches; b; b = b->next)
    if (b->n == n)
      return b;
  struct batch *b = malloc(sizeof(*b));
  if (b == 0)
    die("out of memory", 0);
  memset(b, 0, sizeof(*b));
  b->n = n;
  // everything before the {} is fixed, and the names go after it
  for (int i = 0; i < n->argc - 1; i++)
    b->args[i] = n->argv[i];
  b->nargs = n->argc - 1;
  b->next = batches;
  batches = b;
  return b;
}

// the result of the expression for one file. `*prune` is set when the
// tree below a directory should not be walked.
static int
eval(struct node *n, const char *path, const struct stat *st, int *prune)
{
  if (n == 0)
    return 1;

  switch (n->kind) {
  case N_AND:
    return eval(n->left, path, st, prune) && eval(n->right, path, st, prune);
  case N_OR:
    return eval(n->left, path, st, prune) || eval(n->right, path, st, prune);
  case N_NOT:
    return !eval(n->left, path, st, prune);
  case N_TRUE:
    return 1;
  case N_FALSE:
    return 0;
  case N_PRUNE:
    if (st->type == T_DIR)
      *prune = 1;
    return 1;

  case N_NAME:
  case N_INAME: {
    const char *base = path;
    for (const char *p = path; *p; p++)
      if (*p == '/')
        base = p + 1;
    return fnmatch(n->text, base, n->kind == N_INAME ? FNM_CASEFOLD : 0) == 0;
  }
  case N_PATH:
    return fnmatch(n->text, path, 0) == 0;
  case N_TYPE:
    switch (n->type) {
    case 'f': return st->type == T_FILE;
    case 'd': return st->type == T_DIR;
    case 'l': return st->type == T_SYMLINK;
    case 'b': return st->type == T_DEVICE && (st->rdev >> 8) == STAT_DEV_BLOCK;
    case 'c': return st->type == T_DEVICE && (st->rdev >> 8) != STAT_DEV_BLOCK;
    }
    return 0;
  case N_SIZE: {
    long long unit = n->type;
    // a size is rounded up to whole units, which is what find counts
    long long have = ((long long)st->size + unit - 1) / unit;
    return compare_signed(have, n->num, n->sign);
  }
  case N_NEWER: {
    struct stat other;
    if (stat(n->text, &other) < 0)
      return 0;
    return st->mtime > other.mtime;
  }
  case N_MTIME:
    return compare_signed((long long)((now - st->mtime) / 86400), n->num, n->sign);
  case N_MMIN:
    return compare_signed((long long)((now - st->mtime) / 60), n->num, n->sign);
  case N_PERM:
    if (n->sign < 0)
      return (st->mode & n->num) == (uint)n->num;
    return (st->mode & 07777) == (uint)n->num;
  case N_USER: {
    struct passwd *pw = getpwnam(n->text);
    return pw ? st->uid == (uint)pw->pw_uid : st->uid == (uint)atoi(n->text);
  }
  case N_GROUP: {
    struct group *gr = getgrnam(n->text);
    return gr ? st->gid == (uint)gr->gr_gid : st->gid == (uint)atoi(n->text);
  }
  case N_LINKS:
    return compare_signed(st->nlink, n->num, n->sign);
  case N_INUM:
    return compare_signed(st->ino, n->num, n->sign);
  case N_EMPTY:
    if (st->type == T_DIR) {
      DIR *d = opendir(path);
      if (d == 0)
        return 0;
      struct dirent *e;
      int empty = 1;
      while ((e = readdir(d)) != 0)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
          empty = 0;
          break;
        }
      closedir(d);
      return empty;
    }
    return st->size == 0;

  case N_PRINT:
    printf("%s\n", path);
    return 1;
  case N_PRINT0:
    printf("%s", path);
    putchar(0);
    return 1;
  case N_LS:
    print_ls(path, st);
    return 1;
  case N_DELETE:
    if (st->type == T_DIR) {
      if (rmdir(path) < 0) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        status = 1;
        return 0;
      }
    } else if (unlink(path) < 0) {
      fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
      status = 1;
      return 0;
    }
    return 1;
  case N_QUIT:
    quit_now = 1;
    return 1;

  case N_OK: {
    fprintf(stderr, "< %s ... %s > ? ", n->argv[0], path);
    int c = fgetc(stdin), first = c;
    while (c != '\n' && c != EOF)
      c = fgetc(stdin);
    if (first != 'y' && first != 'Y')
      return 0;
  }
  // fall through: -ok runs the same way once it has been agreed to
  case N_EXEC: {
    if (n->plus) {
      struct batch *b = batch_for(n);
      if (b->nargs >= MAXARGS - 2)
        flush_batch(b);
      b->args[b->nargs] = strdup(path);
      if (b->args[b->nargs] == 0)
        die("out of memory", 0);
      b->nargs++;
      return 1;
    }
    char *argv[MAXARGS];
    for (int i = 0; i < n->argc; i++)
      argv[i] = strcmp(n->argv[i], "{}") == 0 ? (char *)path : n->argv[i];
    argv[n->argc] = 0;
    return run_command(argv) == 0;
  }
  }
  return 1;
}

static void
walk(const char *path, struct node *expr, int depth)
{
  struct stat st;

  if (quit_now)
    return;
  if (lstat(path, &st) < 0) {
    fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }

  int prune = 0;
  int deep_enough = depth >= mindepth;
  int matched = 0;
  if (deep_enough)
    matched = eval(expr, path, &st, &prune);
  else
    eval(expr, path, &st, &prune); // -prune still counts above mindepth
  if (deep_enough && matched && !has_action)
    printf("%s\n", path);
  if (quit_now)
    return;

  if (st.type != T_DIR || prune || depth >= maxdepth)
    return;

  DIR *d = opendir(path);
  if (d == 0) {
    fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  // the names are gathered before walking, because a -delete or an
  // -exec below may change the directory under the reader
  static char (*names)[NAME_MAX + 1];
  static int nnames_max;
  char (*mine)[NAME_MAX + 1] = 0;
  int n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      mine = realloc(mine, cap * (NAME_MAX + 1));
      if (mine == 0)
        die("out of memory", 0);
    }
    snprintf(mine[n], NAME_MAX + 1, "%s", e->d_name);
    n++;
  }
  closedir(d);
  (void)names;
  (void)nnames_max;

  for (int i = 0; i < n && !quit_now; i++) {
    char child[PATH_MAX];
    int pn = strlen(path);
    snprintf(child, sizeof(child), "%s%s%s", path,
             pn > 0 && path[pn - 1] == '/' ? "" : "/", mine[i]);
    walk(child, expr, depth + 1);
  }
  free(mine);
}

int
main(int argc, char *argv[])
{
  const char *paths[MAXARGS];
  int npaths = 0, i = 1;

  now = (uint64)time(0);

  // everything up to the first word that looks like an expression is a
  // path to start from
  for (; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1])
      break;
    if (strcmp(a, "(") == 0 || strcmp(a, "\\(") == 0 || strcmp(a, "!") == 0)
      break;
    if (npaths < MAXARGS)
      paths[npaths++] = a;
  }
  if (npaths == 0)
    paths[npaths++] = ".";

  words = argv + i;
  nwords = argc - i;
  at = 0;
  struct node *expr = parse_or();
  if (at < nwords)
    die("cannot make sense of", words[at]);

  for (int k = 0; k < npaths && !quit_now; k++)
    walk(paths[k], expr, 0);

  for (struct batch *b = batches; b; b = b->next)
    flush_batch(b);
  exit(status);
}
