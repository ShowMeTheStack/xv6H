// diff: what changed between two files.
//
//   diff [-u] [-c] [-q] [-s] [-i] [-w] [-b] [-B] [-N] [-r] [-U N] A B
//
//   -u    unified output, the form patch and everyone else reads
//   -c    context output
//   -U N  N lines of context (with -u)
//   -q    say only whether they differ
//   -s    say so when they are the same
//   -i    ignore the difference between upper and lower case
//   -w    ignore all white space
//   -b    ignore how much white space there is
//   -B    ignore lines that are blank
//   -N    a file that is not there counts as an empty one
//   -r    compare directories all the way down
//
// the difference itself is Myers' algorithm: walk diagonals of the
// edit graph outward by edit distance, keeping the furthest point
// reached on each, and stop the moment the far corner is reached. the
// path is recovered by walking the saved rows backwards. that gives
// the shortest edit script, which is the one that reads as a person
// would describe the change.
//
// the exit status is 0 when the files are the same, 1 when they are
// not, and 2 when something went wrong.
#include "user/user.h"

#define MAXD 4000   // as far as the search goes before giving up

static int unified = 1, context_form, brief, report_same;
static int fold_case, ignore_ws, ignore_ws_amount, ignore_blank;
static int absent_is_empty, recursive, context_lines = 3;
static int status;

struct file {
  char *text;
  char **line;     // each line, without its newline
  unsigned *hash;
  int n;
  int no_final_newline;
};

static void
die(const char *what, const char *name)
{
  fprintf(stderr, "diff: %s%s%s\n", what, name ? ": " : "", name ? name : "");
  exit(2);
}

// the hash a line compares by, which is where -i, -w and -b live
static unsigned
line_hash(const char *s)
{
  unsigned h = 2166136261u;
  int last_was_space = 1;

  for (const char *p = s; *p; p++) {
    int c = (unsigned char)*p;
    if (ignore_ws && (c == ' ' || c == '\t'))
      continue;
    if (ignore_ws_amount && (c == ' ' || c == '\t')) {
      if (last_was_space)
        continue;
      c = ' ';
      last_was_space = 1;
    } else {
      last_was_space = 0;
    }
    if (fold_case && c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    h = (h ^ (unsigned)c) * 16777619u;
  }
  return h ? h : 1;
}

static int
same_line(const char *a, const char *b)
{
  if (!fold_case && !ignore_ws && !ignore_ws_amount)
    return strcmp(a, b) == 0;
  const char *p = a, *q = b;
  for (;;) {
    if (ignore_ws) {
      while (*p == ' ' || *p == '\t')
        p++;
      while (*q == ' ' || *q == '\t')
        q++;
    } else if (ignore_ws_amount) {
      if ((*p == ' ' || *p == '\t') && (*q == ' ' || *q == '\t')) {
        while (*p == ' ' || *p == '\t')
          p++;
        while (*q == ' ' || *q == '\t')
          q++;
        continue;
      }
    }
    int cp = (unsigned char)*p, cq = (unsigned char)*q;
    if (fold_case) {
      if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
      if (cq >= 'A' && cq <= 'Z') cq += 'a' - 'A';
    }
    if (cp != cq)
      return 0;
    if (cp == 0)
      return 1;
    p++;
    q++;
  }
}

static int
is_blank(const char *s)
{
  for (const char *p = s; *p; p++)
    if (*p != ' ' && *p != '\t')
      return 0;
  return 1;
}

static int
read_file(const char *path, struct file *f)
{
  memset(f, 0, sizeof(*f));

  int cap = 4096, n = 0;
  char *buf = malloc(cap);
  if (buf == 0)
    die("out of memory", 0);

  // /dev/null is spelt out rather than opened, so that -N can hand it
  // in for a file that is not there on a system where it might not be
  if (strcmp(path, "/dev/null") != 0) {
    FILE *fp = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    if (fp == 0) {
      free(buf);
      return -1;
    }
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, fp)) > 0) {
      n += got;
      if (n + 1 >= cap) {
        cap *= 2;
        buf = realloc(buf, cap);
        if (buf == 0)
          die("out of memory", 0);
      }
    }
    if (fp != stdin)
      fclose(fp);
  }
  buf[n] = 0;
  f->text = buf;

  // count the lines, then point at each
  int lines = 0;
  for (int i = 0; i < n; i++)
    if (buf[i] == '\n')
      lines++;
  if (n > 0 && buf[n - 1] != '\n') {
    lines++;
    f->no_final_newline = 1;
  }
  f->line = malloc((lines + 1) * sizeof(char *));
  f->hash = malloc((lines + 1) * sizeof(unsigned));
  if (f->line == 0 || f->hash == 0)
    die("out of memory", 0);

  char *p = buf;
  for (int i = 0; i < lines; i++) {
    f->line[i] = p;
    char *e = strchr(p, '\n');
    if (e) {
      *e = 0;
      p = e + 1;
    } else {
      p += strlen(p);
    }
  }
  f->n = lines;

  if (ignore_blank) {
    // a blank line is dropped altogether, which is what -B means
    int k = 0;
    for (int i = 0; i < f->n; i++)
      if (!is_blank(f->line[i]))
        f->line[k++] = f->line[i];
    f->n = k;
  }
  for (int i = 0; i < f->n; i++)
    f->hash[i] = line_hash(f->line[i]);
  return 0;
}

// ---- the difference itself ----

// what happened to each line: kept, taken away, or put in
enum { KEEP, DEL, ADD };

static char *ops;     // one per (a line, then b line) in the script
static int nops, maxops;

static void
push_op(int op)
{
  if (nops == maxops) {
    maxops = maxops ? maxops * 2 : 1024;
    ops = realloc(ops, maxops);
    if (ops == 0)
      die("out of memory", 0);
  }
  ops[nops++] = (char)op;
}

static int
lines_equal(const struct file *a, int i, const struct file *b, int j)
{
  return a->hash[i] == b->hash[j] && same_line(a->line[i], b->line[j]);
}

// Myers: the furthest point reached on each diagonal after d edits.
// every row of V is kept so the path can be walked back afterwards.
static int
myers(const struct file *a, int alo, int ahi, const struct file *b, int blo, int bhi)
{
  int n = ahi - alo, m = bhi - blo;
  int max = n + m;

  if (max > MAXD)
    return -1; // too far apart to search; the caller falls back
  int off = max;
  int width = 2 * max + 1;
  int *v = malloc(width * sizeof(int));
  int **trace = malloc((max + 1) * sizeof(int *));
  if (v == 0 || trace == 0)
    die("out of memory", 0);
  for (int i = 0; i < width; i++)
    v[i] = 0;

  int found = -1;
  for (int d = 0; d <= max; d++) {
    trace[d] = malloc(width * sizeof(int));
    if (trace[d] == 0)
      die("out of memory", 0);
    memmove(trace[d], v, width * sizeof(int));
    for (int k = -d; k <= d; k += 2) {
      int x;
      // step down when the diagonal below reached further, which is
      // the choice between putting a line in and taking one away
      if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1]))
        x = v[off + k + 1];
      else
        x = v[off + k - 1] + 1;
      int y = x - k;
      while (x < n && y < m && lines_equal(a, alo + x, b, blo + y)) {
        x++;
        y++;
      }
      v[off + k] = x;
      if (x >= n && y >= m) {
        found = d;
        break;
      }
    }
    if (found >= 0)
      break;
  }
  if (found < 0) {
    free(v);
    for (int d = 0; d <= max; d++)
      free(trace[d]);
    free(trace);
    return -1;
  }

  // walk it back: from the far corner to the near one, writing the
  // script in reverse and turning it round at the end
  static char *rev;
  static int nrev, maxrev;
  nrev = 0;

  int x = n, y = m;
  for (int d = found; d > 0; d--) {
    int *prev = trace[d];
    int k = x - y;
    int prevk;
    if (k == -d || (k != d && prev[off + k - 1] < prev[off + k + 1]))
      prevk = k + 1;
    else
      prevk = k - 1;
    int prevx = prev[off + prevk];
    int prevy = prevx - prevk;
    while (x > prevx && y > prevy) {
      if (nrev == maxrev) {
        maxrev = maxrev ? maxrev * 2 : 1024;
        rev = realloc(rev, maxrev);
        if (rev == 0)
          die("out of memory", 0);
      }
      rev[nrev++] = KEEP;
      x--;
      y--;
    }
    if (nrev == maxrev) {
      maxrev = maxrev ? maxrev * 2 : 1024;
      rev = realloc(rev, maxrev);
      if (rev == 0)
        die("out of memory", 0);
    }
    rev[nrev++] = (x == prevx) ? ADD : DEL;
    x = prevx;
    y = prevy;
  }
  while (x > 0 && y > 0) {
    if (nrev == maxrev) {
      maxrev = maxrev ? maxrev * 2 : 1024;
      rev = realloc(rev, maxrev);
      if (rev == 0)
        die("out of memory", 0);
    }
    rev[nrev++] = KEEP;
    x--;
    y--;
  }

  for (int i = nrev - 1; i >= 0; i--)
    push_op(rev[i]);

  free(v);
  for (int d = 0; d <= found; d++)
    free(trace[d]);
  free(trace);
  return 0;
}

// ---- printing ----

static void
print_normal(const struct file *a, const struct file *b)
{
  int ai = 0, bi = 0, i = 0;

  while (i < nops) {
    if (ops[i] == KEEP) {
      ai++;
      bi++;
      i++;
      continue;
    }
    int dels = 0, adds = 0, j = i;
    while (j < nops && ops[j] == DEL) {
      dels++;
      j++;
    }
    while (j < nops && ops[j] == ADD) {
      adds++;
      j++;
    }
    char kind = dels && adds ? 'c' : dels ? 'd' : 'a';
    // the ranges are printed the way every diff prints them: a `d`
    // names where the lines would have gone in the other file
    if (dels) {
      if (dels == 1)
        printf("%d", ai + 1);
      else
        printf("%d,%d", ai + 1, ai + dels);
    } else {
      printf("%d", ai);
    }
    putchar(kind);
    if (adds) {
      if (adds == 1)
        printf("%d", bi + 1);
      else
        printf("%d,%d", bi + 1, bi + adds);
    } else {
      printf("%d", bi);
    }
    printf("\n");
    for (int k = 0; k < dels; k++)
      printf("< %s\n", a->line[ai + k]);
    if (dels && adds)
      printf("---\n");
    for (int k = 0; k < adds; k++)
      printf("> %s\n", b->line[bi + k]);
    ai += dels;
    bi += adds;
    i = j;
  }
}

static void
print_unified(const struct file *a, const struct file *b, const char *na,
              const char *nb)
{
  printf("--- %s\n", na);
  printf("+++ %s\n", nb);

  int i = 0, ai = 0, bi = 0;
  while (i < nops) {
    if (ops[i] == KEEP) {
      ai++;
      bi++;
      i++;
      continue;
    }
    // a hunk begins `context_lines` before this change and runs until
    // that many unchanged lines have gone by with no further change
    int start = i, sa = ai, sb = bi, back = 0;
    while (start > 0 && ops[start - 1] == KEEP && back < context_lines) {
      start--;
      sa--;
      sb--;
      back++;
    }
    // how far the hunk reaches: a change, then at most twice the
    // context in unchanged lines, then another change -- two changes
    // closer than that belong in one hunk rather than two
    int end = i, gap = 0;
    for (int scan = i; scan < nops; scan++) {
      if (ops[scan] != KEEP) {
        end = scan + 1;
        gap = 0;
      } else if (++gap > 2 * context_lines) {
        break;
      }
    }
    // then the trailing context
    for (int tail = 0; end < nops && ops[end] == KEEP && tail < context_lines;
         tail++)
      end++;

    int ea = ai, eb = bi;
    for (int k = i; k < end; k++) {
      if (ops[k] != ADD)
        ea++;
      if (ops[k] != DEL)
        eb++;
    }

    int alen = ea - sa, blen = eb - sb;
    printf("@@ -%d,%d +%d,%d @@\n", alen ? sa + 1 : sa, alen,
           blen ? sb + 1 : sb, blen);
    int pa = sa, pb = sb;
    for (int k = start; k < end; k++) {
      if (ops[k] == KEEP) {
        printf(" %s\n", a->line[pa]);
        pa++;
        pb++;
      } else if (ops[k] == DEL) {
        printf("-%s\n", a->line[pa]);
        if (pa == a->n - 1 && a->no_final_newline)
          printf("\\ No newline at end of file\n");
        pa++;
      } else {
        printf("+%s\n", b->line[pb]);
        if (pb == b->n - 1 && b->no_final_newline)
          printf("\\ No newline at end of file\n");
        pb++;
      }
    }
    ai = ea;
    bi = eb;
    i = end;
  }
}

static void
print_context(const struct file *a, const struct file *b, const char *na,
              const char *nb)
{
  printf("*** %s\n", na);
  printf("--- %s\n", nb);

  int i = 0, ai = 0, bi = 0;
  while (i < nops) {
    if (ops[i] == KEEP) {
      ai++;
      bi++;
      i++;
      continue;
    }
    int start = i, sa = ai, sb = bi, back = 0;
    while (start > 0 && ops[start - 1] == KEEP && back < context_lines) {
      start--;
      sa--;
      sb--;
      back++;
    }
    int end = i, gap = 0;
    for (int scan = i; scan < nops; scan++) {
      if (ops[scan] != KEEP) {
        end = scan + 1;
        gap = 0;
      } else if (++gap > 2 * context_lines) {
        break;
      }
    }
    for (int tail = 0; end < nops && ops[end] == KEEP && tail < context_lines;
         tail++)
      end++;

    int ea = ai, eb = bi;
    for (int k = i; k < end; k++) {
      if (ops[k] != ADD)
        ea++;
      if (ops[k] != DEL)
        eb++;
    }

    printf("***************\n");
    printf("*** %d,%d ****\n", sa + 1, ea);
    int pa = sa;
    for (int k = start; k < end; k++) {
      if (ops[k] == KEEP) {
        printf("  %s\n", a->line[pa++]);
      } else if (ops[k] == DEL) {
        // a deletion beside an addition is a change, and context diff
        // marks it with an exclamation mark rather than a minus
        int changed = 0;
        for (int j = start; j < end; j++)
          if (ops[j] == ADD)
            changed = 1;
        printf("%s %s\n", changed ? "!" : "-", a->line[pa++]);
      }
    }
    printf("--- %d,%d ----\n", sb + 1, eb);
    int pb = sb;
    for (int k = start; k < end; k++) {
      if (ops[k] == KEEP) {
        printf("  %s\n", b->line[pb++]);
      } else if (ops[k] == ADD) {
        int changed = 0;
        for (int j = start; j < end; j++)
          if (ops[j] == DEL)
            changed = 1;
        printf("%s %s\n", changed ? "!" : "+", b->line[pb++]);
      }
    }
    ai = ea;
    bi = eb;
    i = end;
  }
}

static int
diff_files(const char *pa, const char *pb, const char *label_a,
           const char *label_b)
{
  struct file a, b;

  if (read_file(pa, &a) < 0) {
    if (!absent_is_empty) {
      fprintf(stderr, "diff: %s: %s\n", pa, strerror(errno));
      status = 2;
      return 2;
    }
    memset(&a, 0, sizeof(a));
  }
  if (read_file(pb, &b) < 0) {
    if (!absent_is_empty) {
      fprintf(stderr, "diff: %s: %s\n", pb, strerror(errno));
      status = 2;
      return 2;
    }
    memset(&b, 0, sizeof(b));
  }

  // the common start and end are not part of any change
  int lo = 0;
  while (lo < a.n && lo < b.n && lines_equal(&a, lo, &b, lo))
    lo++;
  int ahi = a.n, bhi = b.n;
  while (ahi > lo && bhi > lo && lines_equal(&a, ahi - 1, &b, bhi - 1)) {
    ahi--;
    bhi--;
  }

  nops = 0;
  for (int i = 0; i < lo; i++)
    push_op(KEEP);
  if (ahi > lo || bhi > lo) {
    if (myers(&a, lo, ahi, &b, lo, bhi) < 0) {
      // too far apart for the search; say the whole middle changed
      for (int i = lo; i < ahi; i++)
        push_op(DEL);
      for (int i = lo; i < bhi; i++)
        push_op(ADD);
    }
  }
  for (int i = ahi; i < a.n; i++)
    push_op(KEEP);

  int differ = 0;
  for (int i = 0; i < nops; i++)
    if (ops[i] != KEEP) {
      differ = 1;
      break;
    }
  if (!differ && a.no_final_newline != b.no_final_newline)
    differ = 1;

  if (!differ) {
    if (report_same)
      printf("Files %s and %s are identical\n", label_a, label_b);
  } else if (brief) {
    printf("Files %s and %s differ\n", label_a, label_b);
  } else if (context_form) {
    print_context(&a, &b, label_a, label_b);
  } else if (unified) {
    print_unified(&a, &b, label_a, label_b);
  } else {
    print_normal(&a, &b);
  }

  free(a.text);
  free(a.line);
  free(a.hash);
  free(b.text);
  free(b.line);
  free(b.hash);
  if (differ && status == 0)
    status = 1;
  return differ;
}

// ---- directories ----

static int
name_in(const char *dir, const char *name, char *out, int size)
{
  int n = strlen(dir);
  return snprintf(out, size, "%s%s%s", dir, n > 0 && dir[n - 1] == '/' ? "" : "/",
                  name);
}

static int
cmp_names(const void *a, const void *b)
{
  return strcmp(*(const char **)a, *(const char **)b);
}

static char **
dir_names(const char *path, int *count)
{
  DIR *d = opendir(path);
  struct dirent *e;
  char **names = 0;
  int n = 0, cap = 0;

  *count = 0;
  if (d == 0)
    return 0;
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      names = realloc(names, cap * sizeof(char *));
      if (names == 0)
        die("out of memory", 0);
    }
    names[n] = strdup(e->d_name);
    if (names[n] == 0)
      die("out of memory", 0);
    n++;
  }
  closedir(d);
  qsort(names, n, sizeof(char *), cmp_names);
  *count = n;
  return names;
}

static void diff_dirs(const char *pa, const char *pb);

static void
diff_entry(const char *pa, const char *pb)
{
  struct stat sa, sb;
  int ha = stat(pa, &sa) == 0, hb = stat(pb, &sb) == 0;

  if (ha && hb && sa.type == T_DIR && sb.type == T_DIR) {
    if (recursive)
      diff_dirs(pa, pb);
    else
      printf("Common subdirectories: %s and %s\n", pa, pb);
    return;
  }
  if (ha && hb && (sa.type == T_DIR) != (sb.type == T_DIR)) {
    printf("File %s is a %s while file %s is a %s\n", pa,
           sa.type == T_DIR ? "directory" : "regular file", pb,
           sb.type == T_DIR ? "directory" : "regular file");
    if (status == 0)
      status = 1;
    return;
  }
  if (!ha || !hb) {
    if (!absent_is_empty) {
      printf("Only in %s: %s\n", ha ? pa : pb, ha ? pa : pb);
      if (status == 0)
        status = 1;
      return;
    }
  }
  diff_files(pa, pb, pa, pb);
}

static void
diff_dirs(const char *pa, const char *pb)
{
  int na, nb;
  char **a = dir_names(pa, &na);
  char **b = dir_names(pb, &nb);

  if (a == 0 || b == 0) {
    fprintf(stderr, "diff: %s: %s\n", a == 0 ? pa : pb, strerror(errno));
    status = 2;
    return;
  }
  int i = 0, j = 0;
  while (i < na || j < nb) {
    int c;
    if (i >= na)
      c = 1;
    else if (j >= nb)
      c = -1;
    else
      c = strcmp(a[i], b[j]);

    char fa[PATH_MAX], fb[PATH_MAX];
    if (c < 0) {
      name_in(pa, a[i], fa, sizeof(fa));
      if (absent_is_empty) {
        name_in(pb, a[i], fb, sizeof(fb));
        diff_files(fa, "/dev/null", fa, fb);
      } else {
        printf("Only in %s: %s\n", pa, a[i]);
        if (status == 0)
          status = 1;
      }
      i++;
    } else if (c > 0) {
      name_in(pb, b[j], fb, sizeof(fb));
      if (absent_is_empty) {
        name_in(pa, b[j], fa, sizeof(fa));
        diff_files("/dev/null", fb, fa, fb);
      } else {
        printf("Only in %s: %s\n", pb, b[j]);
        if (status == 0)
          status = 1;
      }
      j++;
    } else {
      name_in(pa, a[i], fa, sizeof(fa));
      name_in(pb, b[j], fb, sizeof(fb));
      diff_entry(fa, fb);
      i++;
      j++;
    }
  }
  for (int k = 0; k < na; k++)
    free(a[k]);
  for (int k = 0; k < nb; k++)
    free(b[k]);
  free(a);
  free(b);
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
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'u': unified = 1; context_form = 0; break;
      case 'c': context_form = 1; unified = 0; break;
      case 'e': unified = 0; context_form = 0; break; // the normal form
      case 'n': unified = 0; context_form = 0; break;
      case 'q': brief = 1; break;
      case 's': report_same = 1; break;
      case 'i': fold_case = 1; break;
      case 'w': ignore_ws = 1; break;
      case 'b': ignore_ws_amount = 1; break;
      case 'B': ignore_blank = 1; break;
      case 'N': absent_is_empty = 1; break;
      case 'r': recursive = 1; break;
      case 'U': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          die("-U wants a number", 0);
        context_lines = atoi(v);
        unified = 1;
        took_value = 1;
        break;
      }
      default:
        if (*p >= '0' && *p <= '9') {
          context_lines = atoi(p);
          took_value = 1;
          break;
        }
        fprintf(stderr, "usage: diff [-ucqsiwbBNr] [-U N] A B\n");
        exit(2);
      }
    }
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: diff [-ucqsiwbBNr] [-U N] A B\n");
    exit(2);
  }

  struct stat sa, sb;
  const char *pa = argv[i], *pb = argv[i + 1];
  int ha = stat(pa, &sa) == 0, hb = stat(pb, &sb) == 0;

  if (ha && hb && sa.type == T_DIR && sb.type == T_DIR) {
    diff_dirs(pa, pb);
    exit(status);
  }
  // diff A DIR means the file of that name inside the directory
  static char joined[PATH_MAX];
  if (hb && sb.type == T_DIR && strcmp(pa, "-") != 0) {
    const char *base = pa;
    for (const char *p = pa; *p; p++)
      if (*p == '/')
        base = p + 1;
    name_in(pb, base, joined, sizeof(joined));
    pb = joined;
  } else if (ha && sa.type == T_DIR && strcmp(pb, "-") != 0) {
    const char *base = pb;
    for (const char *p = pb; *p; p++)
      if (*p == '/')
        base = p + 1;
    name_in(pa, base, joined, sizeof(joined));
    pa = joined;
  }
  diff_files(pa, pb, pa, pb);
  exit(status);
}
