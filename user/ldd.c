// ldd FILE...: the shared libraries a program needs, and where they
// would be found.
#include <elf.h>
#include "user/user.h"

static int
readat(int fd, void *buf, int n, uint64 off)
{
  return pread(fd, buf, n, off) == n;
}

// the file a library name resolves to, by the loader's own rules
static const char *
locate(const char *name, char *buf, int len)
{
  static const char *const dirs[] = {"/lib/", "/", 0};
  struct stat st;

  if (strchr(name, '/'))
    return stat(name, &st) == 0 ? name : 0;
  for (int i = 0; dirs[i]; i++) {
    snprintf(buf, len, "%s%s", dirs[i], name);
    if (stat(buf, &st) == 0)
      return buf;
  }
  return 0;
}

static int
report(const char *path, int depth)
{
  int fd = open(path, O_RDONLY);
  Elf64_Ehdr eh;

  if (fd < 0) {
    fprintf(stderr, "ldd: cannot open %s\n", path);
    return -1;
  }
  if (!readat(fd, &eh, sizeof(eh), 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
    fprintf(stderr, "ldd: %s: not an ELF file\n", path);
    close(fd);
    return -1;
  }

  Elf64_Phdr ph;
  uint64 dynoff = 0, dynsize = 0, dynvaddr = 0;
  char interp[128];
  interp[0] = 0;
  for (int i = 0; i < eh.e_phnum; i++) {
    if (!readat(fd, &ph, sizeof(ph), eh.e_phoff + (uint64)i * eh.e_phentsize))
      break;
    if (ph.p_type == PT_INTERP && ph.p_filesz < sizeof(interp)) {
      readat(fd, interp, ph.p_filesz, ph.p_offset);
      interp[ph.p_filesz] = 0;
    } else if (ph.p_type == PT_DYNAMIC) {
      dynoff = ph.p_offset;
      dynsize = ph.p_filesz;
      dynvaddr = ph.p_vaddr;
    }
  }
  (void)dynvaddr;

  if (dynoff == 0) {
    if (depth == 0)
      printf("\tstatically linked\n");
    close(fd);
    return 0;
  }

  // the string table, to read the names out of the dynamic section
  uint64 strtab_vaddr = 0, strsz = 0;
  for (uint64 o = 0; o + sizeof(Elf64_Dyn) <= dynsize; o += sizeof(Elf64_Dyn)) {
    Elf64_Dyn d;
    if (!readat(fd, &d, sizeof(d), dynoff + o))
      break;
    if (d.d_tag == DT_NULL)
      break;
    if (d.d_tag == DT_STRTAB)
      strtab_vaddr = d.d_un.d_ptr;
    else if (d.d_tag == DT_STRSZ)
      strsz = d.d_un.d_val;
  }

  // the string table's file offset: find the segment that holds it
  uint64 stroff = 0;
  for (int i = 0; i < eh.e_phnum && strtab_vaddr; i++) {
    if (!readat(fd, &ph, sizeof(ph), eh.e_phoff + (uint64)i * eh.e_phentsize))
      break;
    if (ph.p_type == PT_LOAD && strtab_vaddr >= ph.p_vaddr && strtab_vaddr < ph.p_vaddr + ph.p_filesz)
      stroff = ph.p_offset + (strtab_vaddr - ph.p_vaddr);
  }
  if (stroff == 0 || strsz == 0 || strsz > (1 << 20)) {
    fprintf(stderr, "ldd: %s: no string table\n", path);
    close(fd);
    return -1;
  }
  char *strtab = malloc(strsz);
  if (strtab == 0 || !readat(fd, strtab, strsz, stroff)) {
    fprintf(stderr, "ldd: %s: cannot read the string table\n", path);
    free(strtab);
    close(fd);
    return -1;
  }

  if (depth == 0 && interp[0])
    printf("\t%s => %s\n", interp, interp);
  for (uint64 o = 0; o + sizeof(Elf64_Dyn) <= dynsize; o += sizeof(Elf64_Dyn)) {
    Elf64_Dyn d;
    if (!readat(fd, &d, sizeof(d), dynoff + o))
      break;
    if (d.d_tag == DT_NULL)
      break;
    if (d.d_tag != DT_NEEDED || d.d_un.d_val >= strsz)
      continue;
    const char *name = strtab + d.d_un.d_val;
    char buf[128];
    const char *found = locate(name, buf, sizeof(buf));
    if (found)
      printf("\t%s => %s\n", name, found);
    else
      printf("\t%s => not found\n", name);
  }
  free(strtab);
  close(fd);
  return 0;
}

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: ldd FILE...\n");
    exit(1);
  }
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    if (argc > 2)
      printf("%s:\n", argv[i]);
    if (report(argv[i], 0) < 0)
      bad = 1;
  }
  exit(bad);
}
