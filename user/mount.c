// mount [-t TYPE] [-o OPTIONS] [-r] SOURCE TARGET, or mount with no
// arguments to list what is mounted.
#include "user/user.h"

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
    else {
      fprintf(stderr, "usage: mount [-t TYPE] [-o OPTIONS] [-r] SOURCE TARGET\n");
      exit(1);
    }
  }
  if (argc - i != 2) {
    fprintf(stderr, "usage: mount [-t TYPE] [-o OPTIONS] [-r] SOURCE TARGET\n");
    exit(1);
  }
  if (strstr(opts, "ro"))
    flags |= MS_RDONLY;
  if (mount(argv[i], argv[i + 1], type, flags, opts) < 0) {
    fprintf(stderr, "mount: %s on %s: %s\n", argv[i], argv[i + 1], strerror(errno));
    exit(1);
  }
  exit(0);
}
