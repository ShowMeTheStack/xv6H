#ifndef _SPAWN_H
#define _SPAWN_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sched.h>
#include <signal.h>

#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER  0x20
#define POSIX_SPAWN_USEVFORK      0x40
#define POSIX_SPAWN_SETSID        0x80

// one queued file action. the type is opaque: the list is built and
// replayed entirely inside spawn.c.
struct __spawn_action;

typedef struct {
  struct __spawn_action *actions;
  int used, size;
} posix_spawn_file_actions_t;

typedef struct {
  short flags;
  pid_t pgroup;
  sigset_t sigdefault, sigmask;
  int policy;
  struct sched_param param;
} posix_spawnattr_t;

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *at, char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *at, char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path,
                                     int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa, const char *path);

int posix_spawnattr_init(posix_spawnattr_t *at);
int posix_spawnattr_destroy(posix_spawnattr_t *at);
int posix_spawnattr_setflags(posix_spawnattr_t *at, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t *at, short *flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t *at, pid_t pgroup);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *at, pid_t *pgroup);
int posix_spawnattr_setsigmask(posix_spawnattr_t *at, const sigset_t *mask);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *at, sigset_t *mask);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *at, const sigset_t *set);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *at, sigset_t *set);
int posix_spawnattr_setschedparam(posix_spawnattr_t *at, const struct sched_param *p);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *at, struct sched_param *p);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *at, int policy);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *at, int *policy);
#endif
