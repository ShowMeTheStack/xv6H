// AF_UNIX sockets (unixsock.c)
#ifndef XV6_UNIXSOCK_H
#define XV6_UNIXSOCK_H
#include "types.h"

#define UN_PATHMAX 108   // what sockaddr_un holds
#define UN_MAXFDS  8     // descriptors one message may carry

struct unsock;
struct file;

// who sent a message, filled in by the kernel and not by the sender
struct ucred {
  int pid;
  uint uid;
  uint gid;
};

// one socket as /proc/net/unix sees it, copied out under its lock
struct unsockinfo {
  int index;
  int refs;
  int type;
  int listening;
  int connected;
  int abstract;
  char path[UN_PATHMAX];
  int pathlen;
};

void unixsock_init(void);
void unixsock_walk(void (*fn)(const struct unsockinfo *si, void *arg),
                   void *arg);
struct unsock *unixsock_alloc(int type);
struct unsock *unixsock_dup(struct unsock *s);
void unixsock_close(struct unsock *s);
int unixsock_bind(struct unsock *s, const char *path, int len);
int unixsock_listen(struct unsock *s, int backlog);
int unixsock_connect(struct unsock *s, const char *path, int len);
struct unsock *unixsock_accept(struct unsock *s, int *err);
int unixsock_pair(int type, struct unsock **a, struct unsock **b);
int unixsock_send(struct unsock *s, int user, uint64 addr, int n,
                  const char *dest, int destlen, struct file **fds, int nfds);
int unixsock_recv(struct unsock *s, int user, uint64 addr, int n, char *from,
                  int *fromlen, struct file **fds, int *nfds,
                  struct ucred *cred, int flags);
int unixsock_shutdown(struct unsock *s, int how);
int unixsock_ready(struct unsock *s, int write);
int unixsock_name(struct unsock *s, int peer, char *out, int max);
int unixsock_type(struct unsock *s);
int unixsock_getcred(struct unsock *s, struct ucred *out);
void unixsock_passcred(struct unsock *s, int on);
int unixsock_passcred_on(struct unsock *s);
#endif
