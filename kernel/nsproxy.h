#ifndef XV6_NSPROXY_H
#define XV6_NSPROXY_H
//
// namespaces: the six views a process has of the machine, held together
// by one object so that a fork copies a pointer and an unshare copies
// only what it is asked to (kernel/nsproxy.c).
//
//   uts    the machine's name
//   ipc    what /dev/shm and the named semaphores are
//   net    the network stack (kernel/net.h)
//   pid    what a process's number is
//   mnt    what is mounted where, and what "/" means
//   user   who a uid is
//
// The root namespaces are the machine's own and are never freed. A
// process with no nsproxy of its own is in all six of them, which is
// how everything that came before this keeps working.
//

#define UTS_LEN 65          // 64 and the terminator, as Linux has it

#define NUTSNS   8          // namespaces of each kind at once
#define NNSPROXY 32         // combinations of them

struct utsns {
  int ref;                  // nsproxies pointing here; 0 is a free slot
  int id;
  char nodename[UTS_LEN];
  char domainname[UTS_LEN];
};

// A process's number is a fact about a namespace, not about the process:
// the same process is 1 inside a container and 4213 outside it. It
// therefore has a number in its own namespace and in every namespace
// above it, and which one it is told depends on who is asking.
#define NPIDNS 8            // pid namespaces at once
#define PIDNS_DEPTH 4       // and how deeply they may nest

struct pidns {
  int ref;                  // processes and nsproxies pointing here
  int id;
  int level;                // 0 is the machine's own
  struct pidns *parent;
  int initpid;              // the root-namespace pid of its init, or 0
  int nprocs;               // processes in it, for the sake of its init
  int next;                 // where to look for a free number
  int dying;                // its init has gone: nothing new is allowed in
};

// one of a process's numbers: which namespace, and what it is called in
// it. pids[0] is always the machine's own namespace, so pids[0].nr is
// what the rest of the kernel has always called p->pid.
struct pidmap {
  struct pidns *ns;
  int nr;
};

#define NMNTNS 8
#define NUSERNS 8

// one range each for users and groups, which is what a container needs:
// "inside, 0..count-1 means host_first..host_first+count-1".
struct userns {
  int ref;
  int id;
  struct userns *parent;
  int level;
  unsigned int uid_inside, uid_host, uid_count;
  unsigned int gid_inside, gid_host, gid_count;
  int uid_set, gid_set;     // written once, as /proc/<pid>/uid_map is
};

struct mntns {
  int ref;
  int id;
  unsigned int visible;     // bit i: the machine's mount i is in this one
  struct super *root;       // what "/" is; 0 means the machine's own root
};

struct nsproxy {
  int ref;                  // processes pointing here
  struct utsns *uts;
  struct netns *net;
  struct pidns *pid;
  struct mntns *mnt;
  struct userns *user;
  // mnt, user and ipc may be null; a null one means the
  // root namespace of that kind, which is what every process had before
  // there was a choice.
};

// the kinds, in the order /proc/<pid>/ns lists them. Only the ones this
// revision has are named; the rest are 0 and do not appear.
#define NSK_UTS  0
#define NSK_IPC  1
#define NSK_NET  2
#define NSK_PID  3
#define NSK_MNT  4
#define NSK_USER 5
#define NS_KINDS 6

const char *ns_kind_name(int kind);
uint64 ns_id_of(int pid, int kind);     // for /proc/<pid>/ns/<kind>
int ns_join(int pid, int kind);         // setns(2)

struct proc;

// the pid namespaces
struct pidns *pidns_root(void);
struct pidns *pidns_current(void);              // the caller's
struct pidns *pidns_of(struct proc *p);
int pidns_alloc_nr(struct pidns *ns);           // a free number in it
void pidns_free_nr(struct pidns *ns, int nr);
// the numbers a new process gets: its namespace's and every one above.
// 0 if the namespace is dying or has no room.
int pidns_enter(struct proc *p, struct pidns *ns, int rootpid);
void pidns_leave(struct proc *p);
int pid_in_ns(struct proc *p, struct pidns *ns);   // 0 if invisible there
int pid_visible(struct proc *p, struct pidns *ns);
struct pidns *pidns_new(struct pidns *parent);
void pidns_init_gone(struct pidns *ns);  // its init has exited: so does it
void pidns_put(struct pidns *ns);

// the mount namespaces
struct mntns *mntns_root(void);
struct mntns *mntns_current(void);
int mntns_sees(struct mntns *ns, int slot);
int mntns_anyone_sees(int slot);
void mntns_add(struct mntns *ns, int slot);       // a mount it has made
void mntns_drop(struct mntns *ns, int slot);      // and one it has undone
void mntns_pivot(struct mntns *ns, int slot);     // everything but this one goes
void mntns_keep(struct mntns *ns, unsigned int visible);  // ...and what is inside it
struct super *mntns_rootsb(struct mntns *ns);
void mntns_set_root(struct mntns *ns, struct super *sb);

// the user namespaces
struct userns *userns_root(void);
struct userns *userns_current(void);
// the number a namespace knows a kernel user by, and the other way. -1
// when there is none: a file owned by somebody the container has no name
// for is owned by nobody in it.
int uid_to_ns(struct userns *ns, unsigned int kuid);
int gid_to_ns(struct userns *ns, unsigned int kgid);
int uid_from_ns(struct userns *ns, unsigned int uid);
int gid_from_ns(struct userns *ns, unsigned int gid);
int userns_set_map(struct userns *ns, int isuid, unsigned int inside,
                   unsigned int host, unsigned int count);
int userns_map_of(struct userns *ns, int isuid, unsigned int *inside,
                  unsigned int *host, unsigned int *count);
int userns_in_root(void);        // is the caller in the machine's own?

void nsinit(void);
struct nsproxy *ns_root(void);
struct nsproxy *ns_current(void);       // the calling process's
struct nsproxy *ns_hold(struct nsproxy *ns);
void ns_put(struct nsproxy *ns);
// a new set for a process that is unsharing, or for a clone: the
// namespaces named in `flags' are copies, the rest are shared. Returns
// 0 and leaves nothing behind on failure.
struct nsproxy *ns_copy(struct nsproxy *old, int flags);
int ns_unshare(int flags);              // for the calling process

// the machine's name, from the caller's namespace
struct utsns *uts_current(void);
int uts_is_root(void);
int uts_sethostname(const char *s, int n);
int uts_setdomainname(const char *s, int n);
const char *uts_hostname(void);         // for printing; not a copy
const char *uts_domainname(void);

#endif
