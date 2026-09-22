#ifndef _NETDB_H
#define _NETDB_H
#include <sys/socket.h>
#include <netinet/in.h>

// turning a name into an address and back. getaddrinfo is the one a
// program should use: it answers with a list of addresses ready to be
// handed to connect(), and it works for a service name as well as a
// number.
struct addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  socklen_t ai_addrlen;
  struct sockaddr *ai_addr;
  char *ai_canonname;
  struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0400
#define AI_ADDRCONFIG  0x0020

#define NI_NUMERICHOST 0x0001
#define NI_NUMERICSERV 0x0002
#define NI_NAMEREQD    0x0004
#define NI_DGRAM       0x0010
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_SOCKTYPE   -7
#define EAI_SERVICE    -8
#define EAI_MEMORY     -10
#define EAI_SYSTEM     -11
#define EAI_OVERFLOW   -12

struct hostent {
  char *h_name;
  char **h_aliases;
  int h_addrtype;
  int h_length;
  char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
  char *s_name;
  char **s_aliases;
  int s_port;         // network order, as getservbyname has always given it
  char *s_proto;
};

struct protoent {
  char *p_name;
  char **p_aliases;
  int p_proto;
};

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int code);
int getnameinfo(const struct sockaddr *addr, socklen_t alen, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags);

struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
struct protoent *getprotobyname(const char *name);
struct protoent *getprotobynumber(int proto);
extern int h_errno;
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#endif
