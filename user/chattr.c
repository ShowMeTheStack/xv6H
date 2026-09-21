// chattr and lsattr: the flags an ext4 inode carries beside its mode.
//
//   chattr [-R] [+-=ATCDSacdeijstu] FILE...
//   lsattr [-R] [-a] [-d] [FILE...]
//
// the two this system acts on are the ones worth having:
//
//   i  immutable: the file cannot be written, renamed, removed, or
//      have a link made to it, and its flags cannot be changed back
//      by anyone without CAP_LINUX_IMMUTABLE
//   a  append only: it can be written, but only at the end, and it
//      cannot be removed or renamed
//
// the rest are read and written faithfully so that an image made here
// and one made by Linux say the same thing about a file, but nothing
// in this kernel acts on them yet; lsattr prints them all.
#include "user/user.h"
#include <sys/ioctl.h>

static int recursive, show_all, dirs_as_files, status;

// the letters, in the order lsattr prints them
static const struct {
  char letter;
  uint flag;
  const char *name;
} attrs[] = {
  {'s', 0x00000001, "secure deletion"},
  {'u', 0x00000002, "undelete"},
  {'S', 0x00000008, "synchronous updates"},
  {'i', 0x00000010, "immutable"},
  {'a', 0x00000020, "append only"},
  {'d', 0x00000040, "no dump"},
  {'A', 0x00000080, "no atime"},
  {'I', 0x00001000, "indexed directory"},
  {'j', 0x00004000, "data journalling"},
  {'D', 0x00010000, "synchronous directory"},
  {'T', 0x00020000, "top of directory"},
  {'e', 0x00080000, "extents"},
  {'E', 0x10000000, "inline data"},
  {'P', 0x20000000, "project inherit"},
  {'F', 0x40000000, "case insensitive"},
  {0, 0, 0}
};

// the order the letters print in, which is lsattr's
static const char *order = "suSiadAIjDTeEPF";

static void
print_flags(uint flags)
{
  for (const char *p = order; *p; p++) {
    uint bit = 0;
    for (int i = 0; attrs[i].letter; i++)
      if (attrs[i].letter == *p)
        bit = attrs[i].flag;
    putchar((flags & bit) ? *p : '-');
  }
}

static uint
letter_flag(int c)
{
  for (int i = 0; attrs[i].letter; i++)
    if (attrs[i].letter == c)
      return attrs[i].flag;
  return 0;
}

static int
get_flags(const char *path, uint *out)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    return -1;
  int r = ioctl(fd, FS_IOC_GETFLAGS, (uint64)out);
  close(fd);
  return r;
}

static int
set_flags(const char *path, uint flags)
{
  // the file is opened for reading: changing the flags is not writing
  // the file, and an immutable file cannot be opened for writing at all
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    return -1;
  int r = ioctl(fd, FS_IOC_SETFLAGS, (uint64)&flags);
  close(fd);
  return r;
}

static void walk(const char *path, void (*fn)(const char *));

static void
list_one(const char *path)
{
  uint flags = 0;

  if (get_flags(path, &flags) < 0) {
    fprintf(stderr, "lsattr: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  print_flags(flags);
  printf(" %s\n", path);
}

static uint add_bits, del_bits, set_bits;
static int use_set;

static void
change_one(const char *path)
{
  uint flags = 0;

  if (get_flags(path, &flags) < 0) {
    fprintf(stderr, "chattr: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  uint want = use_set ? set_bits : ((flags | add_bits) & ~del_bits);
  if (want == flags)
    return;
  if (set_flags(path, want) < 0) {
    fprintf(stderr, "chattr: %s: %s\n", path, strerror(errno));
    status = 1;
  }
}

static void
walk(const char *path, void (*fn)(const char *))
{
  struct stat st;

  if (lstat(path, &st) < 0) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  if (st.type != T_DIR || dirs_as_files) {
    fn(path);
    return;
  }
  if (!recursive) {
    fn(path);
    return;
  }
  fn(path);
  DIR *d = opendir(path);
  if (d == 0)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (!show_all && e->d_name[0] == '.')
      continue;
    char child[PATH_MAX];
    int n = strlen(path);
    snprintf(child, sizeof(child), "%s%s%s", path,
             n > 0 && path[n - 1] == '/' ? "" : "/", e->d_name);
    walk(child, fn);
  }
  closedir(d);
}

int
main(int argc, char *argv[])
{
  const char *me = argv[0];
  int i = 1;

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  int is_lsattr = strcmp(me, "lsattr") == 0;

  for (; i < argc; i++) {
    const char *a = argv[i];

    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strcmp(a, "-R") == 0) {
      recursive = 1;
      continue;
    }
    if (is_lsattr && strcmp(a, "-a") == 0) {
      show_all = 1;
      continue;
    }
    if (strcmp(a, "-d") == 0) {
      dirs_as_files = 1;
      continue;
    }
    if (is_lsattr && (strcmp(a, "-l") == 0 || strcmp(a, "-v") == 0))
      continue;
    if (!is_lsattr && (a[0] == '+' || a[0] == '-' || a[0] == '=')) {
      if (a[0] == '=')
        use_set = 1;
      for (const char *p = a + 1; *p; p++) {
        uint bit = letter_flag(*p);
        if (bit == 0) {
          fprintf(stderr, "chattr: %c is not an attribute\n", *p);
          exit(1);
        }
        if (a[0] == '+')
          add_bits |= bit;
        else if (a[0] == '-')
          del_bits |= bit;
        else
          set_bits |= bit;
      }
      continue;
    }
    break;
  }

  if (!is_lsattr && add_bits == 0 && del_bits == 0 && !use_set) {
    fprintf(stderr, "usage: chattr [-R] [+-=ATCDSacdeijstu] FILE...\n");
    exit(1);
  }

  if (i >= argc) {
    if (is_lsattr)
      walk(".", list_one);
    else {
      fprintf(stderr, "usage: chattr [-R] [+-=...] FILE...\n");
      exit(1);
    }
    exit(status);
  }
  for (; i < argc; i++)
    walk(argv[i], is_lsattr ? list_one : change_one);
  exit(status);
}
