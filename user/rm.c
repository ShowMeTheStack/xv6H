// rm [-f] [-r] FILE...: remove files, and directories with -r.
#include "user/user.h"

static int force, recursive, status;

// -f says to ignore an operand that is not there, and nothing else:
// a file that exists but cannot be removed is still an error, which
// is what makes `rm -f` on an immutable file report rather than go
// quietly.
static void
complain(const char *path)
{
  if (force && errno == ENOENT)
    return;
  fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
  status = 1;
}

// remove a directory's contents, then the directory itself
static int
remove_tree(const char *path)
{
  struct stat st;

  // the link itself, not what it points at: a symbolic link whose
  // target is gone is still a name to remove
  if (lstat(path, &st) < 0) {
    complain(path);
    return -1;
  }
  if (st.type == T_DIR) {
    if (!recursive) {
      fprintf(stderr, "rm: %s: is a directory\n", path);
      status = 1;
      return -1;
    }
    DIR *d = opendir(path);
    if (d == 0) {
      complain(path);
      return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/",
               e->d_name);
      remove_tree(child);
    }
    closedir(d);
  }
  if (unlink(path) < 0) {
    complain(path);
    return -1;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *p = argv[i] + 1; *p; p++) {
      if (*p == 'f')
        force = 1;
      else if (*p == 'r' || *p == 'R')
        recursive = 1;
      else {
        fprintf(stderr, "usage: rm [-fr] FILE...\n");
        exit(1);
      }
    }
  }
  if (i >= argc) {
    if (force)
      exit(0);
    fprintf(stderr, "usage: rm [-fr] FILE...\n");
    exit(1);
  }
  for (; i < argc; i++)
    remove_tree(argv[i]);
  exit(status);
}
