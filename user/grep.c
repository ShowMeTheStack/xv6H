// grep: the lines of a file that match a pattern.
//
//   grep [-EFivnclLqswxohHr] [-m NUM] [-e PATTERN] [-f FILE] PATTERN [FILE...]
//
//   -E  the pattern is an extended regular expression (this is egrep)
//   -F  the patterns are plain strings, not expressions (this is fgrep)
//   -i  ignore the difference between upper and lower case
//   -v  print the lines that do not match
//   -n  put each line's number in front of it
//   -c  print how many lines matched, not the lines
//   -l  print the name of each file that had a match, and no lines
//   -L  print the name of each file that had none
//   -q  print nothing; the exit status is the whole answer
//   -s  say nothing about files that cannot be read
//   -w  the match must be a whole word
//   -x  the match must be the whole line
//   -o  print the part that matched rather than the line
//   -h  never print the file's name; -H always print it
//   -r  walk into directories
//   -m  stop after this many matching lines in each file
//
// the exit status is 0 when something matched, 1 when nothing did, and
// 2 when a file could not be read.
#include "user/user.h"
#include <regex.h>

#define MAXPAT 128

static regex_t re[MAXPAT];
static const char *fixed[MAXPAT];
static int npat;

static int use_ere, use_fixed, fold, invert, numbers, count_only;
static int names_only, names_without, quiet, silent, word, whole_line;
static int only_matching, recursive, show_name = -1;
static long max_count = -1;
static int status = 1;   // 1 until something matches
static int had_error;

static void
add_pattern(const char *pat)
{
  if (npat >= MAXPAT) {
    fprintf(stderr, "grep: too many patterns\n");
    exit(2);
  }
  if (use_fixed) {
    fixed[npat++] = pat;
    return;
  }
  int flags = (use_ere ? REG_EXTENDED : 0) | (fold ? REG_ICASE : 0);
  int r = regcomp(&re[npat], pat, flags);
  if (r != 0) {
    char msg[128];
    regerror(r, &re[npat], msg, sizeof(msg));
    fprintf(stderr, "grep: %s: %s\n", pat, msg);
    exit(2);
  }
  npat++;
}

static void
add_pattern_lines(const char *text)
{
  // -e and -f both take several patterns, one per line
  const char *start = text;

  for (const char *p = text;; p++) {
    if (*p == '\n' || *p == 0) {
      int n = p - start;
      char *copy = malloc(n + 1);
      if (copy == 0) {
        fprintf(stderr, "grep: out of memory\n");
        exit(2);
      }
      memmove(copy, start, n);
      copy[n] = 0;
      add_pattern(copy);
      if (*p == 0)
        return;
      start = p + 1;
    }
  }
}

static int
is_word_char(int c)
{
  return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z');
}

// does `line` hold a match, and if so where? -w and -x narrow what
// counts, so a match that fails them is not the end of the search:
// the rest of the line is tried too.
static int
find_match(const char *line, int *so, int *eo)
{
  int len = strlen(line);

  for (int k = 0; k < npat; k++) {
    int from = 0;
    for (;;) {
      int ms, me;
      if (use_fixed) {
        const char *hit = 0;
        if (fold) {
          int plen = strlen(fixed[k]);
          for (const char *p = line + from; *p; p++)
            if (strncasecmp(p, fixed[k], plen) == 0) {
              hit = p;
              break;
            }
        } else {
          hit = strstr(line + from, fixed[k]);
        }
        if (hit == 0)
          break;
        ms = hit - line;
        me = ms + strlen(fixed[k]);
      } else {
        regmatch_t m;
        int eflags = from > 0 ? REG_NOTBOL : 0;
        if (regexec(&re[k], line + from, 1, &m, eflags) != 0)
          break;
        ms = from + (int)m.rm_so;
        me = from + (int)m.rm_eo;
      }
      int ok = 1;
      if (whole_line && (ms != 0 || me != len))
        ok = 0;
      if (word) {
        if (ms > 0 && is_word_char((unsigned char)line[ms - 1]))
          ok = 0;
        if (me < len && is_word_char((unsigned char)line[me]))
          ok = 0;
      }
      if (ok) {
        *so = ms;
        *eo = me;
        return 1;
      }
      // try again past this one, and past an empty match too
      from = me > ms ? me : ms + 1;
      if (from > len)
        break;
    }
  }
  return 0;
}

static void
print_prefix(const char *name, long lineno)
{
  if (show_name == 1 && name)
    printf("%s:", name);
  if (numbers)
    printf("%ld:", lineno);
}

static void
search(FILE *f, const char *name)
{
  static char line[1 << 16];
  long lineno = 0, matched = 0;

  while (fgets(line, sizeof(line), f)) {
    lineno++;
    int n = strlen(line);
    int had_newline = n > 0 && line[n - 1] == '\n';
    if (had_newline)
      line[--n] = 0;

    int so = 0, eo = 0;
    int hit = find_match(line, &so, &eo);
    if (invert)
      hit = !hit;
    if (!hit)
      continue;

    matched++;
    status = 0;
    if (quiet)
      return;
    if (names_only || names_without)
      break; // one match settles it, and the name is printed below
    if (count_only) {
      // nothing is printed per line, but -m still says when to stop
      if (max_count > 0 && matched >= max_count)
        break;
      continue;
    }
    if (only_matching && !invert) {
      // every match on the line, one per line of output
      int from = 0;
      for (;;) {
        int s2, e2;
        if (!find_match(line + from, &s2, &e2))
          break;
        print_prefix(name, lineno);
        printf("%.*s\n", e2 - s2, line + from + s2);
        from += e2 > s2 ? e2 : s2 + 1;
        if (from > n)
          break;
      }
    } else {
      print_prefix(name, lineno);
      printf("%s\n", line);
    }
    if (max_count > 0 && matched >= max_count)
      break;
  }
  if (count_only && !quiet) {
    if (show_name == 1 && name)
      printf("%s:", name);
    printf("%ld\n", matched);
  }
  if (names_only && matched > 0 && !quiet)
    printf("%s\n", name ? name : "(standard input)");
  if (names_without && matched == 0 && !quiet)
    printf("%s\n", name ? name : "(standard input)");
}

static void search_path(const char *path);

static void
search_dir(const char *path)
{
  DIR *d = opendir(path);
  struct dirent *e;

  if (d == 0) {
    if (!silent) {
      fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
      had_error = 1;
    }
    return;
  }
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    char child[PATH_MAX];
    int n = strlen(path);
    snprintf(child, sizeof(child), "%s%s%s", path,
             n > 0 && path[n - 1] == '/' ? "" : "/", e->d_name);
    search_path(child);
  }
  closedir(d);
}

static void
search_path(const char *path)
{
  struct stat st;

  if (lstat(path, &st) < 0) {
    if (!silent) {
      fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
      had_error = 1;
    }
    return;
  }
  if (st.type == T_DIR) {
    if (recursive)
      search_dir(path);
    else if (!silent)
      fprintf(stderr, "grep: %s: Is a directory\n", path);
    return;
  }
  if (st.type == T_SYMLINK)
    return; // a link found by walking is not followed, as grep -r does
  FILE *f = fopen(path, "r");
  if (f == 0) {
    if (!silent) {
      fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
      had_error = 1;
    }
    return;
  }
  search(f, path);
  fclose(f);
}

static void
usage(void)
{
  fprintf(stderr, "usage: grep [-EFivnclLqswxohHr] [-m NUM] [-e PATTERN]"
                  " [-f FILE] PATTERN [FILE...]\n");
  exit(2);
}

// a whole file, as one string
static char *
slurp(const char *path)
{
  FILE *f = fopen(path, "r");
  if (f == 0) {
    fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
    exit(2);
  }
  int cap = 4096, n = 0;
  char *buf = malloc(cap);
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (n + 1 >= cap) {
      cap *= 2;
      buf = realloc(buf, cap);
      if (buf == 0) {
        fprintf(stderr, "grep: out of memory\n");
        exit(2);
      }
    }
    buf[n++] = (char)c;
  }
  fclose(f);
  // a trailing newline would add an empty pattern, which matches all
  while (n > 0 && buf[n - 1] == '\n')
    n--;
  buf[n] = 0;
  return buf;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  const char *epat[MAXPAT], *fpat[MAXPAT];
  int nepat = 0, nfpat = 0, i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  if (strcmp(me, "egrep") == 0)
    use_ere = 1;
  else if (strcmp(me, "fgrep") == 0)
    use_fixed = 1;
  else if (strcmp(me, "rgrep") == 0)
    recursive = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'E': use_ere = 1; break;
      case 'F': use_fixed = 1; break;
      case 'G': use_ere = 0; break;
      case 'i': case 'y': fold = 1; break;
      case 'v': invert = 1; break;
      case 'n': numbers = 1; break;
      case 'c': count_only = 1; break;
      case 'l': names_only = 1; break;
      case 'L': names_without = 1; break;
      case 'q': quiet = 1; break;
      case 's': silent = 1; break;
      case 'w': word = 1; break;
      case 'x': whole_line = 1; break;
      case 'o': only_matching = 1; break;
      case 'h': show_name = 0; break;
      case 'H': show_name = 1; break;
      case 'r': case 'R': recursive = 1; break;
      case 'a': break; // treat binary as text, which is all this does
      case 'e':
      case 'f':
      case 'm': {
        const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (val == 0)
          usage();
        if (*p == 'e')
          epat[nepat++] = val;
        else if (*p == 'f')
          fpat[nfpat++] = val;
        else
          max_count = atol(val);
        took_value = 1; // the value was the rest of this argument
        break;
      }
      default:
        usage();
      }
    }
  }

  // -F and -i are known before any pattern is compiled, which is why
  // the patterns are gathered first and added here
  for (int k = 0; k < nepat; k++)
    add_pattern_lines(epat[k]);
  for (int k = 0; k < nfpat; k++)
    add_pattern_lines(slurp(fpat[k]));
  if (npat == 0) {
    if (i >= argc)
      usage();
    add_pattern(argv[i++]);
  }

  int nfiles = argc - i;
  if (show_name < 0)
    show_name = (nfiles > 1 || (recursive && nfiles >= 1)) ? 1 : 0;

  if (nfiles == 0) {
    search(stdin, 0);
  } else {
    for (; i < argc; i++) {
      if (strcmp(argv[i], "-") == 0)
        search(stdin, "(standard input)");
      else
        search_path(argv[i]);
      if (quiet && status == 0)
        break;
    }
  }
  exit(had_error ? 2 : status);
}
