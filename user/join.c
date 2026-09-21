// join: two sorted files, matched on a field.
//
//   join [-a N] [-v N] [-e STRING] [-o LIST] [-t CHAR] [-1 N] [-2 N] F1 F2
//
// both files must be sorted on the field being joined, which is field
// one unless -1 or -2 says otherwise. by default only lines that
// match are printed; -a N also prints the lines of file N that did
// not, and -v N prints only those.
#include "user/user.h"

#define MAXF 64
#define LINE 4096

static char sep;           // -t, or 0 for runs of blanks
static int jf1 = 1, jf2 = 1;
static int show_unpaired1, show_unpaired2;
static int only_unpaired1, only_unpaired2;
static const char *empty = "";
static struct { int file, field; } out[MAXF];
static int nout;

// the fields are kept as offsets into `raw` rather than pointers into
// it, because a line is copied whole -- a run of equal keys is held
// aside while the other file is read -- and a pointer would still be
// aimed at the buffer it was parsed in.
struct line {
  char raw[LINE];
  int field[MAXF];
  int nfield;
};

static void
split(struct line *l)
{
  l->nfield = 0;
  char *p = l->raw;
  if (sep) {
    l->field[l->nfield++] = 0;
    for (; *p; p++)
      if (*p == sep && l->nfield < MAXF) {
        *p = 0;
        l->field[l->nfield++] = (p + 1) - l->raw;
      }
  } else {
    while (*p && l->nfield < MAXF) {
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == 0)
        break;
      l->field[l->nfield++] = p - l->raw;
      while (*p && *p != ' ' && *p != '\t')
        p++;
      if (*p)
        *p++ = 0;
    }
  }
}

static int
read_line(FILE *f, struct line *l)
{
  if (fgets(l->raw, sizeof(l->raw), f) == 0)
    return 0;
  int n = strlen(l->raw);
  if (n > 0 && l->raw[n - 1] == '\n')
    l->raw[n - 1] = 0;
  split(l);
  return 1;
}

static const char *
field_of(const struct line *l, int n)
{
  return n >= 1 && n <= l->nfield ? l->raw + l->field[n - 1] : empty;
}

static void
print_joined(const struct line *a, const struct line *b, int which_alone)
{
  char out_sep = sep ? sep : ' ';

  if (nout > 0) {
    for (int i = 0; i < nout; i++) {
      if (i)
        putchar(out_sep);
      const struct line *src = out[i].file == 1 ? a : b;
      if (src == 0)
        printf("%s", empty);
      else if (out[i].field == 0)
        printf("%s", field_of(src, out[i].file == 1 ? jf1 : jf2));
      else
        printf("%s", field_of(src, out[i].field));
    }
    printf("\n");
    return;
  }

  // the default order: the field joined on, then the rest of the
  // first line, then the rest of the second
  if (which_alone == 1) {
    printf("%s", field_of(a, jf1));
    for (int i = 1; i <= a->nfield; i++)
      if (i != jf1)
        printf("%c%s", out_sep, field_of(a, i));
    printf("\n");
    return;
  }
  if (which_alone == 2) {
    printf("%s", field_of(b, jf2));
    for (int i = 1; i <= b->nfield; i++)
      if (i != jf2)
        printf("%c%s", out_sep, field_of(b, i));
    printf("\n");
    return;
  }
  printf("%s", field_of(a, jf1));
  for (int i = 1; i <= a->nfield; i++)
    if (i != jf1)
      printf("%c%s", out_sep, field_of(a, i));
  for (int i = 1; i <= b->nfield; i++)
    if (i != jf2)
      printf("%c%s", out_sep, field_of(b, i));
  printf("\n");
}

static void
parse_out(const char *list)
{
  // -o takes items like "1.2" or "2.1", separated by commas or spaces
  while (*list && nout < MAXF) {
    while (*list == ',' || *list == ' ')
      list++;
    if (*list == 0)
      break;
    if (*list == '0') {
      out[nout].file = 1;
      out[nout].field = 0;
      nout++;
      list++;
      continue;
    }
    int file = *list++ - '0';
    if (*list == '.')
      list++;
    int field = 0;
    while (*list >= '0' && *list <= '9')
      field = field * 10 + (*list++ - '0');
    out[nout].file = file;
    out[nout].field = field;
    nout++;
  }
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
    const char *val = a[2] ? a + 2 : 0;
    switch (a[1]) {
    case 'a':
      if (!val && i + 1 < argc) val = argv[++i];
      if (val && *val == '1') show_unpaired1 = 1; else show_unpaired2 = 1;
      break;
    case 'v':
      if (!val && i + 1 < argc) val = argv[++i];
      if (val && *val == '1') only_unpaired1 = 1; else only_unpaired2 = 1;
      break;
    case 'e':
      if (!val && i + 1 < argc) val = argv[++i];
      empty = val ? val : "";
      break;
    case 't':
      if (!val && i + 1 < argc) val = argv[++i];
      sep = val ? *val : '\t';
      break;
    case 'o':
      if (!val && i + 1 < argc) val = argv[++i];
      if (val) parse_out(val);
      break;
    case '1':
      if (!val && i + 1 < argc) val = argv[++i];
      jf1 = val ? atoi(val) : 1;
      break;
    case '2':
      if (!val && i + 1 < argc) val = argv[++i];
      jf2 = val ? atoi(val) : 1;
      break;
    default:
      fprintf(stderr, "usage: join [-a N] [-v N] [-t C] [-1 N] [-2 N] F1 F2\n");
      exit(1);
    }
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: join [-a N] [-v N] [-t C] [-1 N] [-2 N] F1 F2\n");
    exit(1);
  }
  FILE *fa = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
  FILE *fb = strcmp(argv[i + 1], "-") == 0 ? stdin : fopen(argv[i + 1], "r");
  if (fa == 0 || fb == 0) {
    fprintf(stderr, "join: %s: %s\n", fa == 0 ? argv[i] : argv[i + 1],
            strerror(errno));
    exit(1);
  }
  if (only_unpaired1)
    show_unpaired1 = 1;
  if (only_unpaired2)
    show_unpaired2 = 1;
  int pairs = !(only_unpaired1 || only_unpaired2);

  static struct line a, b;
  int have_a = read_line(fa, &a), have_b = read_line(fb, &b);

  while (have_a && have_b) {
    int c = strcmp(field_of(&a, jf1), field_of(&b, jf2));
    if (c < 0) {
      if (show_unpaired1)
        print_joined(&a, 0, 1);
      have_a = read_line(fa, &a);
    } else if (c > 0) {
      if (show_unpaired2)
        print_joined(0, &b, 2);
      have_b = read_line(fb, &b);
    } else {
      // every line of the run in the first file against every line of
      // the run in the second, which is what a join means
      static struct line runb[512];
      int n = 0;
      while (have_b && strcmp(field_of(&a, jf1), field_of(&b, jf2)) == 0) {
        if (n < 512)
          runb[n++] = b;
        have_b = read_line(fb, &b);
      }
      while (have_a && strcmp(field_of(&a, jf1), field_of(&runb[0], jf2)) == 0) {
        if (pairs)
          for (int k = 0; k < n; k++)
            print_joined(&a, &runb[k], 0);
        have_a = read_line(fa, &a);
      }
    }
  }
  while (have_a) {
    if (show_unpaired1)
      print_joined(&a, 0, 1);
    have_a = read_line(fa, &a);
  }
  while (have_b) {
    if (show_unpaired2)
      print_joined(0, &b, 2);
    have_b = read_line(fb, &b);
  }
  exit(0);
}
