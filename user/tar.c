//
// tar: a directory tree in one file.
//
//   tar c [v] [z] f ARCHIVE [-C DIR] FILE...   make one
//   tar x [v] [z] f ARCHIVE [-C DIR]           take one apart
//   tar t [v] [z] f ARCHIVE                    say what is in one
//
// the format is ustar, which is what every tar reads: a 512-byte
// header per member, the contents rounded up to 512, and two zero
// blocks at the end. GNU's long-name extension ('L' type) is read as
// well, because that is what a tar made elsewhere is likely to use for
// a path over a hundred characters; it is written too when a name does
// not fit the ustar prefix.
//
// z runs the archive through gzip, which is a separate program, so
// that neither has to know about the other.
//
#include "user/user.h"
#include <pwd.h>
#include <grp.h>

#define BLOCK 512

struct header {          // the ustar header, as POSIX defines it
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char chksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char pad[12];
};

static int verbose, use_gzip, status;
static const char *archive;
static const char *chdir_to;

// ---- the numbers in a header ----

static void
put_octal(char *field, int width, unsigned long long v)
{
  // width-1 digits and a NUL, which is what every tar writes
  for (int i = width - 2; i >= 0; i--) {
    field[i] = '0' + (v & 7);
    v >>= 3;
  }
  field[width - 1] = 0;
}

static unsigned long long
get_octal(const char *field, int width)
{
  unsigned long long v = 0;

  for (int i = 0; i < width; i++) {
    if (field[i] == 0 || field[i] == ' ')
      break;
    if (field[i] < '0' || field[i] > '7')
      continue;
    v = v * 8 + (unsigned)(field[i] - '0');
  }
  return v;
}

static unsigned
checksum(const struct header *h)
{
  const unsigned char *p = (const unsigned char *)h;
  unsigned sum = 0;

  for (int i = 0; i < BLOCK; i++)
    sum += (i >= 148 && i < 156) ? ' ' : p[i]; // the field reads as spaces
  return sum;
}

// ---- writing ----

static void
write_block(FILE *f, const void *buf, int n)
{
  char pad[BLOCK];

  if (fwrite(buf, 1, n, f) != (size_t)n) {
    fprintf(stderr, "tar: write: %s\n", strerror(errno));
    exit(1);
  }
  if (n % BLOCK) {
    memset(pad, 0, sizeof(pad));
    int fill = BLOCK - (n % BLOCK);
    if (fwrite(pad, 1, fill, f) != (size_t)fill) {
      fprintf(stderr, "tar: write: %s\n", strerror(errno));
      exit(1);
    }
  }
}

// a name too long for the 100-byte field goes in a block of its own
// first, which is GNU's way and which every tar understands
static void
write_long_name(FILE *f, const char *path)
{
  struct header h;

  memset(&h, 0, sizeof(h));
  snprintf(h.name, sizeof(h.name), "././@LongLink");
  put_octal(h.mode, sizeof(h.mode), 0644);
  put_octal(h.uid, sizeof(h.uid), 0);
  put_octal(h.gid, sizeof(h.gid), 0);
  put_octal(h.size, sizeof(h.size), strlen(path) + 1);
  put_octal(h.mtime, sizeof(h.mtime), 0);
  h.typeflag = 'L';
  memmove(h.magic, "ustar ", 6);
  memmove(h.version, " ", 2);
  put_octal(h.chksum, 7, checksum(&h)); // six digits and a NUL
  h.chksum[7] = ' ';
  write_block(f, &h, BLOCK);
  write_block(f, path, strlen(path) + 1);
}

static void archive_path(FILE *f, const char *path);

static void
archive_one(FILE *f, const char *path, struct stat *st)
{
  struct header h;
  char linktarget[PATH_MAX];

  memset(&h, 0, sizeof(h));
  if (strlen(path) >= sizeof(h.name))
    write_long_name(f, path);
  snprintf(h.name, sizeof(h.name), "%s", path);

  put_octal(h.mode, sizeof(h.mode), st->mode & 07777);
  put_octal(h.uid, sizeof(h.uid), st->uid);
  put_octal(h.gid, sizeof(h.gid), st->gid);
  put_octal(h.mtime, sizeof(h.mtime), st->mtime);
  memmove(h.magic, "ustar", 5);
  memmove(h.version, "00", 2);
  struct passwd *pw = getpwuid(st->uid);
  struct group *gr = getgrgid(st->gid);
  snprintf(h.uname, sizeof(h.uname), "%s", pw ? pw->pw_name : "root");
  snprintf(h.gname, sizeof(h.gname), "%s", gr ? gr->gr_name : "root");

  long long size = 0;
  if (st->type == T_DIR) {
    h.typeflag = '5';
    int n = strlen(h.name);
    if (n > 0 && h.name[n - 1] != '/' && n < (int)sizeof(h.name) - 1)
      h.name[n] = '/';
  } else if (st->type == T_SYMLINK) {
    h.typeflag = '2';
    int n = readlink(path, linktarget, sizeof(linktarget) - 1);
    if (n < 0)
      n = 0;
    linktarget[n] = 0;
    snprintf(h.linkname, sizeof(h.linkname), "%s", linktarget);
  } else if (st->type == T_DEVICE) {
    h.typeflag = '3'; // a character device; this system has no block nodes here
    put_octal(h.devmajor, sizeof(h.devmajor), (st->rdev >> 8) & 0xff);
    put_octal(h.devminor, sizeof(h.devminor), st->rdev & 0xff);
  } else {
    h.typeflag = '0';
    size = st->size;
  }
  put_octal(h.size, sizeof(h.size), size);
  put_octal(h.chksum, 7, checksum(&h)); // six digits and a NUL
  h.chksum[7] = ' ';
  write_block(f, &h, BLOCK);

  if (verbose)
    fprintf(stderr, "%s\n", path);

  if (size > 0) {
    int in = open(path, O_RDONLY);
    if (in < 0) {
      fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
      status = 1;
      // the header said how many bytes follow, so they have to
      long long left = size;
      char zero[BLOCK];
      memset(zero, 0, sizeof(zero));
      while (left > 0) {
        int n = left > BLOCK ? BLOCK : (int)left;
        write_block(f, zero, n);
        left -= n;
      }
      return;
    }
    char buf[BLOCK];
    long long left = size;
    while (left > 0) {
      int want = left > BLOCK ? BLOCK : (int)left;
      int n = read(in, buf, want);
      if (n <= 0) {
        memset(buf, 0, want);
        n = want;
      }
      write_block(f, buf, n);
      left -= n;
    }
    close(in);
  }
}

static void
archive_path(FILE *f, const char *path)
{
  struct stat st;

  if (lstat(path, &st) < 0) {
    fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  archive_one(f, path, &st);
  if (st.type != T_DIR)
    return;
  DIR *d = opendir(path);
  if (d == 0)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s%s%s", path,
             path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
    archive_path(f, child);
  }
  closedir(d);
}

// ---- reading ----

// make every directory on the way to this path
static void
make_path(const char *path)
{
  char copy[PATH_MAX];

  snprintf(copy, sizeof(copy), "%s", path);
  for (char *p = copy + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    mkdir(copy, 0755);
    *p = '/';
  }
}

static void
extract(FILE *f, int list_only)
{
  struct header h;
  char longname[PATH_MAX];
  char paxlink[PATH_MAX];
  int have_longname = 0, have_paxlink = 0;
  int zeroes = 0;

  for (;;) {
    if (fread(&h, 1, BLOCK, f) != BLOCK)
      break;
    // two zero blocks end the archive
    int all_zero = 1;
    for (int i = 0; i < BLOCK; i++)
      if (((char *)&h)[i]) {
        all_zero = 0;
        break;
      }
    if (all_zero) {
      if (++zeroes >= 2)
        break;
      continue;
    }
    zeroes = 0;

    unsigned want = (unsigned)get_octal(h.chksum, sizeof(h.chksum));
    if (want != checksum(&h)) {
      fprintf(stderr, "tar: a header's checksum is wrong; stopping\n");
      status = 1;
      return;
    }

    char name[PATH_MAX];
    if (have_longname) {
      snprintf(name, sizeof(name), "%s", longname);
      have_longname = 0;
    } else if (h.prefix[0]) {
      snprintf(name, sizeof(name), "%.155s/%.100s", h.prefix, h.name);
    } else {
      snprintf(name, sizeof(name), "%.100s", h.name);
    }
    long long size = (long long)get_octal(h.size, sizeof(h.size));
    int mode = (int)get_octal(h.mode, sizeof(h.mode));

    // a pax extended header: the real path (and other things this
    // program does not need) as text records in a member of its own,
    // describing the member that comes next. modern tars write these
    // rather than GNU's 'L' block.
    if (h.typeflag == 'x' || h.typeflag == 'X' || h.typeflag == 'g') {
      static char recs[16384];
      int n = 0;
      long long left = size;
      while (left > 0) {
        char buf[BLOCK];
        if (fread(buf, 1, BLOCK, f) != BLOCK)
          return;
        int take = left > BLOCK ? BLOCK : (int)left;
        if (n + take < (int)sizeof(recs)) {
          memmove(recs + n, buf, take);
          n += take;
        }
        left -= BLOCK;
      }
      if (h.typeflag == 'g')
        continue; // global defaults; nothing here needs them
      // each record is "LENGTH KEY=VALUE\n", where LENGTH counts
      // itself and the newline
      for (int at = 0; at < n;) {
        int reclen = 0, d = at;
        while (d < n && recs[d] >= '0' && recs[d] <= '9')
          reclen = reclen * 10 + (recs[d++] - '0');
        if (reclen <= 0 || at + reclen > n || d >= n || recs[d] != ' ')
          break;
        d++;
        int keyend = d;
        while (keyend < at + reclen && recs[keyend] != '=')
          keyend++;
        int vlen = at + reclen - keyend - 2; // past '=' and before '\n'
        if (vlen > 0 && vlen < PATH_MAX) {
          if (keyend - d == 4 && memcmp(recs + d, "path", 4) == 0) {
            memmove(longname, recs + keyend + 1, vlen);
            longname[vlen] = 0;
            have_longname = 1;
          } else if (keyend - d == 8 && memcmp(recs + d, "linkpath", 8) == 0) {
            memmove(paxlink, recs + keyend + 1, vlen);
            paxlink[vlen] = 0;
            have_paxlink = 1;
          }
        }
        at += reclen;
      }
      continue;
    }

    if (h.typeflag == 'L') {
      // the next header's name is in this member's contents
      long long left = size;
      int n = 0;
      while (left > 0) {
        char buf[BLOCK];
        if (fread(buf, 1, BLOCK, f) != BLOCK)
          return;
        int take = left > BLOCK ? BLOCK : (int)left;
        if (n + take < (int)sizeof(longname)) {
          memmove(longname + n, buf, take);
          n += take;
        }
        left -= BLOCK;
      }
      longname[n < (int)sizeof(longname) ? n : (int)sizeof(longname) - 1] = 0;
      have_longname = 1;
      continue;
    }

    if (list_only) {
      if (verbose)
        printf("%c %5d %8lld %s\n", h.typeflag, mode, size, name);
      else
        printf("%s\n", name);
    } else if (verbose) {
      fprintf(stderr, "%s\n", name);
    }

    // a member whose name leaves the archive's directory is refused:
    // an archive from elsewhere must not be able to write anywhere
    if (!list_only && (name[0] == '/' || strstr(name, "../"))) {
      fprintf(stderr, "tar: %s: refusing a name that leaves the directory\n", name);
      status = 1;
    } else if (!list_only) {
      switch (h.typeflag) {
      case '5':
        make_path(name);
        mkdir(name, mode ? mode : 0755);
        chmod(name, mode ? mode : 0755);
        break;
      case '2':
        make_path(name);
        unlink(name);
        if (symlink(have_paxlink ? paxlink : h.linkname, name) < 0) {
          fprintf(stderr, "tar: %s: %s\n", name, strerror(errno));
          status = 1;
        }
        break;
      case '1': {
        make_path(name);
        unlink(name);
        if (link(have_paxlink ? paxlink : h.linkname, name) < 0) {
          fprintf(stderr, "tar: %s: %s\n", name, strerror(errno));
          status = 1;
        }
        break;
      }
      case '3':
      case '4':
        make_path(name);
        unlink(name);
        mknod(name, (short)get_octal(h.devmajor, sizeof(h.devmajor)),
              (short)get_octal(h.devminor, sizeof(h.devminor)));
        break;
      default: {
        make_path(name);
        int out = open(name, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
        if (out < 0) {
          fprintf(stderr, "tar: %s: %s\n", name, strerror(errno));
          status = 1;
        }
        long long left = size;
        while (left > 0) {
          char buf[BLOCK];
          if (fread(buf, 1, BLOCK, f) != BLOCK)
            return;
          int take = left > BLOCK ? BLOCK : (int)left;
          if (out >= 0 && write(out, buf, take) != take) {
            fprintf(stderr, "tar: %s: %s\n", name, strerror(errno));
            status = 1;
          }
          left -= BLOCK;
        }
        if (out >= 0) {
          close(out);
          chmod(name, mode ? mode : 0644);
        }
        size = 0;
        break;
      }
      }
    }
    // skip whatever contents are left
    have_paxlink = 0;
    long long left = size;
    while (left > 0) {
      char buf[BLOCK];
      if (fread(buf, 1, BLOCK, f) != BLOCK)
        return;
      left -= BLOCK;
    }
  }
}

int
main(int argc, char *argv[])
{
  int create = 0, extract_mode = 0, list = 0, i = 1;

  if (argc < 2) {
    fprintf(stderr, "usage: tar {c|x|t}[v][z]f ARCHIVE [-C DIR] [FILE...]\n");
    exit(1);
  }
  // the first argument is the bundle of letters every tar takes
  for (const char *p = argv[1]; *p; p++) {
    switch (*p) {
    case 'c': create = 1; break;
    case 'x': extract_mode = 1; break;
    case 't': list = 1; break;
    case 'v': verbose = 1; break;
    case 'z': use_gzip = 1; break;
    case 'f':
      if (i + 1 < argc)
        archive = argv[++i];
      break;
    case '-': break;
    default:
      fprintf(stderr, "tar: unknown letter %c\n", *p);
      exit(1);
    }
  }
  i++;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
      chdir_to = argv[++i];
    else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      archive = argv[++i];
    else
      break;
  }
  if (create + extract_mode + list != 1) {
    fprintf(stderr, "tar: one of c, x or t is needed\n");
    exit(1);
  }
  if (archive == 0) {
    fprintf(stderr, "tar: f and an archive name are needed\n");
    exit(1);
  }

  FILE *f;
  char cmd[PATH_MAX + 64];
  if (create) {
    if (use_gzip) {
      snprintf(cmd, sizeof(cmd), "gzip > %s", archive);
      f = popen(cmd, "w");
    } else {
      f = strcmp(archive, "-") == 0 ? stdout : fopen(archive, "w");
    }
  } else {
    if (use_gzip) {
      snprintf(cmd, sizeof(cmd), "gunzip < %s", archive);
      f = popen(cmd, "r");
    } else {
      f = strcmp(archive, "-") == 0 ? stdin : fopen(archive, "r");
    }
  }
  if (f == 0) {
    fprintf(stderr, "tar: %s: %s\n", archive, strerror(errno));
    exit(1);
  }
  if (chdir_to && chdir(chdir_to) < 0) {
    fprintf(stderr, "tar: %s: %s\n", chdir_to, strerror(errno));
    exit(1);
  }

  if (create) {
    for (; i < argc; i++)
      archive_path(f, argv[i]);
    char zero[BLOCK * 2];
    memset(zero, 0, sizeof(zero));
    write_block(f, zero, sizeof(zero));
  } else {
    extract(f, list);
  }

  if (use_gzip)
    pclose(f);
  else if (f != stdout && f != stdin)
    fclose(f);
  exit(status);
}
