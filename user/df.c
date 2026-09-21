// df: how much room each mounted file system has.
//
//   df [-h] [-i] [PATH...]
#include "user/user.h"
#include <sys/statfs.h>

static int human, inodes;

static void
human_size(uint64 bytes, char *out, int max)
{
  static const char unit[] = "BKMGT";
  int i = 0;
  double v = (double)bytes;

  while (v >= 1024 && unit[i + 1]) {
    v /= 1024;
    i++;
  }
  if (i == 0)
    snprintf(out, max, "%.0f", v);
  else
    snprintf(out, max, "%.1f%c", v, unit[i]);
}

static void
show(const char *source, const char *path)
{
  struct statfs st;

  if (statfs(path, &st) < 0) {
    fprintf(stderr, "df: %s: %s\n", path, strerror(errno));
    return;
  }
  if (inodes) {
    printf("%-16s %9llu %9llu %9llu %4llu%% %s\n", source, (unsigned long long)st.f_files,
           (unsigned long long)(st.f_files - st.f_ffree), (unsigned long long)st.f_ffree,
           st.f_files ? (unsigned long long)((st.f_files - st.f_ffree) * 100 / st.f_files) : 0ULL,
           path);
    return;
  }
  uint64 total = st.f_blocks * st.f_bsize;
  uint64 avail = st.f_bavail * st.f_bsize;
  uint64 used = total - st.f_bfree * st.f_bsize;
  if (human) {
    char t[16], u[16], a[16];
    human_size(total, t, sizeof(t));
    human_size(used, u, sizeof(u));
    human_size(avail, a, sizeof(a));
    printf("%-16s %6s %6s %6s %4llu%% %s\n", source, t, u, a,
           total ? (unsigned long long)(used * 100 / total) : 0ULL, path);
  } else {
    printf("%-16s %9llu %9llu %9llu %4llu%% %s\n", source, (unsigned long long)(total / 1024),
           (unsigned long long)(used / 1024), (unsigned long long)(avail / 1024),
           total ? (unsigned long long)(used * 100 / total) : 0ULL, path);
  }
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *p = argv[i] + 1; *p; p++) {
      if (*p == 'h')
        human = 1;
      else if (*p == 'i')
        inodes = 1;
      else {
        fprintf(stderr, "usage: df [-h] [-i] [PATH...]\n");
        exit(1);
      }
    }

  if (inodes)
    printf("%-16s %9s %9s %9s %5s %s\n", "Filesystem", "Inodes", "IUsed", "IFree", "IUse%",
           "Mounted on");
  else if (human)
    printf("%-16s %6s %6s %6s %5s %s\n", "Filesystem", "Size", "Used", "Avail", "Use%",
           "Mounted on");
  else
    printf("%-16s %9s %9s %9s %5s %s\n", "Filesystem", "1K-blocks", "Used", "Available", "Use%",
           "Mounted on");

  if (i < argc) {
    for (; i < argc; i++)
      show("-", argv[i]);
    exit(0);
  }
  // every mount, from /proc/mounts
  FILE *f = fopen("/proc/mounts", "r");
  if (f == 0) {
    show("-", "/");
    exit(0);
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char source[128], path[128], type[32];
    if (sscanf(line, "%127s %127s %31s", source, path, type) != 3)
      continue;
    show(source, path);
  }
  fclose(f);
  exit(0);
}
