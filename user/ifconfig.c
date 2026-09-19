// ifconfig: show the network interface.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  struct netinfo ni;
  char a[20], b[20], c[20], d[20];
  if (netinfo(&ni) < 0) {
    printf("no network device\n");
    exit(1);
  }
  char macs[18];
  const char *hex = "0123456789abcdef";
  for (int i = 0; i < 6; i++) {
    macs[3 * i] = hex[ni.mac[i] >> 4];
    macs[3 * i + 1] = hex[ni.mac[i] & 15];
    macs[3 * i + 2] = (i == 5) ? 0 : ':';
  }
  printf("eth0: mac %s %s\n", macs, ni.up ? (ni.dhcp ? "(dhcp)" : "(static)") : "(not configured)");
  if (ni.up)
    printf("      ip %s mask %s gateway %s dns %s\n", ip2str(ni.ip, a), ip2str(ni.mask, b),
           ip2str(ni.gw, c), ip2str(ni.dns, d));
  printf("      rx %ld tx %ld dropped rx %ld tx %ld, %ld blocked by policy\n", ni.rx_packets,
         ni.tx_packets, ni.rx_dropped, ni.tx_dropped, ni.policy_dropped);
  exit(0);
}
