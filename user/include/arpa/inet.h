#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>

// an address as text and back again. inet_ntop and inet_pton are the
// ones to use: they say which family they are working in and they
// cannot be mistaken for a number.
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_pton(int af, const char *src, void *dst);
char *inet_ntoa(struct in_addr in);
in_addr_t inet_addr(const char *s);
int inet_aton(const char *s, struct in_addr *out);
#endif
