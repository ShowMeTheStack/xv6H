#ifndef _SYS_UN_H
#define _SYS_UN_H
#include <sys/socket.h>

// a socket named by a path in the file system. the path is where the
// two ends find each other; nothing goes over a wire, so a connection
// made this way is reachable from this machine and from nowhere else.
//
// a leading zero byte in sun_path means an abstract name -- one that
// lives in a namespace of its own and leaves nothing behind in the
// file system. that is Linux's extension and it is here because a
// program that wants a socket without a file to clean up should not
// have to invent one.
#define UNIX_PATH_MAX 108

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[UNIX_PATH_MAX];
};

#define SUN_LEN(p)                                                             \
  ((socklen_t)(((struct sockaddr_un *)0)->sun_path) + strlen((p)->sun_path))
#endif
