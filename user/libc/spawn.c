//
// <spawn.h>: posix_spawn, which is fork and exec with the work in
// between written down in advance.
//
// the child cannot report a failure by returning, and it must not
// report one by exiting with a status, because a status of 127 is
// something the program itself might have produced. so the parent
// makes a pipe with O_CLOEXEC before forking: if the exec succeeds the
// descriptor closes itself and the parent reads end-of-file, and if
// anything fails the child writes its errno into the pipe and the
// parent hands that exact number back. that is why posix_spawn returns
// an error number instead of setting errno.
//
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>

enum { ACT_OPEN, ACT_CLOSE, ACT_DUP2, ACT_CHDIR };

struct __spawn_action {
  int type;
  int fd, newfd, oflag;
  mode_t mode;
  char *path; // this library's copy: the caller's may be gone by now
};

// ---- the list of file actions ----

int
posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
  fa->actions = 0;
  fa->used = fa->size = 0;
  return 0;
}

int
posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
  for (int i = 0; i < fa->used; i++)
    free(fa->actions[i].path);
  free(fa->actions);
  fa->actions = 0;
  fa->used = fa->size = 0;
  return 0;
}

// make room for one more action and return it, already zeroed.
static struct __spawn_action *
addaction(posix_spawn_file_actions_t *fa)
{
  if (fa->used == fa->size) {
    int size = fa->size ? fa->size * 2 : 8;
    struct __spawn_action *p = realloc(fa->actions, size * sizeof(*p));
    if (p == 0)
      return 0;
    fa->actions = p;
    fa->size = size;
  }
  struct __spawn_action *a = &fa->actions[fa->used++];
  memset(a, 0, sizeof(*a));
  return a;
}

int
posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path,
                                 int oflag, mode_t mode)
{
  if (fd < 0)
    return EBADF;

  struct __spawn_action *a = addaction(fa);
  if (a == 0)
    return ENOMEM;
  a->path = strdup(path);
  if (a->path == 0) {
    fa->used--;
    return ENOMEM;
  }
  a->type = ACT_OPEN;
  a->fd = fd;
  a->oflag = oflag;
  a->mode = mode;
  return 0;
}

int
posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
  if (fd < 0)
    return EBADF;

  struct __spawn_action *a = addaction(fa);
  if (a == 0)
    return ENOMEM;
  a->type = ACT_CLOSE;
  a->fd = fd;
  return 0;
}

int
posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd)
{
  if (fd < 0 || newfd < 0)
    return EBADF;

  struct __spawn_action *a = addaction(fa);
  if (a == 0)
    return ENOMEM;
  a->type = ACT_DUP2;
  a->fd = fd;
  a->newfd = newfd;
  return 0;
}

int
posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa, const char *path)
{
  struct __spawn_action *a = addaction(fa);

  if (a == 0)
    return ENOMEM;
  a->path = strdup(path);
  if (a->path == 0) {
    fa->used--;
    return ENOMEM;
  }
  a->type = ACT_CHDIR;
  return 0;
}

// ---- the attributes ----

int
posix_spawnattr_init(posix_spawnattr_t *at)
{
  memset(at, 0, sizeof(*at));
  sigemptyset(&at->sigdefault);
  sigemptyset(&at->sigmask);
  at->policy = SCHED_OTHER;
  return 0;
}

int
posix_spawnattr_destroy(posix_spawnattr_t *at)
{
  (void)at; // nothing in one of these is allocated
  return 0;
}

int
posix_spawnattr_setflags(posix_spawnattr_t *at, short flags)
{
  at->flags = flags;
  return 0;
}

int
posix_spawnattr_getflags(const posix_spawnattr_t *at, short *flags)
{
  *flags = at->flags;
  return 0;
}

int
posix_spawnattr_setpgroup(posix_spawnattr_t *at, pid_t pgroup)
{
  at->pgroup = pgroup;
  return 0;
}

int
posix_spawnattr_getpgroup(const posix_spawnattr_t *at, pid_t *pgroup)
{
  *pgroup = at->pgroup;
  return 0;
}

int
posix_spawnattr_setsigmask(posix_spawnattr_t *at, const sigset_t *mask)
{
  at->sigmask = *mask;
  return 0;
}

int
posix_spawnattr_getsigmask(const posix_spawnattr_t *at, sigset_t *mask)
{
  *mask = at->sigmask;
  return 0;
}

int
posix_spawnattr_setsigdefault(posix_spawnattr_t *at, const sigset_t *set)
{
  at->sigdefault = *set;
  return 0;
}

int
posix_spawnattr_getsigdefault(const posix_spawnattr_t *at, sigset_t *set)
{
  *set = at->sigdefault;
  return 0;
}

int
posix_spawnattr_setschedparam(posix_spawnattr_t *at, const struct sched_param *p)
{
  at->param = *p;
  return 0;
}

int
posix_spawnattr_getschedparam(const posix_spawnattr_t *at, struct sched_param *p)
{
  *p = at->param;
  return 0;
}

int
posix_spawnattr_setschedpolicy(posix_spawnattr_t *at, int policy)
{
  at->policy = policy;
  return 0;
}

int
posix_spawnattr_getschedpolicy(const posix_spawnattr_t *at, int *policy)
{
  *policy = at->policy;
  return 0;
}

// ---- the child ----

// everything between the fork and the exec, in the order POSIX gives:
// the process attributes first, then the file actions, and the signal
// mask last of all, so that the steps above still run with whatever
// the caller had blocked. this never returns; a failure goes into the
// pipe and the child exits.
static void __noreturn
child(const char *path, int usepath, const posix_spawn_file_actions_t *fa,
      const posix_spawnattr_t *at, char *const argv[], char *const envp[], int errfd)
{
  if (at) {
    short f = at->flags;
    // RESETIDS has nothing to do while every process runs as root.
    if ((f & POSIX_SPAWN_SETSID) && setsid() < 0)
      goto fail;
    if ((f & POSIX_SPAWN_SETPGROUP) && setpgid(0, at->pgroup) < 0)
      goto fail;
    if (f & POSIX_SPAWN_SETSIGDEF) {
      // a signal the parent ignored stays ignored across the exec
      // unless it is named here, which is the whole point of the flag.
      for (int s = 1; s < NSIG; s++)
        if (sigismember(&at->sigdefault, s))
          signal(s, SIG_DFL);
    }
    if ((f & POSIX_SPAWN_SETSCHEDULER) &&
        sched_setscheduler(0, at->policy, &at->param) < 0)
      goto fail;
    if ((f & POSIX_SPAWN_SETSCHEDPARAM) && !(f & POSIX_SPAWN_SETSCHEDULER) &&
        sched_setparam(0, &at->param) < 0)
      goto fail;
  }

  for (int i = 0; fa && i < fa->used; i++) {
    const struct __spawn_action *a = &fa->actions[i];
    switch (a->type) {
    case ACT_OPEN: {
      int fd = open(a->path, a->oflag, a->mode);
      if (fd < 0)
        goto fail;
      if (fd != a->fd) {
        if (dup2(fd, a->fd) < 0) {
          close(fd);
          goto fail;
        }
        close(fd);
      }
      break;
    }
    case ACT_CLOSE:
      if (close(a->fd) < 0)
        goto fail;
      break;
    case ACT_DUP2:
      // dup2 to the same descriptor is a no-op that must still clear
      // the close-on-exec flag, which is what POSIX asks for here.
      if (a->fd == a->newfd) {
        if (fcntl(a->newfd, F_SETFD, 0) < 0)
          goto fail;
      } else if (dup2(a->fd, a->newfd) < 0) {
        goto fail;
      }
      break;
    default: // ACT_CHDIR
      if (chdir(a->path) < 0)
        goto fail;
      break;
    }
  }

  if (at && (at->flags & POSIX_SPAWN_SETSIGMASK))
    sigprocmask(SIG_SETMASK, &at->sigmask, 0);

  if (usepath)
    execvpe(path, argv, envp);
  else
    execve(path, argv, envp);

fail : {
  int e = errno;
  // the parent is waiting on this pipe; a short write leaves it
  // reading end-of-file, which it reads as "the exec worked", so
  // there is nothing better to do about one than exit.
  ssize_t r = write(errfd, &e, sizeof(e));
  (void)r;
  _exit(127);
}
}

static int
spawn(pid_t *pid, const char *path, int usepath, const posix_spawn_file_actions_t *fa,
      const posix_spawnattr_t *at, char *const argv[], char *const envp[])
{
  int fds[2];

  if (pipe2(fds, O_CLOEXEC) < 0)
    return errno;
  // the file actions name descriptors chosen by a caller who knows
  // nothing about this pipe, so move the writing end out of the range
  // a program is likely to ask for.
  int hi = fcntl(fds[1], F_DUPFD_CLOEXEC, 64);
  if (hi >= 0) {
    close(fds[1]);
    fds[1] = hi;
  }

  // vfork is fork on this system, so the flag costs nothing to honour
  // and will mean something the day vfork does.
  pid_t p = (at && (at->flags & POSIX_SPAWN_USEVFORK)) ? vfork() : fork();
  if (p < 0) {
    int e = errno;
    close(fds[0]);
    close(fds[1]);
    return e;
  }
  if (p == 0) {
    close(fds[0]);
    child(path, usepath, fa, at, argv, envp, fds[1]);
  }

  close(fds[1]);
  int err = 0;
  ssize_t r;
  while ((r = read(fds[0], &err, sizeof(err))) < 0 && errno == EINTR)
    ;
  close(fds[0]);
  if (r == (ssize_t)sizeof(err) && err != 0) {
    // the child never ran the program, so it is this library's job to
    // reap it: the caller was never told a process existed.
    int st;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR)
      ;
    return err;
  }
  if (pid)
    *pid = p;
  return 0;
}

int
posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
            const posix_spawnattr_t *at, char *const argv[], char *const envp[])
{
  return spawn(pid, path, 0, fa, at, argv, envp);
}

int
posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
             const posix_spawnattr_t *at, char *const argv[], char *const envp[])
{
  return spawn(pid, file, 1, fa, at, argv, envp);
}
