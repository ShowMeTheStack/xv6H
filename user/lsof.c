// lsof and fuser: which process has what open.
//
//   lsof [-p PID] [-u USER] [-c NAME] [-t] [FILE|DIR...]
//   fuser [-v] [-k] [-s] [-SIGNAL] FILE...
//
//   -p  only this process      -u  only this user
//   -c  only processes with this name
//   -t  only the process numbers, for feeding to kill
//   -k  fuser: signal the processes it finds
//   -s  fuser: say nothing; the exit status is the answer
//
// what a process has open is read from /proc/PID/fd, where each
// descriptor is a link naming what it points at.
//
// this kernel keeps no path for an open file -- there is no dentry
// cache, and getcwd walks up from the working directory in the
// library rather than asking for a name. so a descriptor names what
// it holds by identity instead: `ext4:[DEV:INO]` for a file, and
// `pipe:[N]` or `socket:[N]` for the things that never had a name.
// a FILE argument is turned into its own device and inode before the
// comparison, so `lsof /etc/passwd` and `fuser /var/log/messages`
// find what has them open however that name was spelled -- which is
// the question being asked, and a more exact answer than matching
// the text of a path would give.
#include "user/user.h"
#include <pwd.h>

static int terse, verbose, do_kill, silent, kill_signal = SIGKILL;
static int only_pid = -1, only_uid = -1;
static const char *only_cmd;
static int found_any;
static int status_no_such;

struct match {
  int pid;
  int uid;
  char cmd[32];
  int fd;
  char how[8];      // cwd, root, txt, or the number
  char path[PATH_MAX];
};

static void
process_name(int pid, char *out, int size)
{
  char path[64], line[256];
  FILE *f;

  out[0] = 0;
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  f = fopen(path, "r");
  if (f == 0)
    return;
  if (fgets(line, sizeof(line), f)) {
    const char *a = strchr(line, '(');
    const char *b = strrchr(line, ')');
    if (a && b && b > a) {
      int n = b - a - 1;
      if (n > size - 1)
        n = size - 1;
      memmove(out, a + 1, n);
      out[n] = 0;
    }
  }
  fclose(f);
}

static int
process_uid(int pid)
{
  char path[64];
  struct stat st;

  snprintf(path, sizeof(path), "/proc/%d", pid);
  return stat(path, &st) == 0 ? (int)st.uid : 0;
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

// what a link in /proc/PID/fd says: the kind, and the numbers that
// name the object
struct ident {
  char kind[16];
  long dev, ino;
};

static int
parse_ident(const char *s, struct ident *id)
{
  memset(id, 0, sizeof(*id));
  id->dev = -1;
  id->ino = -1;

  const char *colon = strchr(s, ':');
  if (colon == 0)
    return -1;
  int n = colon - s;
  if (n > (int)sizeof(id->kind) - 1)
    n = sizeof(id->kind) - 1;
  memmove(id->kind, s, n);
  id->kind[n] = 0;

  const char *p = strchr(colon, '[');
  if (p == 0)
    return -1;
  p++;
  long a = atol(p);
  const char *inner = strchr(p, ':');
  if (inner) {
    id->dev = a;
    id->ino = atol(inner + 1);
  } else {
    id->ino = a;
  }
  return 0;
}

// the thing the user named, as the numbers that identify it
struct target {
  long dev, ino;
  long major, minor;   // a device file names itself by these instead
  int is_device;
  int is_dir;
  const char *name;
};

static int
same_object(const char *link, const struct target *t)
{
  struct ident id;

  if (parse_ident(link, &id) < 0)
    return 0;
  // a device descriptor names the driver it reaches, not the inode of
  // the file that was opened to reach it, so that is what it is
  // compared against
  if (strcmp(id.kind, "dev") == 0) {
    if (!t->is_device)
      return 0;
    return id.dev == t->major && id.ino == t->minor;
  }
  if (t->is_device)
    return 0;
  if (id.dev >= 0 && t->dev >= 0 && id.dev != t->dev)
    return 0;
  return id.ino == t->ino;
}

static int header_done;

static void
report(const struct match *m)
{
  char u[16];

  found_any = 1;
  if (terse) {
    printf("%d\n", m->pid);
    return;
  }
  user_name(m->uid, u, sizeof(u));
  if (!header_done) {
    printf("COMMAND     PID USER      FD      NAME\n");
    header_done = 1;
  }
  printf("%-11s %4d %-9s %-7s %s\n", m->cmd, m->pid, u, m->how, m->path);
}

// every descriptor of one process, and its cwd and root
static void
scan_process(int pid, const struct target *wants, int nwants,
             void (*hit)(const struct match *))
{
  struct match m;
  char dirpath[64], linkpath[128];

  memset(&m, 0, sizeof(m));
  m.pid = pid;
  m.uid = process_uid(pid);
  process_name(pid, m.cmd, sizeof(m.cmd));

  if (only_uid >= 0 && m.uid != only_uid)
    return;
  if (only_cmd && strcmp(m.cmd, only_cmd) != 0)
    return;

  // the two links every process has, then its descriptors
  static const char *special[] = {"cwd", "root", 0};
  for (int s = 0; special[s]; s++) {
    snprintf(linkpath, sizeof(linkpath), "/proc/%d/%s", pid, special[s]);
    int n = readlink(linkpath, m.path, sizeof(m.path) - 1);
    if (n <= 0)
      continue;
    m.path[n] = 0;
    m.fd = -1;
    snprintf(m.how, sizeof(m.how), "%s", special[s]);
    if (nwants == 0) {
      hit(&m);
    } else {
      for (int w = 0; w < nwants; w++)
        if (same_object(m.path, &wants[w])) {
          hit(&m);
          break;
        }
    }
  }

  snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", pid);
  DIR *d = opendir(dirpath);
  if (d == 0)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    int fd = atoi(e->d_name);
    snprintf(linkpath, sizeof(linkpath), "/proc/%d/fd/%s", pid, e->d_name);
    int n = readlink(linkpath, m.path, sizeof(m.path) - 1);
    if (n <= 0)
      continue;
    m.path[n] = 0;
    m.fd = fd;
    snprintf(m.how, sizeof(m.how), "%d", fd);
    if (nwants == 0) {
      hit(&m);
      continue;
    }
    for (int w = 0; w < nwants; w++)
      if (same_object(m.path, &wants[w])) {
        hit(&m);
        break;
      }
  }
  closedir(d);
}

static int killed_pids[256];
static int nkilled;

static void
fuser_hit(const struct match *m)
{
  found_any = 1;
  for (int i = 0; i < nkilled; i++)
    if (killed_pids[i] == m->pid)
      return;      // one line per process, not one per descriptor
  if (nkilled < 256)
    killed_pids[nkilled++] = m->pid;

  if (silent)
    return;
  if (verbose) {
    char u[16];
    user_name(m->uid, u, sizeof(u));
    fprintf(stderr, "%-16s %5d %-8s %s\n", m->path, m->pid, u, m->cmd);
  } else {
    printf("%d%s", m->pid, m->fd < 0 ? "c" : "");
    printf(" ");
  }
}

static int
signal_by_name(const char *s)
{
  static const struct {
    const char *name;
    int sig;
  } names[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
    {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {0, 0}
  };

  if (*s >= '0' && *s <= '9')
    return atoi(s);
  if (strncasecmp(s, "SIG", 3) == 0)
    s += 3;
  for (int i = 0; names[i].name; i++)
    if (strcasecmp(s, names[i].name) == 0)
      return names[i].sig;
  return -1;
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  struct target wants[64];
  int nwants = 0, i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  int is_fuser = strcmp(me, "fuser") == 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (is_fuser && signal_by_name(a + 1) > 0 &&
        strcmp(a, "-v") != 0 && strcmp(a, "-k") != 0 && strcmp(a, "-s") != 0) {
      kill_signal = signal_by_name(a + 1);
      continue;
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 't': terse = 1; break;
      case 'v': verbose = 1; break;
      case 'k': do_kill = 1; break;
      case 's': silent = 1; break;
      case 'm': break;
      case 'n': case 'a': break;
      case 'p': case 'u': case 'c': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0) {
          fprintf(stderr, "usage: %s ...\n", me);
          exit(1);
        }
        if (*p == 'p') {
          only_pid = atoi(v);
        } else if (*p == 'u') {
          struct passwd *pw = getpwnam(v);
          only_uid = pw ? (int)pw->pw_uid : atoi(v);
        } else {
          only_cmd = v;
        }
        took_value = 1;
        break;
      }
      default:
        fprintf(stderr, "usage: %s [-p PID] [-u USER] [-c NAME] [-t]"
                        " [FILE...]\n", me);
        exit(1);
      }
    }
  }

  for (; i < argc && nwants < 64; i++) {
    struct stat st;
    if (stat(argv[i], &st) < 0) {
      fprintf(stderr, "%s: %s: %s\n", me, argv[i], strerror(errno));
      status_no_such = 1;
      continue;
    }
    wants[nwants].dev = st.dev;
    wants[nwants].ino = st.ino;
    wants[nwants].is_device = st.type == T_DEVICE;
    wants[nwants].major = (st.rdev >> 8) & 0xff;
    wants[nwants].minor = st.rdev & 0xff;
    wants[nwants].is_dir = st.type == T_DIR;
    wants[nwants].name = argv[i];
    nwants++;
  }
  if (is_fuser && nwants == 0) {
    fprintf(stderr, "usage: fuser [-v] [-k] [-SIGNAL] FILE...\n");
    exit(1);
  }

  DIR *d = opendir("/proc");
  if (d == 0) {
    fprintf(stderr, "%s: /proc: %s\n", me, strerror(errno));
    exit(1);
  }
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    int pid = 0, ok = e->d_name[0] != 0;
    for (const char *p = e->d_name; *p; p++) {
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
    scan_process(pid, wants, nwants, is_fuser ? fuser_hit : report);
  }
  closedir(d);

  if (is_fuser) {
    if (!silent && !verbose && found_any)
      printf("\n");
    if (do_kill)
      for (int k = 0; k < nkilled; k++)
        if (killed_pids[k] != getpid() && kill(killed_pids[k], kill_signal) < 0)
          fprintf(stderr, "fuser: kill %d: %s\n", killed_pids[k],
                  strerror(errno));
    exit(found_any ? 0 : 1);
  }
  exit(status_no_such ? 1 : (found_any || nwants == 0 ? 0 : 1));
}
