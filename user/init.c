//
// init: process one, which starts everything else and outlives it.
//
// it reads /etc/inittab, which is lines of
//
//   id:runlevels:action:process
//
// and acts on them in order. the actions are the sysvinit ones:
//
//   sysinit   run once, before anything else, and wait for it
//   boot      run once at boot without waiting
//   bootwait  run once at boot and wait
//   wait      run when the runlevel is entered, and wait
//   once      run when the runlevel is entered, do not wait
//   respawn   keep it running: start it again whenever it stops
//   ctrlaltdel  run when the keyboard interrupt asks for a reboot
//   powerfail   run when the machine is being turned off
//
// a runlevel is a digit; an entry runs in the runlevels its field
// lists, or in all of them when that field is empty. this system has
// one multi-user runlevel, 2, and 0 and 6 for halt and reboot, but
// nothing here knows that: the machinery is the general one.
//
// without /etc/inittab init falls back to mounting what a machine
// needs and running a shell on the console, so a system whose
// configuration has been lost still comes up.
//
#include "user/user.h"
#include <signal.h>

#define MAXENTRY 32
#define INITTAB "/etc/inittab"

enum action {
  A_UNKNOWN = 0,
  A_SYSINIT,
  A_BOOT,
  A_BOOTWAIT,
  A_WAIT,
  A_ONCE,
  A_RESPAWN,
  A_CTRLALTDEL,
  A_POWERFAIL,
  A_OFF,
};

struct entry {
  char id[8];
  char levels[8];
  enum action action;
  char process[128];
  pid_t pid;      // running, or 0
  int started;    // a once/wait entry has had its turn
  int failures;   // how often it has died at once, to stop a loop
  long last_start;
};

static struct entry entries[MAXENTRY];
static int nentries;
static int runlevel = 2;
static volatile int reread_wanted, shutdown_wanted, stopping;

char *envp[] = {"PATH=/bin:/sbin:/usr/bin", "HOME=/root", "SHELL=/bin/sh", "TERM=xv6",
                "USER=root", 0};

// ---- what a machine needs before anything runs ----

static void
mount_basics(void)
{
  mkdir("/dev", 0755);
  mount("dev", "/dev", "devfs", 0, 0);

  if (open("/dev/console", O_RDWR) < 0) {
    // no devfs: make the console the old way, so that a kernel without
    // it still has somewhere to print.
    // major 1 is the console, as kernel/file.h fixes it
    mknod("console", 1, 0);
    open("console", O_RDWR);
  }
  dup(0); // stdout
  dup(0); // stderr

  // the kernel's own state, as files: /proc for the reports and the
  // processes, /sys for the objects -- the disks, the mounts, the
  // machine -- one line per attribute.
  mkdir("/proc", 0755);
  if (mount("proc", "/proc", "procfs", 0, 0) < 0)
    printf("init: cannot mount /proc\n");
  mkdir("/sys", 0755);
  if (mount("sys", "/sys", "sysfs", 0, 0) < 0)
    printf("init: cannot mount /sys\n");

  // and somewhere to put what does not have to survive a reboot
  mkdir("/tmp", 0755);
  if (mount("tmpfs", "/tmp", "tmpfs", 0, 0) < 0)
    printf("init: cannot mount /tmp\n");
  chmod("/tmp", 01777); // everyone may write; only an owner may remove
  mkdir("/var", 0755);
  mkdir("/var/run", 0755);
  mkdir("/var/log", 0755);
}

// ---- reading the table ----

static enum action
action_of(const char *s)
{
  static const struct {
    const char *name;
    enum action a;
  } names[] = {
      {"sysinit", A_SYSINIT},       {"boot", A_BOOT},         {"bootwait", A_BOOTWAIT},
      {"wait", A_WAIT},             {"once", A_ONCE},         {"respawn", A_RESPAWN},
      {"ctrlaltdel", A_CTRLALTDEL}, {"powerfail", A_POWERFAIL}, {"off", A_OFF},
      {0, A_UNKNOWN},
  };

  for (int i = 0; names[i].name; i++)
    if (strcmp(names[i].name, s) == 0)
      return names[i].a;
  return A_UNKNOWN;
}

static int
read_inittab(void)
{
  FILE *f = fopen(INITTAB, "r");
  char line[256];

  if (f == 0)
    return -1;
  nentries = 0;
  while (fgets(line, sizeof(line), f) && nentries < MAXENTRY) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == '\n' || *p == 0)
      continue;
    int len = strlen(p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
      p[--len] = 0;

    // id:runlevels:action:process, with the process able to contain
    // colons of its own, so only the first three are split
    char *f1 = p, *f2, *f3, *f4;
    if ((f2 = strchr(f1, ':')) == 0)
      continue;
    *f2++ = 0;
    if ((f3 = strchr(f2, ':')) == 0)
      continue;
    *f3++ = 0;
    if ((f4 = strchr(f3, ':')) == 0)
      continue;
    *f4++ = 0;

    struct entry *e = &entries[nentries];
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", f1);
    snprintf(e->levels, sizeof(e->levels), "%s", f2);
    e->action = action_of(f3);
    snprintf(e->process, sizeof(e->process), "%s", f4);
    if (e->action == A_UNKNOWN || e->process[0] == 0)
      continue;
    nentries++;
  }
  fclose(f);
  return nentries > 0 ? 0 : -1;
}

static int
in_runlevel(const struct entry *e)
{
  if (e->levels[0] == 0)
    return 1;
  for (const char *p = e->levels; *p; p++)
    if (*p - '0' == runlevel)
      return 1;
  return 0;
}

// ---- running one ----

// every process init starts gets the console and its own session, so
// that a program that wants a terminal has one and a job-control shell
// can take it.
static pid_t
spawn(const struct entry *e)
{
  pid_t pid = fork();

  if (pid < 0) {
    printf("init: fork failed\n");
    return -1;
  }
  if (pid > 0)
    return pid;

  setsid();
  int fd = open("/dev/console", O_RDWR);
  if (fd >= 0) {
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
      close(fd);
  }
  // a plain command is run as it is, so that ps shows the program and
  // not a shell wrapped round it; anything with shell syntax in it --
  // a pipe, a redirection, a variable -- goes through the shell, which
  // is what makes an inittab line able to say those things.
  if (strpbrk(e->process, "|&;<>()$`\\\"'*?[#~=%") == 0) {
    char copy[sizeof(e->process)];
    char *argv[16];
    int n = 0;
    snprintf(copy, sizeof(copy), "%s", e->process);
    for (char *p = copy; *p && n < 15;) {
      while (*p == ' ' || *p == '\t')
        *p++ = 0;
      if (*p == 0)
        break;
      argv[n++] = p;
      while (*p && *p != ' ' && *p != '\t')
        p++;
    }
    argv[n] = 0;
    if (n > 0) {
      execve(argv[0], argv, envp);
      printf("init: cannot run %s\n", argv[0]);
      exit(1);
    }
  }
  char *argv[] = {"/bin/sh", "-c", (char *)e->process, 0};
  execve(argv[0], argv, envp);
  printf("init: cannot run %s\n", e->process);
  exit(1);
}

static void
run_and_wait(struct entry *e)
{
  pid_t pid = spawn(e);

  if (pid < 0)
    return;
  for (;;) {
    int status;
    pid_t got = wait(&status);
    if (got == pid)
      return;
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0)
      return;
  }
}

// ---- signals ----

static void
on_hup(int sig)
{
  (void)sig;
  reread_wanted = 1;
}

static void
on_term(int sig)
{
  (void)sig;
  shutdown_wanted = sig;
}

// shutdown asks for this before it starts killing things: nothing is
// respawned from here on, or init would start a new shell over the
// messages of a machine that is going down.
static void
on_usr1(int sig)
{
  (void)sig;
  stopping = 1;
}

// ---- the fallback, when there is no table ----

static void
fallback(void)
{
  printf("init: no %s; starting a shell on the console\n", INITTAB);
  nentries = 0;
  struct entry *e = &entries[nentries++];
  memset(e, 0, sizeof(*e));
  snprintf(e->id, sizeof(e->id), "sh");
  e->action = A_RESPAWN;
  snprintf(e->process, sizeof(e->process), "/bin/sh");
}

int
main(void)
{
  umask(022);
  mount_basics();

  signal(SIGHUP, on_hup);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  signal(SIGUSR1, on_usr1);

  if (read_inittab() < 0)
    fallback();

  // sysinit first, and boot after it: what a machine needs before
  // anything else runs, and what it does once at every boot
  for (int i = 0; i < nentries; i++)
    if (entries[i].action == A_SYSINIT) {
      run_and_wait(&entries[i]);
      entries[i].started = 1;
    }
  for (int i = 0; i < nentries; i++)
    if (entries[i].action == A_BOOTWAIT) {
      run_and_wait(&entries[i]);
      entries[i].started = 1;
    } else if (entries[i].action == A_BOOT) {
      entries[i].pid = spawn(&entries[i]);
      entries[i].started = 1;
    }

  for (;;) {
    // what this runlevel asks for
    for (int i = 0; i < nentries; i++) {
      struct entry *e = &entries[i];
      if (!in_runlevel(e) || e->action == A_OFF)
        continue;
      if ((e->action == A_WAIT || e->action == A_ONCE) && !e->started) {
        e->started = 1;
        if (e->action == A_WAIT)
          run_and_wait(e);
        else
          e->pid = spawn(e);
        continue;
      }
      if (e->action == A_RESPAWN && e->pid == 0 && !stopping) {
        // a program that dies at once, again and again, is a mistake
        // in the table: say so and stop trying rather than spend the
        // machine on it.
        long now = (long)time(0);
        if (now - e->last_start < 2 && ++e->failures >= 5) {
          printf("init: %s respawning too fast; disabled for now\n", e->id);
          e->action = A_OFF;
          continue;
        }
        if (now - e->last_start >= 2)
          e->failures = 0;
        e->last_start = now;
        // the line every test and every person waits for
        if (strstr(e->process, "/bin/sh") || strstr(e->process, "getty"))
          printf("init: starting sh\n");
        e->pid = spawn(e);
      }
    }

    int status;
    pid_t got = wait(&status);
    if (got < 0) {
      if (errno == EINTR) {
        if (reread_wanted) {
          reread_wanted = 0;
          // telinit leaves the runlevel it wants here before the
          // signal; without one this is just a re-read
          FILE *rf = fopen("/var/run/runlevel", "r");
          if (rf) {
            int want = fgetc(rf);
            fclose(rf);
            unlink("/var/run/runlevel");
            if (want >= '0' && want <= '6' && want - '0' != runlevel) {
              runlevel = want - '0';
              printf("init: entering runlevel %d\n", runlevel);
              // the once and wait entries of the new level have not
              // had their turn yet, and a respawn entry that is not in
              // it is stopped
              for (int i = 0; i < nentries; i++) {
                entries[i].started = 0;
                if (entries[i].pid > 0 && !in_runlevel(&entries[i]) &&
                    entries[i].action == A_RESPAWN) {
                  kill(entries[i].pid, SIGTERM);
                }
              }
              continue;
            }
          }
          printf("init: re-reading %s\n", INITTAB);
          // the entries that are running keep running: only the table
          // of what should be is replaced
          pid_t keep[MAXENTRY];
          char keepid[MAXENTRY][8];
          int nkeep = 0;
          for (int i = 0; i < nentries; i++)
            if (entries[i].pid > 0) {
              keep[nkeep] = entries[i].pid;
              snprintf(keepid[nkeep], sizeof(keepid[nkeep]), "%s", entries[i].id);
              nkeep++;
            }
          if (read_inittab() == 0)
            for (int k = 0; k < nkeep; k++)
              for (int i = 0; i < nentries; i++)
                if (strcmp(entries[i].id, keepid[k]) == 0)
                  entries[i].pid = keep[k];
        }
        if (shutdown_wanted) {
          int sig = shutdown_wanted;
          shutdown_wanted = 0;
          for (int i = 0; i < nentries; i++)
            if (entries[i].action == A_CTRLALTDEL) {
              run_and_wait(&entries[i]);
              break;
            }
          (void)sig;
        }
        continue;
      }
      // no children at all: wait for something to happen rather than
      // spin. a respawn entry will be started on the next pass.
      struct timespec ts = {0, 100 * 1000 * 1000};
      nanosleep(&ts, 0);
      continue;
    }
    for (int i = 0; i < nentries; i++)
      if (entries[i].pid == got)
        entries[i].pid = 0;
  }
}
