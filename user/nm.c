// nm: the symbols in an object file.
//
//   nm [-a] [-D] [-g] [-n] [-p] [-r] [-u] [-C] [-S] [--defined-only] FILE...
//
//   -a  every symbol, including the ones for files and sections
//   -D  the dynamic symbols rather than the static ones
//   -g  only the ones another file could use
//   -n  in address order
//   -p  in the order they appear, with no sorting
//   -r  the reverse of whatever order was chosen
//   -u  only the ones this file does not define
//   -S  print the size beside the address
//
// the letter beside each name is the familiar one: T for text, D for
// initialized data, B for bss, R for read-only, U for undefined, A for
// absolute, W for weak; lowercase when the symbol is local.
#include "user/user.h"
#include <elf.h>

static int show_all, want_dynamic, globals_only, by_address, no_sort;
static int reverse, undefined_only, defined_only, show_size;
static int many, status;

struct sym {
  char name[128];
  uint64 value;
  uint64 size;
  char kind;
  int defined;
  int order;
};

static struct sym *syms;
static int nsyms, maxsyms;

static void
push(const struct sym *s)
{
  if (nsyms == maxsyms) {
    maxsyms = maxsyms ? maxsyms * 2 : 256;
    syms = realloc(syms, maxsyms * sizeof(*syms));
    if (syms == 0) {
      fprintf(stderr, "nm: out of memory\n");
      exit(1);
    }
  }
  syms[nsyms] = *s;
  syms[nsyms].order = nsyms;
  nsyms++;
}

// the letter for a symbol, from its section and its binding
static char
classify(const Elf64_Sym *s, const Elf64_Shdr *sections, int nsections,
         const char *shstr, int shstrsize)
{
  int type = ELF64_ST_TYPE(s->st_info);
  int bind = ELF64_ST_BIND(s->st_info);
  char c;

  if (s->st_shndx == SHN_UNDEF)
    return bind == STB_WEAK ? 'w' : 'U';
  if (s->st_shndx == SHN_ABS)
    c = 'A';
  else if (s->st_shndx == SHN_COMMON)
    c = 'C';
  else if (s->st_shndx < nsections) {
    const Elf64_Shdr *sh = &sections[s->st_shndx];
    const char *name = sh->sh_name < (uint32)shstrsize ? shstr + sh->sh_name : "";
    if (sh->sh_type == SHT_NOBITS)
      c = 'B';
    else if (sh->sh_flags & SHF_EXECINSTR)
      c = 'T';
    else if (sh->sh_flags & SHF_WRITE)
      c = 'D';
    else
      c = 'R';
    // the sections whose names say more than their flags do
    if (strncmp(name, ".rodata", 7) == 0)
      c = 'R';
    else if (strcmp(name, ".bss") == 0)
      c = 'B';
  } else {
    c = '?';
  }

  if (bind == STB_WEAK)
    return type == STT_OBJECT ? 'V' : 'W';
  if (bind == STB_LOCAL)
    return c - 'A' + 'a';
  return c;
}

static int
compare(const void *va, const void *vb)
{
  const struct sym *a = va, *b = vb;
  int r;

  if (no_sort)
    r = a->order - b->order;
  else if (by_address)
    r = a->value < b->value ? -1 : a->value > b->value ? 1 : strcmp(a->name, b->name);
  else
    r = strcmp(a->name, b->name);
  return reverse ? -r : r;
}

static int
read_at(int fd, void *buf, int n, long long off)
{
  if (lseek(fd, off, SEEK_SET) < 0)
    return -1;
  int done = 0;
  while (done < n) {
    int r = read(fd, (char *)buf + done, n - done);
    if (r <= 0)
      return -1;
    done += r;
  }
  return 0;
}

static void
do_file(const char *path)
{
  Elf64_Ehdr eh;
  int fd = open(path, O_RDONLY);

  if (fd < 0) {
    fprintf(stderr, "nm: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  if (read_at(fd, &eh, sizeof(eh), 0) < 0 ||
      memcmp(eh.e_ident, ELFMAG, 4) != 0) {
    fprintf(stderr, "nm: %s: file format not recognized\n", path);
    status = 1;
    close(fd);
    return;
  }
  if (eh.e_shnum == 0 || eh.e_shoff == 0) {
    fprintf(stderr, "nm: %s: no symbols\n", path);
    close(fd);
    return;
  }

  int nsections = eh.e_shnum;
  Elf64_Shdr *sections = malloc(nsections * sizeof(Elf64_Shdr));
  if (sections == 0 ||
      read_at(fd, sections, nsections * sizeof(Elf64_Shdr), eh.e_shoff) < 0) {
    fprintf(stderr, "nm: %s: cannot read the section table\n", path);
    status = 1;
    free(sections);
    close(fd);
    return;
  }

  // the names of the sections
  char *shstr = 0;
  int shstrsize = 0;
  if (eh.e_shstrndx < nsections) {
    shstrsize = sections[eh.e_shstrndx].sh_size;
    shstr = malloc(shstrsize + 1);
    if (shstr && read_at(fd, shstr, shstrsize, sections[eh.e_shstrndx].sh_offset) < 0) {
      free(shstr);
      shstr = 0;
      shstrsize = 0;
    }
    if (shstr)
      shstr[shstrsize] = 0;
  }

  int want = want_dynamic ? SHT_DYNSYM : SHT_SYMTAB;
  int symidx = -1;
  for (int i = 0; i < nsections; i++)
    if (sections[i].sh_type == want)
      symidx = i;
  if (symidx < 0) {
    // a stripped program still has its dynamic symbols
    for (int i = 0; i < nsections; i++)
      if (sections[i].sh_type == SHT_DYNSYM)
        symidx = i;
  }
  if (symidx < 0) {
    fprintf(stderr, "nm: %s: no symbols\n", path);
    free(shstr);
    free(sections);
    close(fd);
    return;
  }

  int stridx = sections[symidx].sh_link;
  char *strtab = 0;
  int strsize = 0;
  if (stridx > 0 && stridx < nsections) {
    strsize = sections[stridx].sh_size;
    strtab = malloc(strsize + 1);
    if (strtab && read_at(fd, strtab, strsize, sections[stridx].sh_offset) < 0) {
      free(strtab);
      strtab = 0;
      strsize = 0;
    }
    if (strtab)
      strtab[strsize] = 0;
  }

  int count = sections[symidx].sh_size / sizeof(Elf64_Sym);
  Elf64_Sym *table = malloc(count * sizeof(Elf64_Sym));
  if (table == 0 ||
      read_at(fd, table, count * sizeof(Elf64_Sym), sections[symidx].sh_offset) < 0) {
    fprintf(stderr, "nm: %s: cannot read the symbols\n", path);
    status = 1;
    free(table);
    free(strtab);
    free(shstr);
    free(sections);
    close(fd);
    return;
  }

  nsyms = 0;
  for (int i = 0; i < count; i++) {
    const Elf64_Sym *s = &table[i];
    int type = ELF64_ST_TYPE(s->st_info);
    int bind = ELF64_ST_BIND(s->st_info);
    const char *name = (strtab && s->st_name < (uint32)strsize)
                           ? strtab + s->st_name : "";

    if (name[0] == 0)
      continue;
    if (!show_all && (type == STT_FILE || type == STT_SECTION))
      continue;
    if (globals_only && bind == STB_LOCAL)
      continue;

    struct sym out;
    memset(&out, 0, sizeof(out));
    snprintf(out.name, sizeof(out.name), "%s", name);
    out.value = s->st_value;
    out.size = s->st_size;
    out.kind = classify(s, sections, nsections, shstr, shstrsize);
    out.defined = s->st_shndx != SHN_UNDEF;

    if (undefined_only && out.defined)
      continue;
    if (defined_only && !out.defined)
      continue;
    push(&out);
  }

  qsort(syms, nsyms, sizeof(*syms), compare);

  if (many)
    printf("\n%s:\n", path);
  for (int i = 0; i < nsyms; i++) {
    if (!syms[i].defined)
      printf("                 ");
    else if (show_size)
      printf("%016llx %016llx ", (unsigned long long)syms[i].value,
             (unsigned long long)syms[i].size);
    else
      printf("%016llx ", (unsigned long long)syms[i].value);
    if (syms[i].defined && show_size)
      ;
    printf("%c %s\n", syms[i].kind, syms[i].name);
  }
  if (nsyms == 0)
    fprintf(stderr, "nm: %s: no symbols\n", path);

  free(table);
  free(strtab);
  free(shstr);
  free(sections);
  close(fd);
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
    if (strcmp(argv[i], "--defined-only") == 0) {
      defined_only = 1;
      continue;
    }
    if (strcmp(argv[i], "--undefined-only") == 0) {
      undefined_only = 1;
      continue;
    }
    if (strcmp(argv[i], "--dynamic") == 0) {
      want_dynamic = 1;
      continue;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'a': show_all = 1; break;
      case 'D': want_dynamic = 1; break;
      case 'g': globals_only = 1; break;
      case 'n': case 'v': by_address = 1; break;
      case 'p': no_sort = 1; break;
      case 'r': reverse = 1; break;
      case 'u': undefined_only = 1; break;
      case 'S': show_size = 1; break;
      case 'C': break;   // there is nothing here whose name is mangled
      case 'B': case 'P': break;
      default:
        fprintf(stderr, "usage: nm [-aDgnpruS] FILE...\n");
        exit(1);
      }
    }
  }

  if (i >= argc) {
    // nm with no argument reads a.out, as it always has
    do_file("a.out");
    exit(status);
  }
  many = argc - i > 1;
  for (; i < argc; i++)
    do_file(argv[i]);
  exit(status);
}
