// patch: apply what diff wrote.
//
//   patch [-p N] [-R] [-i FILE] [-o FILE] [-b] [-N] [-F N] [--dry-run] [FILE]
//
//   -p N        strip this many leading directories from the names
//   -R          apply it backwards
//   -i FILE     read the patch from here rather than standard input
//   -o FILE     write the result here rather than over the original
//   -b          keep the original as FILE.orig
//   -N          skip a hunk that is already applied instead of failing
//   -F N        look this many thousand lines away for a hunk
//   --dry-run   say what would happen and change nothing
//
// unified diffs are what this reads: that is what diff here writes and
// what anything else is likely to hand it.
//
// a hunk is first tried where its header says it should go. failing
// that it is looked for on either side, which is what lets a patch
// still apply after the file has moved around it; the offset is then
// reported. a hunk that cannot be placed at all goes to FILE.rej and
// the exit status is 1.
#include "user/user.h"

static int strip = -1, reverse, dry_run, make_backup, skip_applied;
static const char *patch_file, *out_file, *target_arg;
static long fuzz_window = 1000;
static int status;

struct hunk {
  long old_start, old_count, new_start, new_count;
  char **line;          // each with its leading ' ', '-' or '+'
  int n, max;
  char header[256];
  int rejected;
};

struct filepatch {
  char from[PATH_MAX], to[PATH_MAX];
  struct hunk *hunk;
  int n, max;
  int trailing_newline;
};

static struct filepatch *files;
static int nfiles, maxfiles;

// the file being patched, a line at a time and without newlines
static char **flines;
static int nflines, maxflines;

static void
die(const char *what, const char *name)
{
  fprintf(stderr, "patch: %s%s%s\n", what, name ? ": " : "", name ? name : "");
  exit(2);
}

static void *
xrealloc(void *p, int n)
{
  void *q = realloc(p, n);
  if (q == 0)
    die("out of memory", 0);
  return q;
}

static char *
xstrdup(const char *s)
{
  char *p = strdup(s);
  if (p == 0)
    die("out of memory", 0);
  return p;
}

static void
push(char ***v, int *n, int *max, char *s)
{
  if (*n == *max) {
    *max = *max ? *max * 2 : 64;
    *v = xrealloc(*v, *max * sizeof(char *));
  }
  (*v)[(*n)++] = s;
}

// ---- reading the patch ----

// the name out of a "--- name<tab>timestamp" line
static void
header_name(const char *line, char *out, int size)
{
  const char *p = line + 4;
  int n = 0;

  while (*p == ' ')
    p++;
  while (*p && *p != '\t' && n < size - 1)
    out[n++] = *p++;
  out[n] = 0;
  // some diffs separate the timestamp with spaces rather than a tab
  char *sp = strchr(out, ' ');
  if (sp && sp[1] >= '0' && sp[1] <= '9')
    *sp = 0;
}

static struct filepatch *
new_file(void)
{
  if (nfiles == maxfiles) {
    maxfiles = maxfiles ? maxfiles * 2 : 8;
    files = xrealloc(files, maxfiles * sizeof(*files));
  }
  struct filepatch *fp = &files[nfiles++];
  memset(fp, 0, sizeof(*fp));
  fp->trailing_newline = 1;
  return fp;
}

static struct hunk *
new_hunk(struct filepatch *fp)
{
  if (fp->n == fp->max) {
    fp->max = fp->max ? fp->max * 2 : 16;
    fp->hunk = xrealloc(fp->hunk, fp->max * sizeof(*fp->hunk));
  }
  struct hunk *h = &fp->hunk[fp->n++];
  memset(h, 0, sizeof(*h));
  return h;
}

// how many lines of each side a hunk has taken so far
static void
hunk_seen(const struct hunk *h, long *old, long *new)
{
  *old = 0;
  *new = 0;
  for (int k = 0; k < h->n; k++) {
    if (h->line[k][0] != '+')
      (*old)++;
    if (h->line[k][0] != '-')
      (*new)++;
  }
}

static int
parse_hunk_header(struct hunk *h, const char *line)
{
  const char *p = line + 2;

  while (*p == ' ')
    p++;
  if (*p != '-')
    return -1;
  p++;
  h->old_start = atol(p);
  while (*p >= '0' && *p <= '9')
    p++;
  h->old_count = *p == ',' ? atol(p + 1) : 1;
  while (*p && *p != '+')
    p++;
  if (*p != '+')
    return -1;
  p++;
  h->new_start = atol(p);
  while (*p >= '0' && *p <= '9')
    p++;
  h->new_count = *p == ',' ? atol(p + 1) : 1;
  return 0;
}

static void
read_patch(FILE *pf)
{
  static char line[1 << 16];
  struct filepatch *fp = 0;
  struct hunk *h = 0;

  while (fgets(line, sizeof(line), pf)) {
    int n = strlen(line);
    if (n > 0 && line[n - 1] == '\n')
      line[--n] = 0;

    if (strncmp(line, "--- ", 4) == 0) {
      fp = new_file();
      h = 0;
      header_name(line, fp->from, sizeof(fp->from));
      continue;
    }
    if (strncmp(line, "+++ ", 4) == 0 && fp) {
      header_name(line, fp->to, sizeof(fp->to));
      continue;
    }
    if (strncmp(line, "@@", 2) == 0 && fp) {
      h = new_hunk(fp);
      snprintf(h->header, sizeof(h->header), "%s", line);
      if (parse_hunk_header(h, line) < 0)
        die("cannot read a hunk header", line);
      continue;
    }
    if (h == 0)
      continue; // anything before the first hunk is commentary

    if (line[0] == '\\') {
      fp->trailing_newline = 0;
      continue;
    }
    long seen_old, seen_new;
    hunk_seen(h, &seen_old, &seen_new);
    if (seen_old >= h->old_count && seen_new >= h->new_count) {
      h = 0;   // this hunk is complete; what follows is not part of it
      continue;
    }
    if (line[0] == ' ' || line[0] == '-' || line[0] == '+') {
      push(&h->line, &h->n, &h->max, xstrdup(line));
    } else if (line[0] == 0) {
      // a context line whose single leading space a mailer ate
      push(&h->line, &h->n, &h->max, xstrdup(" "));
    }
  }
}

// ---- applying it ----

static const char *
strip_name(const char *name, int levels)
{
  const char *p = name;

  if (levels < 0) {
    const char *base = name;
    for (const char *q = name; *q; q++)
      if (*q == '/')
        base = q + 1;
    return base;
  }
  for (int i = 0; i < levels; i++) {
    const char *slash = strchr(p, '/');
    if (slash == 0)
      return p;
    p = slash + 1;
    while (*p == '/')
      p++;
  }
  return p;
}

static int
read_target(const char *path)
{
  static char buf[1 << 16];
  FILE *f = fopen(path, "r");

  for (int i = 0; i < nflines; i++)
    free(flines[i]);
  nflines = 0;
  if (f == 0)
    return -1;
  while (fgets(buf, sizeof(buf), f)) {
    int n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n')
      buf[n - 1] = 0;
    push(&flines, &nflines, &maxflines, xstrdup(buf));
  }
  fclose(f);
  return 0;
}

// what a hunk line counts as, with -R swapping the two sides
static char
kind_of(const struct hunk *h, int k)
{
  char c = h->line[k][0];

  if (!reverse)
    return c;
  return c == '-' ? '+' : c == '+' ? '-' : c;
}

// does the hunk's "before" text sit at `at`?
static int
hunk_fits(const struct hunk *h, long at)
{
  long i = at;

  if (at < 0)
    return 0;
  for (int k = 0; k < h->n; k++) {
    if (kind_of(h, k) == '+')
      continue;
    if (i >= nflines || strcmp(flines[i], h->line[k] + 1) != 0)
      return 0;
    i++;
  }
  return 1;
}

// is the hunk's "after" text there already?
static int
hunk_applied(const struct hunk *h, long at)
{
  long i = at;

  if (at < 0)
    return 0;
  for (int k = 0; k < h->n; k++) {
    if (kind_of(h, k) == '-')
      continue;
    if (i >= nflines || strcmp(flines[i], h->line[k] + 1) != 0)
      return 0;
    i++;
  }
  return 1;
}

static long
place(const struct hunk *h, long want, long *offset)
{
  *offset = 0;
  if (want < 0)
    want = 0;
  if (hunk_fits(h, want))
    return want;
  for (long d = 1; d <= fuzz_window; d++) {
    if (want - d >= 0 && hunk_fits(h, want - d)) {
      *offset = -d;
      return want - d;
    }
    if (want + d <= nflines && hunk_fits(h, want + d)) {
      *offset = d;
      return want + d;
    }
  }
  return -1;
}

static void
apply_hunk(const struct hunk *h, long at)
{
  char **out = 0;
  int nout = 0, maxout = 0;

  for (long i = 0; i < at; i++)
    push(&out, &nout, &maxout, flines[i]);

  long consumed = 0;
  for (int k = 0; k < h->n; k++) {
    char kind = kind_of(h, k);
    if (kind == '-') {
      consumed++;                       // the line goes away
    } else if (kind == '+') {
      push(&out, &nout, &maxout, xstrdup(h->line[k] + 1));
    } else {
      push(&out, &nout, &maxout, flines[at + consumed]);
      consumed++;
    }
  }
  for (long i = at + consumed; i < nflines; i++)
    push(&out, &nout, &maxout, flines[i]);

  // the lines that were taken away are the only ones to give back
  long i = at;
  for (int k = 0; k < h->n; k++) {
    char kind = kind_of(h, k);
    if (kind == '+')
      continue;
    if (kind == '-')
      free(flines[i]);
    i++;
  }
  free(flines);
  flines = out;
  nflines = nout;
  maxflines = maxout;
}

static void
write_out(const char *path, int trailing_newline)
{
  char tmp[PATH_MAX];

  if (dry_run)
    return;
  snprintf(tmp, sizeof(tmp), "%s.patchtmp", path);
  FILE *f = fopen(tmp, "w");
  if (f == 0)
    die("cannot write", tmp);
  for (int i = 0; i < nflines; i++) {
    fputs(flines[i], f);
    if (i + 1 < nflines || trailing_newline)
      fputc('\n', f);
  }
  if (fclose(f) != 0)
    die("cannot write", tmp);

  if (make_backup) {
    char bak[PATH_MAX];
    snprintf(bak, sizeof(bak), "%s.orig", path);
    unlink(bak);
    link(path, bak);
  }
  unlink(path);
  if (rename(tmp, path) < 0) {
    FILE *in = fopen(tmp, "r");
    FILE *out = fopen(path, "w");
    if (in == 0 || out == 0)
      die("cannot write", path);
    int c;
    while ((c = fgetc(in)) != EOF)
      fputc(c, out);
    fclose(in);
    fclose(out);
    unlink(tmp);
  }
}

static void
write_reject(const struct filepatch *fp, const char *path)
{
  char rej[PATH_MAX];
  int any = 0;

  for (int i = 0; i < fp->n; i++)
    if (fp->hunk[i].rejected)
      any = 1;
  if (!any)
    return;
  snprintf(rej, sizeof(rej), "%s.rej", path);
  FILE *f = fopen(rej, "w");
  if (f == 0) {
    fprintf(stderr, "patch: cannot write %s\n", rej);
    return;
  }
  fprintf(f, "--- %s\n+++ %s\n", fp->from, fp->to);
  for (int i = 0; i < fp->n; i++) {
    if (!fp->hunk[i].rejected)
      continue;
    fprintf(f, "%s\n", fp->hunk[i].header);
    for (int k = 0; k < fp->hunk[i].n; k++)
      fprintf(f, "%s\n", fp->hunk[i].line[k]);
  }
  fclose(f);
  fprintf(stderr, "patch: the hunks that did not apply are in %s\n", rej);
}

static void
do_file(struct filepatch *fp)
{
  const char *name = target_arg;

  if (name == 0) {
    const char *a = strip_name(fp->to, strip);
    const char *b = strip_name(fp->from, strip);
    // /dev/null on one side means a file made or removed, and the
    // other side is the one that names it
    name = strcmp(fp->to, "/dev/null") == 0 ? b : a;
  }

  if (read_target(name) < 0) {
    if (strcmp(fp->from, "/dev/null") == 0) {
      nflines = 0; // a file this patch creates
    } else {
      fprintf(stderr, "patch: %s: %s\n", name, strerror(errno));
      status = 2;
      return;
    }
  }
  printf("patching file %s\n", name);

  long drift = 0;
  for (int k = 0; k < fp->n; k++) {
    struct hunk *h = &fp->hunk[k];
    long want = (reverse ? h->new_start : h->old_start) - 1 + drift;
    long offset = 0;
    long at = place(h, want, &offset);

    if (at < 0) {
      if (skip_applied) {
        long dummy;
        // already applied? then there is nothing to do and no failure
        struct hunk applied_probe = *h;
        long probe = place(&applied_probe, want, &dummy);
        (void)probe;
        int done = 0;
        for (long d = 0; d <= fuzz_window && !done; d++) {
          if (hunk_applied(h, want - d) || hunk_applied(h, want + d))
            done = 1;
        }
        if (done) {
          printf("Hunk #%d is already applied; skipping\n", k + 1);
          continue;
        }
      }
      fprintf(stderr, "patch: hunk #%d FAILED\n", k + 1);
      h->rejected = 1;
      if (status == 0)
        status = 1;
      continue;
    }
    if (offset != 0)
      printf("Hunk #%d succeeded at %ld (offset %ld lines)\n", k + 1, at + 1,
             offset);
    drift += at - want;
    long before = nflines;
    apply_hunk(h, at);
    drift += nflines - before;
  }

  if (status != 2)
    write_out(out_file ? out_file : name, fp->trailing_newline);
  write_reject(fp, name);
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strcmp(a, "--dry-run") == 0) {
      dry_run = 1;
      continue;
    }
    if (strcmp(a, "--reverse") == 0) {
      reverse = 1;
      continue;
    }
    if (strcmp(a, "--forward") == 0) {
      skip_applied = 1;
      continue;
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'R': reverse = 1; break;
      case 'b': make_backup = 1; break;
      case 'N': skip_applied = 1; break;
      case 'f': break;  // never ask; this patch never asks anyway
      case 'l': break;  // loose whitespace matching, which is not done here
      case 'p': case 'i': case 'o': case 'F': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          die("missing value for", a);
        if (*p == 'p')
          strip = atoi(v);
        else if (*p == 'i')
          patch_file = v;
        else if (*p == 'o')
          out_file = v;
        else
          fuzz_window = atol(v) > 0 ? atol(v) * 1000 : 1000;
        took_value = 1;
        break;
      }
      default:
        fprintf(stderr, "usage: patch [-p N] [-R] [-i FILE] [-o FILE] [-bN]"
                        " [-F N] [--dry-run] [FILE]\n");
        exit(2);
      }
    }
  }
  if (i < argc)
    target_arg = argv[i];

  FILE *pf = patch_file ? fopen(patch_file, "r") : stdin;
  if (pf == 0)
    die("cannot read", patch_file);
  read_patch(pf);
  if (pf != stdin)
    fclose(pf);

  if (nfiles == 0) {
    fprintf(stderr, "patch: the input is not a unified diff\n");
    exit(2);
  }
  for (int k = 0; k < nfiles; k++)
    do_file(&files[k]);
  exit(status);
}
