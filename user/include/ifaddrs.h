#ifndef _IFADDRS_H
#define _IFADDRS_H
#include <sys/socket.h>

// the addresses this machine has, as a list a program can walk. what
// fills it is /proc/net/dev and the interface's own address, so it
// says the same thing ifconfig does.
struct ifaddrs {
  struct ifaddrs *ifa_next;
  char *ifa_name;
  unsigned int ifa_flags;
  struct sockaddr *ifa_addr;
  struct sockaddr *ifa_netmask;
  struct sockaddr *ifa_broadaddr;
  struct sockaddr *ifa_dstaddr;
  void *ifa_data;
};

int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);
#endif
