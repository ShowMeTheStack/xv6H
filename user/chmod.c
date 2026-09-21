// chmod: the permission bits of a file.
//
//   chmod 644 FILE...            the bits, in octal
//   chmod u+x,go-w FILE...       who, what, which
//   chmod -R ... DIR             every file under it as well
#include "user/user.h"

static int recursive, status;

// a symbolic mode: [ugoa]*[-+=][rwxXst]*, comma separated, applied to
// what the file already has.
static int
symbolic(const char *spec, mode_t old, int isdir, mode_t *out)
{
  mode_t mode = old;

  for (const char *p = spec; *p;) {
    int who = 0;
    for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
      who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 07 : 07777;
    if (who == 0) {
      // no "who" means all of them, less what the umask takes away,
      // which is what every chmod does with a bare "+x"
      mode_t m = umask(0);
      umask(m);
      who = 07777 & ~m;
    }
    char op = *p;
    if (op != '+' && op != '-' && op != '=')
      return -1;
    p++;
    mode_t bits = 0;
    for (; *p && *p != ','; p++) {
      switch (*p) {
      case 'r': bits |= 0444; break;
      case 'w': bits |= 0222; break;
      case 'x': bits |= 0111; break;
      case 'X': if (isdir || (mode & 0111)) bits |= 0111; break;
      case 's': bits |= 06000; break;
      case 't': bits |= 01000; break;
      default: return -1;
      }
    }
    bits &= who;
    if (op == '+')
      mode |= bits;
    else if (op == '-')
      mode &= ~bits;
    else
      mode = (mode & ~(mode_t)who) | bits;
    if (*p == ',')
      p++;
  }
  *out = mode;
  return 0;
}

static void
apply(const char *spec, const char *path)
{
  struct stat st;

  if (lstat(path, &st) < 0) {
    fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  mode_t mode;
  if (spec[0] >= '0' && spec[0] <= '7')
    mode = (mode_t)strtol(spec, 0, 8);
  else if (symbolic(spec, st.mode, st.type == T_DIR, &mode) < 0) {
    fprintf(stderr, "chmod: %s: bad mode\n", spec);
    exit(1);
  }
  if (st.type != T_SYMLINK && chmod(path, mode) < 0) {
    fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
    status = 1;
  }
  if (recursive && st.type == T_DIR) {
    DIR *d = opendir(path);
    if (d == 0)
      return;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s%s%s", path,
               path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
      apply(spec, child);
    }
    closedir(d);
  }
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && (argv[i][1] == 'R' || argv[i][1] == 'r'); i++)
    recursive = 1;
  if (argc - i < 2) {
    fprintf(stderr, "usage: chmod [-R] MODE FILE...\n");
    exit(1);
  }
  const char *spec = argv[i++];
  for (; i < argc; i++)
    apply(spec, argv[i]);
  exit(status);
}
