// mount [-t TYPE] [-o OPTIONS] [-r] SOURCE TARGET, or mount with no
// arguments to list what is mounted.
#include "user/user.h"

// one name out of a comma-separated option list, matched whole so that
// "ro" does not find itself inside "errors=remount-ro"
static int
has_opt(const char *opts, const char *name)
{
  int n = strlen(name);

  for (const char *p = opts; p && *p;) {
    if (strncmp(p, name, n) == 0 && (p[n] == 0 || p[n] == ',' || p[n] == '='))
      return 1;
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *type = "ext4", *opts = "";
  int flags = 0, i = 1;

  if (argc == 1) {
    // what is mounted, read from /proc/mounts when there is one and
    // from the kernel's own list until then
    int fd = open("/proc/mounts", O_RDONLY);
    if (fd >= 0) {
      char buf[512];
      int n;
      while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
      close(fd);
      exit(0);
    }
    fprintf(stderr, "mount: /proc is not mounted, so there is nothing to read\n");
    exit(1);
  }

  for (; i < argc && argv[i][0] == '-'; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
      type = argv[++i];
    else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      opts = argv[++i];
    else if (strcmp(argv[i], "-r") == 0)
      flags |= MS_RDONLY;
    else if (strcmp(argv[i], "-w") == 0)
      flags &= ~MS_RDONLY;
    else if (strcmp(argv[i], "--bind") == 0)
      flags |= MS_BIND;
    else {
      fprintf(stderr, "usage: mount [-t TYPE] [-o OPTIONS] [-r|-w] [--bind] SOURCE TARGET\n");
      exit(1);
    }
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: mount [-t TYPE] [-o OPTIONS] [-r|-w] [--bind] SOURCE TARGET\n");
    exit(1);
  }
  // the kernel reads the option list too, but the flags it can be told
  // directly are told directly, so that a program reading /proc/mounts
  // sees the same answer either way.
  if (has_opt(opts, "ro"))
    flags |= MS_RDONLY;
  if (has_opt(opts, "bind"))
    flags |= MS_BIND;
  if (has_opt(opts, "remount"))
    flags |= MS_REMOUNT;
  if (has_opt(opts, "noexec"))
    flags |= MS_NOEXEC;
  if (has_opt(opts, "nosuid"))
    flags |= MS_NOSUID;
  if (has_opt(opts, "nodev"))
    flags |= MS_NODEV;
  if (mount(argv[i], argv[i + 1], type, flags, opts) < 0) {
    fprintf(stderr, "mount: %s on %s: %s\n", argv[i], argv[i + 1], strerror(errno));
    exit(1);
  }
  exit(0);
}
