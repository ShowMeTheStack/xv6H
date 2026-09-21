// ls: what is in a directory, or about a file.
//
//   ls [-1aAdFhilnRrSt] [FILE...]
//
//   -1  one name per line, which is what this ls does anyway
//   -a  the names beginning with a dot too
//   -A  the same, but without . and ..
//   -d  the directory itself rather than what is in it
//   -F  a mark after the name saying what kind of file it is
//   -h  sizes in K, M and G
//   -i  the inode number in front
//   -l  the long form: mode, links, owner, group, size, time, name
//   -n  the long form with numbers instead of user and group names
//   -R  walk into every directory below
//   -r  the reverse order
//   -S  largest first
//   -t  newest first
//
// names are sorted, because a directory hands them back in whatever
// order it keeps them and that is not an order anyone can use.
// colours are off: this is a list of names, not a decoration.
#include "user/user.h"
#include <pwd.h>
#include <grp.h>

static int longform, showall, almostall, dironly, oneline;
static int classify, human, inodes, numeric, recurse;
static int reverse, by_size, by_time;
static int status;

struct entry {
  char name[NAME_MAX + 1];
  struct stat st;
  int stated;
};

static struct entry *entries;
static int nentries, maxentries;

static void
mode_string(const struct stat *st, char *out)
{
  static const char bits[] = "rwxrwxrwx";
  unsigned m = st->mode;

  out[0] = st->type == T_DIR        ? 'd'
           : st->type == T_SYMLINK  ? 'l'
           : st->type == T_DEVICE   ? ((st->rdev >> 8) == STAT_DEV_BLOCK ? 'b' : 'c')
                                    : '-';
  for (int i = 0; i < 9; i++)
    out[1 + i] = (m & (0400 >> i)) ? bits[i] : '-';
  if (m & S_ISUID)
    out[3] = (m & 0100) ? 's' : 'S';
  if (m & S_ISGID)
    out[6] = (m & 010) ? 's' : 'S';
  if (m & S_ISVTX)
    out[9] = (m & 01) ? 't' : 'T';
  out[10] = 0;
}

// the mark -F puts after a name
static char
type_mark(const struct stat *st)
{
  if (st->type == T_DIR)
    return '/';
  if (st->type == T_SYMLINK)
    return '@';
  if (st->type == T_FILE && (st->mode & 0111))
    return '*';
  return 0;
}

static void
human_size(uint64 n, char *out, int size)
{
  static const char suffix[] = "BKMGTP";
  int i = 0;
  uint64 v = n;

  while (v >= 1024 && i < 5) {
    v /= 1024;
    i++;
  }
  if (i == 0) {
    snprintf(out, size, "%llu", (unsigned long long)n);
    return;
  }
  // one decimal below ten, as every ls -h does
  uint64 scaled = n;
  for (int k = 0; k < i - 1; k++)
    scaled /= 1024;
  unsigned tenths = (unsigned)((scaled % 1024) * 10 / 1024);
  if (v < 10)
    snprintf(out, size, "%llu.%u%c", (unsigned long long)v, tenths, suffix[i]);
  else
    snprintf(out, size, "%llu%c", (unsigned long long)v, suffix[i]);
}

static void
time_string(uint64 t, char *out, int size)
{
  time_t tt = (time_t)t;
  struct tm tm;

  if (t == 0) {
    snprintf(out, size, "%s", "            ");
    return;
  }
  gmtime_r(&tt, &tm);
  strftime(out, size, "%b %e %H:%M", &tm);
}

static void
show(const char *path, const char *name, const struct stat *st)
{
  if (inodes)
    printf("%6u ", st->ino);
  if (longform) {
    char perms[12], sizebuf[32], when[32];
    mode_string(st, perms);
    if (human)
      human_size(st->size, sizebuf, sizeof(sizebuf));
    else
      snprintf(sizebuf, sizeof(sizebuf), "%llu", (unsigned long long)st->size);
    time_string(st->mtime, when, sizeof(when));
    if (numeric) {
      printf("%s %2d %5u %5u %8s %s %s", perms, st->nlink, st->uid, st->gid,
             sizebuf, when, name);
    } else {
      struct passwd *pw = getpwuid(st->uid);
      struct group *gr = getgrgid(st->gid);
      char u[16], g[16];
      if (pw)
        snprintf(u, sizeof(u), "%s", pw->pw_name);
      else
        snprintf(u, sizeof(u), "%u", st->uid);
      if (gr)
        snprintf(g, sizeof(g), "%s", gr->gr_name);
      else
        snprintf(g, sizeof(g), "%u", st->gid);
      printf("%s %2d %-8s %-8s %8s %s %s", perms, st->nlink, u, g, sizebuf, when,
             name);
    }
    if (st->type == T_SYMLINK) {
      char to[PATH_MAX];
      int n = readlink(path, to, sizeof(to) - 1);
      if (n > 0) {
        to[n] = 0;
        printf(" -> %s", to);
      }
    }
  } else {
    printf("%s", name);
  }
  if (classify) {
    char c = type_mark(st);
    if (c)
      putchar(c);
  }
  printf("\n");
}

static int
compare(const void *va, const void *vb)
{
  const struct entry *a = va, *b = vb;
  int r;

  if (by_time) {
    r = a->st.mtime < b->st.mtime ? 1 : a->st.mtime > b->st.mtime ? -1 : 0;
    if (r == 0)
      r = strcmp(a->name, b->name);
  } else if (by_size) {
    r = a->st.size < b->st.size ? 1 : a->st.size > b->st.size ? -1 : 0;
    if (r == 0)
      r = strcmp(a->name, b->name);
  } else {
    r = strcmp(a->name, b->name);
  }
  return reverse ? -r : r;
}

static void
push(const char *name, const struct stat *st, int stated)
{
  if (nentries == maxentries) {
    int want = maxentries ? maxentries * 2 : 64;
    struct entry *bigger = realloc(entries, want * sizeof(*entries));
    if (bigger == 0) {
      fprintf(stderr, "ls: out of memory\n");
      exit(2);
    }
    entries = bigger;
    maxentries = want;
  }
  snprintf(entries[nentries].name, sizeof(entries[nentries].name), "%s", name);
  entries[nentries].st = *st;
  entries[nentries].stated = stated;
  nentries++;
}

static void list_dir(const char *path, int header);

static void
list_dir(const char *path, int header)
{
  DIR *d = opendir(path);
  struct dirent *e;
  int first = nentries;

  if (d == 0) {
    fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
    status = 2;
    return;
  }
  if (header)
    printf("%s:\n", path);

  while ((e = readdir(d)) != 0) {
    if (!showall && !almostall && e->d_name[0] == '.')
      continue;
    if (almostall && (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0))
      continue;
    char full[PATH_MAX];
    int n = strlen(path);
    snprintf(full, sizeof(full), "%s%s%s", path, n > 0 && path[n - 1] == '/' ? "" : "/",
             e->d_name);
    struct stat es;
    int stated = 1;
    if (lstat(full, &es) < 0) {
      memset(&es, 0, sizeof(es));
      es.ino = e->d_ino;
      stated = 0;
    }
    push(e->d_name, &es, stated);
  }
  closedir(d);

  int n = nentries - first;
  qsort(entries + first, n, sizeof(*entries), compare);

  if (longform) {
    uint64 blocks = 0;
    for (int i = first; i < nentries; i++)
      blocks += entries[i].st.blocks;
    printf("total %llu\n", (unsigned long long)(blocks / 2));
  }
  for (int i = first; i < nentries; i++) {
    char full[PATH_MAX];
    int pn = strlen(path);
    snprintf(full, sizeof(full), "%s%s%s", path,
             pn > 0 && path[pn - 1] == '/' ? "" : "/", entries[i].name);
    show(full, entries[i].name, &entries[i].st);
  }

  if (recurse) {
    // the names are taken out of the array before recursing, because
    // the walk below uses the same array
    static char names[4096][NAME_MAX + 1];
    int ndirs = 0;
    for (int i = first; i < nentries && ndirs < 4096; i++)
      if (entries[i].st.type == T_DIR && strcmp(entries[i].name, ".") != 0 &&
          strcmp(entries[i].name, "..") != 0)
        snprintf(names[ndirs++], NAME_MAX + 1, "%s", entries[i].name);
    nentries = first;
    for (int i = 0; i < ndirs; i++) {
      char full[PATH_MAX];
      int pn = strlen(path);
      snprintf(full, sizeof(full), "%s%s%s", path,
               pn > 0 && path[pn - 1] == '/' ? "" : "/", names[i]);
      printf("\n");
      list_dir(full, 1);
    }
  }
  nentries = first;
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case '1': oneline = 1; break;
      case 'a': showall = 1; break;
      case 'A': almostall = 1; break;
      case 'd': dironly = 1; break;
      case 'F': classify = 1; break;
      case 'h': human = 1; break;
      case 'i': inodes = 1; break;
      case 'l': longform = 1; break;
      case 'n': longform = 1; numeric = 1; break;
      case 'R': recurse = 1; break;
      case 'r': reverse = 1; break;
      case 'S': by_size = 1; break;
      case 't': by_time = 1; break;
      case 'C': break; // columns; this ls prints one name per line
      default:
        fprintf(stderr, "usage: ls [-1aAdFhilnRrSt] [FILE...]\n");
        exit(2);
      }
    }
  }
  (void)oneline;

  if (i >= argc) {
    list_dir(".", 0);
    exit(status);
  }

  // the operands that are not directories are listed first, together
  // and sorted, and then each directory in turn -- which is the order
  // every ls has used since the beginning
  int ndirs = 0;
  for (int k = i; k < argc; k++) {
    struct stat st;
    if (lstat(argv[k], &st) < 0) {
      fprintf(stderr, "ls: %s: %s\n", argv[k], strerror(errno));
      status = 2;
      continue;
    }
    if (st.type == T_DIR && !dironly) {
      ndirs++;
      continue;
    }
    push(argv[k], &st, 1);
  }
  qsort(entries, nentries, sizeof(*entries), compare);
  for (int k = 0; k < nentries; k++)
    show(entries[k].name, entries[k].name, &entries[k].st);
  int had_files = nentries;
  nentries = 0;

  int shown = 0;
  for (int k = i; k < argc; k++) {
    struct stat st;
    if (lstat(argv[k], &st) < 0)
      continue;
    if (st.type != T_DIR || dironly)
      continue;
    if (had_files || ndirs > 1 || shown)
      printf(had_files || shown ? "\n%s:\n" : "%s:\n", argv[k]);
    list_dir(argv[k], 0);
    shown = 1;
  }
  exit(status);
}
