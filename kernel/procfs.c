//
// /proc: the kernel's state as files.
//
// nothing here is stored. a file's contents are generated when it is
// read, into a buffer the size of one page, and the read then takes
// its slice of that. this is how Linux's procfs works for its small
// files, and it keeps the file system itself to a table of names and
// the functions behind them.
//
// the inode numbers are made up: a fixed number for each of the files
// at the top level, and for a process's own directory the pid shifted
// up with the file's index in the low bits, so an inode number says
// what it refers to without a table.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "defs.h"
#include "blk.h"
#include "vfs.h"
#include "page.h"
#include "mm.h"
#include "platform.h"
#include "errno.h"

#define PROCFS_DEV 32

// the inode numbers of what lives at the top level
#define PINO_ROOT      1
#define PINO_MOUNTS    2
#define PINO_MEMINFO   3
#define PINO_CPUINFO   4
#define PINO_UPTIME    5
#define PINO_VERSION   6
#define PINO_FILESYSTEMS 7
#define PINO_PARTITIONS  8
#define PINO_DISKSTATS   9
#define PINO_SELF       10
#define PINO_STAT       11
#define PINO_FIRST_PID 1024

// a process's files: the inode number is (pid << 4) + index + base
#define PPID_DIR     0
#define PPID_STATUS  1
#define PPID_CMDLINE 2
#define PPID_STAT    3
#define PPID_MAPS    4
#define PPID_NFILES  5

static const char *const pid_files[PPID_NFILES] = {0, "status", "cmdline", "stat", "maps"};

struct topfile {
  const char *name;
  uint inum;
};

static const struct topfile topfiles[] = {
    {"mounts", PINO_MOUNTS},         {"meminfo", PINO_MEMINFO},
    {"cpuinfo", PINO_CPUINFO},       {"uptime", PINO_UPTIME},
    {"version", PINO_VERSION},       {"filesystems", PINO_FILESYSTEMS},
    {"partitions", PINO_PARTITIONS}, {"diskstats", PINO_DISKSTATS},
    {"self", PINO_SELF},             {"stat", PINO_STAT},
    {0, 0},
};

static const struct inode_ops procfs_iops;
static struct super *proc_sb;

// ---- generating a file ----

// a small printf into a fixed buffer, which is all these files need
struct gen {
  char *buf;
  int len, cap;
};

static void
gen_str(struct gen *g, const char *s)
{
  while (*s && g->len < g->cap - 1)
    g->buf[g->len++] = *s++;
}

static void
gen_num(struct gen *g, uint64 v)
{
  char tmp[24];
  int n = 0;

  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  }
  while (n > 0 && g->len < g->cap - 1)
    g->buf[g->len++] = tmp[--n];
}

static void
gen_kv(struct gen *g, const char *key, uint64 value, const char *unit)
{
  gen_str(g, key);
  gen_str(g, ":\t");
  gen_num(g, value);
  if (unit) {
    gen_str(g, " ");
    gen_str(g, unit);
  }
  gen_str(g, "\n");
}

static void
mounts_emit(const char *source, const char *path, const char *type, int readonly, void *arg)
{
  struct gen *g = arg;

  gen_str(g, source);
  gen_str(g, " ");
  gen_str(g, path);
  gen_str(g, " ");
  gen_str(g, type);
  gen_str(g, readonly ? " ro 0 0\n" : " rw 0 0\n");
}

extern uint ticks;

static void
gen_top(struct gen *g, uint inum)
{
  switch (inum) {
  case PINO_MOUNTS:
    vfs_list(mounts_emit, g);
    break;

  case PINO_MEMINFO: {
    struct meminfo mi;
    kmem_info(&mi);
    gen_kv(g, "MemTotal", mi.total_pages * 4, "kB");
    gen_kv(g, "MemFree", mi.free_pages * 4, "kB");
    gen_kv(g, "MemAvailable", mi.free_pages * 4, "kB");
    uint64 hits, misses, dirty;
    blk_cache_stats(&hits, &misses, &dirty);
    gen_kv(g, "Cached", 0, "kB");
    gen_kv(g, "Dirty", dirty * 4, "kB");
    gen_kv(g, "PageTables", mi.db_pages * 4, "kB");
    gen_kv(g, "Reserved", mi.reserve_pages * 4, "kB");
    break;
  }

  case PINO_CPUINFO:
    for (int i = 0; i < plat.ncpu; i++) {
      gen_str(g, "processor\t: ");
      gen_num(g, i);
      gen_str(g, "\nhart\t\t: ");
      gen_num(g, i);
      gen_str(g, "\nisa\t\t: rv64imafdc");
      gen_str(g, plat.has_h ? "h\n" : "\n");
      gen_str(g, "mmu\t\t: sv");
      gen_num(g, plat.mmu_levels == 4 ? 48 : 39);
      gen_str(g, "\n\n");
    }
    break;

  case PINO_UPTIME: {
    uint64 ns = clock_monotonic_ns();
    gen_num(g, ns / 1000000000UL);
    gen_str(g, ".");
    gen_num(g, (ns / 10000000UL) % 100);
    gen_str(g, " 0.00\n");
    break;
  }

  case PINO_VERSION:
    gen_str(g, "xv6H (riscv64) hypervisor and operating system\n");
    break;

  case PINO_FILESYSTEMS:
    gen_str(g, "\text4\n\tprocfs\n\txv6\n");
    break;

  case PINO_PARTITIONS:
    gen_str(g, "major minor  #blocks  name\n\n");
    for (int i = 0; i < MAXDISK; i++) {
      struct gendisk *d = blk_by_index(i);
      if (d == 0)
        continue;
      gen_str(g, "   8 ");
      gen_num(g, i);
      gen_str(g, " ");
      gen_num(g, d->nsectors / 2);
      gen_str(g, " ");
      gen_str(g, d->name);
      gen_str(g, "\n");
    }
    break;

  case PINO_DISKSTATS:
    for (int i = 0; i < MAXDISK; i++) {
      struct gendisk *d = blk_by_index(i);
      if (d == 0)
        continue;
      gen_str(g, d->name);
      gen_str(g, " reads ");
      gen_num(g, d->reads);
      gen_str(g, " writes ");
      gen_num(g, d->writes);
      gen_str(g, "\n");
    }
    break;

  case PINO_STAT: {
    gen_str(g, "processes ");
    gen_num(g, proc_count());
    gen_str(g, "\nbtime 0\n");
    break;
  }

  case PINO_SELF:
    gen_num(g, myproc()->pid);
    gen_str(g, "\n");
    break;

  default:
    break;
  }
}

// one process's file
static void
gen_pid(struct gen *g, int pid, int which)
{
  struct proc *p = proc_get(pid);

  if (p == 0)
    return;
  switch (which) {
  case PPID_STATUS:
    gen_str(g, "Name:\t");
    gen_str(g, p->name);
    gen_str(g, "\nPid:\t");
    gen_num(g, p->pid);
    gen_str(g, "\nPPid:\t");
    gen_num(g, p->parent ? p->parent->pid : 0);
    gen_str(g, "\nState:\t");
    switch (p->state) {
    case RUNNING: gen_str(g, "R (running)"); break;
    case RUNNABLE: gen_str(g, "R (runnable)"); break;
    case SLEEPING: gen_str(g, "S (sleeping)"); break;
    case ZOMBIE: gen_str(g, "Z (zombie)"); break;
    default: gen_str(g, "? (unknown)"); break;
    }
    gen_str(g, "\nThreads:\t");
    gen_num(g, proc_nthreads(p));
    gen_str(g, "\nVmSize:\t");
    gen_num(g, p->mm ? p->mm->pages * 4 : 0);
    gen_str(g, " kB\n");
    break;

  case PPID_CMDLINE:
    gen_str(g, p->name);
    break;

  case PPID_STAT:
    gen_num(g, p->pid);
    gen_str(g, " (");
    gen_str(g, p->name);
    gen_str(g, ") ");
    gen_str(g, p->state == RUNNING ? "R " : (p->state == SLEEPING ? "S " : "R "));
    gen_num(g, p->parent ? p->parent->pid : 0);
    gen_str(g, " ");
    gen_num(g, p->pgid);
    gen_str(g, " ");
    gen_num(g, p->sid);
    gen_str(g, "\n");
    break;

  case PPID_MAPS:
    gen_str(g, "(areas are listed once the address space grows a reader)\n");
    break;

  default:
    break;
  }
  proc_release(p);
}

// ---- inodes ----

static int
is_pid_dir(uint inum, int *pid, int *which)
{
  if (inum < PINO_FIRST_PID)
    return 0;
  uint v = inum - PINO_FIRST_PID;
  *pid = v >> 4;
  *which = v & 0xf;
  return 1;
}

static uint
pid_inum(int pid, int which)
{
  return PINO_FIRST_PID + ((uint)pid << 4) + which;
}

static void
procfs_load(struct inode *ip)
{
  int pid, which;

  ip->nlink = 1;
  ip->major = 0;
  ip->minor = 0;
  ip->size = 0;
  if (ip->inum == PINO_ROOT) {
    ip->type = T_DIR;
    ip->nlink = 2;
  } else if (is_pid_dir(ip->inum, &pid, &which)) {
    ip->type = which == PPID_DIR ? T_DIR : T_FILE;
  } else {
    ip->type = T_FILE;
  }
  ip->valid = 1;
}

static int
procfs_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  char *page = kalloc();
  int pid, which;

  if (page == 0)
    return -ENOMEM;
  struct gen g = {page, 0, PGSIZE};
  if (is_pid_dir(ip->inum, &pid, &which))
    gen_pid(&g, pid, which);
  else
    gen_top(&g, ip->inum);
  g.buf[g.len] = 0;

  int avail = g.len - (int)off;
  if (avail <= 0) {
    kfree(page);
    return 0;
  }
  if ((int)n > avail)
    n = avail;
  if (either_copyout(user_dst, dst, page + off, n) < 0) {
    kfree(page);
    return -EFAULT;
  }
  kfree(page);
  return n;
}

static struct inode *
procfs_iget(uint inum)
{
  struct inode *ip = iget(PROCFS_DEV, inum);

  if (ip) {
    ip->sb = proc_sb;
    ip->ops = &procfs_iops;
  }
  return ip;
}

static struct inode *
procfs_lookup(struct inode *dp, char *name, uint *poff)
{
  int pid, which;

  if (poff)
    *poff = 0;
  if (dp->inum == PINO_ROOT) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(PINO_ROOT);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(PINO_ROOT);
    for (int i = 0; topfiles[i].name; i++)
      if (strncmp(name, topfiles[i].name, DIRSIZ) == 0)
        return procfs_iget(topfiles[i].inum);
    // a number is a process, if it is one
    int n = 0;
    for (char *p = name; *p; p++) {
      if (*p < '0' || *p > '9')
        return 0;
      n = n * 10 + (*p - '0');
    }
    struct proc *p = proc_get(n);
    if (p == 0)
      return 0;
    proc_release(p);
    return procfs_iget(pid_inum(n, PPID_DIR));
  }
  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_DIR) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(dp->inum);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(PINO_ROOT);
    for (int i = 1; i < PPID_NFILES; i++)
      if (pid_files[i] && strncmp(name, pid_files[i], DIRSIZ) == 0)
        return procfs_iget(pid_inum(pid, i));
  }
  return 0;
}

static int
procfs_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  int pid, which;

  if (dp->inum == PINO_ROOT) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    i -= 2;
    int ntop = 0;
    while (topfiles[ntop].name)
      ntop++;
    if (i < (uint64)ntop) {
      safestrcpy(name, topfiles[i].name, namemax);
      *ino = topfiles[i].inum;
      *type = 8;
      (*pos)++;
      return 1;
    }
    // then one directory per process
    uint64 want = i - ntop;
    uint64 seen = 0;
    proc_iter_start();
    for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
      if (p->state == UNUSED)
        continue;
      if (seen == want) {
        int pidv = p->pid;
        proc_iter_end();
        // the name is the number
        char tmp[16];
        int n = 0;
        int v = pidv;
        if (v == 0)
          tmp[n++] = '0';
        while (v) {
          tmp[n++] = '0' + v % 10;
          v /= 10;
        }
        int k = 0;
        while (n > 0 && k < namemax - 1)
          name[k++] = tmp[--n];
        name[k] = 0;
        *ino = pid_inum(pidv, PPID_DIR);
        *type = 4;
        (*pos)++;
        return 1;
      }
      seen++;
    }
    proc_iter_end();
    return 0;
  }

  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_DIR) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = dp->inum;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    i -= 1; // the files start at index 1 in pid_files
    if (i < PPID_NFILES && pid_files[i]) {
      safestrcpy(name, pid_files[i], namemax);
      *ino = pid_inum(pid, i);
      *type = 8;
      (*pos)++;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int
procfs_write(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  (void)ip;
  (void)user_src;
  (void)src;
  (void)off;
  (void)n;
  return -EACCES;
}

static const struct inode_ops procfs_iops = {
    .load = procfs_load,
    .read = procfs_read,
    .write = procfs_write,
    .lookup = procfs_lookup,
    .readdir = procfs_readdir,
};

static struct inode *
procfs_sb_iget(struct super *sb, uint inum)
{
  (void)sb;
  return procfs_iget(inum);
}

static void
procfs_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
              uint64 *bsize, uint64 *namelen)
{
  (void)sb;
  *blocks = *bfree = 0;
  *files = proc_count();
  *ffree = 0;
  *bsize = PGSIZE;
  *namelen = DIRSIZ;
}

static const struct super_ops procfs_sops = {
    .iget = procfs_sb_iget,
    .statfs = procfs_statfs,
};

static struct super *
procfs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)disk;
  (void)source;
  (void)readonly;
  (void)opts;

  struct super *sb = vfs_super_alloc();
  if (sb == 0)
    return 0;
  sb->ops = &procfs_sops;
  sb->iops = &procfs_iops;
  proc_sb = sb;
  sb->root = procfs_iget(PINO_ROOT);
  if (sb->root == 0) {
    vfs_super_free(sb);
    return 0;
  }
  return sb;
}

static struct fs_type procfs_type = {
    .name = "procfs",
    .mount = procfs_mount,
};

void
procfs_init(void)
{
  vfs_register(&procfs_type);
}
