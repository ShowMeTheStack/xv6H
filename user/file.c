// file: what a file appears to be.
//
//   file [-b] [-i] [-L] FILE...
//
// the answer comes from the first few hundred bytes. there is no
// magic database here: the formats this system actually deals with
// are recognised by their signatures, and anything else is called
// text or data depending on what the bytes are.
//
//   -b   just the answer, without the name
//   -i   a mime type instead of a sentence
//   -L   follow a symbolic link and describe what it points at
#include "user/user.h"
#include <elf.h>

static int brief, mime, follow;

struct magic {
  int offset;
  int len;
  const char *bytes;
  const char *what;
  const char *type;
};

// the signatures, longest and most specific first
static const struct magic magics[] = {
  { 0, 4, "\177ELF",          0 /* handled below */, "application/x-executable" },
  { 0, 2, "\037\213",         "gzip compressed data", "application/gzip" },
  { 0, 4, "PK\003\004",       "Zip archive data", "application/zip" },
  { 257, 5, "ustar",          "POSIX tar archive", "application/x-tar" },
  { 0, 8, "\211PNG\r\n\032\n","PNG image data", "image/png" },
  { 0, 3, "\377\330\377",     "JPEG image data", "image/jpeg" },
  { 0, 6, "GIF89a",           "GIF image data", "image/gif" },
  { 0, 6, "GIF87a",           "GIF image data", "image/gif" },
  { 0, 4, "%PDF",             "PDF document", "application/pdf" },
  { 0, 4, "\xca\xfe\xba\xbe", "Java class data", "application/x-java-applet" },
  { 0, 4, "OggS",             "Ogg data", "application/ogg" },
  { 0, 4, "fLaC",             "FLAC audio", "audio/flac" },
  { 0, 2, "BZ",               "bzip2 compressed data", "application/x-bzip2" },
  { 0, 6, "\3757zXZ\000",     "XZ compressed data", "application/x-xz" },
  { 0, 4, "\x28\xb5\x2f\xfd", "Zstandard compressed data", "application/zstd" },
  { 0, 2, "MZ",               "MS-DOS executable", "application/x-dosexec" },
  { 0, 4, "\x7f\x45\x4c\x46", "ELF", "application/x-executable" },
};

static void
say(const char *path, const char *what, const char *type)
{
  if (brief)
    printf("%s\n", mime ? type : what);
  else
    printf("%s: %s\n", path, mime ? type : what);
}

// an ELF header says a great deal about itself
static void
describe_elf(const char *path, const unsigned char *buf, int n)
{
  static char out[256];
  const char *cls = "invalid";
  const char *kind = "unknown";
  const char *mach = "unknown machine";

  if (n < 20) {
    say(path, "ELF (truncated)", "application/x-executable");
    return;
  }
  cls = buf[4] == 1 ? "32-bit" : buf[4] == 2 ? "64-bit" : "invalid";
  const char *endian = buf[5] == 1 ? "LSB" : "MSB";
  int type = buf[16] | (buf[17] << 8);
  switch (type) {
  case ET_REL: kind = "relocatable"; break;
  case ET_EXEC: kind = "executable"; break;
  case ET_DYN: kind = "pie executable"; break;
  case ET_CORE: kind = "core file"; break;
  }
  int machine = buf[18] | (buf[19] << 8);
  switch (machine) {
  case 243: mach = "UCB RISC-V"; break;
  case 62: mach = "x86-64"; break;
  case 183: mach = "ARM aarch64"; break;
  case 40: mach = "ARM"; break;
  case 3: mach = "Intel 80386"; break;
  }
  snprintf(out, sizeof(out), "ELF %s %s %s, %s, version 1 (SYSV)", cls,
           endian, kind, mach);
  say(path, out, type == ET_DYN ? "application/x-sharedlib"
                                : "application/x-executable");
}

static void
describe(const char *path)
{
  struct stat st;
  unsigned char buf[1024];

  int r = follow ? stat(path, &st) : lstat(path, &st);
  if (r < 0) {
    if (brief)
      printf("cannot open\n");
    else
      printf("%s: cannot open '%s' (%s)\n", path, path, strerror(errno));
    return;
  }
  if (st.type == T_DIR) {
    say(path, "directory", "inode/directory");
    return;
  }
  if (st.type == T_SYMLINK) {
    char target[PATH_MAX];
    int n = readlink(path, target, sizeof(target) - 1);
    target[n > 0 ? n : 0] = 0;
    static char out[PATH_MAX + 32];
    snprintf(out, sizeof(out), "symbolic link to %s", target);
    say(path, out, "inode/symlink");
    return;
  }
  if (st.type == T_DEVICE) {
    say(path, (st.rdev >> 8) == STAT_DEV_BLOCK ? "block special" :
              "character special",
        (st.rdev >> 8) == STAT_DEV_BLOCK ? "inode/blockdevice"
                                         : "inode/chardevice");
    return;
  }
  if (st.size == 0) {
    say(path, "empty", "inode/x-empty");
    return;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    if (brief)
      printf("cannot open\n");
    else
      printf("%s: cannot open '%s' (%s)\n", path, path, strerror(errno));
    return;
  }
  int n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n <= 0) {
    say(path, "empty", "inode/x-empty");
    return;
  }

  for (unsigned k = 0; k < sizeof(magics) / sizeof(magics[0]); k++) {
    const struct magic *m = &magics[k];
    if (n < m->offset + m->len)
      continue;
    if (memcmp(buf + m->offset, m->bytes, m->len) != 0)
      continue;
    if (m->what == 0) {
      describe_elf(path, buf, n);
      return;
    }
    say(path, m->what, m->type);
    return;
  }

  // a script says what runs it on its first line
  if (n > 2 && buf[0] == '#' && buf[1] == '!') {
    static char out[256];
    int e = 2;
    while (e < n && buf[e] != '\n' && e < 200)
      e++;
    while (e > 2 && (buf[e - 1] == ' ' || buf[e - 1] == '\r'))
      e--;
    int s = 2;
    while (s < e && buf[s] == ' ')
      s++;
    snprintf(out, sizeof(out), "%.*s script, ASCII text executable", e - s,
             buf + s);
    say(path, out, "text/x-shellscript");
    return;
  }

  // nothing recognised it: text if every byte could be text
  int printable = 1, has_high = 0, crlf = 0;
  for (int i = 0; i < n; i++) {
    if (buf[i] == 0) {
      printable = 0;
      break;
    }
    if (buf[i] >= 0x80)
      has_high = 1;
    else if (buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\t' &&
             buf[i] != '\r' && buf[i] != '\f' && buf[i] != '\b' &&
             buf[i] != 0x1b) {
      printable = 0;
      break;
    }
    if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
      crlf = 1;
  }
  if (!printable) {
    say(path, "data", "application/octet-stream");
    return;
  }
  static char out[64];
  snprintf(out, sizeof(out), "%s text%s", has_high ? "UTF-8 Unicode" : "ASCII",
           crlf ? ", with CRLF line terminators" : "");
  say(path, out, has_high ? "text/plain; charset=utf-8"
                          : "text/plain; charset=us-ascii");
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'b': brief = 1; break;
      case 'i': mime = 1; break;
      case 'L': follow = 1; break;
      case 'h': follow = 0; break;
      default:
        fprintf(stderr, "usage: file [-b] [-i] [-L] FILE...\n");
        exit(1);
      }
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: file [-b] [-i] [-L] FILE...\n");
    exit(1);
  }
  for (; i < argc; i++)
    describe(argv[i]);
  exit(0);
}
