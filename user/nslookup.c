// nslookup name: resolve a host name.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(2, "usage: nslookup name\n");
    exit(1);
  }
  struct netinfo ni;
  char s[20];
  if (netinfo(&ni) == 0)
    printf("Server: %s\n", ip2str(ni.dns, s));
  uint32 ip;
  if (dns_resolve(argv[1], &ip) < 0) {
    printf("%s: not found\n", argv[1]);
    exit(1);
  }
  printf("Name: %s\nAddress: %s\n", argv[1], ip2str(ip, s));
  exit(0);
}
