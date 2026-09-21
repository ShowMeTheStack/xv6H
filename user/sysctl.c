//
// sysctl: the kernel's settings, by name.
//
//   sysctl NAME               print it
//   sysctl -w NAME=VALUE      set it
//   sysctl NAME=VALUE         the same
//   sysctl -a                 print all of them
//   sysctl -p [FILE]          apply /etc/sysctl.conf
//
// a name is a path under /sys with dots for slashes, which is how
// every unix spells these: kernel.hostname is /sys/kernel/hostname.
//
#include "user/user.h"

#define ROOT "/sys"

static int quiet, status;

static void
name_to_path(const char *name, char *out, int max)
{
  int n = snprintf(out, max, "%s/", ROOT);

  for (const char *p = name; *p && n < max - 1; p++)
    out[n++] = *p == '.' ? '/' : *p;
  out[n] = 0;
}

static void
path_to_name(const char *path, char *out, int max)
{
  const char *p = path + strlen(ROOT) + 1;
  int n = 0;

  for (; *p && n < max - 1; p++)
    out[n++] = *p == '/' ? '.' : *p;
  out[n] = 0;
}

static void
show(const char *path)
{
  char name[256], value[512];
  int fd = open(path, O_RDONLY);

  if (fd < 0) {
    fprintf(stderr, "sysctl: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  int n = read(fd, value, sizeof(value) - 1);
  close(fd);
  if (n < 0)
    n = 0;
  value[n] = 0;
  while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r'))
    value[--n] = 0;
  path_to_name(path, name, sizeof(name));
  if (quiet)
    printf("%s\n", value);
  else
    printf("%s = %s\n", name, value);
}

static void
set(const char *name, const char *value)
{
  char path[512];

  name_to_path(name, path, sizeof(path));
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    fprintf(stderr, "sysctl: %s: %s\n", name, strerror(errno));
    status = 1;
    return;
  }
  if (write(fd, value, strlen(value)) < 0) {
    fprintf(stderr, "sysctl: %s: %s\n", name, strerror(errno));
    status = 1;
  }
  close(fd);
  if (!quiet) {
    name_to_path(name, path, sizeof(path));
    show(path);
  }
}

// every file under /sys, depth first
static void
walk(const char *dir)
{
  DIR *d = opendir(dir);
  struct dirent *e;

  if (d == 0)
    return;
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) < 0)
      continue;
    if (st.type == T_DIR)
      walk(path);
    else
      show(path);
  }
  closedir(d);
}

static void
apply_file(const char *path)
{
  FILE *f = fopen(path, "r");
  char line[512];

  if (f == 0) {
    fprintf(stderr, "sysctl: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == ';' || *p == '\n' || *p == 0)
      continue;
    char *eq = strchr(p, '=');
    if (eq == 0)
      continue;
    *eq = 0;
    char *name = p, *value = eq + 1;
    int n = strlen(name);
    while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t'))
      name[--n] = 0;
    while (*value == ' ' || *value == '\t')
      value++;
    n = strlen(value);
    while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r' || value[n - 1] == ' '))
      value[--n] = 0;
    set(name, value);
  }
  fclose(f);
}

int
main(int argc, char *argv[])
{
  int all = 0, write_mode = 0, from_file = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-A") == 0)
      all = 1;
    else if (strcmp(argv[i], "-w") == 0)
      write_mode = 1;
    else if (strcmp(argv[i], "-n") == 0)
      quiet = 1;
    else if (strcmp(argv[i], "-p") == 0)
      from_file = 1;
    else {
      fprintf(stderr, "usage: sysctl [-a] [-n] [-w] NAME[=VALUE]... | -p [FILE]\n");
      exit(1);
    }
  }
  (void)write_mode;

  if (all) {
    walk(ROOT);
    exit(status);
  }
  if (from_file) {
    apply_file(i < argc ? argv[i] : "/etc/sysctl.conf");
    exit(status);
  }
  if (i >= argc) {
    fprintf(stderr, "usage: sysctl [-a] [-n] [-w] NAME[=VALUE]... | -p [FILE]\n");
    exit(1);
  }
  for (; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) {
      *eq = 0;
      set(argv[i], eq + 1);
    } else {
      char path[512];
      name_to_path(argv[i], path, sizeof(path));
      struct stat st;
      if (stat(path, &st) == 0 && st.type == T_DIR)
        walk(path);
      else
        show(path);
    }
  }
  exit(status);
}
