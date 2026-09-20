// ls [-l] [-a] [FILE...]: what is in a directory, or about a file.
#include "user/user.h"

static int longform, showall;

static const char *
type_name(short type)
{
  switch (type) {
  case T_DIR: return "dir";
  case T_FILE: return "file";
  case T_DEVICE: return "dev";
  default: return "?";
  }
}

static void
show(const char *path, const char *name, struct stat *st)
{
  if (longform)
    printf("%-4s %2d %6d %8ld %s\n", type_name(st->type), st->dev, st->ino, (long)st->size, name);
  else
    printf("%s\n", name);
  (void)path;
}

static void
ls(const char *path)
{
  struct stat st;

  if (stat(path, &st) < 0) {
    fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
    return;
  }
  if (st.type != T_DIR) {
    show(path, path, &st);
    return;
  }

  DIR *d = opendir(path);
  if (d == 0) {
    fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (!showall && e->d_name[0] == '.')
      continue;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/",
             e->d_name);
    struct stat es;
    if (stat(full, &es) < 0) {
      memset(&es, 0, sizeof(es));
      es.ino = e->d_ino;
    }
    show(full, e->d_name, &es);
  }
  closedir(d);
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *p = argv[i] + 1; *p; p++) {
      if (*p == 'l')
        longform = 1;
      else if (*p == 'a')
        showall = 1;
      else {
        fprintf(stderr, "usage: ls [-la] [FILE...]\n");
        exit(1);
      }
    }
  if (i >= argc) {
    ls(".");
    exit(0);
  }
  for (; i < argc; i++) {
    if (argc - i > 1)
      printf("%s:\n", argv[i]);
    ls(argv[i]);
  }
  exit(0);
}
