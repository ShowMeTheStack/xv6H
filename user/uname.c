// uname: what this system is.
//
//   -s kernel name   -n host name   -r release   -v version
//   -m machine       -o operating system   -a all of them
#include "user/user.h"

int
main(int argc, char *argv[])
{
  struct utsname u;
  int s = 0, n = 0, r = 0, v = 0, m = 0, o = 0, any = 0;

  if (uname(&u) < 0) {
    fprintf(stderr, "uname: %s\n", strerror(errno));
    exit(1);
  }
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      fprintf(stderr, "usage: uname [-asnrvmo]\n");
      exit(1);
    }
    for (char *p = argv[i] + 1; *p; p++)
      switch (*p) {
      case 'a': s = n = r = v = m = o = 1; break;
      case 's': s = 1; break;
      case 'n': n = 1; break;
      case 'r': r = 1; break;
      case 'v': v = 1; break;
      case 'm': m = 1; break;
      case 'o': o = 1; break;
      default:
        fprintf(stderr, "usage: uname [-asnrvmo]\n");
        exit(1);
      }
  }
  if (!(s || n || r || v || m || o))
    s = 1;
#define PUT(x) do { if (any) printf(" "); printf("%s", (x)); any = 1; } while (0)
  if (s) PUT(u.sysname);
  if (n) PUT(u.nodename);
  if (r) PUT(u.release);
  if (v) PUT(u.version);
  if (m) PUT(u.machine);
  if (o) PUT("xv6H");
  printf("\n");
  exit(0);
}
