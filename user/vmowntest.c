// vmowntest: a machine belongs to somebody, and a descriptor for it is
// a capability.
//
// What this checks: another user cannot reach a machine they did not
// create; a descriptor passed over a unix socket lets whoever receives
// it work that machine anyway; and an unnamed machine is destroyed
// when the last descriptor for it is closed, while a named one is not.
#include "kernel/types.h"
#include "kernel/vmm.h"
#include "kernel/hwinfo.h"
#include "user/user.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      printf("vmowntest: FAILED %s (line %d)\n", #x, __LINE__);     \
      failures++;                                                   \
    }                                                               \
  } while (0)

#define OTHER_UID 1000  // `user' in the image: somebody who created nothing here

static void
config(struct vmconfig *cfg, const char *name)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->nvcpu = 1;
  cfg->mem_mb = 16;
  strcpy(cfg->kernel, "/hello");
  if (name)
    strcpy(cfg->name, (char *)name);
}

static int
state_of(int id)
{
  struct vminfo vi;
  if (vm_info(id, &vi) < 0)
    return -1;
  return vi.state;
}

// an unnamed machine lives as long as the descriptors that name it
static void
test_lifetime(void)
{
  struct vmconfig cfg;
  config(&cfg, 0);
  int id = vm_create(&cfg);
  CHECK(id >= 0);
  if (id < 0)
    return;
  CHECK(state_of(id) == VM_NEW);

  int fd = vm_open(id, 0, 0);
  CHECK(fd >= 0);
  int fd2 = dup(fd);
  CHECK(fd2 >= 0);
  // one of two descriptors going changes nothing
  close(fd2);
  CHECK(state_of(id) == VM_NEW);
  // and the last one takes the machine with it
  close(fd);
  CHECK(state_of(id) == VM_FREE);
  printf("vmowntest: an unnamed machine ends with its last descriptor\n");
}

// a named one does not: the name is how it is found again
static void
test_named(void)
{
  struct vmconfig cfg;
  config(&cfg, "owned");
  int id = vm_create(&cfg);
  CHECK(id >= 0);
  if (id < 0)
    return;

  // two machines of one name, for one user, is refused
  struct vmconfig dup2cfg;
  config(&dup2cfg, "owned");
  CHECK(vm_create(&dup2cfg) < 0);

  int fd = vm_open(id, 0, 0);
  CHECK(fd >= 0);
  close(fd);
  CHECK(state_of(id) == VM_NEW);

  // by name, and it is the same machine
  fd = vm_open(-1, "owned", 0);
  CHECK(fd >= 0);
  struct stat st;
  CHECK(fstat(fd, &st) == 0 && (int)st.ino == id);
  close(fd);

  CHECK(vm_open(-1, "no-such-machine", 0) < 0);
  CHECK(vm_destroy(id) == 0);
  CHECK(state_of(id) == VM_FREE);
  printf("vmowntest: a named machine outlives its descriptors\n");
}

// somebody else's machine is not there for them
static void
test_owner(void)
{
  struct vmconfig cfg;
  config(&cfg, "mine");
  int id = vm_create(&cfg);
  CHECK(id >= 0);
  if (id < 0)
    return;

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int bad = 0;
    if (setuid(OTHER_UID) < 0)
      exit(99);
    if (vm_open(id, 0, 0) >= 0)
      bad |= 1;                    // opened by id
    if (vm_open(-1, "mine", 0) >= 0)
      bad |= 2;                    // opened by name
    if (vm_destroy(id) >= 0)
      bad |= 4;                    // destroyed it
    if (vm_ctl(id, VMCTL_PAUSE, 0) >= 0)
      bad |= 8;                    // paused it
    if (vm_run(id, 0) >= 0)
      bad |= 16;                   // ran it
    struct vminfo vi;
    // it does not even say that the slot is in use
    if (vm_info(id, &vi) == 0 && vi.state != VM_FREE)
      bad |= 32;
    exit(bad);
  }
  int st = 0;
  wait(&st);
  CHECK(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
    printf("vmowntest: another user got in: 0x%x\n", WEXITSTATUS(st));
  CHECK(state_of(id) == VM_NEW);   // and it is still there
  CHECK(vm_destroy(id) == 0);
  printf("vmowntest: another user cannot reach it\n");
}

// but a descriptor handed to them is a key: the machine is theirs to
// work for as long as they hold it, whoever they are
static void
test_passed(void)
{
  struct vmconfig cfg;
  config(&cfg, 0);
  int id = vm_create(&cfg);
  CHECK(id >= 0);
  if (id < 0)
    return;
  int fd = vm_open(id, 0, 0);
  CHECK(fd >= 0);

  // a vcpu, so that the guest runs and says something
  int runner = fork();
  CHECK(runner >= 0);
  if (runner == 0) {
    vm_run(id, 0);
    exit(0);
  }

  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    // the receiver is somebody else entirely
    if (setuid(OTHER_UID) < 0)
      exit(99);
    char c = 0;
    struct iovec iov = {&c, 1};
    char ctl[CMSG_SPACE(sizeof(int)) + 32];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    memset(ctl, 0, sizeof(ctl));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof(ctl);
    if (recvmsg(sv[1], &msg, 0) != 1)
      exit(1);
    int got = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
      if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
        memcpy(&got, CMSG_DATA(cm), sizeof(got));
    if (got < 0)
      exit(2);
    // the guest's console, read through the descriptor
    char buf[256];
    int n = read(got, buf, sizeof(buf) - 1);
    if (n <= 0)
      exit(3);
    buf[n] = 0;
    // and control of the machine it names, which this user does not own
    if (vm_ctl(id, VMCTL_PAUSE, 0) < 0)
      exit(4);
    if (vm_ctl(id, VMCTL_RESUME, 0) < 0)
      exit(5);
    close(got);
    exit(0);
  }
  close(sv[1]);
  char c = 'x';
  struct iovec iov = {&c, 1};
  char ctl[CMSG_SPACE(sizeof(int))];
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  memset(ctl, 0, sizeof(ctl));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctl;
  msg.msg_controllen = sizeof(ctl);
  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
  CHECK(sendmsg(sv[0], &msg, 0) == 1);
  close(sv[0]);

  int st = 0;
  CHECK(wait(&st) >= 0);
  CHECK(WIFEXITED(st));
  if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
    printf("vmowntest: the receiver could not use the descriptor: %d\n",
           WEXITSTATUS(st));
    failures++;
  }
  // the machine is still here: this process holds a descriptor too
  CHECK(state_of(id) != VM_FREE);
  close(fd);
  kill(runner, SIGKILL);
  while (wait(0) >= 0)
    ;
  vm_destroy(id);
  printf("vmowntest: a descriptor passed over a socket carries the machine\n");
}

int
main(void)
{
  struct hwinfo hi;
  if (hwinfo(&hi) < 0 || !hi.has_h) {
    printf("vmowntest: no hypervisor, nothing to own\n");
    exit(0);
  }
  if (geteuid() != 0) {
    printf("vmowntest: must be root (it becomes somebody else on purpose)\n");
    exit(1);
  }
  test_lifetime();
  test_named();
  test_owner();
  test_passed();
  if (failures) {
    printf("vmowntest: %d failure(s)\n", failures);
    exit(1);
  }
  printf("vmowntest: ok\n");
  exit(0);
}
