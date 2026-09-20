// readelf [-h] [-l] [-d] [-s] [-r] [-a] FILE...: what is inside an ELF
// file. the fields are printed the way binutils prints them, because
// that is what anyone reading the output will expect.
#include <elf.h>
#include "user/user.h"

static int show_header, show_phdrs, show_dynamic, show_syms, show_relocs;

static const char *
ptype(uint32 t)
{
  switch (t) {
  case PT_NULL: return "NULL";
  case PT_LOAD: return "LOAD";
  case PT_DYNAMIC: return "DYNAMIC";
  case PT_INTERP: return "INTERP";
  case PT_NOTE: return "NOTE";
  case PT_PHDR: return "PHDR";
  case PT_TLS: return "TLS";
  case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";
  case PT_GNU_STACK: return "GNU_STACK";
  case PT_GNU_RELRO: return "GNU_RELRO";
  default: return "unknown";
  }
}

static const char *
dtag(int64 t)
{
  switch (t) {
  case DT_NEEDED: return "NEEDED";
  case DT_PLTRELSZ: return "PLTRELSZ";
  case DT_PLTGOT: return "PLTGOT";
  case DT_HASH: return "HASH";
  case DT_STRTAB: return "STRTAB";
  case DT_SYMTAB: return "SYMTAB";
  case DT_RELA: return "RELA";
  case DT_RELASZ: return "RELASZ";
  case DT_RELAENT: return "RELAENT";
  case DT_STRSZ: return "STRSZ";
  case DT_SYMENT: return "SYMENT";
  case DT_INIT: return "INIT";
  case DT_FINI: return "FINI";
  case DT_SONAME: return "SONAME";
  case DT_RPATH: return "RPATH";
  case DT_SYMBOLIC: return "SYMBOLIC";
  case DT_PLTREL: return "PLTREL";
  case DT_DEBUG: return "DEBUG";
  case DT_TEXTREL: return "TEXTREL";
  case DT_JMPREL: return "JMPREL";
  case DT_BIND_NOW: return "BIND_NOW";
  case DT_INIT_ARRAY: return "INIT_ARRAY";
  case DT_FINI_ARRAY: return "FINI_ARRAY";
  case DT_INIT_ARRAYSZ: return "INIT_ARRAYSZ";
  case DT_FINI_ARRAYSZ: return "FINI_ARRAYSZ";
  case DT_RUNPATH: return "RUNPATH";
  case DT_FLAGS: return "FLAGS";
  case DT_GNU_HASH: return "GNU_HASH";
  case DT_FLAGS_1: return "FLAGS_1";
  default: return "unknown";
  }
}

static const char *
rtype(uint32 t)
{
  switch (t) {
  case R_RISCV_NONE: return "R_RISCV_NONE";
  case R_RISCV_32: return "R_RISCV_32";
  case R_RISCV_64: return "R_RISCV_64";
  case R_RISCV_RELATIVE: return "R_RISCV_RELATIVE";
  case R_RISCV_COPY: return "R_RISCV_COPY";
  case R_RISCV_JUMP_SLOT: return "R_RISCV_JUMP_SLOT";
  case R_RISCV_TLS_DTPMOD64: return "R_RISCV_TLS_DTPMOD64";
  case R_RISCV_TLS_DTPREL64: return "R_RISCV_TLS_DTPREL64";
  case R_RISCV_TLS_TPREL64: return "R_RISCV_TLS_TPREL64";
  case R_RISCV_IRELATIVE: return "R_RISCV_IRELATIVE";
  default: return "unknown";
  }
}

// the file offset an address belongs to, through the program headers
static uint64
vaddr_to_off(Elf64_Phdr *ph, int n, uint64 vaddr)
{
  for (int i = 0; i < n; i++)
    if (ph[i].p_type == PT_LOAD && vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_filesz)
      return ph[i].p_offset + (vaddr - ph[i].p_vaddr);
  return 0;
}

static int
dump(const char *path)
{
  int fd = open(path, O_RDONLY);
  Elf64_Ehdr eh;

  if (fd < 0) {
    fprintf(stderr, "readelf: cannot open %s\n", path);
    return -1;
  }
  if (pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
    fprintf(stderr, "readelf: %s: not an ELF file\n", path);
    close(fd);
    return -1;
  }

  if (show_header) {
    static const char *const types[] = {"NONE", "REL", "EXEC", "DYN", "CORE"};
    printf("ELF Header:\n");
    printf("  Class:                             ELF%d\n", eh.e_ident[EI_CLASS] == 2 ? 64 : 32);
    printf("  Type:                              %s\n", eh.e_type < 5 ? types[eh.e_type] : "?");
    printf("  Machine:                           %s\n", eh.e_machine == EM_RISCV ? "RISC-V" : "?");
    printf("  Entry point address:               0x%lx\n", eh.e_entry);
    printf("  Start of program headers:          %ld (bytes into file)\n", eh.e_phoff);
    printf("  Number of program headers:         %d\n", eh.e_phnum);
    printf("  Size of section headers:           %d (bytes)\n", eh.e_shentsize);
    printf("  Number of section headers:         %d\n", eh.e_shnum);
  }

  Elf64_Phdr *ph = malloc((uint64)eh.e_phnum * sizeof(Elf64_Phdr));
  if (ph == 0 || pread(fd, ph, eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff) !=
                     (int)(eh.e_phnum * sizeof(Elf64_Phdr))) {
    fprintf(stderr, "readelf: %s: cannot read the program headers\n", path);
    free(ph);
    close(fd);
    return -1;
  }

  if (show_phdrs) {
    printf("\nProgram Headers:\n");
    printf("  Type           Offset   VirtAddr           FileSiz  MemSiz   Flg Align\n");
    for (int i = 0; i < eh.e_phnum; i++) {
      printf("  %-14s 0x%06lx 0x%016lx 0x%06lx 0x%06lx %c%c%c 0x%lx\n", ptype(ph[i].p_type),
             ph[i].p_offset, ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz,
             (ph[i].p_flags & PF_R) ? 'R' : ' ', (ph[i].p_flags & PF_W) ? 'W' : ' ',
             (ph[i].p_flags & PF_X) ? 'E' : ' ', ph[i].p_align);
      if (ph[i].p_type == PT_INTERP) {
        char interp[128];
        int n = ph[i].p_filesz < sizeof(interp) ? (int)ph[i].p_filesz : (int)sizeof(interp) - 1;
        if (pread(fd, interp, n, ph[i].p_offset) == n) {
          interp[n] = 0;
          printf("      [Requesting program interpreter: %s]\n", interp);
        }
      }
    }
  }

  // the dynamic section, and the tables it points at
  uint64 dynoff = 0, dynsize = 0;
  for (int i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_DYNAMIC) {
      dynoff = ph[i].p_offset;
      dynsize = ph[i].p_filesz;
    }

  char *strtab = 0;
  uint64 strsz = 0, symtab_off = 0, hash_off = 0;
  uint64 rela_off = 0, relasz = 0, jmprel_off = 0, pltrelsz = 0;
  if (dynoff) {
    uint64 strtab_v = 0, symtab_v = 0, hash_v = 0, rela_v = 0, jmprel_v = 0;
    for (uint64 o = 0; o + sizeof(Elf64_Dyn) <= dynsize; o += sizeof(Elf64_Dyn)) {
      Elf64_Dyn d;
      if (pread(fd, &d, sizeof(d), dynoff + o) != sizeof(d) || d.d_tag == DT_NULL)
        break;
      switch (d.d_tag) {
      case DT_STRTAB: strtab_v = d.d_un.d_ptr; break;
      case DT_STRSZ: strsz = d.d_un.d_val; break;
      case DT_SYMTAB: symtab_v = d.d_un.d_ptr; break;
      case DT_HASH: hash_v = d.d_un.d_ptr; break;
      case DT_RELA: rela_v = d.d_un.d_ptr; break;
      case DT_RELASZ: relasz = d.d_un.d_val; break;
      case DT_JMPREL: jmprel_v = d.d_un.d_ptr; break;
      case DT_PLTRELSZ: pltrelsz = d.d_un.d_val; break;
      default: break;
      }
    }
    if (strtab_v && strsz && strsz < (1 << 22)) {
      uint64 off = vaddr_to_off(ph, eh.e_phnum, strtab_v);
      strtab = malloc(strsz);
      if (strtab && pread(fd, strtab, strsz, off) != (int)strsz) {
        free(strtab);
        strtab = 0;
      }
    }
    symtab_off = symtab_v ? vaddr_to_off(ph, eh.e_phnum, symtab_v) : 0;
    hash_off = hash_v ? vaddr_to_off(ph, eh.e_phnum, hash_v) : 0;
    rela_off = rela_v ? vaddr_to_off(ph, eh.e_phnum, rela_v) : 0;
    jmprel_off = jmprel_v ? vaddr_to_off(ph, eh.e_phnum, jmprel_v) : 0;
  }

  if (show_dynamic && dynoff) {
    printf("\nDynamic section at offset 0x%lx:\n", dynoff);
    printf("  Tag                Type              Name/Value\n");
    for (uint64 o = 0; o + sizeof(Elf64_Dyn) <= dynsize; o += sizeof(Elf64_Dyn)) {
      Elf64_Dyn d;
      if (pread(fd, &d, sizeof(d), dynoff + o) != sizeof(d))
        break;
      if (d.d_tag == DT_NULL)
        break;
      printf("  0x%016lx %-17s ", (uint64)d.d_tag, dtag(d.d_tag));
      if ((d.d_tag == DT_NEEDED || d.d_tag == DT_SONAME || d.d_tag == DT_RPATH ||
           d.d_tag == DT_RUNPATH) &&
          strtab && d.d_un.d_val < strsz)
        printf("%s\n", strtab + d.d_un.d_val);
      else
        printf("0x%lx\n", d.d_un.d_val);
    }
  }

  // how many symbols there are: the hash table's chain count says so
  uint32 nsyms = 0;
  if (hash_off) {
    uint32 h[2];
    if (pread(fd, h, sizeof(h), hash_off) == sizeof(h))
      nsyms = h[1];
  }

  if (show_syms && symtab_off && strtab && nsyms) {
    static const char *const bind[] = {"LOCAL", "GLOBAL", "WEAK"};
    static const char *const type[] = {"NOTYPE", "OBJECT", "FUNC", "SECTION", "FILE", "COMMON", "TLS"};
    printf("\nSymbol table contains %d entries:\n", nsyms);
    printf("   Num:    Value          Size Type    Bind   Name\n");
    for (uint32 i = 0; i < nsyms; i++) {
      Elf64_Sym sym;
      if (pread(fd, &sym, sizeof(sym), symtab_off + (uint64)i * sizeof(sym)) != sizeof(sym))
        break;
      int b = ELF64_ST_BIND(sym.st_info), t = ELF64_ST_TYPE(sym.st_info);
      printf("  %4d: %016lx %5ld %-7s %-6s %s\n", i, sym.st_value, sym.st_size,
             t < 7 ? type[t] : "?", b < 3 ? bind[b] : "?",
             sym.st_name < strsz ? strtab + sym.st_name : "");
    }
  }

  if (show_relocs && (rela_off || jmprel_off)) {
    struct {
      uint64 off, size;
      const char *what;
    } sets[] = {{rela_off, relasz, ".rela.dyn"}, {jmprel_off, pltrelsz, ".rela.plt"}};
    for (int k = 0; k < 2; k++) {
      if (sets[k].off == 0 || sets[k].size == 0)
        continue;
      printf("\nRelocation section '%s' contains %ld entries:\n", sets[k].what,
             sets[k].size / sizeof(Elf64_Rela));
      printf("  Offset           Type                 Symbol + Addend\n");
      for (uint64 o = 0; o + sizeof(Elf64_Rela) <= sets[k].size; o += sizeof(Elf64_Rela)) {
        Elf64_Rela r;
        if (pread(fd, &r, sizeof(r), sets[k].off + o) != sizeof(r))
          break;
        uint32 symi = ELF64_R_SYM(r.r_info);
        const char *name = "";
        if (symi && symtab_off && strtab) {
          Elf64_Sym sym;
          if (pread(fd, &sym, sizeof(sym), symtab_off + (uint64)symi * sizeof(sym)) == sizeof(sym) &&
              sym.st_name < strsz)
            name = strtab + sym.st_name;
        }
        printf("  %016lx %-20s %s + %ld\n", r.r_offset, rtype(ELF64_R_TYPE(r.r_info)), name,
               (long)r.r_addend);
      }
    }
  }

  free(strtab);
  free(ph);
  close(fd);
  return 0;
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-'; i++) {
    for (char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'h': show_header = 1; break;
      case 'l': show_phdrs = 1; break;
      case 'd': show_dynamic = 1; break;
      case 's': show_syms = 1; break;
      case 'r': show_relocs = 1; break;
      case 'a':
        show_header = show_phdrs = show_dynamic = show_syms = show_relocs = 1;
        break;
      default:
        fprintf(stderr, "usage: readelf [-hldsra] FILE...\n");
        exit(1);
      }
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: readelf [-hldsra] FILE...\n");
    exit(1);
  }
  if (!show_header && !show_phdrs && !show_dynamic && !show_syms && !show_relocs)
    show_header = show_phdrs = show_dynamic = 1;

  int bad = 0;
  for (; i < argc; i++) {
    if (argc - i > 1)
      printf("\nFile: %s\n", argv[i]);
    if (dump(argv[i]) < 0)
      bad = 1;
  }
  exit(bad);
}
