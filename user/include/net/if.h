#ifndef _NET_IF_H
#define _NET_IF_H
#include <sys/socket.h>

// interfaces by name and by number. there is one interface here and
// the loopback that every machine has, so the numbering is short --
// but a program that walks it does not need to know that.
#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

// the flags an interface has, with Linux's numbers
#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_DEBUG       0x0004
#define IFF_LOOPBACK    0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_NOARP       0x0080
#define IFF_PROMISC     0x0100
#define IFF_MULTICAST   0x1000
#define IFF_RUNNING     0x0040

struct if_nameindex {
  unsigned int if_index;
  char *if_name;
};

unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int index, char *name);
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *p);
#endif
