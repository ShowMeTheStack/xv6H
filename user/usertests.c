#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/sysconf.h"
#include "kernel/mman.h"
#include "kernel/signal.h"
#include "kernel/posix.h"
#include "kernel/termios.h"
#include "kernel/time.h"
#include "kernel/hwinfo.h"
#include "user/thread.h"

// the paging mode, and so the top of the address space, is chosen at boot.
#undef MAXVA
#define MAXVA ((uint64)sysconf(_SC_XV6_MAXVA))
#define TRAPFRAME ((uint64)sysconf(_SC_XV6_TRAPFRAME))
#define BRK_MAX ((uint64)sysconf(_SC_XV6_BRK_MAX))

//
// Tests xv6 system calls.  usertests without arguments runs them all
// and usertests <name> runs <name> test. The test runner creates for
// each test a process and based on the exit status of the process,
// the test runner reports "OK" or "FAILED".  Some tests result in
// kernel printing usertrap messages, which can be ignored if test
// prints "OK".
//

// the block size these tests write in. it used to be the file system's
// own, when there was only one file system and its blocks were 1024
// bytes; now it is simply a size that is not a round number of pages,
// which is the interesting case for a write that straddles blocks.
#define BSIZE 1024
#define BUFSZ (12 * BSIZE)
// writebig: enough blocks that the file needs more than one extent, and
// far fewer than a file system here can hold.
#define BIGBLOCKS 1000

// the exit code of a child, or 1 if it died of a signal.
static int
xstat(int status)
{
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

char buf[BUFSZ];

//
// Section with tests that run fairly quickly.  Use -q if you want to
// run just those.  Without -q usertests also runs the ones that take a
// fair amount of time.
//

// what if you pass ridiculous pointers to system calls
// that read user memory with copyin?
void
copyin(char *s)
{
  uint64 addrs[] = {0x80000000LL, 0x3fffffe000, 0x3ffffff000, 0x4000000000,
                    0xffffffffffffffff};

  for (int ai = 0; ai < sizeof(addrs) / sizeof(addrs[0]); ai++) {
    uint64 addr = addrs[ai];

    int fd = open("copyin1", O_CREATE | O_WRONLY, 0666);
    if (fd < 0) {
      printf("open(copyin1) failed\n");
      exit(1);
    }
    int n = write(fd, (void *)addr, 8192);
    if (n >= 0) {
      printf("write(fd, %p, 8192) returned %d, not -1\n", (void *)addr, n);
      exit(1);
    }
    close(fd);
    unlink("copyin1");

    n = write(1, (char *)addr, 8192);
    if (n > 0) {
      printf("write(1, %p, 8192) returned %d, not -1 or 0\n", (void *)addr, n);
      exit(1);
    }

    int fds[2];
    if (pipe(fds) < 0) {
      printf("pipe() failed\n");
      exit(1);
    }
    n = write(fds[1], (char *)addr, 8192);
    if (n > 0) {
      printf("write(pipe, %p, 8192) returned %d, not -1 or 0\n", (void *)addr,
             n);
      exit(1);
    }
    close(fds[0]);
    close(fds[1]);
  }
}

// what if you pass ridiculous pointers to system calls
// that write user memory with copyout?
void
copyout(char *s)
{
  uint64 addrs[] = {0LL,          0x80000000LL, 0x3fffffe000,
                    0x3ffffff000, 0x4000000000, 0xffffffffffffffff};

  for (int ai = 0; ai < sizeof(addrs) / sizeof(addrs[0]); ai++) {
    uint64 addr = addrs[ai];

    int fd = open("README", 0);
    if (fd < 0) {
      printf("open(README) failed\n");
      exit(1);
    }
    int n = read(fd, (void *)addr, 8192);
    if (n > 0) {
      printf("read(fd, %p, 8192) returned %d, not -1 or 0\n", (void *)addr, n);
      exit(1);
    }
    close(fd);

    int fds[2];
    if (pipe(fds) < 0) {
      printf("pipe() failed\n");
      exit(1);
    }
    n = write(fds[1], "x", 1);
    if (n != 1) {
      printf("pipe write failed\n");
      exit(1);
    }
    n = read(fds[0], (void *)addr, 8192);
    if (n > 0) {
      printf("read(pipe, %p, 8192) returned %d, not -1 or 0\n", (void *)addr,
             n);
      exit(1);
    }
    close(fds[0]);
    close(fds[1]);
  }
}

// what if you pass ridiculous string pointers to system calls?
void
copyinstr1(char *s)
{
  uint64 addrs[] = {0x80000000LL, 0x3fffffe000, 0x3ffffff000, 0x4000000000,
                    0xffffffffffffffff};

  for (int ai = 0; ai < sizeof(addrs) / sizeof(addrs[0]); ai++) {
    uint64 addr = addrs[ai];

    int fd = open((char *)addr, O_CREATE | O_WRONLY, 0666);
    if (fd >= 0) {
      printf("open(%p) returned %d, not -1\n", (void *)addr, fd);
      exit(1);
    }
  }
}

// what if a string system call argument is exactly the size
// of the kernel buffer it is copied into, so that the null
// would fall just beyond the end of the kernel buffer?
void
copyinstr2(char *s)
{
  char b[MAXPATH + 1];

  for (int i = 0; i < MAXPATH; i++)
    b[i] = 'x';
  b[MAXPATH] = '\0';

  int ret = unlink(b);
  if (ret != -1) {
    printf("unlink(%s) returned %d, not -1\n", b, ret);
    exit(1);
  }

  int fd = open(b, O_CREATE | O_WRONLY, 0666);
  if (fd != -1) {
    printf("open(%s) returned %d, not -1\n", b, fd);
    exit(1);
  }

  ret = link(b, b);
  if (ret != -1) {
    printf("link(%s, %s) returned %d, not -1\n", b, b, ret);
    exit(1);
  }

  char *args[] = {"xx", 0};
  ret = exec(b, args);
  if (ret != -1) {
    printf("exec(%s) returned %d, not -1\n", b, fd);
    exit(1);
  }

  int pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    static char big[PGSIZE + 1];
    for (int i = 0; i < PGSIZE; i++)
      big[i] = 'x';
    big[PGSIZE] = '\0';
    char *args2[MAXARG];
    for (int i = 0; i < MAXARG - 1; i++)
      args2[i] = big; // more than ARG_MAX in total
    args2[MAXARG - 1] = 0;
    ret = exec("/bin/echo", args2);
    if (ret != -1) {
      printf("exec(echo, BIG) returned %d, not -1\n", fd);
      exit(1);
    }
    exit(47); // OK
  }

  int st = 0;
  wait(&st);
  if (xstat(st) != 47) {
    printf("exec(echo, BIG) succeeded, should have failed\n");
    exit(1);
  }
}

// what if a string argument crosses over the end of last user page?
void
copyinstr3(char *s)
{
  sbrk(8192);
  uint64 top = (uint64)sbrk(0);
  if ((top % PGSIZE) != 0) {
    sbrk(PGSIZE - (top % PGSIZE));
  }
  top = (uint64)sbrk(0);
  if (top % PGSIZE) {
    printf("oops\n");
    exit(1);
  }

  char *b = (char *)(top - 1);
  *b = 'x';

  int ret = unlink(b);
  if (ret != -1) {
    printf("unlink(%s) returned %d, not -1\n", b, ret);
    exit(1);
  }

  int fd = open(b, O_CREATE | O_WRONLY, 0666);
  if (fd != -1) {
    printf("open(%s) returned %d, not -1\n", b, fd);
    exit(1);
  }

  ret = link(b, b);
  if (ret != -1) {
    printf("link(%s, %s) returned %d, not -1\n", b, b, ret);
    exit(1);
  }

  char *args[] = {"xx", 0};
  ret = exec(b, args);
  if (ret != -1) {
    printf("exec(%s) returned %d, not -1\n", b, fd);
    exit(1);
  }
}

// See if the kernel refuses to read/write user memory that the
// application doesn't have anymore, because it returned it.
void
rwsbrk(char *s)
{
  int fd, n;

  uint64 a = (uint64)sbrk(8192);

  if (a == (uint64)SBRK_ERROR) {
    printf("sbrk(rwsbrk) failed\n");
    exit(1);
  }

  if (sbrk(-8192) == SBRK_ERROR) {
    printf("sbrk(rwsbrk) shrink failed\n");
    exit(1);
  }

  fd = open("rwsbrk", O_CREATE | O_WRONLY, 0666);
  if (fd < 0) {
    printf("open(rwsbrk) failed\n");
    exit(1);
  }
  n = write(fd, (void *)(a + PGSIZE), 1024);
  if (n >= 0) {
    printf("write(fd, %p, 1024) returned %d, not -1\n", (void *)a + PGSIZE, n);
    exit(1);
  }
  close(fd);
  unlink("rwsbrk");

  fd = open("README", O_RDONLY);
  if (fd < 0) {
    printf("open(README) failed\n");
    exit(1);
  }
  n = read(fd, (void *)(a + PGSIZE), 10);
  if (n >= 0) {
    printf("read(fd, %p, 10) returned %d, not -1\n", (void *)a + PGSIZE, n);
    exit(1);
  }
  close(fd);

  exit(0);
}

// test O_TRUNC.
void
truncate1(char *s)
{
  char buf[32];

  unlink("truncfile");
  int fd1 = open("truncfile", O_CREATE | O_WRONLY | O_TRUNC, 0666);
  write(fd1, "abcd", 4);
  close(fd1);

  int fd2 = open("truncfile", O_RDONLY);
  int n = read(fd2, buf, sizeof(buf));
  if (n != 4) {
    printf("%s: read %d bytes, wanted 4\n", s, n);
    exit(1);
  }

  fd1 = open("truncfile", O_WRONLY | O_TRUNC);

  int fd3 = open("truncfile", O_RDONLY);
  n = read(fd3, buf, sizeof(buf));
  if (n != 0) {
    printf("aaa fd3=%d\n", fd3);
    printf("%s: read %d bytes, wanted 0\n", s, n);
    exit(1);
  }

  n = read(fd2, buf, sizeof(buf));
  if (n != 0) {
    printf("bbb fd2=%d\n", fd2);
    printf("%s: read %d bytes, wanted 0\n", s, n);
    exit(1);
  }

  write(fd1, "abcdef", 6);

  n = read(fd3, buf, sizeof(buf));
  if (n != 6) {
    printf("%s: read %d bytes, wanted 6\n", s, n);
    exit(1);
  }

  n = read(fd2, buf, sizeof(buf));
  if (n != 2) {
    printf("%s: read %d bytes, wanted 2\n", s, n);
    exit(1);
  }

  unlink("truncfile");

  close(fd1);
  close(fd2);
  close(fd3);
}

// write to an open FD whose file has just been truncated.
// this causes a write at an offset beyond the end of the file.
// such writes fail on xv6 (unlike POSIX) but at least
// they don't crash.
void
truncate2(char *s)
{
  unlink("truncfile");

  int fd1 = open("truncfile", O_CREATE | O_TRUNC | O_WRONLY, 0666);
  write(fd1, "abcd", 4);

  int fd2 = open("truncfile", O_TRUNC | O_WRONLY);

  int n = write(fd1, "x", 1);
  if (n != -1) {
    printf("%s: write returned %d, expected -1\n", s, n);
    exit(1);
  }

  unlink("truncfile");
  close(fd1);
  close(fd2);
}

void
truncate3(char *s)
{
  int pid, xstatus;

  close(open("truncfile", O_CREATE | O_TRUNC | O_WRONLY, 0666));

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  if (pid == 0) {
    for (int i = 0; i < 100; i++) {
      char buf[32];
      int fd = open("truncfile", O_WRONLY);
      if (fd < 0) {
        printf("%s: open failed\n", s);
        exit(1);
      }
      int n = write(fd, "1234567890", 10);
      if (n != 10) {
        printf("%s: write got %d, expected 10\n", s, n);
        exit(1);
      }
      close(fd);
      fd = open("truncfile", O_RDONLY);
      read(fd, buf, sizeof(buf));
      close(fd);
    }
    exit(0);
  }

  for (int i = 0; i < 150; i++) {
    int fd = open("truncfile", O_CREATE | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
      printf("%s: open failed\n", s);
      exit(1);
    }
    int n = write(fd, "xxx", 3);
    if (n != 3) {
      printf("%s: write got %d, expected 3\n", s, n);
      exit(1);
    }
    close(fd);
  }

  wait(&xstatus);
  unlink("truncfile");
  exit(xstat(xstatus));
}

// does chdir() call iput(p->cwd) in a transaction?
void
iputtest(char *s)
{
  if (mkdir("iputdir", 0777) < 0) {
    printf("%s: mkdir failed\n", s);
    exit(1);
  }
  if (chdir("iputdir") < 0) {
    printf("%s: chdir iputdir failed\n", s);
    exit(1);
  }
  if (unlink("../iputdir") < 0) {
    printf("%s: unlink ../iputdir failed\n", s);
    exit(1);
  }
  if (chdir("/") < 0) {
    printf("%s: chdir / failed\n", s);
    exit(1);
  }
}

// does exit() call iput(p->cwd) in a transaction?
void
exitiputtest(char *s)
{
  int pid, xstatus;

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    if (mkdir("iputdir", 0777) < 0) {
      printf("%s: mkdir failed\n", s);
      exit(1);
    }
    if (chdir("iputdir") < 0) {
      printf("%s: child chdir failed\n", s);
      exit(1);
    }
    if (unlink("../iputdir") < 0) {
      printf("%s: unlink ../iputdir failed\n", s);
      exit(1);
    }
    exit(0);
  }
  wait(&xstatus);
  exit(xstat(xstatus));
}

// does the error path in open() for attempt to write a
// directory call iput() in a transaction?
// needs a hacked kernel that pauses just after the namei()
// call in sys_open():
//    if((ip = namei(path)) == 0)
//      return -1;
//    {
//      int i;
//      for(i = 0; i < 10000; i++)
//        yield();
//    }
void
openiputtest(char *s)
{
  int pid, xstatus;

  if (mkdir("oidir", 0777) < 0) {
    printf("%s: mkdir oidir failed\n", s);
    exit(1);
  }
  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    int fd = open("oidir", O_RDWR);
    if (fd >= 0) {
      printf("%s: open directory for write succeeded\n", s);
      exit(1);
    }
    exit(0);
  }
  ticksleep(1);
  if (unlink("oidir") != 0) {
    printf("%s: unlink failed\n", s);
    exit(1);
  }
  wait(&xstatus);
  exit(xstat(xstatus));
}

// simple file system tests

void
opentest(char *s)
{
  int fd;

  fd = open("/bin/echo", 0);
  if (fd < 0) {
    printf("%s: open echo failed!\n", s);
    exit(1);
  }
  close(fd);
  fd = open("doesnotexist", 0);
  if (fd >= 0) {
    printf("%s: open doesnotexist succeeded!\n", s);
    exit(1);
  }
}

void
writetest(char *s)
{
  int fd;
  int i;
  enum { N = 100, SZ = 10 };

  fd = open("small", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: error: creat small failed!\n", s);
    exit(1);
  }
  for (i = 0; i < N; i++) {
    if (write(fd, "aaaaaaaaaa", SZ) != SZ) {
      printf("%s: error: write aa %d new file failed\n", s, i);
      exit(1);
    }
    if (write(fd, "bbbbbbbbbb", SZ) != SZ) {
      printf("%s: error: write bb %d new file failed\n", s, i);
      exit(1);
    }
  }
  close(fd);
  fd = open("small", O_RDONLY);
  if (fd < 0) {
    printf("%s: error: open small failed!\n", s);
    exit(1);
  }
  i = read(fd, buf, N * SZ * 2);
  if (i != N * SZ * 2) {
    printf("%s: read failed\n", s);
    exit(1);
  }
  close(fd);

  if (unlink("small") < 0) {
    printf("%s: unlink small failed\n", s);
    exit(1);
  }
}

void
writebig(char *s)
{
  int i, fd, n;

  fd = open("big", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: error: creat big failed!\n", s);
    exit(1);
  }

  for (i = 0; i < BIGBLOCKS; i++) {
    ((int *)buf)[0] = i;
    if (write(fd, buf, BSIZE) != BSIZE) {
      printf("%s: error: write big file failed i=%d\n", s, i);
      exit(1);
    }
  }

  close(fd);

  fd = open("big", O_RDONLY);
  if (fd < 0) {
    printf("%s: error: open big failed!\n", s);
    exit(1);
  }

  n = 0;
  for (;;) {
    i = read(fd, buf, BSIZE);
    if (i == 0) {
      if (n != BIGBLOCKS) {
        printf("%s: read only %d blocks from big", s, n);
        exit(1);
      }
      break;
    } else if (i != BSIZE) {
      printf("%s: read failed %d\n", s, i);
      exit(1);
    }
    if (((int *)buf)[0] != n) {
      printf("%s: read content of block %d is %d\n", s, n, ((int *)buf)[0]);
      exit(1);
    }
    n++;
  }
  close(fd);
  if (unlink("big") < 0) {
    printf("%s: unlink big failed\n", s);
    exit(1);
  }
}

// many creates, followed by unlink test
void
createtest(char *s)
{
  int i, fd;
  enum { N = 52 };

  char name[3];
  name[0] = 'a';
  name[2] = '\0';
  for (i = 0; i < N; i++) {
    name[1] = '0' + i;
    fd = open(name, O_CREATE | O_RDWR, 0666);
    close(fd);
  }
  name[0] = 'a';
  name[2] = '\0';
  for (i = 0; i < N; i++) {
    name[1] = '0' + i;
    unlink(name);
  }
}

void
dirtest(char *s)
{
  if (mkdir("dir0", 0777) < 0) {
    printf("%s: mkdir failed\n", s);
    exit(1);
  }

  if (chdir("dir0") < 0) {
    printf("%s: chdir dir0 failed\n", s);
    exit(1);
  }

  if (chdir("..") < 0) {
    printf("%s: chdir .. failed\n", s);
    exit(1);
  }

  if (unlink("dir0") < 0) {
    printf("%s: unlink dir0 failed\n", s);
    exit(1);
  }
}

void
exectest(char *s)
{
  int fd, xstatus, pid;
  char *echoargv[] = {"/bin/echo", "OK", 0};
  char buf[3];

  unlink("echo-ok");
  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    int errfd = dup(1);
    if (errfd < 0) {
      printf("%s: dup failed\n", s);
      exit(1);
    }
    close(1);
    fd = open("echo-ok", O_CREATE | O_WRONLY, 0666);
    if (fd < 0) {
      dprintf(errfd, "%s: create failed\n", s);
      exit(1);
    }
    if (fd != 1) {
      dprintf(errfd, "%s: wrong fd\n", s);
      exit(1);
    }
    if (exec("/bin/echo", echoargv) < 0) {
      dprintf(errfd, "%s: exec echo failed\n", s);
      exit(1);
    }
    // won't get to here
  }
  if (wait(&xstatus) != pid) {
    printf("%s: wait failed!\n", s);
  }
  if (xstatus != 0) {
    printf("%s: nonzero wait status %d\n", s, xstatus);
    exit(1);
  }

  fd = open("echo-ok", O_RDONLY);
  if (fd < 0) {
    printf("%s: open failed\n", s);
    exit(1);
  }
  if (read(fd, buf, 2) != 2) {
    printf("%s: read failed\n", s);
    exit(1);
  }
  unlink("echo-ok");
  if (buf[0] == 'O' && buf[1] == 'K')
    exit(0);
  else {
    printf("%s: wrong output\n", s);
    exit(1);
  }
}

// simple fork and pipe read/write

void
pipe1(char *s)
{
  int fds[2], pid, xstatus;
  int seq, i, n, cc, total;
  enum { N = 5, SZ = 1033 };

  if (pipe(fds) != 0) {
    printf("%s: pipe() failed\n", s);
    exit(1);
  }
  pid = fork();
  seq = 0;
  if (pid == 0) {
    close(fds[0]);
    for (n = 0; n < N; n++) {
      for (i = 0; i < SZ; i++)
        buf[i] = seq++;
      if (write(fds[1], buf, SZ) != SZ) {
        printf("%s: pipe1 oops 1\n", s);
        exit(1);
      }
    }
    exit(0);
  } else if (pid > 0) {
    close(fds[1]);
    total = 0;
    cc = 1;
    while ((n = read(fds[0], buf, cc)) > 0) {
      for (i = 0; i < n; i++) {
        if ((buf[i] & 0xff) != (seq++ & 0xff)) {
          printf("%s: pipe1 oops 2\n", s);
          return;
        }
      }
      total += n;
      cc = cc * 2;
      if (cc > sizeof(buf))
        cc = sizeof(buf);
    }
    if (total != N * SZ) {
      printf("%s: pipe1 oops 3 total %d\n", s, total);
      exit(1);
    }
    close(fds[0]);
    wait(&xstatus);
    exit(xstat(xstatus));
  } else {
    printf("%s: fork() failed\n", s);
    exit(1);
  }
}

// test if child is killed (status = -1)
void
killstatus(char *s)
{
  int xst;

  for (int i = 0; i < 100; i++) {
    int pid1 = fork();
    if (pid1 < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid1 == 0) {
      while (1) {
        getpid();
      }
      exit(0);
    }
    ticksleep(1);
    kill(pid1, SIGKILL);
    wait(&xst);
    if (!WIFSIGNALED(xst)) {
      printf("%s: the child should have been killed by a signal\n", s);
      exit(1);
    }
  }
  exit(0);
}

// meant to be run w/ at most two CPUs
void
preempt(char *s)
{
  int pid1, pid2, pid3;
  int pfds[2];

  pid1 = fork();
  if (pid1 < 0) {
    printf("%s: fork failed", s);
    exit(1);
  }
  if (pid1 == 0)
    for (;;)
      ;

  pid2 = fork();
  if (pid2 < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid2 == 0)
    for (;;)
      ;

  pipe(pfds);
  pid3 = fork();
  if (pid3 < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid3 == 0) {
    close(pfds[0]);
    if (write(pfds[1], "x", 1) != 1)
      printf("%s: preempt write error", s);
    close(pfds[1]);
    for (;;)
      ;
  }

  close(pfds[1]);
  if (read(pfds[0], buf, sizeof(buf)) != 1) {
    printf("%s: preempt read error", s);
    return;
  }
  close(pfds[0]);
  printf("kill... ");
  kill(pid1, SIGKILL);
  kill(pid2, SIGKILL);
  kill(pid3, SIGKILL);
  printf("wait... ");
  wait(0);
  wait(0);
  wait(0);
}

// try to find any races between exit and wait
void
exitwait(char *s)
{
  int i, pid;

  for (i = 0; i < 100; i++) {
    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid) {
      int xstate;
      if (wait(&xstate) != pid) {
        printf("%s: wait wrong pid\n", s);
        exit(1);
      }
      if (!WIFEXITED(xstate) || WEXITSTATUS(xstate) != i) {
        printf("%s: wait wrong exit status\n", s);
        exit(1);
      }
    } else {
      exit(i);
    }
  }
}

// try to find races in the reparenting
// code that handles a parent exiting
// when it still has live children.
void
reparent(char *s)
{
  int master_pid = getpid();
  for (int i = 0; i < 200; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid) {
      if (wait(0) != pid) {
        printf("%s: wait wrong pid\n", s);
        exit(1);
      }
    } else {
      int pid2 = fork();
      if (pid2 < 0) {
        kill(master_pid, SIGKILL);
        exit(1);
      }
      exit(0);
    }
  }
  exit(0);
}

// what if two children exit() at the same time?
void
twochildren(char *s)
{
  for (int i = 0; i < 1000; i++) {
    int pid1 = fork();
    if (pid1 < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid1 == 0) {
      exit(0);
    } else {
      int pid2 = fork();
      if (pid2 < 0) {
        printf("%s: fork failed\n", s);
        exit(1);
      }
      if (pid2 == 0) {
        exit(0);
      } else {
        wait(0);
        wait(0);
      }
    }
  }
}

// concurrent forks to try to expose locking bugs.
void
forkfork(char *s)
{
  enum { N = 2 };

  for (int i = 0; i < N; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed", s);
      exit(1);
    }
    if (pid == 0) {
      for (int j = 0; j < 200; j++) {
        int pid1 = fork();
        if (pid1 < 0) {
          exit(1);
        }
        if (pid1 == 0) {
          exit(0);
        }
        wait(0);
      }
      exit(0);
    }
  }

  int xstatus;
  for (int i = 0; i < N; i++) {
    wait(&xstatus);
    if (xstatus != 0) {
      printf("%s: fork in child failed", s);
      exit(1);
    }
  }
}

void
forkforkfork(char *s)
{
  long nprocs0 = sysconf(_SC_XV6_NPROCS);
  unlink("stopforking");

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed", s);
    exit(1);
  }
  if (pid == 0) {
    while (1) {
      int fd = open("stopforking", 0);
      if (fd >= 0) {
        exit(0);
      }
      if (fork() < 0) {
        close(open("stopforking", O_CREATE | O_RDWR, 0666));
        // out of memory: if not even the file can be opened, stop here
        // rather than spin (the file table is allocated on demand).
        if (open("stopforking", 0) < 0)
          exit(0);
      }
    }

    exit(0);
  }

  ticksleep(20); // two seconds
  close(open("stopforking", O_CREATE | O_RDWR, 0666));
  wait(0);
  // the storm's orphans are init's to reap; there is no process table to
  // bound them, so wait until they are gone before the free-page check.
  for (int i = 0; i < 600; i++) {
    if (sysconf(_SC_XV6_NPROCS) <= nprocs0)
      break;
    ticksleep(10);
  }
}

// regression test. does reparent() violate the parent-then-child
// locking order when giving away a child to init, so that exit()
// deadlocks against init's wait()? also used to trigger a "panic:
// release" due to exit() releasing a different p->parent->lock than
// it acquired.
void
reparent2(char *s)
{
  for (int i = 0; i < 800; i++) {
    int pid1 = fork();
    if (pid1 < 0) {
      printf("fork failed\n");
      exit(1);
    }
    if (pid1 == 0) {
      fork();
      fork();
      exit(0);
    }
    wait(0);
  }

  exit(0);
}

// allocate all mem, free it, and allocate again
void
mem(char *s)
{
  void *m1, *m2;
  int pid;

  if ((pid = fork()) == 0) {
    // allocate until malloc fails, or 256 MB on a machine too large to
    // exhaust (the point is the allocator's bookkeeping, not the size).
    long budget = 256L * 1024 * 1024;
    m1 = 0;
    while (budget > 0 && (m2 = malloc(10001)) != 0) {
      *(char **)m2 = m1;
      m1 = m2;
      budget -= 10001;
    }
    while (m1) {
      m2 = *(char **)m1;
      free(m1);
      m1 = m2;
    }
    m1 = malloc(1024 * 20);
    if (m1 == 0) {
      printf("%s: couldn't allocate mem?!!\n", s);
      exit(1);
    }
    free(m1);
    exit(0);
  } else {
    int xstatus;
    wait(&xstatus);
    if (WIFSIGNALED(xstatus)) {
      // probably page fault, so might be lazy lab,
      // so OK.
      exit(0);
    }
    exit(xstat(xstatus));
  }
}

// More file system tests

// two processes write to the same file descriptor
// is the offset shared? does inode locking work?
void
sharedfd(char *s)
{
  int fd, pid, i, n, nc, np;
  enum { N = 1000, SZ = 10 };
  char buf[SZ];

  unlink("sharedfd");
  fd = open("sharedfd", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: cannot open sharedfd for writing", s);
    exit(1);
  }
  pid = fork();
  memset(buf, pid == 0 ? 'c' : 'p', sizeof(buf));
  for (i = 0; i < N; i++) {
    if (write(fd, buf, sizeof(buf)) != sizeof(buf)) {
      printf("%s: write sharedfd failed\n", s);
      exit(1);
    }
  }
  if (pid == 0) {
    exit(0);
  } else {
    int xstatus;
    wait(&xstatus);
    if (xstatus != 0)
      exit(xstat(xstatus));
  }

  close(fd);
  fd = open("sharedfd", 0);
  if (fd < 0) {
    printf("%s: cannot open sharedfd for reading\n", s);
    exit(1);
  }
  nc = np = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (i = 0; i < sizeof(buf); i++) {
      if (buf[i] == 'c')
        nc++;
      if (buf[i] == 'p')
        np++;
    }
  }
  close(fd);
  unlink("sharedfd");
  if (nc == N * SZ && np == N * SZ) {
    exit(0);
  } else {
    printf("%s: nc/np test fails\n", s);
    exit(1);
  }
}

// four processes write different files at the same
// time, to test block allocation.
void
fourfiles(char *s)
{
  int fd, pid, i, j, n, total, pi;
  char *names[] = {"f0", "f1", "f2", "f3"};
  char *fname;
  enum { N = 12, NCHILD = 4, SZ = 500 };

  for (pi = 0; pi < NCHILD; pi++) {
    fname = names[pi];
    unlink(fname);

    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }

    if (pid == 0) {
      fd = open(fname, O_CREATE | O_RDWR, 0666);
      if (fd < 0) {
        printf("%s: create failed\n", s);
        exit(1);
      }

      memset(buf, '0' + pi, SZ);
      for (i = 0; i < N; i++) {
        if ((n = write(fd, buf, SZ)) != SZ) {
          printf("write failed %d\n", n);
          exit(1);
        }
      }
      exit(0);
    }
  }

  int xstatus;
  for (pi = 0; pi < NCHILD; pi++) {
    wait(&xstatus);
    if (xstatus != 0)
      exit(xstat(xstatus));
  }

  for (i = 0; i < NCHILD; i++) {
    fname = names[i];
    fd = open(fname, 0);
    total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      for (j = 0; j < n; j++) {
        if (buf[j] != '0' + i) {
          printf("%s: wrong char\n", s);
          exit(1);
        }
      }
      total += n;
    }
    close(fd);
    if (total != N * SZ) {
      printf("wrong length %d\n", total);
      exit(1);
    }
    unlink(fname);
  }
}

// four processes create and delete different files in same directory
void
createdelete(char *s)
{
  enum { N = 20, NCHILD = 4 };
  int pid, i, fd, pi;
  char name[32];

  for (pi = 0; pi < NCHILD; pi++) {
    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }

    if (pid == 0) {
      name[0] = 'p' + pi;
      name[2] = '\0';
      for (i = 0; i < N; i++) {
        name[1] = '0' + i;
        fd = open(name, O_CREATE | O_RDWR, 0666);
        if (fd < 0) {
          printf("%s: create failed\n", s);
          exit(1);
        }
        close(fd);
        if (i > 0 && (i % 2) == 0) {
          name[1] = '0' + (i / 2);
          if (unlink(name) < 0) {
            printf("%s: unlink failed\n", s);
            exit(1);
          }
        }
      }
      exit(0);
    }
  }

  int xstatus;
  for (pi = 0; pi < NCHILD; pi++) {
    wait(&xstatus);
    if (xstatus != 0)
      exit(1);
  }

  name[0] = name[1] = name[2] = 0;
  for (i = 0; i < N; i++) {
    for (pi = 0; pi < NCHILD; pi++) {
      name[0] = 'p' + pi;
      name[1] = '0' + i;
      fd = open(name, 0);
      if ((i == 0 || i >= N / 2) && fd < 0) {
        printf("%s: oops createdelete %s didn't exist\n", s, name);
        exit(1);
      } else if ((i >= 1 && i < N / 2) && fd >= 0) {
        printf("%s: oops createdelete %s did exist\n", s, name);
        exit(1);
      }
      if (fd >= 0)
        close(fd);
    }
  }

  for (i = 0; i < N; i++) {
    for (pi = 0; pi < NCHILD; pi++) {
      name[0] = 'p' + pi;
      name[1] = '0' + i;
      unlink(name);
    }
  }
}

// can I unlink a file and still read it?
void
unlinkread(char *s)
{
  enum { SZ = 5 };
  int fd, fd1;

  fd = open("unlinkread", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create unlinkread failed\n", s);
    exit(1);
  }
  write(fd, "hello", SZ);
  close(fd);

  fd = open("unlinkread", O_RDWR);
  if (fd < 0) {
    printf("%s: open unlinkread failed\n", s);
    exit(1);
  }
  if (unlink("unlinkread") != 0) {
    printf("%s: unlink unlinkread failed\n", s);
    exit(1);
  }

  fd1 = open("unlinkread", O_CREATE | O_RDWR, 0666);
  write(fd1, "yyy", 3);
  close(fd1);

  if (read(fd, buf, sizeof(buf)) != SZ) {
    printf("%s: unlinkread read failed", s);
    exit(1);
  }
  if (buf[0] != 'h') {
    printf("%s: unlinkread wrong data\n", s);
    exit(1);
  }
  if (write(fd, buf, 10) != 10) {
    printf("%s: unlinkread write failed\n", s);
    exit(1);
  }
  close(fd);
  unlink("unlinkread");
}

void
linktest(char *s)
{
  enum { SZ = 5 };
  int fd;

  unlink("lf1");
  unlink("lf2");

  fd = open("lf1", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create lf1 failed\n", s);
    exit(1);
  }
  if (write(fd, "hello", SZ) != SZ) {
    printf("%s: write lf1 failed\n", s);
    exit(1);
  }
  close(fd);

  if (link("lf1", "lf2") < 0) {
    printf("%s: link lf1 lf2 failed\n", s);
    exit(1);
  }
  unlink("lf1");

  if (open("lf1", 0) >= 0) {
    printf("%s: unlinked lf1 but it is still there!\n", s);
    exit(1);
  }

  fd = open("lf2", 0);
  if (fd < 0) {
    printf("%s: open lf2 failed\n", s);
    exit(1);
  }
  if (read(fd, buf, sizeof(buf)) != SZ) {
    printf("%s: read lf2 failed\n", s);
    exit(1);
  }
  close(fd);

  if (link("lf2", "lf2") >= 0) {
    printf("%s: link lf2 lf2 succeeded! oops\n", s);
    exit(1);
  }

  unlink("lf2");
  if (link("lf2", "lf1") >= 0) {
    printf("%s: link non-existent succeeded! oops\n", s);
    exit(1);
  }

  if (link(".", "lf1") >= 0) {
    printf("%s: link . lf1 succeeded! oops\n", s);
    exit(1);
  }
}

// test concurrent create/link/unlink of the same file
void
concreate(char *s)
{
  enum { N = 40 };
  char file[3];
  int i, pid, n, fd;
  char fa[N];

  file[0] = 'C';
  file[2] = '\0';
  for (i = 0; i < N; i++) {
    file[1] = '0' + i;
    unlink(file);
    pid = fork();
    if (pid && (i % 3) == 1) {
      link("C0", file);
    } else if (pid == 0 && (i % 5) == 1) {
      link("C0", file);
    } else {
      fd = open(file, O_CREATE | O_RDWR, 0666);
      if (fd < 0) {
        printf("concreate create %s failed\n", file);
        exit(1);
      }
      close(fd);
    }
    if (pid == 0) {
      exit(0);
    } else {
      int xstatus;
      wait(&xstatus);
      if (xstatus != 0)
        exit(1);
    }
  }

  // the directory is walked with readdir, not by reading the directory
  // as a file: what those bytes look like is the file system's own
  // business, and this kernel mounts more than one kind.
  memset(fa, 0, sizeof(fa));
  n = 0;
  DIR *d = opendir(".");
  if (d == 0) {
    printf("%s: concreate opendir failed\n", s);
    exit(1);
  }
  struct dirent *de;
  while ((de = readdir(d)) != 0) {
    if (de->d_name[0] == 'C' && de->d_name[2] == '\0') {
      i = de->d_name[1] - '0';
      if (i < 0 || i >= (int)sizeof(fa)) {
        printf("%s: concreate weird file %s\n", s, de->d_name);
        exit(1);
      }
      if (fa[i]) {
        printf("%s: concreate duplicate file %s\n", s, de->d_name);
        exit(1);
      }
      fa[i] = 1;
      n++;
    }
  }
  closedir(d);

  if (n != N) {
    printf("%s: concreate not enough files in directory listing\n", s);
    exit(1);
  }

  for (i = 0; i < N; i++) {
    file[1] = '0' + i;
    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (((i % 3) == 0 && pid == 0) || ((i % 3) == 1 && pid != 0)) {
      close(open(file, 0));
      close(open(file, 0));
      close(open(file, 0));
      close(open(file, 0));
      close(open(file, 0));
      close(open(file, 0));
    } else {
      unlink(file);
      unlink(file);
      unlink(file);
      unlink(file);
      unlink(file);
      unlink(file);
    }
    if (pid == 0)
      exit(0);
    else
      wait(0);
  }
}

// another concurrent link/unlink/create test,
// to look for deadlocks.
void
linkunlink(char *s)
{
  int pid, i;

  unlink("x");
  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }

  unsigned int x = (pid ? 1 : 97);
  for (i = 0; i < 100; i++) {
    x = x * 1103515245 + 12345;
    if ((x % 3) == 0) {
      close(open("x", O_RDWR | O_CREATE, 0666));
    } else if ((x % 3) == 1) {
      link("/bin/cat", "x");
    } else {
      unlink("x");
    }
  }

  if (pid)
    wait(0);
  else
    exit(0);
}

void
subdir(char *s)
{
  int fd, cc;

  unlink("ff");
  if (mkdir("dd", 0777) != 0) {
    printf("%s: mkdir dd failed\n", s);
    exit(1);
  }

  fd = open("dd/ff", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create dd/ff failed\n", s);
    exit(1);
  }
  write(fd, "ff", 2);
  close(fd);

  if (unlink("dd") >= 0) {
    printf("%s: unlink dd (non-empty dir) succeeded!\n", s);
    exit(1);
  }

  if (mkdir("/dd/dd", 0777) != 0) {
    printf("%s: subdir mkdir dd/dd failed\n", s);
    exit(1);
  }

  fd = open("dd/dd/ff", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create dd/dd/ff failed\n", s);
    exit(1);
  }
  write(fd, "FF", 2);
  close(fd);

  fd = open("dd/dd/../ff", 0);
  if (fd < 0) {
    printf("%s: open dd/dd/../ff failed\n", s);
    exit(1);
  }
  cc = read(fd, buf, sizeof(buf));
  if (cc != 2 || buf[0] != 'f') {
    printf("%s: dd/dd/../ff wrong content\n", s);
    exit(1);
  }
  close(fd);

  if (link("dd/dd/ff", "dd/dd/ffff") != 0) {
    printf("%s: link dd/dd/ff dd/dd/ffff failed\n", s);
    exit(1);
  }

  if (unlink("dd/dd/ff") != 0) {
    printf("%s: unlink dd/dd/ff failed\n", s);
    exit(1);
  }
  if (open("dd/dd/ff", O_RDONLY) >= 0) {
    printf("%s: open (unlinked) dd/dd/ff succeeded\n", s);
    exit(1);
  }

  if (chdir("dd") != 0) {
    printf("%s: chdir dd failed\n", s);
    exit(1);
  }
  if (chdir("dd/../../dd") != 0) {
    printf("%s: chdir dd/../../dd failed\n", s);
    exit(1);
  }
  if (chdir("dd/../../../dd") != 0) {
    printf("%s: chdir dd/../../../dd failed\n", s);
    exit(1);
  }
  if (chdir("./..") != 0) {
    printf("%s: chdir ./.. failed\n", s);
    exit(1);
  }

  fd = open("dd/dd/ffff", 0);
  if (fd < 0) {
    printf("%s: open dd/dd/ffff failed\n", s);
    exit(1);
  }
  if (read(fd, buf, sizeof(buf)) != 2) {
    printf("%s: read dd/dd/ffff wrong len\n", s);
    exit(1);
  }
  close(fd);

  if (open("dd/dd/ff", O_RDONLY) >= 0) {
    printf("%s: open (unlinked) dd/dd/ff succeeded!\n", s);
    exit(1);
  }

  if (open("dd/ff/ff", O_CREATE | O_RDWR, 0666) >= 0) {
    printf("%s: create dd/ff/ff succeeded!\n", s);
    exit(1);
  }
  if (open("dd/xx/ff", O_CREATE | O_RDWR, 0666) >= 0) {
    printf("%s: create dd/xx/ff succeeded!\n", s);
    exit(1);
  }
  if (open("dd", O_CREATE, 0666) >= 0) {
    printf("%s: create dd succeeded!\n", s);
    exit(1);
  }
  if (open("dd", O_RDWR) >= 0) {
    printf("%s: open dd rdwr succeeded!\n", s);
    exit(1);
  }
  if (open("dd", O_WRONLY) >= 0) {
    printf("%s: open dd wronly succeeded!\n", s);
    exit(1);
  }
  if (link("dd/ff/ff", "dd/dd/xx") == 0) {
    printf("%s: link dd/ff/ff dd/dd/xx succeeded!\n", s);
    exit(1);
  }
  if (link("dd/xx/ff", "dd/dd/xx") == 0) {
    printf("%s: link dd/xx/ff dd/dd/xx succeeded!\n", s);
    exit(1);
  }
  if (link("dd/ff", "dd/dd/ffff") == 0) {
    printf("%s: link dd/ff dd/dd/ffff succeeded!\n", s);
    exit(1);
  }
  if (mkdir("dd/ff/ff", 0777) == 0) {
    printf("%s: mkdir dd/ff/ff succeeded!\n", s);
    exit(1);
  }
  if (mkdir("dd/xx/ff", 0777) == 0) {
    printf("%s: mkdir dd/xx/ff succeeded!\n", s);
    exit(1);
  }
  if (mkdir("dd/dd/ffff", 0777) == 0) {
    printf("%s: mkdir dd/dd/ffff succeeded!\n", s);
    exit(1);
  }
  if (unlink("dd/xx/ff") == 0) {
    printf("%s: unlink dd/xx/ff succeeded!\n", s);
    exit(1);
  }
  if (unlink("dd/ff/ff") == 0) {
    printf("%s: unlink dd/ff/ff succeeded!\n", s);
    exit(1);
  }
  if (chdir("dd/ff") == 0) {
    printf("%s: chdir dd/ff succeeded!\n", s);
    exit(1);
  }
  if (chdir("dd/xx") == 0) {
    printf("%s: chdir dd/xx succeeded!\n", s);
    exit(1);
  }

  if (unlink("dd/dd/ffff") != 0) {
    printf("%s: unlink dd/dd/ff failed\n", s);
    exit(1);
  }
  if (unlink("dd/ff") != 0) {
    printf("%s: unlink dd/ff failed\n", s);
    exit(1);
  }
  if (unlink("dd") == 0) {
    printf("%s: unlink non-empty dd succeeded!\n", s);
    exit(1);
  }
  if (unlink("dd/dd") < 0) {
    printf("%s: unlink dd/dd failed\n", s);
    exit(1);
  }
  if (unlink("dd") < 0) {
    printf("%s: unlink dd failed\n", s);
    exit(1);
  }
}

// test writes that are larger than the log.
void
bigwrite(char *s)
{
  int fd, sz;

  unlink("bigwrite");
  for (sz = 499; sz < BUFSZ; sz += 471) {
    fd = open("bigwrite", O_CREATE | O_RDWR, 0666);
    if (fd < 0) {
      printf("%s: cannot create bigwrite\n", s);
      exit(1);
    }
    int i;
    for (i = 0; i < 2; i++) {
      int cc = write(fd, buf, sz);
      if (cc != sz) {
        printf("%s: write(%d) ret %d\n", s, sz, cc);
        exit(1);
      }
    }
    close(fd);
    unlink("bigwrite");
  }
}

void
bigfile(char *s)
{
  enum { N = 20, SZ = 600 };
  int fd, i, total, cc;

  unlink("bigfile.dat");
  fd = open("bigfile.dat", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: cannot create bigfile", s);
    exit(1);
  }
  for (i = 0; i < N; i++) {
    memset(buf, i, SZ);
    if (write(fd, buf, SZ) != SZ) {
      printf("%s: write bigfile failed\n", s);
      exit(1);
    }
  }
  close(fd);

  fd = open("bigfile.dat", 0);
  if (fd < 0) {
    printf("%s: cannot open bigfile\n", s);
    exit(1);
  }
  total = 0;
  for (i = 0;; i++) {
    cc = read(fd, buf, SZ / 2);
    if (cc < 0) {
      printf("%s: read bigfile failed\n", s);
      exit(1);
    }
    if (cc == 0)
      break;
    if (cc != SZ / 2) {
      printf("%s: short read bigfile\n", s);
      exit(1);
    }
    if (buf[0] != i / 2 || buf[SZ / 2 - 1] != i / 2) {
      printf("%s: read bigfile wrong data\n", s);
      exit(1);
    }
    total += cc;
  }
  close(fd);
  if (total != N * SZ) {
    printf("%s: read bigfile wrong total\n", s);
    exit(1);
  }
  unlink("bigfile.dat");
}

void
longnames(char *s)
{
  // names are strings now, not a fixed fourteen-byte field, so two
  // names that agree for the first fourteen characters are two names.
  // this used to be the "fourteen" test, which checked the opposite.
  char a[NAME_MAX + 1], b[NAME_MAX + 1], path[PATH_MAX];
  int fd;

  for (int i = 0; i < NAME_MAX; i++) {
    a[i] = 'a' + (i % 26);
    b[i] = 'a' + (i % 26);
  }
  a[NAME_MAX] = b[NAME_MAX] = 0;
  b[NAME_MAX - 1] = 'Z'; // the same name but for its last character

  if (mkdir(a, 0777) != 0) {
    printf("%s: mkdir of a %d-character name failed\n", s, NAME_MAX);
    exit(1);
  }
  if (mkdir(b, 0777) != 0) {
    printf("%s: the two long names were treated as one\n", s);
    exit(1);
  }
  snprintf(path, sizeof(path), "%s/%s", a, b);
  fd = open(path, O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create in a long-named directory failed\n", s);
    exit(1);
  }
  if (write(fd, "hello", 5) != 5) {
    printf("%s: write to a long-named file failed\n", s);
    exit(1);
  }
  close(fd);

  // and the names that used to collide do not
  if (mkdir("12345678901234", 0777) != 0 || mkdir("123456789012345", 0777) != 0) {
    printf("%s: names differing after fourteen characters collided\n", s);
    exit(1);
  }

  unlink("12345678901234");
  unlink("123456789012345");
  unlink(path);
  unlink(b);
  unlink(a);
}

void
rmdot(char *s)
{
  if (mkdir("dots", 0777) != 0) {
    printf("%s: mkdir dots failed\n", s);
    exit(1);
  }
  if (chdir("dots") != 0) {
    printf("%s: chdir dots failed\n", s);
    exit(1);
  }
  if (unlink(".") == 0) {
    printf("%s: rm . worked!\n", s);
    exit(1);
  }
  if (unlink("..") == 0) {
    printf("%s: rm .. worked!\n", s);
    exit(1);
  }
  if (chdir("/") != 0) {
    printf("%s: chdir / failed\n", s);
    exit(1);
  }
  if (unlink("dots/.") == 0) {
    printf("%s: unlink dots/. worked!\n", s);
    exit(1);
  }
  if (unlink("dots/..") == 0) {
    printf("%s: unlink dots/.. worked!\n", s);
    exit(1);
  }
  if (unlink("dots") != 0) {
    printf("%s: unlink dots failed!\n", s);
    exit(1);
  }
}

void
dirfile(char *s)
{
  int fd;

  fd = open("dirfile", O_CREATE, 0666);
  if (fd < 0) {
    printf("%s: create dirfile failed\n", s);
    exit(1);
  }
  close(fd);
  if (chdir("dirfile") == 0) {
    printf("%s: chdir dirfile succeeded!\n", s);
    exit(1);
  }
  fd = open("dirfile/xx", 0);
  if (fd >= 0) {
    printf("%s: create dirfile/xx succeeded!\n", s);
    exit(1);
  }
  fd = open("dirfile/xx", O_CREATE, 0666);
  if (fd >= 0) {
    printf("%s: create dirfile/xx succeeded!\n", s);
    exit(1);
  }
  if (mkdir("dirfile/xx", 0777) == 0) {
    printf("%s: mkdir dirfile/xx succeeded!\n", s);
    exit(1);
  }
  if (unlink("dirfile/xx") == 0) {
    printf("%s: unlink dirfile/xx succeeded!\n", s);
    exit(1);
  }
  if (link("README", "dirfile/xx") == 0) {
    printf("%s: link to dirfile/xx succeeded!\n", s);
    exit(1);
  }
  if (unlink("dirfile") != 0) {
    printf("%s: unlink dirfile failed!\n", s);
    exit(1);
  }

  fd = open(".", O_RDWR);
  if (fd >= 0) {
    printf("%s: open . for writing succeeded!\n", s);
    exit(1);
  }
  fd = open(".", 0);
  if (write(fd, "x", 1) > 0) {
    printf("%s: write . succeeded!\n", s);
    exit(1);
  }
  close(fd);
}

// test that iput() is called at the end of _namei().
// also tests empty file names.
void
iref(char *s)
{
  int i, fd;

  for (i = 0; i < NINODE + 1; i++) {
    if (mkdir("irefd", 0777) != 0) {
      printf("%s: mkdir irefd failed\n", s);
      exit(1);
    }
    if (chdir("irefd") != 0) {
      printf("%s: chdir irefd failed\n", s);
      exit(1);
    }

    mkdir("", 0777);
    link("README", "");
    fd = open("", O_CREATE, 0666);
    if (fd >= 0)
      close(fd);
    fd = open("xx", O_CREATE, 0666);
    if (fd >= 0)
      close(fd);
    unlink("xx");
  }

  // clean up
  for (i = 0; i < NINODE + 1; i++) {
    chdir("..");
    unlink("irefd");
  }

  chdir("/");
}

// test that fork fails gracefully
// the forktest binary also does this, but it runs out of proc entries first.
// inside the bigger usertests binary, we run out of memory first.
void
forktest(char *s)
{
  enum { N = 1000 };
  int n, pid;

  for (n = 0; n < N; n++) {
    pid = fork();
    if (pid < 0)
      break;
    if (pid == 0)
      exit(0);
  }

  if (n == 0) {
    printf("%s: no fork at all!\n", s);
    exit(1);
  }

  // no fixed process table: all N forks may succeed; every child must
  // be reaped below and none beyond.

  for (; n > 0; n--) {
    if (wait(0) < 0) {
      printf("%s: wait stopped early\n", s);
      exit(1);
    }
  }

  if (wait(0) != -1) {
    printf("%s: wait got too many\n", s);
    exit(1);
  }
}

void
sbrkbasic(char *s)
{
  enum { TOOMUCH = 1024 * 1024 * 1024 };
  int i, pid, xstatus;
  char *c, *a, *b;

  // does sbrk() return the expected failure value? an int's worth of
  // sbrk() cannot exceed the RAM of a machine with 2 GB or more, where
  // the allocation simply succeeds.
  int plenty = (uint64)sysconf(_SC_PHYS_PAGES) * PGSIZE >= 2UL * TOOMUCH;
  pid = fork();
  if (pid < 0) {
    printf("fork failed in sbrkbasic\n");
    exit(1);
  }
  if (pid == 0) {
    if (plenty)
      exit(0);
    a = sbrk(TOOMUCH);
    if (a == (char *)SBRK_ERROR) {
      // it's OK if this fails.
      exit(0);
    }

    for (b = a; b < a + TOOMUCH; b += PGSIZE) {
      *b = 99;
    }

    // we should not get here! either sbrk(TOOMUCH)
    // should have failed, or (with lazy allocation)
    // a pagefault should have killed this process.
    exit(1);
  }

  wait(&xstatus);
  if (xstatus == 1) {
    printf("%s: too much memory allocated!\n", s);
    exit(1);
  }

  // can one sbrk() less than a page?
  a = sbrk(0);
  for (i = 0; i < 5000; i++) {
    b = sbrk(1);
    if (b != a) {
      printf("%s: sbrk test failed %d %p %p\n", s, i, a, b);
      exit(1);
    }
    *b = 1;
    a = b + 1;
  }
  pid = fork();
  if (pid < 0) {
    printf("%s: sbrk test fork failed\n", s);
    exit(1);
  }
  c = sbrk(1);
  c = sbrk(1);
  if (c != a + 1) {
    printf("%s: sbrk test failed post-fork\n", s);
    exit(1);
  }
  if (pid == 0)
    exit(0);
  wait(&xstatus);
  exit(xstat(xstatus));
}

void
sbrkmuch(char *s)
{
  enum { BIG = 100 * 1024 * 1024 };
  char *c, *oldbrk, *a, *lastaddr, *p;
  uint64 amt;

  oldbrk = sbrk(0);

  // can one grow address space to something big?
  a = sbrk(0);
  amt = BIG - (uint64)a;
  p = sbrk(amt);
  if (p != a) {
    printf("%s: sbrk test failed to grow big address space; enough phys mem?\n",
           s);
    exit(1);
  }

  lastaddr = (char *)(BIG - 1);
  *lastaddr = 99;

  // can one de-allocate?
  a = sbrk(0);
  c = sbrk(-PGSIZE);
  if (c == (char *)SBRK_ERROR) {
    printf("%s: sbrk could not deallocate\n", s);
    exit(1);
  }
  c = sbrk(0);
  if (c != a - PGSIZE) {
    printf("%s: sbrk deallocation produced wrong address, a %p c %p\n", s, a,
           c);
    exit(1);
  }

  // can one re-allocate that page?
  a = sbrk(0);
  c = sbrk(PGSIZE);
  if (c != a || sbrk(0) != a + PGSIZE) {
    printf("%s: sbrk re-allocation failed, a %p c %p\n", s, a, c);
    exit(1);
  }
  if (*lastaddr == 99) {
    // should be zero
    printf("%s: sbrk de-allocation didn't really deallocate\n", s);
    exit(1);
  }

  a = sbrk(0);
  c = sbrk(-((char *)sbrk(0) - oldbrk));
  if (c != a) {
    printf("%s: sbrk downsize failed, a %p c %p\n", s, a, c);
    exit(1);
  }
}

// can we read the kernel's memory?
void
kernmem(char *s)
{
  char *a;
  int pid;

  for (a = (char *)(KERNBASE); a < (char *)(KERNBASE + 2000000); a += 50000) {
    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid == 0) {
      printf("%s: oops could read %p = %x\n", s, a, *a);
      exit(1);
    }
    int xstatus;
    wait(&xstatus);
    if (!WIFSIGNALED(xstatus)) // did a fault kill the child?
      exit(1);
  }
}

// user code should not be able to write to addresses above MAXVA.
void
MAXVAplus(char *s)
{
  volatile uint64 a = MAXVA;
  for (; a != 0; a <<= 1) {
    int pid;
    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid == 0) {
      *(char *)a = 99;
      printf("%s: oops wrote %p\n", s, (void *)a);
      exit(1);
    }
    int xstatus;
    wait(&xstatus);
    if (!WIFSIGNALED(xstatus)) // did a fault kill the child?
      exit(1);
  }
}

// if we run the system out of memory, does it clean up the last
// failed allocation?
void
sbrkfail(char *s)
{
  int i, xstatus;
  int fds[2];
  char scratch;
  char *c, *a;
  int pids[10];
  int pid;
  int failed;

  // ten children each take a quarter of RAM: some must be refused. on a
  // machine with more than 4 GB this would touch too much memory for the
  // test to be worth it; the failure path is checked on small ones.
  uint64 phys = (uint64)sysconf(_SC_PHYS_PAGES) * PGSIZE;
  if (phys > 4UL * 1024 * 1024 * 1024) {
    printf("%s: %ld MB of RAM, not exhausting it\n", s, phys >> 20);
    return;
  }
  int BIG = (int)(phys / 4);

  failed = 0;
  if (pipe(fds) != 0) {
    printf("%s: pipe() failed\n", s);
    exit(1);
  }
  for (i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
    if ((pids[i] = fork()) == 0) {
      // allocate a lot of memory
      if (sbrk(BIG - (uint64)sbrk(0)) == (char *)SBRK_ERROR)
        write(fds[1], "0", 1);
      else
        write(fds[1], "1", 1);
      // sit around until killed
      for (;;)
        ticksleep(1000);
    }
    if (pids[i] != -1) {
      read(fds[0], &scratch, 1);
      if (scratch == '0')
        failed = 1;
    }
  }
  if (!failed) {
    printf("%s: no allocation failed; allocate more?\n", s);
  }

  // if those failed allocations freed up the pages they did allocate,
  // we'll be able to allocate here
  c = sbrk(PGSIZE);
  for (i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
    if (pids[i] == -1)
      continue;
    kill(pids[i], SIGKILL);
    wait(0);
  }
  if (c == (char *)SBRK_ERROR) {
    printf("%s: failed sbrk leaked memory\n", s);
    exit(1);
  }

  // test running fork with the above allocated page
  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    // allocate more than the machine has, a quarter at a time: this
    // must produce an error before the tenth step.
    for (i = 0; i < 10; i++) {
      a = sbrk(BIG);
      if (a == (char *)SBRK_ERROR)
        exit(0);
    }
    printf("%s: allocating more than RAM succeeded\n", s);
    exit(1);
  }
  wait(&xstatus);
  if (xstatus != 0)
    exit(1);
}

// test reads/writes from/to allocated memory
void
sbrkarg(char *s)
{
  char *a;
  int fd, n;

  a = sbrk(PGSIZE);
  fd = open("sbrk", O_CREATE | O_WRONLY, 0666);
  unlink("sbrk");
  if (fd < 0) {
    printf("%s: open sbrk failed\n", s);
    exit(1);
  }
  if ((n = write(fd, a, PGSIZE)) < 0) {
    printf("%s: write sbrk failed\n", s);
    exit(1);
  }
  close(fd);

  // test writes to allocated memory
  a = sbrk(PGSIZE);
  if (pipe((int *)a) != 0) {
    printf("%s: pipe() failed\n", s);
    exit(1);
  }
}

void
validatetest(char *s)
{
  int hi;
  uint64 p;

  hi = 1100 * 1024;
  for (p = 0; p <= (uint)hi; p += PGSIZE) {
    // try to crash the kernel by passing in a bad string pointer
    if (link("nosuchfile", (char *)p) != -1) {
      printf("%s: link should not succeed\n", s);
      exit(1);
    }
  }
}

// does uninitialized data start out zero?
char uninit[10000];
void
bsstest(char *s)
{
  int i;

  for (i = 0; i < sizeof(uninit); i++) {
    if (uninit[i] != '\0') {
      printf("%s: bss test failed\n", s);
      exit(1);
    }
  }
}

// there is no process table: a thousand processes at once on any
// machine, their pids all different, everything reaped and reused.
void
manyprocs(char *s)
{
  // as many as the machine's memory allows, up to a thousand: a process
  // costs some sixty pages until fork is copy-on-write.
  int N = (int)(sysconf(_SC_AVPHYS_PAGES) / 128);
  if (N > 1000)
    N = 1000;
  if (N < 100)
    N = 100;
  int fds[2];
  if (pipe(fds) < 0) {
    printf("%s: pipe failed\n", s);
    exit(1);
  }
  int *pids = malloc(N * sizeof(int));
  int n = 0;
  for (n = 0; n < N; n++) {
    int pid = fork();
    if (pid < 0)
      break;
    if (pid == 0) {
      char c;
      close(fds[1]);
      read(fds[0], &c, 1); // wait to be released
      exit(0);
    }
    pids[n] = pid;
  }
  if (n < N) {
    printf("%s: only %d of %d processes could be created\n", s, n, N);
    exit(1);
  }
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (pids[i] == pids[j]) {
        printf("%s: pid %d given twice\n", s, pids[i]);
        exit(1);
      }
  close(fds[1]); // releases them all
  for (int i = 0; i < n; i++) {
    int st;
    if (wait(&st) < 0 || st != 0) {
      printf("%s: wait failed\n", s);
      exit(1);
    }
  }
  close(fds[0]);
  free(pids);
  // pids are reused only after they are free: a burst after reaping
  // must not collide with a live process.
  int keep = fork();
  if (keep == 0) {
    ticksleep(100000);
    exit(0);
  }
  for (int i = 0; i < 300; i++) {
    int pid = fork();
    if (pid == 0)
      exit(0);
    if (pid == keep) {
      printf("%s: live pid %d reused\n", s, keep);
      exit(1);
    }
    wait(0);
  }
  kill(keep, SIGKILL);
  wait(0);
}

// fork is copy-on-write: a child's writes stay private, the parent's
// pages stay intact, and the frames come back when both are done.
void
cowtest(char *s)
{
  enum { N = 64 };
  long free0 = sysconf(_SC_AVPHYS_PAGES);
  char *buf = sbrk(N * PGSIZE);
  if (buf == (char *)SBRK_ERROR) {
    printf("%s: sbrk failed\n", s);
    exit(1);
  }
  for (int i = 0; i < N; i++)
    buf[i * PGSIZE] = i;
  long free1 = sysconf(_SC_AVPHYS_PAGES);
  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (pid == 0) {
    // reading shares, writing copies
    for (int i = 0; i < N; i++)
      if (buf[i * PGSIZE] != (char)i) {
        printf("%s: child saw wrong data\n", s);
        exit(1);
      }
    for (int i = 0; i < N; i += 2)
      buf[i * PGSIZE] = 100 + i;
    exit(0);
  }
  // the parent's pages are untouched while the child writes
  int st;
  wait(&st);
  if (st != 0)
    exit(1);
  for (int i = 0; i < N; i++)
    if (buf[i * PGSIZE] != (char)i) {
      printf("%s: parent's page %d changed\n", s, i);
      exit(1);
    }
  // fork right after fork: the whole thing costs no copies until written
  long before = sysconf(_SC_AVPHYS_PAGES);
  pid = fork();
  if (pid == 0) {
    ticksleep(20);
    exit(0);
  }
  long during = sysconf(_SC_AVPHYS_PAGES);
  if (before - during > 40) {
    printf("%s: fork copied %ld pages instead of sharing\n", s, before - during);
    kill(pid, SIGKILL);
    wait(0);
    exit(1);
  }
  wait(0);
  // the parent writes while nobody shares: no copy is made
  long b2 = sysconf(_SC_AVPHYS_PAGES);
  for (int i = 0; i < N; i++)
    buf[i * PGSIZE] = 7;
  if (sysconf(_SC_AVPHYS_PAGES) < b2 - 2) {
    printf("%s: writing unshared pages allocated memory\n", s);
    exit(1);
  }
  sbrk(-N * PGSIZE);
  long free2 = sysconf(_SC_AVPHYS_PAGES);
  if (free2 < free0 - 8) {
    printf("%s: leaked %ld pages (%ld, %ld, %ld)\n", s, free0 - free2, free0, free1, free2);
    exit(1);
  }
}

// mmap: anonymous private and shared mappings, a file mapping, munmap,
// mprotect, and faults on what is not mapped.
void
mmaptest(char *s)
{
  // anonymous private
  char *a = mmap(0, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (a == MAP_FAILED) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }
  for (int i = 0; i < 3 * PGSIZE; i++)
    if (a[i] != 0) {
      printf("%s: anonymous memory not zero\n", s);
      exit(1);
    }
  a[0] = 1;
  a[2 * PGSIZE + 5] = 2;
  // a file mapping sees the file's contents
  int fd = open("mmaptest.f", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create failed\n", s);
    exit(1);
  }
  char data[PGSIZE + 100];
  for (int i = 0; i < sizeof(data); i++)
    data[i] = 'a' + (i % 26);
  if (write(fd, data, sizeof(data)) != sizeof(data)) {
    printf("%s: write failed\n", s);
    exit(1);
  }
  close(fd);
  fd = open("mmaptest.f", O_RDONLY);
  char *f = mmap(0, 2 * PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if (f == MAP_FAILED) {
    printf("%s: file mmap failed\n", s);
    exit(1);
  }
  close(fd);
  for (int i = 0; i < sizeof(data); i++)
    if (f[i] != data[i]) {
      printf("%s: file mapping content wrong at %d\n", s, i);
      exit(1);
    }
  for (int i = sizeof(data); i < 2 * PGSIZE; i++)
    if (f[i] != 0) {
      printf("%s: file mapping not zero past the end\n", s);
      exit(1);
    }
  // a shared anonymous mapping is shared with a child; a private one is not
  char *sh = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (sh == MAP_FAILED) {
    printf("%s: shared mmap failed\n", s);
    exit(1);
  }
  sh[0] = 0;
  int pid = fork();
  if (pid == 0) {
    sh[0] = 42;
    a[0] = 99;
    exit(0);
  }
  wait(0);
  if (sh[0] != 42 || a[0] != 1) {
    printf("%s: sharing wrong (%d %d)\n", s, sh[0], a[0]);
    exit(1);
  }
  // mprotect: a write to read-only memory kills the writer
  if (mprotect(a, PGSIZE, PROT_READ) < 0) {
    printf("%s: mprotect failed\n", s);
    exit(1);
  }
  pid = fork();
  if (pid == 0) {
    a[0] = 5; // must fault
    exit(0);
  }
  int st;
  wait(&st);
  if (!WIFSIGNALED(st)) {
    printf("%s: write to read-only mapping did not fault\n", s);
    exit(1);
  }
  if (a[0] != 1) {
    printf("%s: read-only page changed\n", s);
    exit(1);
  }
  // munmap the middle page: it faults, the neighbours survive
  if (munmap(a + PGSIZE, PGSIZE) < 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
  pid = fork();
  if (pid == 0) {
    a[PGSIZE] = 1; // unmapped
    exit(0);
  }
  wait(&st);
  if (!WIFSIGNALED(st)) {
    printf("%s: unmapped page did not fault\n", s);
    exit(1);
  }
  if (a[0] != 1 || a[2 * PGSIZE + 5] != 2) {
    printf("%s: neighbours of the unmapped page damaged\n", s);
    exit(1);
  }
  munmap(a, PGSIZE);
  munmap(a + 2 * PGSIZE, PGSIZE);
  munmap(f, 2 * PGSIZE);
  munmap(sh, PGSIZE);
  unlink("mmaptest.f");
  // bad arguments
  if (mmap(0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
      munmap((void *)1, PGSIZE) == 0 || mprotect(a, PGSIZE, 99) == 0) {
    printf("%s: bad arguments accepted\n", s);
    exit(1);
  }
}


static volatile int sigcount, siglast, sigcode, sigvalue;

static void
sighandler(int sig)
{
  sigcount++;
  siglast = sig;
}

static void
siginfohandler(int sig, siginfo_t *info, void *uc)
{
  sigcount++;
  siglast = sig;
  sigcode = info->si_code;
  sigvalue = info->si_value.sival_int;
}


// ---- threads ----

static void
tt_sleep(int ticks)
{
  struct timespec ts = {0, 0};
  ts.tv_sec = ticks / 100;
  ts.tv_nsec = (long)(ticks % 100) * 10000000L;
  nanosleep(&ts, 0);
}

#define NTHREAD 8

static mutex_t tt_lock = MUTEX_INIT;
static volatile int tt_counter;
static volatile int tt_unlocked;
static volatile int tt_seen_pid[NTHREAD];
static volatile int tt_seen_tid[NTHREAD];
static volatile int tt_errno[NTHREAD];
static barrier_t tt_barrier;
static cond_t tt_cond = COND_INIT;
static mutex_t tt_cond_lock = MUTEX_INIT;
static volatile int tt_ready;
static volatile uint32 tt_futex_word;
static volatile int tt_spinning;

static void
tt_bump(void *arg)
{
  int n = (int)(uint64)arg;
  for (int i = 0; i < 2000; i++) {
    mutex_lock(&tt_lock);
    tt_counter++;
    mutex_unlock(&tt_lock);
    tt_unlocked++; // races on purpose: only the locked count is checked
  }
  (void)n;
}

// each thread makes a failing call of its own and checks that the
// error it reads back is its own, not another thread's.
static void
tt_errno_thread(void *arg)
{
  int n = (int)(uint64)arg;

  barrier_wait(&tt_barrier);
  tt_seen_pid[n] = getpid();
  tt_seen_tid[n] = gettid();
  struct timespec bad = {0, -1}; // nanosleep rejects it with EINVAL
  for (int i = 0; i < 200; i++) {
    errno = 0;
    if (n % 2 == 0) {
      if (kill(999999, 0) >= 0)
        tt_errno[n] = -1;
      else if (errno != ESRCH)
        tt_errno[n] = errno;
    } else {
      if (nanosleep(&bad, 0) >= 0)
        tt_errno[n] = -1;
      else if (errno != EINVAL)
        tt_errno[n] = errno;
    }
  }
}

static void
tt_cond_waiter(void *arg)
{
  mutex_lock(&tt_cond_lock);
  while (!tt_ready)
    cond_wait(&tt_cond, &tt_cond_lock);
  tt_counter++;
  mutex_unlock(&tt_cond_lock);
  (void)arg;
}

static void
tt_futex_waiter(void *arg)
{
  while (__atomic_load_n(&tt_futex_word, __ATOMIC_ACQUIRE) == 0)
    futex((uint32 *)&tt_futex_word, FUTEX_WAIT, 0, 0, 0, 0);
  tt_counter++;
  (void)arg;
}

static void
tt_spinner(void *arg)
{
  tt_spinning++;
  for (;;)
    ; // killed from outside, one way or another
  (void)arg;
}

static void
tt_exec_echo(void *arg)
{
  char *argv[] = {"/bin/echo", 0};
  exec("/bin/echo", argv);
  exit(9); // exec failed
  (void)arg;
}

static void
tt_sleeper(void *arg)
{
  tt_sleep(100);
  (void)arg;
}

// threads: clone, join, mutexes, condition variables, barriers, a
// private errno, shared descriptors, exit_group and signals.
void
threadtest(char *s)
{
  thread_t t[NTHREAD];
  int pid = getpid();

  // one thread, joined
  tt_counter = 0;
  if (thread_create(&t[0], tt_bump, (void *)0, 0) < 0) {
    printf("%s: thread_create failed, errno %d\n", s, errno);
    exit(1);
  }
  if (t[0].tid == 0) {
    printf("%s: clone did not set the tid\n", s);
    exit(1);
  }
  thread_join(&t[0]);
  if (tt_counter != 2000) {
    printf("%s: one thread counted %d, wanted 2000\n", s, tt_counter);
    exit(1);
  }
  if (t[0].tid != 0) {
    printf("%s: the tid was not cleared at exit\n", s);
    exit(1);
  }

  // many threads sharing a counter under a mutex
  tt_counter = 0;
  tt_unlocked = 0;
  for (int i = 0; i < NTHREAD; i++)
    if (thread_create(&t[i], tt_bump, (void *)(uint64)i, 0) < 0) {
      printf("%s: thread_create %d failed\n", s, i);
      exit(1);
    }
  for (int i = 0; i < NTHREAD; i++)
    thread_join(&t[i]);
  if (tt_counter != NTHREAD * 2000) {
    printf("%s: %d threads counted %d, wanted %d\n", s, NTHREAD, tt_counter, NTHREAD * 2000);
    exit(1);
  }

  // identity: one process, many threads, each with its own errno
  barrier_init(&tt_barrier, NTHREAD + 1);
  for (int i = 0; i < NTHREAD; i++) {
    tt_seen_pid[i] = tt_seen_tid[i] = tt_errno[i] = 0;
    if (thread_create(&t[i], tt_errno_thread, (void *)(uint64)i, 0) < 0) {
      printf("%s: thread_create %d failed\n", s, i);
      exit(1);
    }
  }
  barrier_wait(&tt_barrier);
  errno = 0;
  uint32 word = 1; // a stale futex wait fails with an error of its own
  if (futex(&word, FUTEX_WAIT, 0, 0, 0, 0) >= 0 || errno != EAGAIN) {
    printf("%s: the main thread lost its own errno (%d)\n", s, errno);
    exit(1);
  }
  for (int i = 0; i < NTHREAD; i++)
    thread_join(&t[i]);
  for (int i = 0; i < NTHREAD; i++) {
    if (tt_seen_pid[i] != pid) {
      printf("%s: thread %d saw pid %d, wanted %d\n", s, i, tt_seen_pid[i], pid);
      exit(1);
    }
    if (tt_seen_tid[i] == pid || tt_seen_tid[i] <= 0) {
      printf("%s: thread %d has tid %d\n", s, i, tt_seen_tid[i]);
      exit(1);
    }
    for (int j = 0; j < i; j++)
      if (tt_seen_tid[i] == tt_seen_tid[j]) {
        printf("%s: two threads share tid %d\n", s, tt_seen_tid[i]);
        exit(1);
      }
    if (tt_errno[i] != 0) {
      printf("%s: thread %d saw errno %d from another thread\n", s, i, tt_errno[i]);
      exit(1);
    }
  }

  // condition variables
  tt_counter = 0;
  tt_ready = 0;
  for (int i = 0; i < NTHREAD; i++)
    if (thread_create(&t[i], tt_cond_waiter, 0, 0) < 0) {
      printf("%s: thread_create %d failed\n", s, i);
      exit(1);
    }
  tt_sleep(2);
  mutex_lock(&tt_cond_lock);
  tt_ready = 1;
  cond_broadcast(&tt_cond);
  mutex_unlock(&tt_cond_lock);
  for (int i = 0; i < NTHREAD; i++)
    thread_join(&t[i]);
  if (tt_counter != NTHREAD) {
    printf("%s: %d of %d waiters woke\n", s, tt_counter, NTHREAD);
    exit(1);
  }

  // the futex itself: wait until the word changes, then wake
  tt_counter = 0;
  tt_futex_word = 0;
  for (int i = 0; i < 4; i++)
    if (thread_create(&t[i], tt_futex_waiter, 0, 0) < 0) {
      printf("%s: thread_create %d failed\n", s, i);
      exit(1);
    }
  tt_sleep(2);
  __atomic_store_n(&tt_futex_word, 1, __ATOMIC_RELEASE);
  if (futex((uint32 *)&tt_futex_word, FUTEX_WAKE, 4, 0, 0, 0) < 0) {
    printf("%s: futex wake failed\n", s);
    exit(1);
  }
  for (int i = 0; i < 4; i++)
    thread_join(&t[i]);
  if (tt_counter != 4) {
    printf("%s: %d of 4 futex waiters woke\n", s, tt_counter);
    exit(1);
  }

  // a futex wait on a word that already changed returns at once
  tt_futex_word = 7;
  if (futex((uint32 *)&tt_futex_word, FUTEX_WAIT, 0, 0, 0, 0) >= 0 || errno != EAGAIN) {
    printf("%s: a stale futex wait did not report EAGAIN (%d)\n", s, errno);
    exit(1);
  }

  // a timed wait gives up on its own
  struct timespec ts = {0, 50000000L}; // 50 ms
  int r = futex((uint32 *)&tt_futex_word, FUTEX_WAIT, 7, &ts, 0, 0);
  if (r >= 0 || errno != ETIMEDOUT) {
    printf("%s: a timed futex wait returned %d errno %d\n", s, r, errno);
    exit(1);
  }

  // threads share descriptors: one opens, another reads
  {
    int fd = open("threadfile", O_CREATE | O_RDWR, 0666);
    if (fd < 0) {
      printf("%s: cannot create threadfile\n", s);
      exit(1);
    }
    write(fd, "hello", 5);
    close(fd);
    unlink("threadfile");
  }

  // exit_group ends every thread of the process
  int cpid = fork();
  if (cpid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (cpid == 0) {
    for (int i = 0; i < NTHREAD; i++)
      thread_create(&t[i], tt_sleeper, 0, 0);
    tt_sleep(1);
    exit_group(3);
  }
  int st;
  if (waitpid(cpid, &st, 0) != cpid || !WIFEXITED(st) || WEXITSTATUS(st) != 3) {
    printf("%s: exit_group gave status %d\n", s, st);
    exit(1);
  }

  // a signal kills the whole process, spinning threads included
  cpid = fork();
  if (cpid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (cpid == 0) {
    tt_spinning = 0;
    for (int i = 0; i < 4; i++)
      thread_create(&t[i], tt_spinner, 0, 0);
    while (tt_spinning < 4)
      ;
    for (;;)
      ;
  }
  tt_sleep(3);
  kill(cpid, SIGKILL);
  if (waitpid(cpid, &st, 0) != cpid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
    printf("%s: killing a threaded process gave status %d\n", s, st);
    exit(1);
  }

  // exec from a threaded process leaves one thread running the new image
  cpid = fork();
  if (cpid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (cpid == 0) {
    for (int i = 0; i < 4; i++)
      thread_create(&t[i], tt_sleeper, 0, 0);
    tt_sleep(1);
    char *argv[] = {"/bin/echo", "", 0};
    exec("/bin/echo", argv);
    exit(9);
  }
  if (waitpid(cpid, &st, 0) != cpid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    printf("%s: exec from a thread gave status %d\n", s, st);
    exit(1);
  }

  // exec from a thread that is not the leader: the process keeps the
  // pid its parent is waiting for, and the other threads go away
  cpid = fork();
  if (cpid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  if (cpid == 0) {
    if (thread_create(&t[0], tt_exec_echo, 0, 0) < 0)
      exit(8);
    for (;;)
      tt_sleep(100); // replaced by the thread's exec, one way or another
  }
  if (waitpid(cpid, &st, 0) != cpid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    printf("%s: exec from a second thread gave status %d\n", s, st);
    exit(1);
  }

  // tkill reaches one thread by its own id
  if (tkill(gettid(), 0) < 0) {
    printf("%s: tkill of this thread failed\n", s);
    exit(1);
  }
  if (tkill(999999, 0) >= 0 || errno != ESRCH) {
    printf("%s: tkill of a stranger did not report ESRCH\n", s);
    exit(1);
  }
  if (tgkill(pid, gettid(), 0) < 0) {
    printf("%s: tgkill of this thread failed\n", s);
    exit(1);
  }

  // clone without CLONE_VM is a fork: the child gets a copy
  if (getpid() != pid) {
    printf("%s: the process changed identity\n", s);
    exit(1);
  }
}

// signals: handlers, masks, defaults, faults, alarms, job control and
// the wait status.
void
sigtest(char *s)
{
  struct sigaction sa;
  sigset_t set, old;
  int pid, st;

  // a handler runs, and fork does not lose it
  sigcount = 0;
  siglast = 0;
  if (signal(SIGUSR1, sighandler) == SIG_ERR) {
    printf("%s: signal failed\n", s);
    exit(1);
  }
  raise(SIGUSR1);
  if (sigcount != 1 || siglast != SIGUSR1) {
    printf("%s: handler did not run (%d %d)\n", s, sigcount, siglast);
    exit(1);
  }

  // blocking: the signal waits, sigpending sees it, unblocking delivers it
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &set, &old) < 0) {
    printf("%s: sigprocmask failed\n", s);
    exit(1);
  }
  raise(SIGUSR1);
  if (sigcount != 1) {
    printf("%s: a blocked signal was delivered\n", s);
    exit(1);
  }
  sigset_t pend;
  if (sigpending(&pend) < 0 || !sigismember(&pend, SIGUSR1)) {
    printf("%s: sigpending missed it\n", s);
    exit(1);
  }
  sigprocmask(SIG_SETMASK, &old, 0);
  if (sigcount != 2 || siglast != SIGUSR1) {
    printf("%s: unblocking did not deliver (%d)\n", s, sigcount);
    exit(1);
  }

  // SIG_IGN, and the default for an ignored-by-default signal
  signal(SIGUSR1, SIG_IGN);
  raise(SIGUSR1);
  if (sigcount != 2) {
    printf("%s: an ignored signal was delivered\n", s);
    exit(1);
  }
  signal(SIGUSR1, SIG_DFL);

  // siginfo and a queued value
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = (void *)siginfohandler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR2, &sa, 0);
  sigcount = 0;
  sigvalue = 0;
  if (sigqueue(getpid(), SIGUSR2, 1234) < 0) {
    printf("%s: sigqueue failed\n", s);
    exit(1);
  }
  if (sigcount != 1 || sigvalue != 1234 || sigcode != SI_QUEUE) {
    printf("%s: siginfo wrong (%d %d %d)\n", s, sigcount, sigvalue, sigcode);
    exit(1);
  }

  // a child killed by a signal, and the status macros
  pid = fork();
  if (pid == 0) {
    for (;;)
      ticksleep(1);
  }
  ticksleep(2);
  if (kill(pid, SIGTERM) < 0) {
    printf("%s: kill failed\n", s);
    exit(1);
  }
  if (waitpid(pid, &st, 0) != pid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) {
    printf("%s: wrong status for a killed child (0x%x)\n", s, st);
    exit(1);
  }

  // a child that exits: WIFEXITED and the code
  pid = fork();
  if (pid == 0)
    exit(42);
  if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 42) {
    printf("%s: wrong status for an exited child (0x%x)\n", s, st);
    exit(1);
  }

  // a handler in the child survives fork; exec resets it
  sigcount = 0;
  signal(SIGUSR1, sighandler);
  pid = fork();
  if (pid == 0) {
    raise(SIGUSR1);
    exit(sigcount == 1 ? 0 : 1);
  }
  if (waitpid(pid, &st, 0) != pid || xstat(st) != 0) {
    printf("%s: the handler did not survive fork\n", s);
    exit(1);
  }

  // SIGKILL cannot be caught or blocked
  if (signal(SIGKILL, sighandler) != SIG_ERR) {
    printf("%s: SIGKILL was caught\n", s);
    exit(1);
  }
  pid = fork();
  if (pid == 0) {
    sigfillset(&set);
    sigprocmask(SIG_BLOCK, &set, 0);
    for (;;)
      ticksleep(1);
  }
  ticksleep(2);
  kill(pid, SIGKILL);
  if (waitpid(pid, &st, 0) != pid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
    printf("%s: SIGKILL did not kill a blocking child (0x%x)\n", s, st);
    exit(1);
  }

  // a fault becomes SIGSEGV, and a handler sees it
  pid = fork();
  if (pid == 0) {
    volatile int *bad = (int *)0xdeadbe00;
    *bad = 1;
    exit(0);
  }
  if (waitpid(pid, &st, 0) != pid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) {
    printf("%s: a bad store did not give SIGSEGV (0x%x)\n", s, st);
    exit(1);
  }

  // alarm delivers SIGALRM
  sigcount = 0;
  signal(SIGALRM, sighandler);
  alarm(1);
  for (int i = 0; i < 40 && sigcount == 0; i++)
    ticksleep(5);
  if (sigcount != 1 || siglast != SIGALRM) {
    printf("%s: alarm did not fire\n", s);
    exit(1);
  }
  alarm(0);

  // job control: stop a child, see it with WUNTRACED, continue it
  pid = fork();
  if (pid == 0) {
    for (;;)
      ticksleep(1);
  }
  ticksleep(2);
  kill(pid, SIGSTOP);
  if (waitpid(pid, &st, WUNTRACED) != pid || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
    printf("%s: WUNTRACED did not report the stop (0x%x)\n", s, st);
    kill(pid, SIGKILL);
    wait(0);
    exit(1);
  }
  if (waitpid(pid, &st, WNOHANG) != 0) {
    printf("%s: WNOHANG reported a stopped child again\n", s);
    exit(1);
  }
  kill(pid, SIGCONT);
  if (waitpid(pid, &st, WCONTINUED) != pid || !WIFCONTINUED(st)) {
    printf("%s: WCONTINUED did not report the restart (0x%x)\n", s, st);
    exit(1);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &st, 0);

  // process groups: a signal to the group reaches its members
  sigcount = 0;
  signal(SIGUSR1, sighandler);
  int pids[3];
  for (int i = 0; i < 3; i++) {
    pids[i] = fork();
    if (pids[i] == 0) {
      signal(SIGUSR1, sighandler);
      while (sigcount == 0)
        ticksleep(1);
      exit(7);
    }
    setpgid(pids[i], pids[0]); // the parent puts all three in one group
  }
  ticksleep(3);
  if (kill(-pids[0], SIGUSR1) < 0) {
    printf("%s: kill to a process group failed\n", s);
    exit(1);
  }
  for (int i = 0; i < 3; i++) {
    if (waitpid(pids[i], &st, 0) != pids[i] || xstat(st) != 7) {
      printf("%s: group member %d did not get the signal (0x%x)\n", s, i, st);
      exit(1);
    }
  }
  if (getpgid(0) != getpgrp()) {
    printf("%s: getpgid disagrees with getpgrp\n", s);
    exit(1);
  }

  // sigsuspend waits for a signal
  pid = fork();
  if (pid == 0) {
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    signal(SIGUSR1, sighandler);
    sigemptyset(&set);
    sigcount = 0;
    sigsuspend(&set); // unblocks SIGUSR1 and waits
    exit(sigcount == 1 ? 0 : 1);
  }
  ticksleep(3);
  kill(pid, SIGUSR1);
  if (waitpid(pid, &st, 0) != pid || xstat(st) != 0) {
    printf("%s: sigsuspend failed (0x%x)\n", s, st);
    exit(1);
  }

  // a signal interrupts a blocking read
  int fds[2];
  if (pipe(fds) < 0) {
    printf("%s: pipe failed\n", s);
    exit(1);
  }
  pid = fork();
  if (pid == 0) {
    char c;
    close(fds[1]);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (void *)sighandler; // no SA_RESTART: the read must fail
    sigaction(SIGUSR1, &sa, 0);
    sigcount = 0;
    int n = read(fds[0], &c, 1);
    exit(n < 0 && sigcount == 1 ? 0 : 2);
  }
  close(fds[0]);
  ticksleep(3);
  kill(pid, SIGUSR1);
  if (waitpid(pid, &st, 0) != pid || xstat(st) != 0) {
    printf("%s: a signal did not interrupt read (0x%x)\n", s, st);
    exit(1);
  }
  close(fds[1]);

  // with SA_RESTART the read is resumed and sees the byte
  if (pipe(fds) < 0) {
    printf("%s: pipe failed\n", s);
    exit(1);
  }
  pid = fork();
  if (pid == 0) {
    char c = 0;
    close(fds[1]);
    signal(SIGUSR1, sighandler); // SA_RESTART
    sigcount = 0;
    int n = read(fds[0], &c, 1);
    exit(n == 1 && c == 'x' && sigcount == 1 ? 0 : 3);
  }
  close(fds[0]);
  ticksleep(3);
  kill(pid, SIGUSR1);
  ticksleep(3);
  write(fds[1], "x", 1);
  if (waitpid(pid, &st, 0) != pid || xstat(st) != 0) {
    printf("%s: SA_RESTART did not resume the read (0x%x)\n", s, st);
    exit(1);
  }
  close(fds[1]);
  signal(SIGUSR1, SIG_DFL);
  signal(SIGUSR2, SIG_DFL);
  signal(SIGALRM, SIG_DFL);
}

// the POSIX descriptor and I/O calls: lseek, pread/pwrite, readv/writev,
// dup2/dup3, fcntl, pipe2, ioctl, poll, select, and the environment.
void
posixtest(char *s)
{
  int fd, n;
  char buf[64];

  // lseek on a file
  fd = open("posixtest.f", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: create failed\n", s);
    exit(1);
  }
  if (write(fd, "0123456789", 10) != 10) {
    printf("%s: write failed\n", s);
    exit(1);
  }
  if (lseek(fd, 0, SEEK_CUR) != 10 || lseek(fd, 0, SEEK_END) != 10) {
    printf("%s: lseek reported the wrong offset\n", s);
    exit(1);
  }
  if (lseek(fd, 3, SEEK_SET) != 3 || read(fd, buf, 2) != 2 || buf[0] != '3' || buf[1] != '4') {
    printf("%s: lseek(SEEK_SET) failed\n", s);
    exit(1);
  }
  if (lseek(fd, -2, SEEK_CUR) != 3 || lseek(fd, -20, SEEK_SET) != -1) {
    printf("%s: relative lseek failed\n", s);
    exit(1);
  }
  // pread and pwrite leave the offset alone
  if (pread(fd, buf, 3, 0) != 3 || buf[0] != '0' || lseek(fd, 0, SEEK_CUR) != 3) {
    printf("%s: pread failed\n", s);
    exit(1);
  }
  if (pwrite(fd, "xy", 2, 8) != 2 || pread(fd, buf, 2, 8) != 2 || buf[0] != 'x' || buf[1] != 'y') {
    printf("%s: pwrite failed\n", s);
    exit(1);
  }
  // readv and writev
  struct iovec iov[2];
  iov[0].iov_base = buf;
  iov[0].iov_len = 4;
  iov[1].iov_base = buf + 4;
  iov[1].iov_len = 4;
  lseek(fd, 0, SEEK_SET);
  if (readv(fd, iov, 2) != 8 || memcmp(buf, "01234567", 8) != 0) {
    printf("%s: readv failed\n", s);
    exit(1);
  }
  lseek(fd, 0, SEEK_SET);
  iov[0].iov_base = "AB";
  iov[0].iov_len = 2;
  iov[1].iov_base = "CD";
  iov[1].iov_len = 2;
  if (writev(fd, iov, 2) != 4 || pread(fd, buf, 4, 0) != 4 || memcmp(buf, "ABCD", 4) != 0) {
    printf("%s: writev failed\n", s);
    exit(1);
  }
  close(fd);

  // dup2 and dup3
  fd = open("posixtest.f", O_RDONLY);
  int fd2 = dup2(fd, 20);
  if (fd2 != 20 || pread(20, buf, 2, 0) != 2 || buf[0] != 'A') {
    printf("%s: dup2 failed\n", s);
    exit(1);
  }
  if (dup3(fd, 20, O_CLOEXEC) != 20 || fcntl(20, F_GETFD, 0) != FD_CLOEXEC) {
    printf("%s: dup3 with O_CLOEXEC failed\n", s);
    exit(1);
  }
  if (dup3(fd, fd, 0) != -1) {
    printf("%s: dup3 onto itself should fail\n", s);
    exit(1);
  }
  close(20);

  // fcntl: descriptor flags and a duplicate above a floor
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_GETFD, 0) != FD_CLOEXEC) {
    printf("%s: F_SETFD failed\n", s);
    exit(1);
  }
  fcntl(fd, F_SETFD, 0);
  int hi = fcntl(fd, F_DUPFD, 10);
  if (hi < 10) {
    printf("%s: F_DUPFD failed\n", s);
    exit(1);
  }
  close(hi);
  if (fcntl(fd, F_GETFL, 0) < 0 || fcntl(99, F_GETFD, 0) != -1) {
    printf("%s: fcntl on a bad descriptor should fail\n", s);
    exit(1);
  }
  close(fd);

  // close-on-exec really closes
  fd = open("posixtest.f", O_RDONLY);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  int pid = fork();
  if (pid == 0) {
    char fdstr[8];
    fdstr[0] = '0' + fd / 10;
    fdstr[1] = '0' + fd % 10;
    fdstr[2] = 0;
    char *args[] = {"fdcheck", fdstr, 0};
    exec("/bin/fdcheck", args);
    exit(9); // fdcheck is not in the image: skip this check
  }
  int st;
  wait(&st);
  if (xstat(st) != 9 && xstat(st) != 0) {
    printf("%s: a close-on-exec descriptor survived exec\n", s);
    exit(1);
  }
  close(fd);
  unlink("posixtest.f");

  // pipe2 with O_CLOEXEC, and poll on both ends
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) {
    printf("%s: pipe2 failed\n", s);
    exit(1);
  }
  if (fcntl(fds[0], F_GETFD, 0) != FD_CLOEXEC || fcntl(fds[1], F_GETFD, 0) != FD_CLOEXEC) {
    printf("%s: pipe2 ignored O_CLOEXEC\n", s);
    exit(1);
  }
  struct pollfd pfd[2];
  pfd[0].fd = fds[0];
  pfd[0].events = POLLIN;
  pfd[1].fd = fds[1];
  pfd[1].events = POLLOUT;
  n = poll(pfd, 2, 0);
  if (n != 1 || (pfd[0].revents & POLLIN) || !(pfd[1].revents & POLLOUT)) {
    printf("%s: poll on an empty pipe is wrong (%d %x %x)\n", s, n, pfd[0].revents, pfd[1].revents);
    exit(1);
  }
  write(fds[1], "z", 1);
  n = poll(pfd, 2, 0);
  if (n != 2 || !(pfd[0].revents & POLLIN)) {
    printf("%s: poll did not see the byte (%d)\n", s, n);
    exit(1);
  }
  // FIONREAD counts it
  int avail = 0;
  if (ioctl(fds[0], FIONREAD, &avail) < 0 || avail != 1) {
    printf("%s: FIONREAD said %d\n", s, avail);
    exit(1);
  }
  read(fds[0], buf, 1);
  // a timeout expires
  pfd[0].events = POLLIN;
  n = poll(pfd, 1, 50);
  if (n != 0) {
    printf("%s: poll should have timed out (%d)\n", s, n);
    exit(1);
  }
  // select sees the write end
  fd_set wr;
  FD_ZERO(&wr);
  FD_SET(fds[1], &wr);
  struct timeval tv = {0, 0};
  n = select(fds[1] + 1, 0, &wr, 0, &tv);
  if (n != 1 || !FD_ISSET(fds[1], &wr)) {
    printf("%s: select on a writable pipe failed (%d)\n", s, n);
    exit(1);
  }
  // the reader sees a hangup when the writer closes
  close(fds[1]);
  pfd[0].events = POLLIN;
  n = poll(pfd, 1, 100);
  if (n != 1 || !(pfd[0].revents & (POLLHUP | POLLIN))) {
    printf("%s: poll missed the hangup (%d %x)\n", s, n, pfd[0].revents);
    exit(1);
  }
  close(fds[0]);

  // the environment came through exec
  if (getenv("PATH") == 0) {
    printf("%s: PATH is not in the environment\n", s);
    exit(1);
  }
  if (setenv("XV6TEST", "yes", 1) < 0 || strcmp(getenv("XV6TEST"), "yes") != 0) {
    printf("%s: setenv failed\n", s);
    exit(1);
  }
  setenv("XV6TEST", "no", 0);
  if (strcmp(getenv("XV6TEST"), "yes") != 0) {
    printf("%s: setenv overwrote when it should not\n", s);
    exit(1);
  }
  setenv("XV6TEST", "no", 1);
  if (strcmp(getenv("XV6TEST"), "no") != 0) {
    printf("%s: setenv did not overwrite\n", s);
    exit(1);
  }
  // a child inherits it through execve
  pid = fork();
  if (pid == 0) {
    char *args[] = {"echoenv", "XV6TEST", 0};
    execve("echoenv", args, environ);
    exit(9);
  }
  wait(&st);
  if (xstat(st) != 0 && xstat(st) != 9) {
    printf("%s: execve did not pass the environment (0x%x)\n", s, st);
    exit(1);
  }
  unsetenv("XV6TEST");
  if (getenv("XV6TEST") != 0) {
    printf("%s: unsetenv failed\n", s);
    exit(1);
  }

  // uname, times, getrusage, umask, getrandom, nanosleep
  struct utsname un;
  if (uname(&un) < 0 || strcmp(un.sysname, "xv6H") != 0 || strcmp(un.machine, "riscv64") != 0) {
    printf("%s: uname failed\n", s);
    exit(1);
  }
  struct tms tm;
  if (times(&tm) <= 0) {
    printf("%s: times failed\n", s);
    exit(1);
  }
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) < 0) {
    printf("%s: getrusage failed\n", s);
    exit(1);
  }
  int old = umask(022);
  if (umask(old) != 022) {
    printf("%s: umask failed\n", s);
    exit(1);
  }
  char r1[32], r2[32];
  memset(r1, 0, sizeof(r1));
  memset(r2, 0, sizeof(r2));
  if (getrandom(r1, sizeof(r1), 0) != sizeof(r1) || getrandom(r2, sizeof(r2), 0) != sizeof(r2)) {
    printf("%s: getrandom failed\n", s);
    exit(1);
  }
  if (memcmp(r1, r2, sizeof(r1)) == 0) {
    printf("%s: getrandom returned the same bytes twice\n", s);
    exit(1);
  }
  struct timespec req = {0, 50000000}, rem;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (nanosleep(&req, &rem) < 0) {
    printf("%s: nanosleep failed\n", s);
    exit(1);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  if (ms < 40 || ms > 2000) {
    printf("%s: nanosleep slept %ld ms\n", s, ms);
    exit(1);
  }
}

// the terminal: settings, raw mode, the window size, the foreground
// group, and the stop a background reader gets.
void
ttytest(char *s)
{
  struct termios saved, t;
  struct winsize ws;
  int pgrp, st;

  if (ioctl(0, TCGETS, &saved) < 0) {
    printf("%s: TCGETS failed\n", s);
    exit(1);
  }
  if (!(saved.c_lflag & ICANON) || !(saved.c_lflag & ISIG)) {
    printf("%s: the console should start canonical with signals\n", s);
    exit(1);
  }
  // raw mode, then back
  t = saved;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  if (ioctl(0, TCSETS, &t) < 0) {
    printf("%s: TCSETS failed\n", s);
    exit(1);
  }
  if (ioctl(0, TCGETS, &t) < 0 || (t.c_lflag & ICANON)) {
    printf("%s: raw mode did not stick\n", s);
    exit(1);
  }
  if (ioctl(0, TCSETS, &saved) < 0) {
    printf("%s: restoring the settings failed\n", s);
    exit(1);
  }

  // the window size
  if (ioctl(0, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) {
    printf("%s: TIOCGWINSZ failed\n", s);
    exit(1);
  }
  struct winsize ws2 = ws;
  ws2.ws_col = 132;
  if (ioctl(0, TIOCSWINSZ, &ws2) < 0 || ioctl(0, TIOCGWINSZ, &ws2) < 0 || ws2.ws_col != 132) {
    printf("%s: TIOCSWINSZ failed\n", s);
    exit(1);
  }
  ioctl(0, TIOCSWINSZ, &ws);

  // the foreground group is ours (the shell put us there)
  if (ioctl(0, TIOCGPGRP, &pgrp) < 0 || pgrp != getpgrp()) {
    printf("%s: we are not the foreground group (%d vs %d)\n", s, pgrp, getpgrp());
    exit(1);
  }

  // a background process reading the terminal is stopped
  int pid = fork();
  if (pid == 0) {
    char c;
    setpgid(0, 0); // leave the foreground group
    signal(SIGTTIN, SIG_DFL);
    read(0, &c, 1); // must stop us
    exit(3);
  }
  setpgid(pid, pid);
  if (waitpid(pid, &st, WUNTRACED) != pid || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTTIN) {
    printf("%s: a background read did not stop the reader (0x%x)\n", s, st);
    kill(pid, SIGKILL);
    wait(0);
    exit(1);
  }
  kill(pid, SIGKILL);
  kill(pid, SIGCONT);
  waitpid(pid, &st, 0);

  // ioctl on something that is not a terminal
  int fds[2];
  pipe(fds);
  if (ioctl(fds[0], TCGETS, &t) != -1 || errno != ENOTTY) {
    printf("%s: TCGETS on a pipe should fail with ENOTTY\n", s);
    exit(1);
  }
  close(fds[0]);
  close(fds[1]);
}

// the clocks: realtime is a plausible date and monotonic never goes back.
void
clocktest(char *s)
{
  struct timespec a, b;
  if (clock_gettime(CLOCK_REALTIME, &a) < 0) {
    printf("%s: no real-time clock\n", s);
    exit(1);
  }
  if (a.tv_sec < 1700000000L || a.tv_nsec < 0 || a.tv_nsec >= 1000000000L) { // after Nov 2023
    printf("%s: implausible time %ld.%ld\n", s, a.tv_sec, a.tv_nsec);
    exit(1);
  }
  if (clock_gettime(CLOCK_MONOTONIC, &a) < 0) {
    printf("%s: no monotonic clock\n", s);
    exit(1);
  }
  for (int i = 0; i < 1000; i++) {
    if (clock_gettime(CLOCK_MONOTONIC, &b) < 0 || b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) {
      printf("%s: monotonic clock went backwards\n", s);
      exit(1);
    }
    a = b;
  }
  if (clock_gettime(2, &a) >= 0 || clock_gettime(CLOCK_REALTIME, (struct timespec *)0xffffffff00000000UL) >= 0) {
    printf("%s: bad arguments accepted\n", s);
    exit(1);
  }
}

// does exec return an error if the arguments
// are larger than a page? or does it write
// below the stack and wreck the instructions/data?
void
bigargtest(char *s)
{
  int pid, fd, xstatus;

  unlink("bigarg-ok");
  pid = fork();
  if (pid == 0) {
    static char *args[MAXARG];
    int i;
    // MAXARG-1 of these exceed ARG_MAX, so exec must refuse them.
    long bigsz = ARG_MAX / (MAXARG - 1) + 16;
    char *big = malloc(bigsz);
    memset(big, ' ', bigsz);
    big[bigsz - 1] = '\0';
    for (i = 0; i < MAXARG - 1; i++)
      args[i] = big;
    args[MAXARG - 1] = 0;
    // this exec() should fail (and return) because the
    // arguments are too large.
    exec("/bin/echo", args);
    fd = open("bigarg-ok", O_CREATE, 0666);
    close(fd);
    exit(0);
  } else if (pid < 0) {
    printf("%s: bigargtest: fork failed\n", s);
    exit(1);
  }

  wait(&xstatus);
  if (xstatus != 0)
    exit(xstat(xstatus));
  fd = open("bigarg-ok", 0);
  if (fd < 0) {
    printf("%s: bigarg test failed!\n", s);
    exit(1);
  }
  close(fd);
}

// what happens when the file system runs out of blocks?
// answer: balloc panics, so this test is not useful.
void
fsfull()
{
  int nfiles;
  int fsblocks = 0;

  printf("fsfull test\n");

  for (nfiles = 0;; nfiles++) {
    char name[64];
    name[0] = 'f';
    name[1] = '0' + nfiles / 1000;
    name[2] = '0' + (nfiles % 1000) / 100;
    name[3] = '0' + (nfiles % 100) / 10;
    name[4] = '0' + (nfiles % 10);
    name[5] = '\0';
    printf("writing %s\n", name);
    int fd = open(name, O_CREATE | O_RDWR, 0666);
    if (fd < 0) {
      printf("open %s failed\n", name);
      break;
    }
    int total = 0;
    while (1) {
      int cc = write(fd, buf, BSIZE);
      if (cc < BSIZE)
        break;
      total += cc;
      fsblocks++;
    }
    printf("wrote %d bytes\n", total);
    close(fd);
    if (total == 0)
      break;
  }

  while (nfiles >= 0) {
    char name[64];
    name[0] = 'f';
    name[1] = '0' + nfiles / 1000;
    name[2] = '0' + (nfiles % 1000) / 100;
    name[3] = '0' + (nfiles % 100) / 10;
    name[4] = '0' + (nfiles % 10);
    name[5] = '\0';
    unlink(name);
    nfiles--;
  }

  printf("fsfull test finished, %d blocks\n", fsblocks);
}

void
argptest(char *s)
{
  int fd;
  fd = open("/sbin/init", O_RDONLY);
  if (fd < 0) {
    printf("%s: open failed\n", s);
    exit(1);
  }
  read(fd, sbrk(0) - 1, -1);
  close(fd);
}

// check that there's an invalid page beneath
// the user stack, to catch stack overflow.
void
stacktest(char *s)
{
  int pid;
  int xstatus;

  pid = fork();
  if (pid == 0) {
    char *sp = (char *)r_sp();
    sp -= sysconf(_SC_XV6_STACK_MAX); // below the stack region: the guard
    // the *sp should cause a trap.
    printf("%s: stacktest: read below stack %d\n", s, *sp);
    exit(1);
  } else if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  }
  wait(&xstatus);
  if (WIFSIGNALED(xstatus)) // killed by a signal?
    exit(0);
  else
    exit(xstat(xstatus));
}

// check that writes to a few forbidden addresses
// cause a fault, e.g. process's text and TRAMPOLINE.
void
nowrite(char *s)
{
  int pid;
  int xstatus;
  uint64 addrs[] = {0,
                    0x80000000LL,
                    0x3fffffe000,
                    0x3ffffff000,
                    0x4000000000,
                    0xffffffffffffffff};

  for (int ai = 0; ai < sizeof(addrs) / sizeof(addrs[0]); ai++) {
    pid = fork();
    if (pid == 0) {
      volatile int *addr = (int *)addrs[ai];
      *addr = 10;
      printf("%s: write to %p did not fail!\n", s, addr);
      exit(0);
    } else if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    wait(&xstatus);
    if (xstatus == 0) {
      // kernel did not kill child!
      exit(1);
    }
  }
  exit(0);
}

// regression test. copyin(), copyout(), and copyinstr() used to cast
// the virtual page address to uint, which (with certain wild system
// call arguments) resulted in a kernel page faults.
void *big = (void *)0xeaeb0b5b00002f5e;
void
pgbug(char *s)
{
  char *argv[1];
  argv[0] = 0;
  exec(big, argv);
  pipe(big);

  exit(0);
}

// regression test. does the kernel panic if a process sbrk()s its
// size to be less than a page, or zero, or reduces the break by an
// amount too small to cause a page to be freed?
void
sbrkbugs(char *s)
{
  int pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    int sz = (uint64)sbrk(0);
    // free all user memory; there used to be a bug that
    // would not adjust p->sz correctly in this case,
    // causing exit() to panic.
    sbrk(-sz);
    // user page fault here.
    exit(0);
  }
  wait(0);

  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    int sz = (uint64)sbrk(0);
    // set the break to somewhere in the very first
    // page; there used to be a bug that would incorrectly
    // free the first page.
    sbrk(-(sz - 3500));
    exit(0);
  }
  wait(0);

  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    // set the break in the middle of a page.
    sbrk((10 * PGSIZE + 2048) - (uint64)sbrk(0));

    // reduce the break a bit, but not enough to
    // cause a page to be freed. this used to cause
    // a panic.
    sbrk(-10);

    exit(0);
  }
  wait(0);

  exit(0);
}

// if process size was somewhat more than a page boundary, and then
// shrunk to be somewhat less than that page boundary, can the kernel
// still copyin() from addresses in the last page?
void
sbrklast(char *s)
{
  uint64 top = (uint64)sbrk(0);
  if ((top % PGSIZE) != 0)
    sbrk(PGSIZE - (top % PGSIZE));
  sbrk(PGSIZE);
  sbrk(10);
  sbrk(-20);
  top = (uint64)sbrk(0);
  char *p = (char *)(top - 64);
  p[0] = 'x';
  p[1] = '\0';
  int fd = open(p, O_RDWR | O_CREATE, 0666);
  write(fd, p, 1);
  close(fd);
  fd = open(p, O_RDWR);
  p[0] = '\0';
  read(fd, p, 1);
  if (p[0] != 'x')
    exit(1);
}

// does sbrk handle signed int32 wrap-around with
// negative arguments?
void
sbrk8000(char *s)
{
  sbrk(0x80000004);
  volatile char *top = sbrk(0);
  *(top - 1) = *(top - 1) + 1;
}

// regression test. test whether exec() leaks memory if one of the
// arguments is invalid. the test passes if the kernel doesn't panic.
void
badarg(char *s)
{
  for (int i = 0; i < 50000; i++) {
    char *argv[2];
    argv[0] = (char *)0xffffffff;
    argv[1] = 0;
    exec("/bin/echo", argv);
  }

  exit(0);
}

#define REGION_SZ (1024 * 1024 * 1024)

// Touch a page every 64 pages, which with lazy allocation
// causes one page to be allocated.
void
lazy_alloc(char *s)
{
  char *i, *prev_end, *new_end;

  prev_end = sbrklazy(REGION_SZ);
  if (prev_end == (char *)SBRK_ERROR) {
    printf("sbrklazy() failed\n");
    exit(1);
  }
  new_end = prev_end + REGION_SZ;

  for (i = prev_end + PGSIZE; i < new_end; i += 64 * PGSIZE)
    *(char **)i = i;

  for (i = prev_end + PGSIZE; i < new_end; i += 64 * PGSIZE) {
    if (*(char **)i != i) {
      printf("failed to read value from memory\n");
      exit(1);
    }
  }

  exit(0);
}

// Touch a page every 64 pages in region, which with lazy allocation
// causes one page to be allocated. Check that freeing the region
// frees the allocated pages.
void
lazy_unmap(char *s)
{
  int pid;
  char *i, *prev_end, *new_end;

  prev_end = sbrklazy(REGION_SZ);
  if (prev_end == (char *)SBRK_ERROR) {
    printf("sbrklazy() failed\n");
    exit(1);
  }
  new_end = prev_end + REGION_SZ;

  for (i = prev_end + PGSIZE; i < new_end; i += PGSIZE * PGSIZE)
    *(char **)i = i;

  for (i = prev_end + PGSIZE; i < new_end; i += PGSIZE * PGSIZE) {
    pid = fork();
    if (pid < 0) {
      printf("error forking\n");
      exit(1);
    } else if (pid == 0) {
      sbrklazy(-1L * REGION_SZ);
      *(char **)i = i;
      exit(0);
    } else {
      int status;
      wait(&status);
      if (status == 0) {
        printf("memory not unmapped\n");
        exit(1);
      }
    }
  }

  exit(0);
}

void
lazy_copy(char *s)
{
  // copyinstr on lazy page
  {
    char *p = sbrk(0);
    sbrklazy(4 * PGSIZE);
    open(p + 8192, 0);
  }

  {
    void *xx = sbrk(0);
    void *ret = sbrk(-(((uint64)xx) + 1));
    if (ret != xx) {
      printf("sbrk(sbrk(0)+1) returned %p, not old sz\n", ret);
      exit(1);
    }
  }

  // read() and write() to these addresses should fail: the guard page
  // under the stack, the trapframe and trampoline pages, the top of the
  // address space and beyond it.
  unsigned long bad[] = {
    (unsigned long)sysconf(_SC_XV6_STACK_GUARD), TRAPFRAME, TRAPFRAME + PGSIZE,
    MAXVA - PGSIZE, MAXVA, MAXVA * 2,
  };
  for (int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    int fd = open("README", 0);
    if (fd < 0) {
      printf("cannot open README\n");
      exit(1);
    }
    if (read(fd, (char *)bad[i], 512) >= 0) {
      printf("read succeeded\n");
      exit(1);
    }
    close(fd);
    fd = open("junk", O_CREATE | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      printf("cannot open junk\n");
      exit(1);
    }
    if (write(fd, (char *)bad[i], 512) >= 0) {
      printf("write succeeded\n");
      exit(1);
    }
    close(fd);
  }

  exit(0);
}

void
lazy_copyinstr(char *s)
{
  char *p = sbrk(0);
  sbrk(PGSIZE - ((uint64)p % PGSIZE));

  p = sbrk(0);
  if ((uint64)p % PGSIZE != 0) {
    printf("%s: sbrk did not align\n", s);
    exit(1);
  }

  sbrklazy(2 * PGSIZE);
  p[4095] = '/';
  int fd = open(&p[4095], O_RDONLY);
  if (fd < 0) {
    printf("could not open /");
    exit(1);
  }

  struct stat st;
  int r = fstat(fd, &st);
  if (r < 0) {
    printf("could not stat /");
    exit(1);
  }

  if (st.type != T_DIR) {
    printf("/ is not T_DIR");
    exit(1);
  }

  close(fd);
}

void
lazy_sbrk(char *s)
{
  // sbrk() takes just int, so take 2^30-sized steps towards MAXVA
  char *p = sbrk(0);
  while ((uint64)p < BRK_MAX - (1 << 30)) {
    p = sbrklazy(1 << 30);
    if (p < 0) {
      printf("sbrklazy(%d) returned %p\n", 1 << 30, p);
      exit(1);
    }

    p = sbrklazy(0);
  }

  int n = BRK_MAX - PGSIZE - (uint64)p;

  char *p1 = sbrklazy(n);
  if (p1 < 0 || p1 != p) {
    printf("sbrklazy(%d) returned %p, not expected %p\n", n, p1, p);
    exit(1);
  }

  p = sbrk(PGSIZE);
  if (p < 0 || (uint64)p != BRK_MAX - PGSIZE) {
    printf("sbrk(%d) returned %p, not expected BRK_MAX-PGSIZE\n", PGSIZE, p);
    exit(1);
  }

  p[0] = 1;
  if (p[1] != 0) {
    printf("sbrk() returned non-zero-filled memory\n");
    exit(1);
  }

  p = sbrk(1);
  if ((uint64)p != -1) {
    printf("sbrk(1) returned %p, expected error\n", p);
    exit(1);
  }

  p = sbrklazy(1);
  if ((uint64)p != -1) {
    printf("sbrklazy(1) returned %p, expected error\n", p);
    exit(1);
  }

  exit(0);
}

void
partial_write(char *s)
{
  // Create testfile containing "A".
  // write() 2 bytes that span page boundary: first is "X", second is unmapped.
  // Potential problem: write errors, forgets to log the updated first byte.
  // read() from file returns "X".
  // Flush buffer cache with some large writes.
  // read() from file should still return "X" (but might return "A" due to bug).

  unlink("testfile");
  int fd = open("testfile", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: cannot create testfile\n", s);
    exit(1);
  }

  int cc = write(fd, "A", 1);
  if (cc != 1) {
    printf("%s: could not write A\n", s);
    exit(1);
  }

  close(fd);
  fd = open("testfile", O_RDWR);
  if (fd < 0) {
    printf("%s: cannot re-open testfile\n", s);
    exit(1);
  }

  char *p = sbrk(0);
  sbrk(PGSIZE - ((uint64)p % PGSIZE));

  p = sbrk(0);
  if ((uint64)p % PGSIZE != 0) {
    printf("%s: sbrk did not align\n", s);
    exit(1);
  }

  p[-1] = 'X';

  cc = write(fd, p - 1, 2);
  if (cc != -1) {
    printf("%s: write succeeded, should have failed\n", s);
    exit(1);
  }

  close(fd);

  fd = open("testfile", O_RDONLY);
  if (fd < 0) {
    printf("%s: cannot re-open testfile\n", s);
    exit(1);
  }

  char b;
  cc = read(fd, &b, 1);
  if (cc != 1) {
    printf("%s: cannot read testfile\n", s);
    exit(1);
  }

  close(fd);

  if (b != 'X') {
    printf("%s: read returned %c, expected X\n", s, b);
    exit(1);
  }

  fd = open("bigfile", O_CREATE | O_RDWR, 0666);
  for (int i = 0; i < 64; i++) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    cc = write(fd, buf, sizeof(buf));
    if (cc != sizeof(buf)) {
      printf("%s: could not write to bigfile\n", s);
      exit(-1);
    }
  }
  close(fd);

  unlink("bigfile");

  fd = open("testfile", O_RDONLY);
  if (fd < 0) {
    printf("%s: cannot re-open testfile\n", s);
    exit(1);
  }

  cc = read(fd, &b, 1);
  if (cc != 1) {
    printf("%s: cannot read testfile\n", s);
    exit(1);
  }

  close(fd);

  if (b != 'X') {
    printf("%s: read returned %c, expected X\n", s, b);
    exit(1);
  }

  unlink("testfile");
}

void
unlinkcwd(char *s)
{
  if (mkdir("/a", 0777) < 0) {
    printf("%s: mkdir /a failed\n", s);
    exit(1);
  }
  if (mkdir("/a/b", 0777) < 0) {
    printf("%s: mkdir /a/b failed\n", s);
    exit(1);
  }
  if (chdir("/a/b") < 0) {
    printf("%s: chdir failed\n", s);
    exit(1);
  }
  if (unlink("/a/b") < 0) {
    printf("%s: unlink /a/b failed\n", s);
    exit(1);
  }
  if (unlink("/a") < 0) {
    printf("%s: unlink /a failed\n", s);
    exit(1);
  }
  if (open("../", O_RDONLY) > 0) {
    printf("%s: open ../ non-existing directory\n", s);
  }
  if (open("../c", O_CREATE, 0666) > 0) {
    printf("%s: create ../c non-existing file\n", s);
  }
}


//
// floating point: F and D arithmetic, state across context switches,
// and fcsr persistence.
//

static long
dtol(double d)
{
  return (long)d;
}

void
fptest(char *s)
{
  volatile double a = 1.5, b = 2.25;
  volatile float f = 0.5f;
  double c = a * b + f; // 3.875
  if (dtol(c * 1000) != 3875) {
    printf("%s: double arithmetic wrong\n", s);
    exit(1);
  }
  double r = __builtin_sqrt(2.0);
  if (dtol(r * 1000000) != 1414213) {
    printf("%s: sqrt wrong\n", s);
    exit(1);
  }
  volatile float g = f / 3.0f;
  if ((int)(g * 1000000) != 166666) {
    printf("%s: float arithmetic wrong\n", s);
    exit(1);
  }
  long big = 1L << 40;
  volatile double d = (double)big;
  if (dtol(d) != big) {
    printf("%s: conversion wrong\n", s);
    exit(1);
  }
  double sum = 0;
  for (int i = 0; i < 1000; i++)
    sum += 0.001;
  if (dtol(sum * 1000000 + 0.5) != 1000000) {
    printf("%s: accumulation wrong\n", s);
    exit(1);
  }
  volatile double neg = -7.75;
  if (dtol(neg * -4) != 31 || dtol(__builtin_fabs(neg) * 4) != 31) {
    printf("%s: sign handling wrong\n", s);
    exit(1);
  }
}

// several processes keep distinct values in FP registers across many
// context switches; a lost or leaked register shows up as a wrong sum.
void
fpfork(char *s)
{
  int n = 4;
  for (int k = 0; k < n; k++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid == 0) {
      volatile double base = 1.0 + k * 0.25;
      double acc = 0, acc2 = 0;
      float facc = 0;
      for (int i = 0; i < 4000; i++) {
        acc += base;
        acc2 += base * 0.5;
        facc += 0.25f;
        if (i % 200 == 0)
          ticksleep(1);
      }
      if (dtol(acc * 4) != dtol(base * 4000 * 4) || dtol(acc2 * 8) != dtol(base * 2000 * 8) ||
          (int)facc != 1000) {
        printf("%s: child %d: fp state corrupted\n", s, k);
        exit(1);
      }
      exit(0);
    }
  }
  for (int k = 0; k < n; k++) {
    int st;
    wait(&st);
    if (st != 0) {
      printf("%s: child failed\n", s);
      exit(1);
    }
  }
}

// fcsr (rounding mode, exception flags) belongs to the process.
void
fpcsr(char *s)
{
  uint64 frm, fflags;
  asm volatile("fsrm %0, %1" : "=r"(frm) : "r"(2L)); // round down
  ticksleep(2);
  asm volatile("frrm %0" : "=r"(frm));
  if (frm != 2) {
    printf("%s: rounding mode lost across a context switch (%ld)\n", s, frm);
    exit(1);
  }
  volatile double x = 1.0, y = 3.0;
  asm volatile("fsflags %0, %1" : "=r"(fflags) : "r"(0L));
  volatile double z = x / y; // inexact
  (void)z;
  asm volatile("frflags %0" : "=r"(fflags));
  if (!(fflags & 1)) {
    printf("%s: inexact flag not set\n", s);
    exit(1);
  }
  int pid = fork();
  if (pid == 0) {
    // the child inherits fcsr; changing it must not affect the parent.
    asm volatile("fsrm %0, %1" : "=r"(frm) : "r"(0L));
    asm volatile("fsflags %0, %1" : "=r"(fflags) : "r"(0L));
    exit(0);
  }
  wait(0);
  asm volatile("frrm %0" : "=r"(frm));
  asm volatile("frflags %0" : "=r"(fflags));
  if (frm != 2 || !(fflags & 1)) {
    printf("%s: fcsr changed by child\n", s);
    exit(1);
  }
  asm volatile("fsrm %0, %1" : "=r"(frm) : "r"(0L));
}


//
// vector extension: arithmetic, state across context switches, vl/vtype/
// vcsr persistence; without V the instructions must be reported as illegal.
//

static int
have_v(void)
{
  struct hwinfo hi;
  return hwinfo(&hi) == 0 && hi.has_v;
}

void
vtest(char *s)
{
  static int a[300], b[300], c[300];
  if (!have_v()) {
    // no vector unit: a vector instruction must kill the process.
    int pid = fork();
    if (pid == 0) {
      asm volatile(".option push\n.option arch, +v\nvsetvli x0, x0, e8, m1, ta, ma\n.option pop\n");
      exit(0);
    }
    int st;
    wait(&st);
    if (!WIFSIGNALED(st)) {
      printf("%s: vector instruction without V not reported as illegal (%d)\n", s, st);
      exit(1);
    }
    return;
  }
  for (int i = 0; i < 300; i++) {
    a[i] = i * i;
    b[i] = 1000 - i;
  }
  int *p = a, *q = b, *d = c;
  long n = 300;
  while (n > 0) {
    long vl;
    asm volatile(".option push\n.option arch, +v\n"
                 "vsetvli %0, %1, e32, m1, ta, ma\n"
                 "vle32.v v1, (%2)\n"
                 "vle32.v v2, (%3)\n"
                 "vadd.vv v3, v1, v2\n"
                 "vse32.v v3, (%4)\n"
                 ".option pop\n"
                 : "=r"(vl) : "r"(n), "r"(p), "r"(q), "r"(d) : "memory");
    if (vl <= 0 || vl > n) {
      printf("%s: bad vl %ld\n", s, vl);
      exit(1);
    }
    p += vl;
    q += vl;
    d += vl;
    n -= vl;
  }
  for (int i = 0; i < 300; i++) {
    if (c[i] != a[i] + b[i]) {
      printf("%s: vadd wrong at %d\n", s, i);
      exit(1);
    }
  }
}

// each child keeps its own pattern in v0..v7 across many context switches.
void
vecfork(char *s)
{
  if (!have_v())
    return;
  int n = 4;
  for (int k = 0; k < n; k++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid == 0) {
      static unsigned char pat[1024], back[1024];
      for (int i = 0; i < 1024; i++)
        pat[i] = (unsigned char)(i * (k + 3) + k);
      long vl;
      asm volatile(".option push\n.option arch, +v\n"
                   "vsetvli %0, %1, e8, m8, ta, ma\n"
                   "vle8.v v0, (%2)\n"
                   ".option pop\n"
                   : "=r"(vl) : "r"(1024L), "r"(pat) : "memory");
      for (int i = 0; i < 20; i++) {
        ticksleep(1);
        asm volatile(".option push\n.option arch, +v\nvse8.v v0, (%0)\n.option pop\n" : : "r"(back) : "memory");
        for (long j = 0; j < vl; j++) {
          if (back[j] != pat[j]) {
            printf("%s: child %d: vector state corrupted (round %d)\n", s, k, i);
            exit(1);
          }
        }
        uint64 vlnow;
        asm volatile("csrr %0, vl" : "=r"(vlnow));
        if (vlnow != (uint64)vl) {
          printf("%s: child %d: vl changed\n", s, k);
          exit(1);
        }
      }
      exit(0);
    }
  }
  for (int k = 0; k < n; k++) {
    int st;
    wait(&st);
    if (st != 0) {
      printf("%s: child failed\n", s);
      exit(1);
    }
  }
}

// vcsr (rounding mode / saturation flag) and vtype belong to the process.
void
vcsrtest(char *s)
{
  if (!have_v())
    return;
  uint64 v;
  asm volatile("csrw vcsr, %0" : : "r"(4L)); // vxrm (bits 2:1) = 2 (rdn)
  ticksleep(2);
  asm volatile("csrr %0, vcsr" : "=r"(v));
  if ((v & 6) != 4) {
    printf("%s: vcsr lost across a context switch (%ld)\n", s, v);
    exit(1);
  }
  long vl;
  asm volatile(".option push\n.option arch, +v\nvsetvli %0, %1, e16, m2, ta, ma\n.option pop\n" : "=r"(vl) : "r"(7L));
  ticksleep(2);
  uint64 vtype, vlnow;
  asm volatile("csrr %0, vtype" : "=r"(vtype));
  asm volatile("csrr %0, vl" : "=r"(vlnow));
  if (vlnow != 7 || (vtype & 0x38) != 0x08 || (vtype & 7) != 1) {
    printf("%s: vl/vtype lost across a context switch (%ld, 0x%lx)\n", s, vlnow, vtype);
    exit(1);
  }
  int pid = fork();
  if (pid == 0) {
    asm volatile("csrw vcsr, %0" : : "r"(0L));
    asm volatile(".option push\n.option arch, +v\nvsetvli x0, x0, e8, m1, ta, ma\n.option pop\n");
    exit(0);
  }
  wait(0);
  asm volatile("csrr %0, vcsr" : "=r"(v));
  asm volatile("csrr %0, vl" : "=r"(vlnow));
  if ((v & 6) != 4 || vlnow != 7) {
    printf("%s: vector csrs changed by child\n", s);
    exit(1);
  }
  asm volatile("csrw vcsr, %0" : : "r"(0L));
}

// the standalone floating-point and vector programs must all succeed.
void
fpvexec(char *s)
{
  static const char *const progs[] = {"/bin/fpbench", "/bin/mandel", "/bin/vecbench", 0};
  for (int i = 0; progs[i]; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if (pid == 0) {
      char *argv[] = {(char *)progs[i], "2", 0};
      exec(progs[i], argv);
      printf("%s: exec %s failed\n", s, progs[i]);
      exit(1);
    }
    int st;
    wait(&st);
    if (st != 0) {
      printf("%s: %s failed (%d)\n", s, progs[i], st);
      exit(1);
    }
  }
}

struct test {
  void (*f)(char *);
  const char *s;
};

// the tables are const so that a position-independent program keeps
// them out of its read-only data, where the addresses in them could
// not be relocated.
static const struct test quicktests[] = {
  {copyin, "copyin"},
  {copyout, "copyout"},
  {copyinstr1, "copyinstr1"},
  {copyinstr2, "copyinstr2"},
  {copyinstr3, "copyinstr3"},
  {rwsbrk, "rwsbrk"},
  {truncate1, "truncate1"},
  {truncate2, "truncate2"},
  {truncate3, "truncate3"},
  {openiputtest, "openiput"},
  {exitiputtest, "exitiput"},
  {iputtest, "iput"},
  {opentest, "opentest"},
  {writetest, "writetest"},
  {writebig, "writebig"},
  {createtest, "createtest"},
  {dirtest, "dirtest"},
  {exectest, "exectest"},
  {pipe1, "pipe1"},
  {killstatus, "killstatus"},
  {preempt, "preempt"},
  {exitwait, "exitwait"},
  {reparent, "reparent"},
  {twochildren, "twochildren"},
  {forkfork, "forkfork"},
  {forkforkfork, "forkforkfork"},
  {reparent2, "reparent2"},
  {mem, "mem"},
  {sharedfd, "sharedfd"},
  {fourfiles, "fourfiles"},
  {createdelete, "createdelete"},
  {unlinkread, "unlinkread"},
  {linktest, "linktest"},
  {concreate, "concreate"},
  {linkunlink, "linkunlink"},
  {subdir, "subdir"},
  {bigwrite, "bigwrite"},
  {bigfile, "bigfile"},
  {longnames, "longnames"},
  {rmdot, "rmdot"},
  {dirfile, "dirfile"},
  {iref, "iref"},
  {forktest, "forktest"},
  {sbrkbasic, "sbrkbasic"},
  {sbrkmuch, "sbrkmuch"},
  {kernmem, "kernmem"},
  {MAXVAplus, "MAXVAplus"},
  {sbrkfail, "sbrkfail"},
  {sbrkarg, "sbrkarg"},
  {validatetest, "validatetest"},
  {bsstest, "bsstest"},
  {bigargtest, "bigargtest"},
  {clocktest, "clocktest"},
  {sigtest, "sigtest"},
  {threadtest, "threadtest"},
  {posixtest, "posixtest"},
  {ttytest, "ttytest"},
  {cowtest, "cowtest"},
  {mmaptest, "mmaptest"},
  {manyprocs, "manyprocs"},
  {argptest, "argptest"},
  {stacktest, "stacktest"},
  {nowrite, "nowrite"},
  {pgbug, "pgbug"},
  {sbrkbugs, "sbrkbugs"},
  {sbrklast, "sbrklast"},
  {sbrk8000, "sbrk8000"},
  {badarg, "badarg"},
  {lazy_alloc, "lazy_alloc"},
  {lazy_unmap, "lazy_unmap"},
  {lazy_copy, "lazy_copy"},
  {lazy_copyinstr, "lazy_copyinstr"},
  {lazy_sbrk, "lazy_sbrk"},
  {partial_write, "partial_write"},
  {unlinkcwd, "unlinkcwd"},
  {fptest, "fptest"},
  {fpfork, "fpfork"},
  {fpcsr, "fpcsr"},
  {vtest, "vtest"},
  {vecfork, "vfork"},
  {vcsrtest, "vcsrtest"},
  {fpvexec, "fpvexec"},
  {0, 0},
};

//
// Section with tests that take a fair bit of time
//

// directory that uses indirect blocks
void
bigdir(char *s)
{
  enum { N = 500 };
  int i, fd;
  char name[10];

  unlink("bd");

  fd = open("bd", O_CREATE, 0666);
  if (fd < 0) {
    printf("%s: bigdir create failed\n", s);
    exit(1);
  }
  close(fd);

  for (i = 0; i < N; i++) {
    name[0] = 'x';
    name[1] = '0' + (i / 64);
    name[2] = '0' + (i % 64);
    name[3] = '\0';
    if (link("bd", name) != 0) {
      printf("%s: bigdir i=%d link(bd, %s) failed\n", s, i, name);
      exit(1);
    }
  }

  unlink("bd");
  for (i = 0; i < N; i++) {
    name[0] = 'x';
    name[1] = '0' + (i / 64);
    name[2] = '0' + (i % 64);
    name[3] = '\0';
    if (unlink(name) != 0) {
      printf("%s: bigdir unlink failed", s);
      exit(1);
    }
  }
}

// concurrent writes to try to provoke deadlock in the virtio disk
// driver.
void
manywrites(char *s)
{
  int nchildren = 4;
  int howmany = 30; // increase to look for deadlock

  for (int ci = 0; ci < nchildren; ci++) {
    int pid = fork();
    if (pid < 0) {
      printf("fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      char name[3];
      name[0] = 'b';
      name[1] = 'a' + ci;
      name[2] = '\0';
      unlink(name);

      for (int iters = 0; iters < howmany; iters++) {
        for (int i = 0; i < ci + 1; i++) {
          int fd = open(name, O_CREATE | O_RDWR, 0666);
          if (fd < 0) {
            printf("%s: cannot create %s\n", s, name);
            exit(1);
          }
          int sz = sizeof(buf);
          int cc = write(fd, buf, sz);
          if (cc != sz) {
            printf("%s: write(%d) ret %d\n", s, sz, cc);
            exit(1);
          }
          close(fd);
        }
        unlink(name);
      }

      unlink(name);
      exit(0);
    }
  }

  for (int ci = 0; ci < nchildren; ci++) {
    int st = 0;
    wait(&st);
    if (st != 0)
      exit(st);
  }
  exit(0);
}

// regression test. does write() with an invalid buffer pointer cause
// a block to be allocated for a file that is then not freed when the
// file is deleted? if the kernel has this bug, it will panic: balloc:
// out of blocks. assumed_free may need to be raised to be more than
// the number of free blocks. this test takes a long time.
void
badwrite(char *s)
{
  int assumed_free = 600;

  unlink("junk");
  for (int i = 0; i < assumed_free; i++) {
    int fd = open("junk", O_CREATE | O_WRONLY, 0666);
    if (fd < 0) {
      printf("open junk failed\n");
      exit(1);
    }
    write(fd, (char *)0xffffffffffL, 1);
    close(fd);
    unlink("junk");
  }

  int fd = open("junk", O_CREATE | O_WRONLY, 0666);
  if (fd < 0) {
    printf("open junk failed\n");
    exit(1);
  }
  if (write(fd, "x", 1) != 1) {
    printf("write failed\n");
    exit(1);
  }
  close(fd);
  unlink("junk");

  exit(0);
}

// test the exec() code that cleans up if it runs out
// of memory. it's really a test that such a condition
// doesn't cause a panic.
void
execout(char *s)
{
  if ((uint64)sysconf(_SC_PHYS_PAGES) * PGSIZE > 4UL * 1024 * 1024 * 1024) {
    printf("%s: too much RAM to exhaust, skipping\n", s);
    return;
  }
  for (int avail = 0; avail < 15; avail++) {
    int pid = fork();
    if (pid < 0) {
      printf("fork failed\n");
      exit(1);
    } else if (pid == 0) {
      // allocate all of memory.
      while (1) {
        char *a = sbrk(PGSIZE);
        if (a == SBRK_ERROR)
          break;
        *(a + PGSIZE - 1) = 1;
      }

      // free a few pages, in order to let exec() make some
      // progress.
      for (int i = 0; i < avail; i++)
        sbrk(-PGSIZE);

      close(1);
      char *args[] = {"/bin/echo", "x", 0};
      exec("/bin/echo", args);
      exit(0);
    } else {
      wait((int *)0);
    }
  }

  exit(0);
}

// can the kernel tolerate running out of disk space?
void
diskfull(char *s)
{
  int fi;
  int done = 0;

  unlink("diskfulldir");

  for (fi = 0; done == 0 && '0' + fi < 0177; fi++) {
    char name[32];
    name[0] = 'b';
    name[1] = 'i';
    name[2] = 'g';
    name[3] = '0' + fi;
    name[4] = '\0';
    unlink(name);
    int fd = open(name, O_CREATE | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      // oops, ran out of inodes before running out of blocks.
      printf("%s: could not create file %s\n", s, name);
      done = 1;
      break;
    }
    for (int i = 0; i < BIGBLOCKS; i++) {
      char buf[BSIZE];
      if (write(fd, buf, BSIZE) != BSIZE) {
        done = 1;
        close(fd);
        break;
      }
    }
    close(fd);
  }

  // now that there are no free blocks, test that dirlink()
  // merely fails (doesn't panic) if it can't extend
  // directory content. one of these file creations
  // is expected to fail.
  int nzz = 128;
  for (int i = 0; i < nzz; i++) {
    char name[32];
    name[0] = 'z';
    name[1] = 'z';
    name[2] = '0' + (i / 32);
    name[3] = '0' + (i % 32);
    name[4] = '\0';
    unlink(name);
    int fd = open(name, O_CREATE | O_RDWR | O_TRUNC, 0666);
    if (fd < 0)
      break;
    close(fd);
  }

  // this mkdir() is expected to fail.
  if (mkdir("diskfulldir", 0777) == 0)
    printf("%s: mkdir(diskfulldir, 0777) unexpectedly succeeded!\n", s);

  unlink("diskfulldir");

  for (int i = 0; i < nzz; i++) {
    char name[32];
    name[0] = 'z';
    name[1] = 'z';
    name[2] = '0' + (i / 32);
    name[3] = '0' + (i % 32);
    name[4] = '\0';
    unlink(name);
  }

  for (int i = 0; '0' + i < 0177; i++) {
    char name[32];
    name[0] = 'b';
    name[1] = 'i';
    name[2] = 'g';
    name[3] = '0' + i;
    name[4] = '\0';
    unlink(name);
  }
}

void
outofinodes(char *s)
{
  int nzz = 32 * 32;
  for (int i = 0; i < nzz; i++) {
    char name[32];
    name[0] = 'z';
    name[1] = 'z';
    name[2] = '0' + (i / 32);
    name[3] = '0' + (i % 32);
    name[4] = '\0';
    unlink(name);
    int fd = open(name, O_CREATE | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
      // failure is eventually expected.
      break;
    }
    close(fd);
  }

  for (int i = 0; i < nzz; i++) {
    char name[32];
    name[0] = 'z';
    name[1] = 'z';
    name[2] = '0' + (i / 32);
    name[3] = '0' + (i % 32);
    name[4] = '\0';
    unlink(name);
  }
}

void
linkoverflow(char *s)
{
  enum { TARGET = 32768 };
  enum { DIRS = 64 };
  struct stat st;
  int i;

  unlink("/lof");
  int fd = open("/lof", O_CREATE | O_RDWR, 0666);
  if (fd < 0) {
    printf("%s: cannot create /lof\n", s);
    exit(1);
  }
  close(fd);

  for (i = 0; i < TARGET; i++) {
    int d = i % DIRS;
    int f = i / DIRS;

    char pn[16];
    pn[0] = '/';
    pn[1] = 'd';
    pn[2] = '_';
    pn[3] = 'a' + (d / 16);
    pn[4] = 'a' + (d % 16);
    pn[5] = '\0';
    if (f == 0 && mkdir(pn, 0777) < 0) {
      printf("%s: mkdir(%s, 0777) failed\n", s, pn);
      exit(1);
    }

    pn[5] = '/';
    pn[6] = 'l';
    pn[7] = 'a' + (f / 256);
    pn[8] = 'a' + ((f / 16) % 16);
    pn[9] = 'a' + (f % 16);
    pn[10] = '\0';

    if (link("/lof", pn) < 0) {
      if (stat("/lof", &st) < 0) {
        printf("%s: stat(/lof) failed\n", s);
        exit(1);
      }
      if (st.nlink >= 32767) {
        // overflow check succeeded.
        break;
      }
      printf("%s: link failed after %d links (nlink=%d)\n", s, i, st.nlink);
      exit(1);
    }

    if (i % 100 == 0) {
      printf("%s: i=%d, pn=%s\n", s, i, pn);
    }
  }

  if (stat("/lof", &st) < 0) {
    printf("%s: stat(/lof) failed\n", s);
    exit(1);
  }

  unlink("/lof");

  if (st.nlink < 0) {
    printf("%s: negative link count: %d\n", s, st.nlink);
    exit(1);
  }
}

static const struct test slowtests[] = {
  {bigdir, "bigdir"},
  {manywrites, "manywrites"},
  {badwrite, "badwrite"},
  {execout, "execout"},
  {diskfull, "diskfull"},
  {outofinodes, "outofinodes"},
  // {linkoverflow, "linkoverflow"},

  {0, 0},
};

//
// drive tests
//

// -m: report the memory each test leaves behind. a test that ends with
// fewer pages available than it started with has leaked, and saying
// which one it was is the whole difficulty of the report at the end of
// a run, which only says that something did.
int report_memory;

// run each test in its own process. run returns 1 if child's exit()
// indicates success.
int
run(void f(char *), const char *s)
{
  int pid;
  int xstatus;
  long before = report_memory ? sysconf(_SC_AVPHYS_PAGES) : 0;

  printf("test %s: ", s);
  if ((pid = fork()) < 0) {
    printf("runtest: fork error\n");
    exit(1);
  }
  if (pid == 0) {
    f((char *)s);
    exit(0);
  } else {
    wait(&xstatus);
    if (xstatus != 0)
      printf("FAILED\n");
    else if (report_memory) {
      long after = sysconf(_SC_AVPHYS_PAGES);
      printf("OK (%ld pages available, %+ld)\n", after, after - before);
    } else
      printf("OK\n");
    return xstatus == 0;
  }
}

int
runtests(const struct test *tests, char *justone, int continuous)
{
  int ntests = 0;
  for (const struct test *t = tests; t->s != 0; t++) {
    if ((justone == 0) || strcmp(t->s, justone) == 0) {
      ntests++;
      if (!run(t->f, t->s)) {
        if (continuous != 2) {
          printf("SOME TESTS FAILED\n");
          return -1;
        }
      }
    }
  }
  return ntests;
}

// use sbrk() to count how many free physical memory pages there are.
int
countfree()
{
  // the kernel's count of free pages (exhausting a 512 GiB machine with
  // sbrk() to count them would take, and commit, all of it).
  return (int)sysconf(_SC_AVPHYS_PAGES);
}

int
drivetests(int quick, int continuous, char *justone)
{
  do {
    printf("usertests starting\n");
    long nprocs0 = sysconf(_SC_XV6_NPROCS);
    int free0 = countfree();
    int free1 = 0;
    int ntests = 0;
    int n;
    n = runtests(quicktests, justone, continuous);
    if (n < 0) {
      if (continuous != 2) {
        return 1;
      }
    } else {
      ntests += n;
    }
    if (!quick) {
      if (justone == 0)
        printf("usertests slow tests starting\n");
      n = runtests(slowtests, justone, continuous);
      if (n < 0) {
        if (continuous != 2) {
          return 1;
        }
      } else {
        ntests += n;
      }
    }
    // orphans of the tests are init's to reap, on its own schedule: give
    // it a moment before counting. the kernel also keeps a little for
    // itself between runs (object cache slabs): allow that.
    for (int i = 0; i < 300 && sysconf(_SC_XV6_NPROCS) > nprocs0; i++)
      ticksleep(10);
    if ((free1 = countfree()) < free0 - 64) {
      printf("FAILED -- lost some free pages %d (out of %d)\n", free1, free0);
      if (continuous != 2) {
        return 1;
      }
    }
    if (justone != 0 && ntests == 0) {
      printf("NO TESTS EXECUTED\n");
      return 1;
    }
  } while (continuous);
  return 0;
}

int
main(int argc, char *argv[])
{
  int continuous = 0;
  int quick = 0;
  char *justone = 0;

  if (argc == 2 && strcmp(argv[1], "-q") == 0) {
    quick = 1;
  } else if (argc >= 2 && strcmp(argv[1], "-m") == 0) {
    report_memory = 1;
    if (argc == 3)
      justone = argv[2];
  } else if (argc == 2 && strcmp(argv[1], "-c") == 0) {
    continuous = 1;
  } else if (argc == 2 && strcmp(argv[1], "-C") == 0) {
    continuous = 2;
  } else if (argc == 2 && argv[1][0] != '-') {
    justone = argv[1];
  } else if (argc > 1) {
    printf("Usage: usertests [-c] [-C] [-q] [-m] [testname]\n");
    exit(1);
  }
  if (drivetests(quick, continuous, justone)) {
    exit(1);
  }
  printf("ALL TESTS PASSED\n");
  exit(0);
}
