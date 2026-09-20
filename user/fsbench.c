//
// fsbench: what the file system underneath actually does, in numbers.
//
// it is written to be the same program on xv6H and on Linux, so that a
// figure from one can be put beside a figure from the other: nothing in
// here is outside POSIX, and the only thing it assumes about the file
// system is that it can make a directory and write files in it.
//
//   fsbench [-d DIR] [-s MB] [-b KB] [-n FILES] [-q]
//
// the phases, and what each is meant to show:
//
//   seqwrite    how fast bytes reach the disk in order, buffered
//   seqread     the same bytes back, from the cache and then not
//   randread    seek cost: one block at a time, at random offsets
//   randwrite   the same, writing, which also exercises the journal
//   fsyncsmall  durability per file: create, write, fsync, close
//   metadata    create, stat and unlink, which is all directory work
//   bigdir      one directory with many names: create, walk, look up
//   mixed       all of it at once, which is what a system really does
//
// each phase prints a line a person can read and a line a harness can
// parse; the parseable ones all begin "fsbench:".
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define MB (1024 * 1024)

static char *dir = "/bench";
static long total_bytes = 8 * MB;   // -s
static long blocksize = 64 * 1024;  // -b
static int nfiles = 100;            // -n
static int quiet;

static char *buf;
static char path[256];

// a reproducible pseudo-random sequence: the same offsets are visited on
// every machine, so two runs measure the same work.
static unsigned long long seed = 88172645463325252ULL;

static unsigned long
nextrand(void)
{
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return (unsigned long)(seed >> 11);
}

static double
now(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    return 0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
die(const char *what)
{
  fprintf(stderr, "fsbench: %s: %s\n", what, strerror(errno));
  exit(1);
}

static void
report(const char *name, const char *unit, double amount, double secs, const char *human)
{
  double rate = secs > 0 ? amount / secs : 0;

  if (!quiet)
    printf("%-11s %s\n", name, human);
  printf("fsbench: %s=%.3f secs=%.3f rate=%.2f unit=%s\n", name, amount, secs, rate, unit);
  fflush(stdout);
}

static void
filepath(const char *name)
{
  snprintf(path, sizeof(path), "%s/%s", dir, name);
}

// ---- the phases ----

static double
seq_write(void)
{
  long done = 0;
  double t0, t1;
  int fd;

  filepath("seq");
  fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0)
    die("open seq");
  t0 = now();
  while (done < total_bytes) {
    long chunk = total_bytes - done;
    if (chunk > blocksize)
      chunk = blocksize;
    if (write(fd, buf, chunk) != chunk)
      die("write seq");
    done += chunk;
  }
  if (fsync(fd) < 0)
    die("fsync seq");
  t1 = now();
  close(fd);
  return t1 - t0;
}

static double
seq_read(void)
{
  long done = 0;
  double t0, t1;
  int fd;

  filepath("seq");
  fd = open(path, O_RDONLY);
  if (fd < 0)
    die("open seq for reading");
  t0 = now();
  for (;;) {
    long n = read(fd, buf, blocksize);
    if (n < 0)
      die("read seq");
    if (n == 0)
      break;
    done += n;
  }
  t1 = now();
  close(fd);
  if (done != total_bytes) {
    fprintf(stderr, "fsbench: read back %ld bytes, wrote %ld\n", done, total_bytes);
    exit(1);
  }
  return t1 - t0;
}

// one block at a time, at offsets that do not follow one another
static double
rand_io(int writing, int ops, long iosize)
{
  double t0, t1;
  int fd;
  long nblocks = total_bytes / iosize;

  filepath("seq");
  fd = open(path, writing ? O_RDWR : O_RDONLY);
  if (fd < 0)
    die("open seq for random io");
  t0 = now();
  for (int i = 0; i < ops; i++) {
    long off = (long)(nextrand() % (unsigned long)nblocks) * iosize;
    long n = writing ? pwrite(fd, buf, iosize, off) : pread(fd, buf, iosize, off);
    if (n != iosize)
      die(writing ? "pwrite" : "pread");
  }
  if (writing && fsync(fd) < 0)
    die("fsync random");
  t1 = now();
  close(fd);
  return t1 - t0;
}

// what a program that cares about its data pays per file
static double
fsync_small(int n)
{
  double t0, t1;
  char name[64];

  t0 = now();
  for (int i = 0; i < n; i++) {
    snprintf(name, sizeof(name), "s%d", i);
    filepath(name);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
      die("open small");
    if (write(fd, buf, 1024) != 1024)
      die("write small");
    if (fsync(fd) < 0)
      die("fsync small");
    close(fd);
  }
  t1 = now();
  for (int i = 0; i < n; i++) {
    snprintf(name, sizeof(name), "s%d", i);
    filepath(name);
    unlink(path);
  }
  return t1 - t0;
}

// create, look at, remove: no data at all, only names and inodes
static double
metadata(int n)
{
  double t0, t1;
  char name[64];
  struct stat st;

  t0 = now();
  for (int i = 0; i < n; i++) {
    snprintf(name, sizeof(name), "m%d", i);
    filepath(name);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
      die("open meta");
    close(fd);
    if (stat(path, &st) < 0)
      die("stat meta");
    if (unlink(path) < 0)
      die("unlink meta");
  }
  t1 = now();
  return t1 - t0;
}

// a directory with enough names in it to be worth walking
static void
big_dir(int n, double *create, double *walk, double *lookup)
{
  char sub[256];
  double t0;
  struct stat st;

  snprintf(sub, sizeof(sub), "%s/big", dir);
  if (mkdir(sub, 0755) < 0 && errno != EEXIST)
    die("mkdir big");

  t0 = now();
  for (int i = 0; i < n; i++) {
    snprintf(path, sizeof(path), "%s/f%d", sub, i);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
      die("open in big");
    close(fd);
  }
  *create = now() - t0;

  t0 = now();
  DIR *d = opendir(sub);
  if (d == 0)
    die("opendir big");
  int seen = 0;
  while (readdir(d) != 0)
    seen++;
  closedir(d);
  *walk = now() - t0;
  if (seen < n) {
    fprintf(stderr, "fsbench: readdir saw %d of %d entries\n", seen, n);
    exit(1);
  }

  // every name, in an order the directory was not written in
  t0 = now();
  for (int i = 0; i < n; i++) {
    int k = (int)(nextrand() % (unsigned long)n);
    snprintf(path, sizeof(path), "%s/f%d", sub, k);
    if (stat(path, &st) < 0)
      die("stat in big");
  }
  *lookup = now() - t0;

  for (int i = 0; i < n; i++) {
    snprintf(path, sizeof(path), "%s/f%d", sub, i);
    unlink(path);
  }
  rmdir(sub);
}

// data and names together, the way a build or a mailbox behaves
static double
mixed(int rounds)
{
  double t0, t1;
  char name[64];

  t0 = now();
  for (int i = 0; i < rounds; i++) {
    snprintf(name, sizeof(name), "x%d", i % 8);
    filepath(name);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
      die("open mixed");
    for (int k = 0; k < 8; k++)
      if (write(fd, buf, 4096) != 4096)
        die("write mixed");
    if (lseek(fd, 0, SEEK_SET) < 0)
      die("lseek mixed");
    for (int k = 0; k < 8; k++)
      if (read(fd, buf, 4096) != 4096)
        die("read mixed");
    if ((i % 4) == 0 && fsync(fd) < 0)
      die("fsync mixed");
    close(fd);
    if ((i % 3) == 2)
      unlink(path);
  }
  t1 = now();
  for (int i = 0; i < 8; i++) {
    snprintf(name, sizeof(name), "x%d", i);
    filepath(name);
    unlink(path);
  }
  return t1 - t0;
}

// ---- the program ----

static void
usage(void)
{
  fprintf(stderr, "usage: fsbench [-d DIR] [-s MB] [-b KB] [-n FILES] [-q]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  char human[128];
  double secs;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
      dir = argv[++i];
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
      total_bytes = atol(argv[++i]) * MB;
    else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
      blocksize = atol(argv[++i]) * 1024;
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      nfiles = atoi(argv[++i]);
    else if (strcmp(argv[i], "-q") == 0)
      quiet = 1;
    else
      usage();
  }
  if (total_bytes <= 0 || blocksize <= 0 || nfiles <= 0)
    usage();

  buf = malloc(blocksize);
  if (buf == 0) {
    fprintf(stderr, "fsbench: no memory for a %ld-byte buffer\n", blocksize);
    exit(1);
  }
  for (long i = 0; i < blocksize; i++)
    buf[i] = (char)(i + 1);

  if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    die("mkdir");

  printf("fsbench: dir=%s size=%ldMB block=%ldKB files=%d\n", dir, total_bytes / MB,
         blocksize / 1024, nfiles);

  secs = seq_write();
  snprintf(human, sizeof(human), "%ld MB in %.3fs = %.2f MB/s", total_bytes / MB, secs,
           (double)total_bytes / MB / secs);
  report("seqwrite", "MB/s", (double)total_bytes / MB, secs, human);

  secs = seq_read();
  snprintf(human, sizeof(human), "%ld MB in %.3fs = %.2f MB/s", total_bytes / MB, secs,
           (double)total_bytes / MB / secs);
  report("seqread", "MB/s", (double)total_bytes / MB, secs, human);

  int ops = 512;
  secs = rand_io(0, ops, 4096);
  snprintf(human, sizeof(human), "%d reads of 4KB in %.3fs = %.0f ops/s", ops, secs, ops / secs);
  report("randread", "ops/s", ops, secs, human);

  secs = rand_io(1, ops, 4096);
  snprintf(human, sizeof(human), "%d writes of 4KB in %.3fs = %.0f ops/s", ops, secs, ops / secs);
  report("randwrite", "ops/s", ops, secs, human);

  secs = fsync_small(nfiles);
  snprintf(human, sizeof(human), "%d files created and fsynced in %.3fs = %.0f files/s", nfiles,
           secs, nfiles / secs);
  report("fsyncsmall", "files/s", nfiles, secs, human);

  int nmeta = nfiles * 5;
  secs = metadata(nmeta);
  snprintf(human, sizeof(human), "%d create+stat+unlink in %.3fs = %.0f ops/s", nmeta, secs,
           nmeta / secs);
  report("metadata", "ops/s", nmeta, secs, human);

  double create, walk, look;
  int nbig = nfiles * 5;
  big_dir(nbig, &create, &walk, &look);
  snprintf(human, sizeof(human), "%d entries: create %.3fs, readdir %.3fs, %d lookups %.3fs", nbig,
           create, walk, nbig, look);
  report("bigdir", "entries/s", nbig, create + walk + look, human);

  int rounds = nfiles / 2 > 0 ? nfiles / 2 : 1;
  secs = mixed(rounds);
  snprintf(human, sizeof(human), "%d rounds in %.3fs = %.1f rounds/s", rounds, secs,
           rounds / secs);
  report("mixed", "rounds/s", rounds, secs, human);

  filepath("seq");
  unlink(path);
  rmdir(dir);
  printf("fsbench: done\n");
  return 0;
}
