#ifndef _ELF_H
#define _ELF_H
//
// ELF64 for RISC-V, with the names the standard uses. the kernel has
// its own smaller view of the same format in kernel/elf.h.
//
#include <stdint.h>

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

#define EI_NIDENT 16
#define EI_CLASS  4
#define EI_DATA   5
#define EI_VERSION 6
#define EI_OSABI  7
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ELFMAG "\177ELF"
#define SELFMAG 4

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half e_type;
  Elf64_Half e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;
  Elf64_Off e_phoff;
  Elf64_Off e_shoff;
  Elf64_Word e_flags;
  Elf64_Half e_ehsize;
  Elf64_Half e_phentsize;
  Elf64_Half e_phnum;
  Elf64_Half e_shentsize;
  Elf64_Half e_shnum;
  Elf64_Half e_shstrndx;
} Elf64_Ehdr;

#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4
#define EM_RISCV 243

typedef struct {
  Elf64_Word p_type;
  Elf64_Word p_flags;
  Elf64_Off p_offset;
  Elf64_Addr p_vaddr;
  Elf64_Addr p_paddr;
  Elf64_Xword p_filesz;
  Elf64_Xword p_memsz;
  Elf64_Xword p_align;
} Elf64_Phdr;

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
  Elf64_Word sh_name;
  Elf64_Word sh_type;
  Elf64_Xword sh_flags;
  Elf64_Addr sh_addr;
  Elf64_Off sh_offset;
  Elf64_Xword sh_size;
  Elf64_Word sh_link;
  Elf64_Word sh_info;
  Elf64_Xword sh_addralign;
  Elf64_Xword sh_entsize;
} Elf64_Shdr;

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_DYNSYM   11
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_TLS       0x400

typedef struct {
  Elf64_Word st_name;
  unsigned char st_info;
  unsigned char st_other;
  Elf64_Half st_shndx;
  Elf64_Addr st_value;
  Elf64_Xword st_size;
} Elf64_Sym;

#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
#define ELF64_ST_VISIBILITY(o) ((o) & 3)
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6
#define STT_GNU_IFUNC 10
#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN  2
#define STV_PROTECTED 3
#define SHN_UNDEF 0
#define SHN_ABS   0xfff1
#define SHN_COMMON 0xfff2

typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
  Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
} Elf64_Rel;

#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffUL)
#define ELF64_R_INFO(s, t) (((Elf64_Xword)(s) << 32) + (t))

// the RISC-V relocations a dynamic linker sees
#define R_RISCV_NONE          0
#define R_RISCV_32            1
#define R_RISCV_64            2
#define R_RISCV_RELATIVE      3
#define R_RISCV_COPY          4
#define R_RISCV_JUMP_SLOT     5
#define R_RISCV_TLS_DTPMOD32  6
#define R_RISCV_TLS_DTPMOD64  7
#define R_RISCV_TLS_DTPREL32  8
#define R_RISCV_TLS_DTPREL64  9
#define R_RISCV_TLS_TPREL32   10
#define R_RISCV_TLS_TPREL64   11
#define R_RISCV_IRELATIVE     58

typedef struct {
  Elf64_Sxword d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr d_ptr;
  } d_un;
} Elf64_Dyn;

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_SYMBOLIC 16
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_DEBUG    21
#define DT_TEXTREL  22
#define DT_JMPREL   23
#define DT_BIND_NOW 24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH  29
#define DT_FLAGS    30
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33
#define DT_GNU_HASH 0x6ffffef5
#define DT_FLAGS_1  0x6ffffffb
#define DF_ORIGIN   0x01
#define DF_SYMBOLIC 0x02
#define DF_TEXTREL  0x04
#define DF_BIND_NOW 0x08
#define DF_STATIC_TLS 0x10
#define DF_1_NOW    0x00000001
#define DF_1_GLOBAL 0x00000002
#define DF_1_NODELETE 0x00000008

// the auxiliary vector entry, as the kernel leaves it above the stack
typedef struct {
  uint64_t a_type;
  union {
    uint64_t a_val;
  } a_un;
} Elf64_auxv_t;

#endif
