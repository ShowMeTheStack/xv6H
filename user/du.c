// du: how much room a tree takes.
//
//   du [-s] [-a] [-h] [PATH...]
#include "user/user.h"

static int summary, all, human;

static void
human_size(uint64 kb, char *out, int max)
{
  static const char unit[] = "KMGT";
  int i = 0;
  double v = (double)kb;

  while (v >= 1024 && unit[i + 1]) {
    v /= 1024;
    i++;
  }
  snprintf(out, max, "%.1f%c", v, unit[i]);
}

static void
report(uint64 blocks, const char *path)
{
  if (human) {
    char h[16];
    human_size(blocks / 2, h, sizeof(h)); // 512-byte blocks to kilobytes
    printf("%-8s %s\n", h, path);
  } else {
    printf("%llu\t%s\n", (unsigned long long)(blocks / 2), path);
  }
}

static uint64
walk(const char *path, int top)
{
  struct stat st;

  if (lstat(path, &st) < 0) {
    fprintf(stderr, "du: %s: %s\n", path, strerror(errno));
    return 0;
  }
  uint64 total = st.blocks;
  if (st.type == T_DIR) {
    DIR *d = opendir(path);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
          continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s%s%s", path,
                 path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
        total += walk(child, 0);
      }
      closedir(d);
    }
    if (!summary || top)
      report(total, path);
  } else if (all) {
    report(total, path);
  }
  if (summary && top && st.type != T_DIR)
    report(total, path);
  return total;
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *p = argv[i] + 1; *p; p++) {
      if (*p == 's')
        summary = 1;
      else if (*p == 'a')
        all = 1;
      else if (*p == 'h')
        human = 1;
      else {
        fprintf(stderr, "usage: du [-s] [-a] [-h] [PATH...]\n");
        exit(1);
      }
    }
  if (i >= argc)
    walk(".", 1);
  else
    for (; i < argc; i++)
      walk(argv[i], 1);
  exit(0);
}
