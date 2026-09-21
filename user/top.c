// top: the processes, sorted, kept up to date.
//
//   top [-b] [-n COUNT] [-d SECONDS] [-u USER] [-p PID] [-o FIELD]
//
//   -b  batch: print and go, with no terminal tricks
//   -n  stop after this many screens
//   -d  seconds between them
//   -u  only this user's processes
//   -p  only this process
//   -o  sort by cpu, mem, pid or time
//
// while it is running:
//
//   P  sort by processor time   M  by memory   N  by pid   T  by time
//   k  kill a process           r  renice one
//   u  pick a user              q  leave
//
// everything comes from /proc: the per-process directories for the
// processes, meminfo and uptime for the summary. the processor share
// is the time a process used between one screen and the next, which
// is the only figure that means anything -- the total since it
// started is in the TIME column beside it.
#include "user/user.h"
#include <pwd.h>
#include <sys/ioctl.h>
#include <termios.h>

#define MAXPROC 1024

static int batch, rows = 24, cols = 80;
static long delay = 2, screens;
static int only_uid = -1, only_pid = -1;
static int sort_by = 'P';
static struct termios saved;
static int raw_on;

struct entry {
  int pid, ppid, uid;
  char name[32];
  char state;
  long long utime, stime;     // ticks since it started
  long long last_total;       // what it had on the screen before
  long long pages;
  long long share;            // ticks used since the last screen
};

static struct entry procs[MAXPROC];
static int nprocs;
static struct entry before[MAXPROC];
static int nbefore;

static void
raw_mode(int on)
{
  struct termios t;

  if (batch || !isatty(0))
    return;
  if (on) {
    if (tcgetattr(0, &saved) < 0)
      return;
    t = saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;    // a read that finds nothing comes straight back
    tcsetattr(0, TCSANOW, &t);
    raw_on = 1;
  } else if (raw_on) {
    tcsetattr(0, TCSANOW, &saved);
    raw_on = 0;
  }
}

static void
leave(int code)
{
  raw_mode(0);
  if (!batch)
    printf("\033[?25h");     // the cursor comes back
  fflush(stdout);
  exit(code);
}

static void
on_signal(int sig)
{
  (void)sig;
  leave(0);
}

static void
get_size(void)
{
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 4 && ws.ws_col > 20) {
    rows = ws.ws_row;
    cols = ws.ws_col;
  }
}

static long long
read_key(const char *path, const char *key)
{
  FILE *f = fopen(path, "r");
  char line[256];
  int n = strlen(key);

  if (f == 0)
    return -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key, n) == 0 && line[n] == ':') {
      const char *p = line + n + 1;
      while (*p == ' ' || *p == '\t')
        p++;
      fclose(f);
      return atoll(p);
    }
  }
  fclose(f);
  return -1;
}

// "PID (name) S ppid pgid sid tty utime stime pages"
static int
read_proc(int pid, struct entry *e)
{
  char path[64], line[512];
  FILE *f;

  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  f = fopen(path, "r");
  if (f == 0)
    return -1;
  if (fgets(line, sizeof(line), f) == 0) {
    fclose(f);
    return -1;
  }
  fclose(f);

  memset(e, 0, sizeof(*e));
  e->pid = pid;
  const char *open_paren = strchr(line, '(');
  const char *close_paren = strrchr(line, ')');
  if (open_paren && close_paren && close_paren > open_paren) {
    int n = close_paren - open_paren - 1;
    if (n > (int)sizeof(e->name) - 1)
      n = sizeof(e->name) - 1;
    memmove(e->name, open_paren + 1, n);
    e->name[n] = 0;
  }
  const char *p = close_paren ? close_paren + 1 : line;
  while (*p == ' ')
    p++;
  e->state = *p ? *p : '?';
  // ppid pgid sid tty utime stime pages
  long long v[8];
  int k = 0;
  while (*p && k < 8) {
    while (*p && *p != ' ')
      p++;
    while (*p == ' ')
      p++;
    if (*p < '0' || *p > '9')
      break;
    v[k++] = atoll(p);
  }
  if (k >= 7) {
    e->ppid = (int)v[0];
    e->utime = v[4];
    e->stime = v[5];
    e->pages = v[6];
  }

  // the owner comes from the directory itself
  snprintf(path, sizeof(path), "/proc/%d", pid);
  struct stat st;
  e->uid = stat(path, &st) == 0 ? (int)st.uid : 0;
  return 0;
}

static int
gather(void)
{
  DIR *d = opendir("/proc");
  struct dirent *de;

  nprocs = 0;
  if (d == 0) {
    fprintf(stderr, "top: /proc: %s\n", strerror(errno));
    leave(1);
  }
  while ((de = readdir(d)) != 0 && nprocs < MAXPROC) {
    int pid = 0, ok = de->d_name[0] != 0;
    for (const char *p = de->d_name; *p; p++) {
      if (*p < '0' || *p > '9') {
        ok = 0;
        break;
      }
      pid = pid * 10 + (*p - '0');
    }
    if (!ok)
      continue;
    if (only_pid >= 0 && pid != only_pid)
      continue;
    struct entry e;
    if (read_proc(pid, &e) < 0)
      continue;
    if (only_uid >= 0 && e.uid != only_uid)
      continue;

    // what it used since the last screen
    long long total = e.utime + e.stime;
    e.share = total;
    for (int i = 0; i < nbefore; i++)
      if (before[i].pid == pid) {
        e.share = total - (before[i].utime + before[i].stime);
        if (e.share < 0)
          e.share = 0;
        break;
      }
    procs[nprocs++] = e;
  }
  closedir(d);
  return nprocs;
}

static int
compare(const void *va, const void *vb)
{
  const struct entry *a = va, *b = vb;

  switch (sort_by) {
  case 'M':
    if (a->pages != b->pages)
      return a->pages < b->pages ? 1 : -1;
    break;
  case 'N':
    return a->pid - b->pid;
  case 'T': {
    long long ta = a->utime + a->stime, tb = b->utime + b->stime;
    if (ta != tb)
      return ta < tb ? 1 : -1;
    break;
  }
  default:
    if (a->share != b->share)
      return a->share < b->share ? 1 : -1;
    break;
  }
  return a->pid - b->pid;
}

static void
user_name(int uid, char *out, int size)
{
  struct passwd *pw = getpwuid(uid);

  if (pw)
    snprintf(out, size, "%s", pw->pw_name);
  else
    snprintf(out, size, "%d", uid);
}

static void
draw(void)
{
  long long total = read_key("/proc/meminfo", "MemTotal");
  long long freem = read_key("/proc/meminfo", "MemFree");
  long long cached = read_key("/proc/meminfo", "Cached");
  long long nproc = read_key("/proc/stat", "processes");

  if (!batch)
    printf("\033[H\033[2J");

  // the summary
  long long up = 0;
  FILE *f = fopen("/proc/uptime", "r");
  if (f) {
    char line[64];
    if (fgets(line, sizeof(line), f))
      up = atoll(line);
    fclose(f);
  }
  printf("top - up %lld:%02lld:%02lld, %lld processes\n", up / 3600,
         (up / 60) % 60, up % 60, nproc);
  printf("Mem: %lld kB total, %lld kB free, %lld kB used, %lld kB cached\n",
         total, freem, total - freem, cached);
  printf("Sort: %s   (P cpu  M mem  N pid  T time  k kill  r renice  q quit)\n",
         sort_by == 'M' ? "memory" : sort_by == 'N' ? "pid"
                                   : sort_by == 'T' ? "time" : "cpu");
  printf("\n");
  printf("  PID USER      STAT  %%CPU    MEM(kB)     TIME COMMAND\n");

  int show = batch ? nprocs : rows - 6;
  if (show > nprocs)
    show = nprocs;
  // the share is a fraction of the ticks that went by, which is the
  // delay times a hundred
  long long window = delay * 100;
  if (window <= 0)
    window = 1;
  for (int i = 0; i < show; i++) {
    struct entry *e = &procs[i];
    char u[16];
    user_name(e->uid, u, sizeof(u));
    long long t = e->utime + e->stime;
    long long pct = e->share * 1000 / window;
    printf("%5d %-9s %c   %3lld.%lld %10lld %4lld:%02lld.%02lld %s\n", e->pid, u,
           e->state, pct / 10, pct % 10, e->pages * 4, t / 6000,
           (t / 100) % 60, t % 100, e->name);
  }
  fflush(stdout);
}

// a line typed on the bottom row, for k and r
static int
ask(const char *prompt, char *out, int size)
{
  int n = 0;

  raw_mode(0);
  printf("\r\033[K%s", prompt);
  fflush(stdout);
  if (fgets(out, size, stdin) == 0) {
    raw_mode(1);
    return -1;
  }
  n = strlen(out);
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
    out[--n] = 0;
  raw_mode(1);
  return n;
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    int took_value = 0;
    for (const char *p = argv[i] + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'b': batch = 1; break;
      case 'H': case 'c': break;
      case 'n': case 'd': case 'u': case 'p': case 'o': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0) {
          fprintf(stderr, "usage: top [-b] [-n N] [-d SEC] [-u USER]"
                          " [-p PID] [-o FIELD]\n");
          exit(1);
        }
        switch (*p) {
        case 'n': screens = atol(v); break;
        case 'd': delay = atol(v) > 0 ? atol(v) : 1; break;
        case 'p': only_pid = atoi(v); break;
        case 'o':
          sort_by = (*v == 'm' || *v == 'M') ? 'M'
                    : (*v == 'p' || *v == 'P') ? (v[1] == 'i' ? 'N' : 'P')
                    : (*v == 't' || *v == 'T') ? 'T' : 'P';
          break;
        case 'u': {
          struct passwd *pw = getpwnam(v);
          only_uid = pw ? (int)pw->pw_uid : atoi(v);
          break;
        }
        }
        took_value = 1;
        break;
      }
      default:
        fprintf(stderr, "usage: top [-b] [-n N] [-d SEC] [-u USER]"
                        " [-p PID] [-o FIELD]\n");
        exit(1);
      }
    }
  }

  if (!isatty(1))
    batch = 1;
  get_size();
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  if (!batch) {
    raw_mode(1);
    printf("\033[?25l");    // hide the cursor while it redraws
  }

  for (long screen = 0; screens == 0 || screen < screens; screen++) {
    gather();
    qsort(procs, nprocs, sizeof(procs[0]), compare);
    draw();
    for (int k = 0; k < nprocs && k < MAXPROC; k++)
      before[k] = procs[k];
    nbefore = nprocs;

    if (screens && screen + 1 >= screens)
      break;
    if (batch) {
      sleep(delay);
      continue;
    }

    // wait for the delay, looking now and then for a key
    for (long t = 0; t < delay * 10; t++) {
      char c;
      int n = read(0, &c, 1);
      if (n == 1) {
        if (c == 'q' || c == 'Q')
          leave(0);
        if (c == 'P' || c == 'M' || c == 'N' || c == 'T') {
          sort_by = c;
          break;
        }
        if (c == 'k') {
          char buf[32];
          if (ask("PID to signal: ", buf, sizeof(buf)) > 0) {
            int pid = atoi(buf);
            if (ask("signal [15]: ", buf, sizeof(buf)) >= 0) {
              int sig = buf[0] ? atoi(buf) : SIGTERM;
              if (kill(pid, sig) < 0) {
                printf("\r\033[Kkill %d: %s", pid, strerror(errno));
                fflush(stdout);
              }
            }
          }
          break;
        }
        if (c == 'r') {
          char buf[32];
          if (ask("PID to renice: ", buf, sizeof(buf)) > 0) {
            int pid = atoi(buf);
            if (ask("value: ", buf, sizeof(buf)) > 0)
              if (setpriority(PRIO_PROCESS, pid, atoi(buf)) < 0) {
                printf("\r\033[Krenice %d: %s", pid, strerror(errno));
                fflush(stdout);
              }
          }
          break;
        }
        if (c == 'u') {
          char buf[32];
          if (ask("user (empty for all): ", buf, sizeof(buf)) >= 0) {
            if (buf[0] == 0) {
              only_uid = -1;
            } else {
              struct passwd *pw = getpwnam(buf);
              only_uid = pw ? (int)pw->pw_uid : atoi(buf);
            }
          }
          break;
        }
        if (c == ' ')
          break;   // redraw now
      }
      struct timespec ts = {0, 100 * 1000 * 1000};
      nanosleep(&ts, 0);
    }
  }
  leave(0);
}
