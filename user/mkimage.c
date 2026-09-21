// mkimage: an ext4 image with a directory tree already in it.
//
//   mkimage [-b BLOCKS] [-j BLOCKS] [-L LABEL] [-s MB] IMAGE DIR
//   mkimage [-b BLOCKS] [-m MANIFEST] IMAGE
//
// with a directory it walks it and puts everything under it into the
// image at the same place. with a manifest it reads a line per entry:
//
//   PATH SOURCE          a file, copied from SOURCE
//   PATH dir [MODE]      a directory
//   PATH sym TARGET      a symbolic link
//   PATH mode MODE SRC   a file with that mode
//   # anything           a comment
//
// the image itself is written by mkfs.ext4, which is the same program
// that writes the images this system boots from -- so an image made
// here and one made on the build box are the same shape.
#include "user/user.h"

#define MAXARGS 4096

static char *args[MAXARGS];
static int nargs;
static char *strings;
static int strings_len, strings_max;
static int verbose;

static void
die(const char *what, const char *name)
{
  fprintf(stderr, "mkimage: %s%s%s\n", what, name ? ": " : "",
          name ? name : "");
  exit(1);
}

// the argument list has to outlive the walk, so the strings live in
// one growing buffer and the arguments are fixed up at the end
static int
keep(const char *s)
{
  int n = strlen(s) + 1;

  if (strings_len + n > strings_max) {
    strings_max = strings_max ? strings_max * 2 : 65536;
    while (strings_len + n > strings_max)
      strings_max *= 2;
    strings = realloc(strings, strings_max);
    if (strings == 0)
      die("out of memory", 0);
  }
  int at = strings_len;
  memmove(strings + at, s, n);
  strings_len += n;
  return at;
}

static int offsets[MAXARGS];

static void
push(const char *s)
{
  if (nargs >= MAXARGS - 2)
    die("too many files for one image", 0);
  offsets[nargs] = keep(s);
  nargs++;
}

// mkfs.ext4 takes `path:source` for a file, `path::dir[:mode]` for a
// directory and `path::sym:target` for a link
static void
walk(const char *dir, const char *prefix)
{
  DIR *d = opendir(dir);
  struct dirent *e;

  if (d == 0)
    die("cannot read", dir);
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;

    char source[PATH_MAX], inside[PATH_MAX], spec[PATH_MAX * 2];
    snprintf(source, sizeof(source), "%s/%s", dir, e->d_name);
    snprintf(inside, sizeof(inside), "%s%s%s", prefix, prefix[0] ? "/" : "",
             e->d_name);

    struct stat st;
    if (lstat(source, &st) < 0) {
      fprintf(stderr, "mkimage: %s: %s\n", source, strerror(errno));
      continue;
    }
    if (st.type == T_DIR) {
      snprintf(spec, sizeof(spec), "%s::dir:%o", inside, st.mode & 07777);
      push(spec);
      walk(source, inside);
    } else if (st.type == T_SYMLINK) {
      char target[PATH_MAX];
      int n = readlink(source, target, sizeof(target) - 1);
      if (n < 0) {
        fprintf(stderr, "mkimage: %s: %s\n", source, strerror(errno));
        continue;
      }
      target[n] = 0;
      snprintf(spec, sizeof(spec), "%s::sym:%s", inside, target);
      push(spec);
    } else if (st.type == T_FILE) {
      if ((st.mode & 07777) != 0644)
        snprintf(spec, sizeof(spec), "%s::mode:%o:%s", inside,
                 st.mode & 07777, source);
      else
        snprintf(spec, sizeof(spec), "%s:%s", inside, source);
      push(spec);
    } else {
      fprintf(stderr, "mkimage: %s: not a file, a directory or a link;"
                      " skipped\n", source);
    }
    if (verbose)
      fprintf(stderr, "  %s\n", inside);
  }
  closedir(d);
}

static void
read_manifest(const char *path)
{
  FILE *f = fopen(path, "r");
  char line[PATH_MAX * 2];

  if (f == 0)
    die("cannot read", path);
  while (fgets(line, sizeof(line), f)) {
    int n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == ' '))
      line[--n] = 0;
    if (line[0] == 0 || line[0] == '#')
      continue;

    // PATH WORD [REST]
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    char *name = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    char *what = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    char *rest = p;

    char spec[PATH_MAX * 2];
    if (strcmp(what, "dir") == 0)
      snprintf(spec, sizeof(spec), "%s::dir%s%s", name, rest[0] ? ":" : "",
               rest);
    else if (strcmp(what, "sym") == 0)
      snprintf(spec, sizeof(spec), "%s::sym:%s", name, rest);
    else if (strcmp(what, "mode") == 0)
      snprintf(spec, sizeof(spec), "%s::mode:%s", name, rest);
    else if (what[0] == 0)
      continue;
    else
      snprintf(spec, sizeof(spec), "%s:%s", name, what);
    push(spec);
  }
  fclose(f);
}

int
main(int argc, char *argv[])
{
  const char *blocks = 0, *journal = 0, *label = 0, *manifest = 0;
  long long megabytes = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strcmp(a, "-v") == 0) {
      verbose = 1;
      continue;
    }
    const char *v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
    if (v == 0) {
      fprintf(stderr, "usage: mkimage [-b BLOCKS] [-j BLOCKS] [-L LABEL]"
                      " [-s MB] IMAGE DIR\n");
      exit(1);
    }
    switch (a[1]) {
    case 'b': blocks = v; break;
    case 'j': journal = v; break;
    case 'L': label = v; break;
    case 's': megabytes = atoll(v); break;
    case 'm': manifest = v; break;
    default:
      fprintf(stderr, "usage: mkimage [-b BLOCKS] [-j BLOCKS] [-L LABEL]"
                      " [-s MB] IMAGE DIR\n");
      exit(1);
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: mkimage [-b BLOCKS] [-j BLOCKS] [-L LABEL]"
                    " [-s MB] IMAGE DIR\n");
    exit(1);
  }
  const char *image = argv[i++];
  const char *dir = i < argc ? argv[i++] : 0;

  if (dir == 0 && manifest == 0)
    die("either a directory or a manifest is needed", 0);

  // the size: -s in megabytes, or -b in blocks of four kilobytes
  char blockbuf[32];
  if (megabytes > 0) {
    snprintf(blockbuf, sizeof(blockbuf), "%lld", megabytes * 256);
    blocks = blockbuf;
  }

  push("mkfs.ext4");
  if (blocks) {
    push("-b");
    push(blocks);
  }
  if (journal) {
    push("-j");
    push(journal);
  }
  if (label) {
    push("-L");
    push(label);
  }
  push(image);

  if (manifest)
    read_manifest(manifest);
  if (dir) {
    struct stat st;
    if (stat(dir, &st) < 0 || st.type != T_DIR)
      die("not a directory", dir);
    walk(dir, "");
  }

  // the strings have settled, so the arguments can point into them
  for (int k = 0; k < nargs; k++)
    args[k] = strings + offsets[k];
  args[nargs] = 0;

  if (verbose)
    fprintf(stderr, "mkimage: %d entries into %s\n", nargs, image);

  execvp("mkfs.ext4", args);
  // not on the path: try where it is installed
  args[0] = "/sbin/mkfs.ext4";
  execv("/sbin/mkfs.ext4", args);
  fprintf(stderr, "mkimage: cannot run mkfs.ext4: %s\n", strerror(errno));
  exit(1);
}
